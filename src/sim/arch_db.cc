
#include "sim/arch_db.hh"

#include "params/ArchDBer.hh"

namespace gem5{

ArchDBer::ArchDBer(const Params &p)
    : SimObject(p), dump(p.dump_from_start),
    dump_rolling(p.enable_rolling),
    mem_db(nullptr), zErrMsg(nullptr),rc(0),
    db_path(p.arch_db_file)
{
  int rc = sqlite3_open(":memory:", &mem_db);
  if (rc) {
    sqlite3_close(mem_db);
    fatal("Can't open database: %s\n", sqlite3_errmsg(mem_db));
  }

  fatal_if(db_path == "" || db_path == "None",
            "Arch db file path is not given!");

  for (const auto &s : p.table_cmds) {
    create_table(s);
  }
  registerExitCallback([this](){ save_db(); });
}

static int callback(void *NotUsed, int argc, char **argv, char **azColName){
  return 0;
}

void ArchDBer::create_table(const std::string &sql) {
  // create table
  rc = sqlite3_exec(mem_db, sql.c_str(), callback, 0, &zErrMsg);
  fatal_if(rc != SQLITE_OK, "SQL error: %s\n", zErrMsg);
  inform("Table created: %s\n", sql.c_str());
}

void ArchDBer::start_recording() {
  dump = true;
}

void ArchDBer::save_db() {
  warn("saving memdb to %s ...\n", db_path.c_str());
  sqlite3 *disk_db;
  sqlite3_backup *pBackup;
  int rc = sqlite3_open(db_path.c_str(), &disk_db);
  if (rc == SQLITE_OK){
    pBackup = sqlite3_backup_init(disk_db, "main", mem_db, "main");
    if (pBackup){
      (void)sqlite3_backup_step(pBackup, -1);
      (void)sqlite3_backup_finish(pBackup);
    }
    rc = sqlite3_errcode(disk_db);
  }
  sqlite3_close(disk_db);
}

DBTraceManager *
ArchDBer::addAndGetTrace(const char *name, std::vector<std::pair<std::string, DataType>> fields)
{
  _traces[name] = DBTraceManager(name, fields, mem_db);
  return &_traces[name];
}

void
ArchDBer::memTraceWrite(Tick tick, bool is_load, Addr pc, Addr vaddr, Addr paddr, uint64_t issued, uint64_t translated,
                        uint64_t completed, uint64_t committed, uint64_t writenback, int pf_src)
{
  if (!dump) return;

  sprintf(memTraceSQLBuf,
          "INSERT INTO MemTrace(Tick,IsLoad,PC,VADDR,PADDR,Issued,Translated,Completed,Committed,Writenback,PFSrc) "
          "VALUES(%ld,%d,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%d);",
          tick, is_load, pc, vaddr, paddr, issued, translated, completed, committed, writenback,pf_src);
  rc = sqlite3_exec(mem_db, memTraceSQLBuf, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}


void ArchDBer::L1MissTrace_write(
  uint64_t pc,
  uint64_t source,
  uint64_t paddr,
  uint64_t vaddr,
  uint64_t stamp,
  const char * site
) {
  if (!dump) return;
  char sql[512];
  sprintf(sql,
    "INSERT INTO L1MissTrace(PC,SOURCE,PADDR,VADDR, STAMP, SITE) " \
    "VALUES(%ld, %ld, %ld, %ld, %ld, '%s');",
    pc,source,paddr,vaddr, stamp, site
  );
  rc = sqlite3_exec(mem_db, sql, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}

void ArchDBer::L1EvictTraceWrite(
  uint64_t paddr,
  uint64_t stamp,
  const char * site
) {
  if (!dump) return;
  char sql[512];
  sprintf(sql,
    "INSERT INTO L1EvictTrace(PADDR, STAMP, SITE) " \
    "VALUES(%ld, %ld, '%s');",
    paddr, stamp, site
  );
  rc = sqlite3_exec(mem_db, sql, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}

void
DBTraceManager::init_table() {
  // create table
  char sql[1024];
  int pos = 0;
  pos = sprintf(sql,
    "CREATE TABLE %s(" \
    "ID INTEGER PRIMARY KEY AUTOINCREMENT, " \
    "TICK INT NOT NULL", _name.c_str());
  for (auto it = _fields.begin(); it != _fields.end(); it++) {
    switch (it->second) {
      case UINT64:
        pos += sprintf(sql+pos, ",%s INT NOT NULL", it->first.c_str());
        break;
      case TEXT:
        pos += sprintf(sql+pos, ",%s TEXT", it->first.c_str());
        break;
      default:
        fatal("Unknown data type");
    }
  }
  pos += sprintf(sql+pos, ");");
  assert(pos < 1024);
  printf("%s\n", sql);
  char *zErrMsg;
  int rc = sqlite3_exec(_db, sql, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  } else {
    warn("Table created: %s\n", _name.c_str());
  }
}

void
DBTraceManager::write_record(const Record &record)
{
  char sql[1024];
  int pos = 0;
  pos = sprintf(sql, "INSERT INTO %s(TICK", _name.c_str());
  for (auto it = _fields.begin(); it != _fields.end(); it++) {
    pos += sprintf(sql+pos, ",%s", it->first.c_str());
  }
  pos += sprintf(sql+pos, ") VALUES(%ld", record._tick);
  for (auto it = _fields.begin(); it != _fields.end(); it++) {
    switch (it->second) {
      case UINT64:
      {
        auto &m = record._uint64_data;
        auto data = m.find(it->first);
        if (data == m.end()) {
          fatal("Can't find data for %s\n", it->first.c_str());
        }
        assert(data != m.end());
        pos += sprintf(sql+pos, ",%ld", data->second);
        break;
      }
      case TEXT:
      {
        auto &m = record._text_data;
        auto data = m.find(it->first);
        if (data == m.end()) {
          fatal("Can't find data for %s\n", it->first.c_str());
        }
        assert(data != m.end());
        pos += sprintf(sql+pos, ",'%s'", data->second.c_str());
        break;
      }
      default:
        fatal("Unknown data type!\n");
    }
  }
  pos += sprintf(sql+pos, ");");
  assert(pos < 1024);
  char *zErrMsg;
  int rc = sqlite3_exec(_db, sql, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
    fatal("SQL error: %s\n", zErrMsg);
  };
}

} // namespace gem5

