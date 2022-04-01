#include "Units.hpp"
#include "leanstore/BTreeAdapter.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/LeanStore.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
#include "leanstore/utils/FVector.hpp"
#include "leanstore/utils/Files.hpp"
#include "leanstore/utils/RandomGenerator.hpp"
#include "leanstore/utils/ScrambledZipfGenerator.hpp"
// -------------------------------------------------------------------------------------
#include <gflags/gflags.h>
#include <tbb/tbb.h>
// -------------------------------------------------------------------------------------
#include <iostream>
#include <set>
// -------------------------------------------------------------------------------------
DEFINE_uint32(ycsb_read_ratio, 100, "");
DEFINE_uint64(ycsb_tuple_count, 0, "");
DEFINE_uint32(ycsb_payload_size, 100, "tuple size in bytes");
DEFINE_uint32(ycsb_warmup_rounds, 0, "");
DEFINE_uint32(ycsb_tx_rounds, 1, "");
DEFINE_uint32(ycsb_tx_count, 0, "default = tuples");
DEFINE_bool(verify, false, "");
DEFINE_uint32(cached_btree, 0, "");
DEFINE_double(cached_btree_ram_ratio, 0.0, "");
DEFINE_bool(cache_lazy_migration, false, "");
DEFINE_bool(ycsb_scan, false, "");
DEFINE_bool(ycsb_tx, true, "");
DEFINE_bool(ycsb_count_unique_lookup_keys, true, "");
// -------------------------------------------------------------------------------------
using namespace leanstore;
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
using YCSBKey = u64;
using YCSBPayload = BytesPayload<120>;
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
double calculateMTPS(chrono::high_resolution_clock::time_point begin, chrono::high_resolution_clock::time_point end, u64 factor)
{
   double tps = ((factor * 1.0 / (chrono::duration_cast<chrono::microseconds>(end - begin).count() / 1000000.0)));
   return (tps / 1000000.0);
}

void zipf_stats(utils::ScrambledZipfGenerator * zipf_gen) {
   std::vector<YCSBKey> generated_keys;
   std::unordered_set<YCSBKey> unique_keys;
   for (size_t i = 0; i < FLAGS_ycsb_tuple_count; ++i) {
      YCSBKey key = zipf_gen->zipf_generator.rand() % FLAGS_ycsb_tuple_count;
      generated_keys.push_back(key);
      unique_keys.insert(key);
   }
   sort(generated_keys.begin(), generated_keys.end());
   std::cout << "Zipfian Stats: " << std::endl;
   std::cout << "Skew factor: " << FLAGS_zipf_factor << std::endl;
   std::cout << "# unique keys: " << unique_keys.size() << std::endl;
   std::cout << "p50: " << generated_keys[0.5*generated_keys.size()] << ", covering " << generated_keys[0.5*generated_keys.size()] / (0.0 + FLAGS_ycsb_tuple_count) * 100 << "% of the keys"  << std::endl;
   std::cout << "p75: " << generated_keys[0.75*generated_keys.size()] << ", covering " << generated_keys[0.75*generated_keys.size()] / (0.0 + FLAGS_ycsb_tuple_count) * 100 << "% of the keys"  << std::endl;
   std::cout << "p90: " << generated_keys[0.90*generated_keys.size()] << ", covering " << generated_keys[0.90*generated_keys.size()] / (0.0 + FLAGS_ycsb_tuple_count) * 100 << "% of the keys"  << std::endl;
   std::cout << "p99: " << generated_keys[0.99*generated_keys.size()] << ", covering " << generated_keys[0.99*generated_keys.size()] / (0.0 + FLAGS_ycsb_tuple_count) * 100 << "% of the keys"  << std::endl;
}

