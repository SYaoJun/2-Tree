#pragma once
#include <unordered_map>
#include <iostream>
#include "Units.hpp"
#include "leanstore/storage/btree/BTreeLL.hpp"
#include "leanstore/storage/btree/core/WALMacros.hpp"
#include "ART/ARTIndex.hpp"
#include "stx/btree_map.h"
#include "common/DistributedCounter.hpp"
#include "common/utils.hpp"
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
namespace leanstore
{
[[maybe_unused]] static unsigned fold(uint8_t* writer, const s32& x)
{
   *reinterpret_cast<u32*>(writer) = __builtin_bswap32(x ^ (1ul << 31));
   return sizeof(x);
}

[[maybe_unused]] static unsigned fold(uint8_t* writer, const s64& x)
{
   *reinterpret_cast<u64*>(writer) = __builtin_bswap64(x ^ (1ull << 63));
   return sizeof(x);
}

[[maybe_unused]] static unsigned fold(uint8_t* writer, const u64& x)
{
   *reinterpret_cast<u64*>(writer) = __builtin_bswap64(x);
   return sizeof(x);
}

[[maybe_unused]] static unsigned fold(uint8_t* writer, const u32& x)
{
   *reinterpret_cast<u32*>(writer) = __builtin_bswap32(x);
   return sizeof(x);
}

[[maybe_unused]] static u64 unfold(const u64 &x) {
   return __builtin_bswap64(x);
}

[[maybe_unused]] static u32 unfold(const u32 &x) {
   return __builtin_bswap32(x);
}
// -------------------------------------------------------------------------------------
using OP_RESULT = leanstore::storage::btree::OP_RESULT;

template <typename Key, typename Payload>
struct BTreeVSAdapter : StorageInterface<Key, Payload> {
   leanstore::storage::btree::BTreeInterface& btree;

   DistributedCounter<> total_lookups = 0;
   DistributedCounter<> io_reads = 0;
   DistributedCounter<> io_reads_snapshot = 0;
   DistributedCounter<> io_reads_now = 0;
   DistributedCounter<> btree_buffer_miss = 0;
   DistributedCounter<> btree_buffer_hit = 0;
   DistributedCounter<> scan_ops = 0;
   DistributedCounter<> io_reads_scan = 0;
   uint64_t buffer_pool_background_writes_last = 0;
   IOStats stats;
   alignas(64) std::atomic<uint64_t> bp_dirty_page_flushes_snapshot = 0;
   DTID dt_id;
   leanstore::storage::BufferManager * buf_mgr = nullptr;

   void set_buffer_manager(storage::BufferManager * buf_mgr) override { 
      this->buf_mgr = buf_mgr;
      bp_dirty_page_flushes_snapshot.store(this->buf_mgr->dirty_page_flushes.load());
   }

   BTreeVSAdapter(leanstore::storage::btree::BTreeInterface& btree, DTID dt_id = -1) : btree(btree), dt_id(dt_id) {
      io_reads_snapshot = WorkerCounters::myCounters().io_reads.load();
   }

   void clear_stats() override {
      btree_buffer_miss = btree_buffer_hit = 0;
      io_reads_snapshot = WorkerCounters::myCounters().io_reads.load();
      io_reads_scan = scan_ops = 0;
      io_reads = 0;
      bp_dirty_page_flushes_snapshot.store(this->buf_mgr->dirty_page_flushes.load());
   }

   bool lookup(Key k, Payload& v) override
   {
      leanstore::utils::IOScopedCounter cc([&](u64 ios){ this->io_reads += ios; });
      total_lookups++;
      DeferCode c([&, this](){io_reads_now = WorkerCounters::myCounters().io_reads.load();});
      DeferCodeWithContext ccc([&, this](uint64_t old_reads) {
         auto new_reads = WorkerCounters::myCounters().io_reads.load();
         if (new_reads > old_reads) {
            stats.reads += new_reads - old_reads;
         }
      }, WorkerCounters::myCounters().io_reads.load());
      u8 key_bytes[sizeof(Key)];
      auto old_miss = WorkerCounters::myCounters().io_reads.load();
      auto res = btree.lookup(key_bytes, fold(key_bytes, k), [&](const u8* payload, u16 payload_length) { memcpy(&v, payload, payload_length); }) ==
            OP_RESULT::OK;
      auto new_miss = WorkerCounters::myCounters().io_reads.load();
      assert(new_miss >= old_miss);
      if (old_miss == new_miss) {
         btree_buffer_hit++;
      } else {
         btree_buffer_miss += new_miss - old_miss;
      }
      return res;
   }
   void insert(Key k, Payload& v) override
   {
      DeferCode c([&, this](){io_reads_now = WorkerCounters::myCounters().io_reads.load();});
      DeferCodeWithContext ccc([&, this](uint64_t old_reads) {
         auto new_reads = WorkerCounters::myCounters().io_reads.load();
         if (new_reads > old_reads) {
            stats.reads += new_reads - old_reads;
         }
      }, WorkerCounters::myCounters().io_reads.load());
      u8 key_bytes[sizeof(Key)];
      auto old_miss = WorkerCounters::myCounters().io_reads.load();
      btree.insert(key_bytes, fold(key_bytes, k), reinterpret_cast<u8*>(&v), sizeof(v));
      auto new_miss = WorkerCounters::myCounters().io_reads.load();
      if (old_miss == new_miss) {
         btree_buffer_hit++;
      } else {
         btree_buffer_miss += new_miss - old_miss;
      }
   }


