#include "LeanStore.hpp"

#include "leanstore/profiling/counters/CPUCounters.hpp"
#include "leanstore/profiling/counters/PPCounters.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
#include "leanstore/profiling/tables/BMTable.hpp"
#include "leanstore/profiling/tables/CPUTable.hpp"
#include "leanstore/profiling/tables/CRTable.hpp"
#include "leanstore/profiling/tables/DTTable.hpp"
#include "leanstore/utils/FVector.hpp"
#include "leanstore/utils/ThreadLocalAggregator.hpp"
// -------------------------------------------------------------------------------------
#include "gflags/gflags.h"
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"
#include "tabulate/table.hpp"
// -------------------------------------------------------------------------------------
#include <linux/fs.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <locale>
#include <sstream>
// -------------------------------------------------------------------------------------
using namespace tabulate;
using leanstore::utils::threadlocal::sum;
namespace rs = rapidjson;
namespace leanstore
{
// -------------------------------------------------------------------------------------
LeanStore::LeanStore(double effective_dram_gib): effective_dram_gib(effective_dram_gib)
{
   LeanStore::addStringFlag("ssd_path", &FLAGS_ssd_path);
   if (FLAGS_recover_file != "./leanstore.json") {
      FLAGS_recover = true;
   }
   if (FLAGS_persist_file != "./leanstore.json") {
      FLAGS_persist = true;
   }
   if (FLAGS_recover) {
      deserializeFlags();
   }
   // -------------------------------------------------------------------------------------
   // Check if configurations make sense
   ensure(!FLAGS_vw || FLAGS_wal);
   // -------------------------------------------------------------------------------------
   // Set the default logger to file logger
   // Init SSD pool
   int flags = O_RDWR | O_DIRECT;
   if (FLAGS_trunc) {
      flags |= O_TRUNC | O_CREAT;
   }
   ssd_fd = open(FLAGS_ssd_path.c_str(), flags, 0666);
   posix_check(ssd_fd > -1);
   if (FLAGS_falloc > 0) {
      const u64 gib_size = 1024ull * 1024ull * 1024ull;
      auto dummy_data = (u8*)aligned_alloc(4096, gib_size);
      for (u64 i = 0; i < FLAGS_falloc; i++) {
         const int ret = pwrite(ssd_fd, dummy_data, gib_size, gib_size * i);
         posix_check(ret == gib_size);
      }
      free(dummy_data);
      fsync(ssd_fd);
   }
   ensure(fcntl(ssd_fd, F_GETFL) != -1);
   // -------------------------------------------------------------------------------------
   buffer_manager = make_unique<storage::BufferManager>(ssd_fd, effective_dram_gib);
   BMC::global_bf = buffer_manager.get();
   // -------------------------------------------------------------------------------------
   DTRegistry::global_dt_registry.registerDatastructureType(0, storage::btree::BTreeLL::getMeta());
   DTRegistry::global_dt_registry.registerDatastructureType(1, storage::hashing::LinearHashTable::getMeta());
   DTRegistry::global_dt_registry.registerDatastructureType(2, storage::heap::HeapFile::getMeta());
   DTRegistry::global_dt_registry.registerDatastructureType(3, storage::hashing::LinearHashTableWithOverflowHeap::getMeta());
   // -------------------------------------------------------------------------------------
   if (FLAGS_recover) {
      deserializeState();
   }
   // -------------------------------------------------------------------------------------
   u64 end_of_block_device;
   if (FLAGS_wal_offset_gib == 0) {
      ioctl(ssd_fd, BLKGETSIZE64, &end_of_block_device);
   } else {
      end_of_block_device = FLAGS_wal_offset_gib * 1024 * 1024 * 1024;
   }
   cr_manager = make_unique<cr::CRManager>(ssd_fd, end_of_block_device);
   cr::CRManager::global = cr_manager.get();
}
// -------------------------------------------------------------------------------------
LeanStore::~LeanStore()
{
   bg_threads_keep_running = false;
   while (bg_threads_counter) {
      MYPAUSE();
   }
   if (FLAGS_persist) {
      serializeState();
      buffer_manager->writeAllBufferFrames();
   }
   //  close(ssd_fd);
}
std::string exec_cmd(const char* command) {
   // char tmpname [L_tmpnam];
   // std::tmpnam ( tmpname );
   // std::string scommand = command;
   // std::string cmd = scommand + " >> " + tmpname;
   // std::system(cmd.c_str());
   // std::ifstream file(tmpname, std::ios::in | std::ios::binary );
   // std::string result;
   // if (file) {
   //    while (!file.eof()) result.push_back(file.get())
   //       ;
   //    file.close();
   // }
   // remove(tmpname);
   // return result;
   return "";
}

// -------------------------------------------------------------------------------------
void LeanStore::startProfilingThread(const std::vector<std::string> & additional_columns, std::function<std::vector<string>()> func, const string & results_csv_file_path)
{
   double read_iops = 0;
   double write_iops = 0;
   double read_bw = 0;
   double write_bw = 0;
   double dev_util = 0;
   std::thread profiling_thread([&, this, func](std::vector<std::string> addi_columns) {
      profiling::BMTable bm_table(*buffer_manager.get());
      profiling::DTTable dt_table(*buffer_manager.get());
      profiling::CPUTable cpu_table;
      profiling::CRTable cr_table;
      std::vector<profiling::ProfilingTable*> tables = {&configs_table, &bm_table, &dt_table, &cpu_table, &cr_table};
      // -------------------------------------------------------------------------------------
      std::vector<std::ofstream> csvs;
      std::ofstream::openmode open_flags;
      if (FLAGS_csv_truncate) {
         open_flags = ios::trunc;
      } else {
         open_flags = ios::app;
      }
      for (u64 t_i = 0; t_i < tables.size(); t_i++) {
         tables[t_i]->open();
         // -------------------------------------------------------------------------------------
         csvs.emplace_back();
         auto& csv = csvs.back();
         csv.open(FLAGS_csv_path + "_" + tables[t_i]->getName() + ".csv", open_flags);
         csv.seekp(0, ios::end);
         csv << std::setprecision(2) << std::fixed;
         if (csv.tellp() == 0) {
            csv << "t,c_hash";
            for (auto& c : tables[t_i]->getColumns()) {
               csv << "," << c.first;
            }
            csv << endl;
         }
      }
      // -------------------------------------------------------------------------------------
      config_hash = configs_table.hash();
      // -------------------------------------------------------------------------------------
      u64 seconds = 0;
      bool results_csv_header_written = false;
      std::ofstream results_csv;
      if (!results_csv_file_path.empty()) {
         results_csv.open(results_csv_file_path, open_flags);
      }
      while (bg_threads_keep_running) {
         for (u64 t_i = 0; t_i < tables.size(); t_i++) {
            tables[t_i]->next();
            if (tables[t_i]->size() == 0)
               continue;
            // -------------------------------------------------------------------------------------
            // CSV
            auto& csv = csvs[t_i];
            for (u64 r_i = 0; r_i < tables[t_i]->size(); r_i++) {
               csv << seconds << "," << config_hash;
               for (auto& c : tables[t_i]->getColumns()) {
                  csv << "," << c.second.values[r_i];
               }
               csv << endl;
            }
            // -------------------------------------------------------------------------------------
            // TODO: Websocket, CLI
         }
         // -------------------------------------------------------------------------------------
         const u64 tx = std::stoi(cr_table.get("0", "tx"));
         // Global Stats
         global_stats.accumulated_tx_counter += tx;
         // -------------------------------------------------------------------------------------
         // Console
         // -------------------------------------------------------------------------------------
         const double instr_per_tx = cpu_table.workers_agg_events["instr"] / tx;
         const double cycles_per_tx = cpu_table.workers_agg_events["cycle"] / tx;
         const double l1_per_tx = cpu_table.workers_agg_events["LLC-miss"] / tx;
         //std::cerr << "instrs " << cpu_table.workers_agg_events["instr"] << ",  cycles " << cpu_table.workers_agg_events["cycle"] << ", LLC-miss " << cpu_table.workers_agg_events["LLC-miss"] << " tx " << tx << std::endl;
         // using RowType = std::vector<variant<std::string, const char*, Table>>;
         if (FLAGS_print_tx_console) {
            tabulate::Table table;
            std::vector<variant<std::string, const char *, Table>> header_cells;
            std::vector<std::string> headers_original{"t", "TX P", "TX A", "TX C", "W MiB", "R MiB", "Instrs/TX", "Cycles/TX", "CPUs", "LLC-miss/TX", "WAL T", "WAL R G", "WAL W G",
                           "GCT Rounds", "Split/Merge", "RdRestarts", "IO Read/s", "IO Write/s", "Read BW(MB/s)", "Write BW(MB/s)", "IO Util(%)"};
            header_cells.insert(header_cells.end(), addi_columns.begin(), addi_columns.end());
            header_cells.insert(header_cells.end(), headers_original.begin(), headers_original.end());
            table.add_row(header_cells);

            if (results_csv_header_written == false && !results_csv_file_path.empty()) {
               results_csv_header_written = true;
               results_csv << std::get<0>(header_cells[0]);
               for (size_t i = 1; i < header_cells.size(); ++i) {
                  results_csv << "," << std::get<0>(header_cells[i]);
               }
               results_csv << std::endl;
            }
            std::vector<variant<std::string, const char *, Table>> data_cells;
            std::vector<std::string> data_original{std::to_string(seconds), cr_table.get("0", "tx"), cr_table.get("0", "tx_abort"), cr_table.get("0", "gct_committed_tx"),
                           bm_table.get("0", "w_mib"), bm_table.get("0", "r_mib"), std::to_string(instr_per_tx), std::to_string(cycles_per_tx),
                           std::to_string(cpu_table.workers_agg_events["CPU"]), std::to_string(l1_per_tx), cr_table.get("0", "wal_total"),
                           cr_table.get("0", "wal_read_gib"), cr_table.get("0", "wal_write_gib"), cr_table.get("0", "gct_rounds"), dt_table.get("0", "split_merge_count"), dt_table.get("0", "dt_restarts_read") + dt_table.get("1", "dt_restarts_read") + dt_table.get("2", "dt_restarts_read") + dt_table.get("3", "dt_restarts_read"),
                           std::to_string(read_iops), std::to_string(write_iops), std::to_string(read_bw), std::to_string(write_bw), std::to_string(dev_util)};
            auto additional_column_data = func();
            for (size_t i = 0 ; i < additional_column_data.size(); ++i) {
               data_cells.push_back(additional_column_data[i]);
            }
            for (size_t i = 0; i < data_original.size(); ++i) {
               data_cells.push_back(data_original[i]);
            }

            if (!results_csv_file_path.empty()) {
               results_csv << std::get<0>(data_cells[0]);
               for (size_t i = 1; i < data_cells.size(); ++i) {
                  results_csv << "," << std::get<0>(data_cells[i]);
               }
               results_csv << std::endl;
            }
            table.add_row(data_cells);
            // -------------------------------------------------------------------------------------
            for (size_t i = 0; i < data_cells.size(); ++i) {
               table.column(i).format().width(15);
            }
            
            // -------------------------------------------------------------------------------------
            auto print_table = [](tabulate::Table& table, std::function<bool(u64)> predicate) {
               std::stringstream ss;
               table.print(ss);
               string str = ss.str();
               u64 line_n = 0;
               for (u64 i = 0; i < str.size(); i++) {
                  if (str[i] == '\n') {
                     line_n++;
                  }
                  if (predicate(line_n)) {
                     cout << str[i];
                  }
               }
            };
            if (seconds == 0) {
               print_table(table, [](u64 line_n) { return (line_n < 3) || (line_n == 4); });
            } else {
               print_table(table, [](u64 line_n) { return line_n == 4; });
            }
            // -------------------------------------------------------------------------------------
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            seconds += 1;
            std::locale::global(std::locale::classic());
         }
      }
      bg_threads_counter--;
   }, additional_columns);
   bg_threads_counter++;
   profiling_thread.detach();
}
// -------------------------------------------------------------------------------------
storage::btree::BTreeLL& LeanStore::registerBTreeLL(string name, bool hot_partition)
{
   assert(btrees_ll.find(name) == btrees_ll.end());
   auto& btree = btrees_ll[name];
   DTID dtid = DTRegistry::global_dt_registry.registerDatastructureInstance(0, reinterpret_cast<void*>(&btree), name);
   auto& bf = buffer_manager->allocatePage(true);
   Guard guard(bf.header.latch, GUARD_STATE::EXCLUSIVE);
   bf.header.keep_in_memory = true;
   bf.page.hot_data = true;
   bf.page.dt_id = dtid;
   btree.hot_partition = hot_partition;
   guard.unlock();
   btree.create(dtid, &bf);
   return btree;
}
// -------------------------------------------------------------------------------------
storage::hashing::LinearHashTable& LeanStore::registerHashTable(string name, bool hot_partition)
{
   assert(hts.find(name) == hts.end());
   auto& ht = hts[name];
   DTID dtid = DTRegistry::global_dt_registry.registerDatastructureInstance(1, reinterpret_cast<void*>(&ht), name);
   ht.hot_partition = hot_partition;
   ht.create(dtid);
   return ht;
}
// -------------------------------------------------------------------------------------
storage::hashing::LinearHashTableWithOverflowHeap& LeanStore::registerHashTableWOH(string name, storage::heap::HeapFile& hf, bool hot_partition) {
   assert(htwohs.find(name) == htwohs.end());
   auto& ht = htwohs[name];
   DTID dtid = DTRegistry::global_dt_registry.registerDatastructureInstance(3, reinterpret_cast<void*>(&ht), name);
   ht.hot_partition = hot_partition;
   ht.create(dtid, &hf);
   return ht;
}
// -------------------------------------------------------------------------------------
storage::heap::HeapFile& LeanStore::registerHeapFile(string name, bool hot_partition)
{
   assert(hfs.find(name) == hfs.end());
   auto& hf = hfs[name];
   DTID dtid = DTRegistry::global_dt_registry.registerDatastructureInstance(2, reinterpret_cast<void*>(&hf), name);
   hf.hot_partition = hot_partition;
   hf.create(dtid);
   return hf;
}
// -------------------------------------------------------------------------------------
u64 LeanStore::getConfigHash()
{
   return config_hash;
}
// -------------------------------------------------------------------------------------
LeanStore::GlobalStats LeanStore::getGlobalStats()
{
   return global_stats;
}
// -------------------------------------------------------------------------------------
void LeanStore::serializeState()
{
   // Serialize data structure instances
   std::ofstream json_file;
   json_file.open(FLAGS_persist_file, ios::trunc);
   rs::Document d;
   rs::Document::AllocatorType& allocator = d.GetAllocator();
   d.SetObject();
   // -------------------------------------------------------------------------------------
   std::unordered_map<std::string, std::string> serialized_bm_map = buffer_manager->serialize();
   rs::Value bm_serialized(rs::kObjectType);
   for (const auto& [key, value] : serialized_bm_map) {
      rs::Value k, v;
      k.SetString(key.c_str(), key.length(), allocator);
      v.SetString(value.c_str(), value.length(), allocator);
      bm_serialized.AddMember(k, v, allocator);
   }
   d.AddMember("buffer_manager", bm_serialized, allocator);
   // -------------------------------------------------------------------------------------
   rs::Value dts(rs::kArrayType);
   for (auto& dt : DTRegistry::global_dt_registry.dt_instances_ht) {
      rs::Value dt_json_object(rs::kObjectType);
      const DTID dt_id = dt.first;
      rs::Value name;
      name.SetString(std::get<2>(dt.second).c_str(), std::get<2>(dt.second).length(), allocator);
      dt_json_object.AddMember("name", name, allocator);
      dt_json_object.AddMember("type", rs::Value(std::get<0>(dt.second)), allocator);
      dt_json_object.AddMember("id", rs::Value(dt_id), allocator);
      // -------------------------------------------------------------------------------------
      std::unordered_map<std::string, std::string> serialized_dt_map = DTRegistry::global_dt_registry.serialize(dt_id);
      rs::Value dt_serialized(rs::kObjectType);
      for (const auto& [key, value] : serialized_dt_map) {
         rs::Value k, v;
         k.SetString(key.c_str(), key.length(), allocator);
         v.SetString(value.c_str(), value.length(), allocator);
         dt_serialized.AddMember(k, v, allocator);
      }
      dt_json_object.AddMember("serialized", dt_serialized, allocator);
      // -------------------------------------------------------------------------------------
      dts.PushBack(dt_json_object, allocator);
   }
   d.AddMember("registered_datastructures", dts, allocator);
   // -------------------------------------------------------------------------------------
   serializeFlags(d);
   rs::StringBuffer sb;
   rs::PrettyWriter<rs::StringBuffer> writer(sb);
   d.Accept(writer);
   json_file << sb.GetString();
}
// -------------------------------------------------------------------------------------
void LeanStore::serializeFlags(rs::Document& d)
{
   rs::Value flags_serialized(rs::kObjectType);
   rs::Document::AllocatorType& allocator = d.GetAllocator();
   for (auto flags : persisted_string_flags) {
      rs::Value name(std::get<0>(flags).c_str(), std::get<0>(flags).length(), allocator);
      rs::Value value;
      value.SetString((*std::get<1>(flags)).c_str(), (*std::get<1>(flags)).length(), allocator);
      flags_serialized.AddMember(name, value, allocator);
   }
   for (auto flags : persisted_s64_flags) {
      rs::Value name(std::get<0>(flags).c_str(), std::get<0>(flags).length(), allocator);
      string value_string = std::to_string(*std::get<1>(flags));
      rs::Value value;
      value.SetString(value_string.c_str(), value_string.length(), d.GetAllocator());
      flags_serialized.AddMember(name, value, allocator);
   }
   d.AddMember("flags", flags_serialized, allocator);
}
// -------------------------------------------------------------------------------------
void LeanStore::deserializeState()
{
   std::ifstream json_file;
   json_file.open(FLAGS_recover_file);
   rs::IStreamWrapper isw(json_file);
   rs::Document d;
   d.ParseStream(isw);
   // -------------------------------------------------------------------------------------
   const rs::Value& bm = d["buffer_manager"];
   std::unordered_map<std::string, std::string> serialized_bm_map;
   for (rs::Value::ConstMemberIterator itr = bm.MemberBegin(); itr != bm.MemberEnd(); ++itr) {
      serialized_bm_map[itr->name.GetString()] = itr->value.GetString();
   }
   buffer_manager->deserialize(serialized_bm_map);
   // -------------------------------------------------------------------------------------
   const rs::Value& dts = d["registered_datastructures"];
   assert(dts.IsArray());
   for (auto& dt : dts.GetArray()) {
      assert(dt.IsObject());
      const DTID dt_id = dt["id"].GetInt();
      const DTType dt_type = dt["type"].GetInt();
      const std::string dt_name = dt["name"].GetString();
      std::unordered_map<std::string, std::string> serialized_dt_map;
      const rs::Value& serialized_object = dt["serialized"];
      for (rs::Value::ConstMemberIterator itr = serialized_object.MemberBegin(); itr != serialized_object.MemberEnd(); ++itr) {
         serialized_dt_map[itr->name.GetString()] = itr->value.GetString();
      }
      // -------------------------------------------------------------------------------------
      if (dt_type == 0) {
         auto& btree = btrees_ll[dt_name];
         DTRegistry::global_dt_registry.registerDatastructureInstance(0, reinterpret_cast<void*>(&btree), dt_name, dt_id);
      } else {
         UNREACHABLE();
      }
      DTRegistry::global_dt_registry.deserialize(dt_id, serialized_dt_map);
   }
}
// -------------------------------------------------------------------------------------
void LeanStore::deserializeFlags()
{
   std::ifstream json_file;
   json_file.open(FLAGS_recover_file);
   rs::IStreamWrapper isw(json_file);
   rs::Document d;
   d.ParseStream(isw);
   // -------------------------------------------------------------------------------------
   const rs::Value& flags = d["flags"];
   std::unordered_map<std::string, std::string> flags_serialized;
   for (rs::Value::ConstMemberIterator itr = flags.MemberBegin(); itr != flags.MemberEnd(); ++itr) {
      flags_serialized[itr->name.GetString()] = itr->value.GetString();
   }
   for (auto flags : persisted_string_flags) {
      *std::get<1>(flags) = flags_serialized[std::get<0>(flags)];
   }
   for (auto flags : persisted_s64_flags) {
      *std::get<1>(flags) = atoi(flags_serialized[std::get<0>(flags)].c_str());
   }
}
// -------------------------------------------------------------------------------------
// Static members
std::list<std::tuple<string, fLS::clstring*>> LeanStore::persisted_string_flags = {};
std::list<std::tuple<string, s64*>> LeanStore::persisted_s64_flags = {};
// -------------------------------------------------------------------------------------
}  // namespace leanstore
// -------------------------------------------------------------------------------------