// -------------------------------------------------------------------------------------
int main(int argc, char** argv)
{
   gflags::SetUsageMessage("Leanstore Frontend");
   gflags::ParseCommandLineFlags(&argc, &argv, true);
   // -------------------------------------------------------------------------------------
   tbb::task_scheduler_init taskScheduler(FLAGS_worker_threads);
   // -------------------------------------------------------------------------------------
   chrono::high_resolution_clock::time_point begin, end;
   // -------------------------------------------------------------------------------------
   // LeanStore DB
   double cached_btree_size_gib = 0;
   if (FLAGS_cached_btree) {
      cached_btree_size_gib = FLAGS_dram_gib * FLAGS_cached_btree_ram_ratio;
      FLAGS_dram_gib = FLAGS_dram_gib * (1 - FLAGS_cached_btree_ram_ratio);
   }
   cout << "cached_btree_size_gib " << cached_btree_size_gib << std::endl;
   cout << "wal=" << FLAGS_wal << std::endl;
   cout << "zipf_factor=" << FLAGS_zipf_factor << std::endl;
   cout << "ycsb_read_ratio=" << FLAGS_ycsb_read_ratio << std::endl;
   cout << "run_for_seconds=" << FLAGS_run_for_seconds << std::endl;
   LeanStore db;
   unique_ptr<BTreeInterface<YCSBKey, YCSBPayload>> adapter;
   leanstore::storage::btree::BTreeLL* btree_ptr = nullptr;
   if (FLAGS_recover) {
      btree_ptr = &db.retrieveBTreeLL("ycsb");
   } else {
      btree_ptr = &db.registerBTreeLL("ycsb");
   }
   if (FLAGS_cached_btree == 0) {
      adapter.reset(new BTreeVSAdapter<YCSBKey, YCSBPayload>(*btree_ptr, btree_ptr->dt_id));
   } else if (FLAGS_cached_btree == 1) {
      adapter.reset(new BTreeCachedVSAdapter<YCSBKey, YCSBPayload>(*btree_ptr, cached_btree_size_gib, FLAGS_cache_lazy_migration));
   } else if (FLAGS_cached_btree == 2) {
      adapter.reset(new BTreeCachedNoninlineVSAdapter<YCSBKey, YCSBPayload>(*btree_ptr, cached_btree_size_gib, FLAGS_cache_lazy_migration));
   } else {
      assert(false);
   }

   db.registerConfigEntry("ycsb_read_ratio", FLAGS_ycsb_read_ratio);
   db.registerConfigEntry("ycsb_target_gib", FLAGS_target_gib);
   db.startProfilingThread();
   // -------------------------------------------------------------------------------------
   auto& table = *adapter;
   const u64 ycsb_tuple_count = (FLAGS_ycsb_tuple_count)
                                    ? FLAGS_ycsb_tuple_count
                                    : FLAGS_target_gib * 1024 * 1024 * 1024 * 1.0 / 2.0 / (sizeof(YCSBKey) + sizeof(YCSBPayload));
   // Insert values
   if (FLAGS_recover) {
      // Warmup
      const u64 n = ycsb_tuple_count;
      cout << "Warmup: Scanning..." << endl;
      tbb::parallel_for(tbb::blocked_range<u64>(0, n), [&](const tbb::blocked_range<u64>& range) {
         for (u64 t_i = range.begin(); t_i < range.end(); t_i++) {
            YCSBPayload result;
            table.lookup(t_i, result);
         }
      });
      // -------------------------------------------------------------------------------------
      cout << "time elapsed = " << (chrono::duration_cast<chrono::microseconds>(end - begin).count() / 1000000.0) << endl;
      cout << calculateMTPS(begin, end, n) << " M tps" << endl;
      cout << "-------------------------------------------------------------------------------------" << endl;
   } else {
      const u64 n = ycsb_tuple_count;
      cout << "-------------------------------------------------------------------------------------" << endl;
      cout << "Inserting values" << endl;
      begin = chrono::high_resolution_clock::now();
      {
         tbb::parallel_for(tbb::blocked_range<u64>(0, n), [&](const tbb::blocked_range<u64>& range) {
            vector<u64> keys(range.size());
            std::iota(keys.begin(), keys.end(), range.begin());
            std::random_shuffle(keys.begin(), keys.end());
            for (u64 t_i = 0; t_i < keys.size(); t_i++) {
               YCSBPayload payload;
               utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(YCSBPayload));
               auto& key = keys[t_i];
               table.insert(key, payload);
            }
            // for (u64 t_i = range.begin(); t_i < range.end(); t_i++) {
            //    YCSBPayload payload;
            //    utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(YCSBPayload));
            //    auto& key = t_i;
            //    table.insert(key, payload);
            // }
         });
      }
      end = chrono::high_resolution_clock::now();
      cout << "time elapsed = " << (chrono::duration_cast<chrono::microseconds>(end - begin).count() / 1000000.0) << endl;
      cout << calculateMTPS(begin, end, n) << " M tps" << endl;
      // -------------------------------------------------------------------------------------
      const u64 written_pages = db.getBufferManager().consumedPages();
      const u64 mib = written_pages * PAGE_SIZE / 1024 / 1024;
      cout << "Inserted volume: (pages, MiB) = (" << written_pages << ", " << mib << ")" << endl;
      cout << "-------------------------------------------------------------------------------------" << endl;
   }
   // -------------------------------------------------------------------------------------
   auto zipf_random = std::make_unique<utils::ScrambledZipfGenerator>(0, ycsb_tuple_count, FLAGS_zipf_factor);
   cout << setprecision(4);
   // -------------------------------------------------------------------------------------
   // Scan
   if (FLAGS_ycsb_scan) {
      const u64 n = ycsb_tuple_count;
      cout << "-------------------------------------------------------------------------------------" << endl;
      cout << "Scan" << endl;
      {
         begin = chrono::high_resolution_clock::now();
         tbb::parallel_for(tbb::blocked_range<u64>(0, n), [&](const tbb::blocked_range<u64>& range) {
            for (u64 i = range.begin(); i < range.end(); i++) {
               YCSBPayload result;
               table.lookup(i, result);
            }
         });
         end = chrono::high_resolution_clock::now();
      }
      // -------------------------------------------------------------------------------------
      cout << "time elapsed = " << (chrono::duration_cast<chrono::microseconds>(end - begin).count() / 1000000.0) << endl;
      // -------------------------------------------------------------------------------------
      cout << calculateMTPS(begin, end, n) << " M tps" << endl;
      cout << "-------------------------------------------------------------------------------------" << endl;
   }
   // -------------------------------------------------------------------------------------
   cout << "-------------------------------------------------------------------------------------" << endl;
   cout << "~Transactions" << endl;
   adapter->clear_stats();
   atomic<bool> keep_running = true;
   atomic<u64> running_threads_counter = 0;
   atomic<u64> txs = 0;
   vector<thread> threads;
   begin = chrono::high_resolution_clock::now();
   for (u64 t_i = 0; t_i < FLAGS_worker_threads; t_i++) {
      threads.emplace_back([&]() {
         running_threads_counter++;
         u64 tx = 0;
         while (keep_running) {
            YCSBKey key = zipf_random->rand();
            assert(key < ycsb_tuple_count);
            YCSBPayload result;
            if (FLAGS_ycsb_read_ratio == 100 || utils::RandomGenerator::getRandU64(0, 100) < FLAGS_ycsb_read_ratio) {
               table.lookup(key, result);
            } else {
               YCSBPayload payload;
               utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(YCSBPayload));
               table.update(key, payload);
            }
            WorkerCounters::myCounters().tx++;
            ++tx;
         }
         txs += tx;
         running_threads_counter--;
      });
   }
   {
      // Shutdown threads
      sleep(FLAGS_run_for_seconds);
      keep_running = false;
      while (running_threads_counter) {
         MYPAUSE();
      }
      end = chrono::high_resolution_clock::now();
      cout << "time elapsed = " << (chrono::duration_cast<chrono::microseconds>(end - begin).count() / 1000000.0) << endl;
      // -------------------------------------------------------------------------------------
      cout << "Total commit: " << calculateMTPS(begin, end, txs.load()) << " M tps" << endl;
      zipf_stats(zipf_random.get());
      adapter->report(FLAGS_ycsb_tuple_count, db.getBufferManager().consumedPages());
      cout << "-------------------------------------------------------------------------------------" << endl;
      for (auto& thread : threads) {
         thread.join();
      }
   }
   cout << "-------------------------------------------------------------------------------------" << endl;
   // -------------------------------------------------------------------------------------
   return 0;
}
