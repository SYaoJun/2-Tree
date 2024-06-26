top_component_dram_ratio=0.8
EXEC=/ssd1/xinjing/2-Tree/build/frontend/ycsb_zipf
RUNTIME=3000
SLEEP_COUNT=5
TUPLE_COUNT=100000000


for dram_budget in 3.125 6.25 12.5 25
do
run_time=$RUNTIME
if (( $(echo "$dram_budget > 12.5" |bc -l) )); then
    run_time=400
fi
log_file="2hash_ycsb_zipf_dram_${dram_budget}_exclusive.log"
# empty log file
cat /dev/null > $log_file
for read_ratio in 100 50
do
update_ratio=$((100-read_ratio))
for zipf_factor in 0.9 #0.7 0.8 0.9
do
$EXEC --trunc=1 --ycsb_tuple_count=$TUPLE_COUNT --dram_gib=$dram_budget --worker_threads=10 --index_type=2Hash  --top_component_dram_ratio=$top_component_dram_ratio --ycsb_read_ratio=$read_ratio --ycsb_update_ratio=$update_ratio --ssd_path=/flash1/xinjing/2-Tree/build/leanstore_data/leanstore --run_for_seconds=$run_time --ycsb_request_dist=zipfian --zipf_factor=$zipf_factor --lsmt_db_path=/flash1/xinjing/2-Tree/build/rocksdb --cache_lazy_migration=30 --ycsb_2hash_use_different_hash=false --ycsb_2hash_eviction_by_record=false >> $log_file 2>&1
sleep $SLEEP_COUNT
done
done
done

for dram_budget in 3.125 6.25 12.5 25
do
run_time=$RUNTIME
if (( $(echo "$dram_budget > 12.5" |bc -l) )); then
    run_time=400
fi
log_file="2hash_ycsb_zipf_dram_${dram_budget}_inclusive.log"
# empty log file
cat /dev/null > $log_file
for read_ratio in 100 50
do
update_ratio=$((100-read_ratio))
for zipf_factor in 0.9 #0.7 0.8 0.9
do
$EXEC --trunc=1 --ycsb_tuple_count=$TUPLE_COUNT --dram_gib=$dram_budget --worker_threads=10 --index_type=2Hash  --top_component_dram_ratio=$top_component_dram_ratio --ycsb_read_ratio=$read_ratio --ycsb_update_ratio=$update_ratio --ssd_path=/flash1/xinjing/2-Tree/build/leanstore_data/leanstore --run_for_seconds=$run_time --ycsb_request_dist=zipfian --zipf_factor=$zipf_factor --lsmt_db_path=/flash1/xinjing/2-Tree/build/rocksdb --inclusive_cache --cache_lazy_migration=30 --ycsb_2hash_use_different_hash=false --ycsb_2hash_eviction_by_record=false >> $log_file 2>&1
sleep $SLEEP_COUNT
done
done
done



