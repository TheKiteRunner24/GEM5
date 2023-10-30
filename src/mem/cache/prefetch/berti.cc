//
// Created by linjiawei on 22-10-31.
//

#include "mem/cache/prefetch/berti.hh"

#include "debug/BertiPrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5
{
namespace prefetch
{

BertiPrefetcher::BertiStats::BertiStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(num_train_hit, statistics::units::Count::get(), ""),
      ADD_STAT(num_train_miss, statistics::units::Count::get(), ""),
      ADD_STAT(train_pc, statistics::units::Count::get(), ""),
      ADD_STAT(num_fill_prefetch, statistics::units::Count::get(), ""),
      ADD_STAT(num_fill_miss, statistics::units::Count::get(), ""),
      ADD_STAT(fill_pc, statistics::units::Count::get(), ""),
      ADD_STAT(fill_latency, statistics::units::Count::get(), ""),
      ADD_STAT(pf_delta, statistics::units::Count::get(), "")
{
    train_pc.init(0);
    fill_pc.init(0);
    fill_latency.init(0);
    pf_delta.init(0);
}

BertiPrefetcher::BertiPrefetcher(const BertiPrefetcherParams &p)
    : Queued(p),
      historyTable(p.history_table_assoc, p.history_table_entries,
                   p.history_table_indexing_policy,
                   p.history_table_replacement_policy),
    //   tableOfDeltas(p.table_of_deltas_entries, p.table_of_deltas_entries,
    //                 p.table_of_deltas_indexing_policy,
    //                 p.table_of_deltas_replacement_policy,
    //                 TableOfDeltasEntry()),
      aggressive_pf(p.aggressive_pf),
      statsBerti(this)
{
}

BertiPrefetcher::HistoryTableEntry*
BertiPrefetcher::updateHistoryTable(const PrefetchInfo &pfi)
{
    HistoryTableEntry *entry =
        historyTable.findEntry(pcHash(pfi.getPC()), pfi.isSecure());
    HistoryInfo new_info = {
        .lineAddr = blockIndex(pfi.getAddr()),
        .timestamp = curCycle()
    };
    evict_bestDelta = 0;
    if (entry) {
        historyTable.accessEntry(entry);
        DPRINTF(BertiPrefetcher,
                "History table hit, ip: [%lx] lineAddr: [%d]\n", pfi.getPC(),
                new_info.lineAddr);
        if (entry->history.size() >= maxHistorySize) {
            entry->history.erase(entry->history.begin());
        }
        entry->history.push_back(new_info);
        entry->hysteresis = true;
        return entry;
    } else {
        DPRINTF(BertiPrefetcher, "History table miss, ip: [%lx]\n",
                pfi.getPC());
        entry = historyTable.findVictim(pcHash(pfi.getPC()));
        if (entry->hysteresis) {
            entry->hysteresis = false;
            historyTable.insertEntry(pcHash(entry->pc), entry->isSecure(), entry);
        }
        else {
            if (entry->best_status != NO_PREF) {
                evict_bestDelta = entry->best_delta;
            }
            // only when hysteresis is false
            entry->pc = pfi.getPC();
            entry->history.clear();
            entry->history.push_back(new_info);
            historyTable.insertEntry(pcHash(pfi.getPC()), pfi.isSecure(), entry);
        }
    }
    return nullptr;
}


void BertiPrefetcher::searchTimelyDeltas(
    HistoryTableEntry &entry,
    const Cycles &latency,
    const Cycles &demand_cycle,
    const Addr &blk_addr)
{
    DPRINTF(BertiPrefetcher, "latency: %lu, demand_cycle; %lu\n", latency, demand_cycle);
    std::list<int64_t> new_deltas;
    for (auto it = entry.history.rbegin(); it != entry.history.rend(); it++) {
        int64_t delta = blk_addr - it->lineAddr;
        // if not timely, skip and continue
        if (it->timestamp + latency >= demand_cycle) {
            DPRINTF(BertiPrefetcher, "skip untimely delta: %lu + %lu <= %u : %ld\n", it->timestamp, latency, demand_cycle, delta);
            continue;
        }
        if (delta != 0) {
            new_deltas.push_back(delta);
            DPRINTF(BertiPrefetcher, "Timely delta found: [%d](%d - %d)\n",
                    delta, blk_addr, it->lineAddr);
            if (new_deltas.size() >= 6) {
                break;
            }
        }
    }

    entry.counter++;

    for (auto &delta : new_deltas) {
        bool miss = true;
        for (auto &delta_info : entry.deltas) {
            if (delta_info.coverageCounter != 0 && delta_info.delta == delta) {
                delta_info.coverageCounter++;
                miss = false;
                break;
            }
        }
        // miss
        if (miss) {
            // find the smallest coverage and replace
            int replace_idx = 0;
            for (auto i = 0; i < entry.deltas.size(); i++) {
                if (entry.deltas[replace_idx].coverageCounter >= entry.deltas[i].coverageCounter) {
                    replace_idx = i;
                }
            }
            entry.deltas[replace_idx].delta = delta;
            entry.deltas[replace_idx].coverageCounter = 1;
            entry.deltas[replace_idx].status = NO_PREF;
            DPRINTF(BertiPrefetcher, "Add new delta: %d\n", delta);
        }
    }

    if (entry.counter >= 6) {
        entry.updateStatus();
        if (entry.counter >= 16) {
            entry.resetConfidence(false);
        }
    }
    printDeltaTableEntry(entry);
}

void
BertiPrefetcher::calculatePrefetch(
    const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses, bool late, PrefetchSourceType pf_source, bool miss_repeat)
{
    DPRINTF(BertiPrefetcher,
            "Train prefetcher, ip: [%lx] "
            "Addr: [%d] miss: %d last lat: [%d]\n",
            pfi.getPC(), blockAddress(pfi.getAddr()),
            pfi.isCacheMiss(), lastFillLatency);

    if (pfi.isCacheMiss()) {
        statsBerti.num_train_miss++;
    } else {
        statsBerti.num_train_hit++;
        HistoryTableEntry *hist_entry = historyTable.findEntry(
            pcHash(pfi.getPC()), pfi.isSecure());
        if (hist_entry) {
            std::vector<int64_t> deltas;
            searchTimelyDeltas(*hist_entry, lastFillLatency,
                               curCycle(),
                               blockIndex(pfi.getAddr()));
            // updateTableOfDeltas(pfi.getPC(), pfi.isSecure(), deltas);
        }
    }
    statsBerti.train_pc.sample(pfi.getPC());

    /** 1.train: update history table */
    auto entry = updateHistoryTable(pfi);
    /** 2.prefetch: search table of deltas, issue prefetch request */
    if (entry) {
        DPRINTF(BertiPrefetcher, "Delta table hit, ip: [%lx]\n", pfi.getPC());
        if (aggressive_pf) {
            for (auto &delta_info : entry->deltas) {
                if (delta_info.status != NO_PREF) {
                    DPRINTF(BertiPrefetcher, "Using delta [%d] to prefetch\n",
                            delta_info.delta);
                    int64_t delta = delta_info.delta;
                    statsBerti.pf_delta.sample(delta);
                    Addr pf_addr =
                        (blockIndex(pfi.getAddr()) + delta) << lBlkSize;
                    sendPFWithFilter(pf_addr, addresses, 32, PrefetchSourceType::Berti);
                }
            }
        } else {
            if (entry->best_delta != 0) {
                DPRINTF(BertiPrefetcher, "Using best delta [%d] to prefetch\n",
                        entry->best_delta);
                statsBerti.pf_delta.sample(entry->best_delta);
                Addr pf_addr = (blockIndex(pfi.getAddr()) +
                                entry->best_delta) << lBlkSize;
                sendPFWithFilter(pf_addr, addresses, 32, PrefetchSourceType::Berti);
            }
        }

        if (entry->best_status == L1_PREF) {
            temp_bestDelta = entry->best_delta;
        }
        else {
            temp_bestDelta = 0;
        }
    }
    else {
        temp_bestDelta = 0;
    }
}

bool
BertiPrefetcher::sendPFWithFilter(Addr addr, std::vector<AddrPriority> &addresses, int prio,
                                        PrefetchSourceType src)
{
    if (filter->contains(addr)) {
        DPRINTF(BertiPrefetcher, "Skip recently prefetched: %lx\n", addr);
        return false;
    } else {
        DPRINTF(BertiPrefetcher, "Send pf: %lx\n", addr);
        filter->insert(addr, 0);
        addresses.push_back(AddrPriority(addr, prio, src));
        return true;
    }
}


void
BertiPrefetcher::notifyFill(const PacketPtr &pkt)
{
    if (pkt->req->isInstFetch() ||
        !pkt->req->hasVaddr() || !pkt->req->hasPC()) {
        DPRINTF(BertiPrefetcher, "Skip packet: %s\n", pkt->print());
        return;
    }

    DPRINTF(BertiPrefetcher,
            "Cache Fill: %s isPF: %d, pc: %lx\n",
            pkt->print(), pkt->req->isPrefetch(), pkt->req->getPC());

    if (pkt->req->isPrefetch()) {
        statsBerti.num_fill_prefetch++;
        return;
    } else {
        statsBerti.num_fill_miss++;
    }

    // fill latency
    Cycles latency = ticksToCycles(curTick() - pkt->req->time());
    latency += Cycles(20);
    // update lastFillLatency for prefetch on hit
    lastFillLatency = latency;

    statsBerti.fill_pc.sample(pkt->req->getPC());

    HistoryTableEntry *entry =
        historyTable.findEntry(pcHash(pkt->req->getPC()), pkt->req->isSecure());
    if (!entry)
        return;

    /** Search history table, find deltas. */
    Cycles demand_cycle = ticksToCycles(pkt->req->time());
    // Cycles wrappedLatency;
    // if (latency > 500){
    //     wrappedLatency = Cycles(500);
    // } else if (latency % 10 == 0) {
    //     wrappedLatency = Cycles((latency / 10) * 10);
    // } else {
    //     wrappedLatency = Cycles( ((latency / 10) + 1) * 10 );
    // }
    // statsBerti.fill_latency.sample(wrappedLatency);

    DPRINTF(BertiPrefetcher, "Updating table of deltas, latency [%d]\n",
            latency);

    std::vector<int64_t> timely_deltas = std::vector<int64_t>();
    searchTimelyDeltas(*entry, latency, demand_cycle,
                       blockIndex(pkt->req->getVaddr()));

    // /** Update table of deltas. */
    // updateTableOfDeltas(pkt->req->getPC(), pkt->req->isSecure(),
    //                     timely_deltas);
}

}
}