   void scan(Key start_key, std::function<bool(const Key&, const Payload &)> processor, [[maybe_unused]]int length) {
      scan_ops++;
      DeferCode c([&, this](){io_reads_now = WorkerCounters::myCounters().io_reads.load();});
      DeferCodeWithContext ccc([&, this](uint64_t old_reads) {
         auto new_reads = WorkerCounters::myCounters().io_reads.load();
         if (new_reads > old_reads) {
            stats.reads += new_reads - old_reads;
         }
      }, WorkerCounters::myCounters().io_reads.load());
      u8 key_bytes[sizeof(Key)];
      auto io_reads_old = WorkerCounters::myCounters().io_reads.load();
      btree.scanAsc(key_bytes, fold(key_bytes, start_key),
      [&](const u8 * key, u16 key_length, const u8 * value, u16 value_length) -> bool {
         auto real_key = unfold(*(Key*)(key));
         assert(key_length == sizeof(Key));
         assert(value_length == sizeof(Payload));
         const Payload * p = reinterpret_cast<const Payload*>(value);
         if (processor(real_key, *p)) {
            return false;
         }
         return true;
      }, [](){});
      io_reads_scan += WorkerCounters::myCounters().io_reads.load() - io_reads_old;
   }

   void update(Key k, Payload& v) override
   {
      leanstore::utils::IOScopedCounter cc([&](u64 ios){ this->io_reads += ios; });
      total_lookups++;
      DeferCode c([&, this](){io_reads_now = WorkerCounters::myCounters().io_reads.load();});
      DeferCodeWithContext ccc([&, this](uint64_t old_reads) {
         auto new_reads = WorkerCounters::myCounters().io_reads.load();
         if (new_reads > old_reads) {
            stats.reads += new_reads - old_reads;
         }
      }, WorkerCounters::myCounters().io_reads.load());
      u8 key_bytes[sizeof(Key)];
      auto old_miss = WorkerCounters::myCounters().io_reads.load();
      [[maybe_unused]] auto op_res = btree.updateSameSize(key_bytes, fold(key_bytes, k), 
                                         [&](u8* payload, u16 payload_length) { memcpy(payload, &v, payload_length); },
                                         Payload::wal_update_generator);
      auto new_miss = WorkerCounters::myCounters().io_reads.load();
      if (old_miss == new_miss) {
         btree_buffer_hit++;
      } else {
         btree_buffer_miss += new_miss - old_miss;
      }
   }

   void put(Key k, Payload& v) {
      update(k, v);
   }

   void report(u64 entries, u64 pages) override {
      assert(this->buf_mgr->dirty_page_flushes >= this->bp_dirty_page_flushes_snapshot);
      auto io_writes = this->buf_mgr->dirty_page_flushes - this->bp_dirty_page_flushes_snapshot;
      assert(io_reads_now >= io_reads_snapshot);
      auto total_io_reads_during_benchmark = io_reads_now - io_reads_snapshot;
      std::cout << "Total IO reads during benchmark " << total_io_reads_during_benchmark << std::endl;
      std::cout << "BTree # entries " << entries << std::endl;
      std::cout << "BTree # pages " << pages << std::endl;
      std::cout << "BTree height " << btree.getHeight() << std::endl;
      auto minimal_pages = entries * (sizeof(Key) + sizeof(Payload)) / leanstore::storage::PAGE_SIZE;
      std::cout << "BTree average fill factor " <<  (minimal_pages + 0.0) / pages << std::endl;
      double btree_hit_rate = btree_buffer_hit / (btree_buffer_hit + btree_buffer_miss + 1.0);
      std::cout << "BTree buffer hits/misses " <<  btree_buffer_hit << "/" << btree_buffer_miss << std::endl;
      std::cout << "BTree buffer hit rate " <<  btree_hit_rate << " miss rate " << (1 - btree_hit_rate) << std::endl;
      std::cout << total_lookups<< " lookups, " << io_reads.get() << " i/o reads, " << io_writes << " i/o writes, " << io_reads / (total_lookups + 0.00) << " i/o reads/lookup, " <<  (io_reads + io_writes) / (total_lookups + 0.00) << " ios/lookup" << std::endl;
      std::cout << "Scan ops " << scan_ops << ", ios_read_scan " << io_reads_scan << ", #ios/scan " <<  io_reads_scan/(scan_ops + 0.01);
   }
   std::vector<std::string> stats_column_names() { return {"io_r", "io_w"}; }
   std::vector<std::string> stats_columns() { 
      IOStats empty;
      auto s = exchange_stats(empty);
      return {std::to_string(s.reads),
              std::to_string(s.writes)}; 
   }

   IOStats exchange_stats(IOStats s) { 
      IOStats ret;
      auto t = buffer_pool_background_writes_last;
      buffer_pool_background_writes_last =  this->buf_mgr->dirty_page_flushes;
      ret.reads = stats.reads;
      ret.writes = buffer_pool_background_writes_last - t;
      stats = s;
      return ret;
   }
};


// -------------------------------------------------------------------------------------
template <u64 size>
struct BytesPayload {
   u8 value[size];
   BytesPayload(u8 byte) {
      memset(value, byte, sizeof(value));
   }
   BytesPayload() {}
   bool operator==(BytesPayload& other) { return (std::memcmp(value, other.value, sizeof(value)) == 0); }
   bool operator!=(BytesPayload& other) { return !(operator==(other)); }
   BytesPayload(const BytesPayload& other) { 
      std::memcpy(value, other.value, sizeof(value)); 
   }
   BytesPayload& operator=(const BytesPayload& other)
   {
      std::memcpy(value, other.value, sizeof(value));
      return *this;
   }

   constexpr static leanstore::storage::btree::WALUpdateGenerator wal_update_generator = WALUpdate1(BytesPayload, value);
};
}  // namespace leanstore
