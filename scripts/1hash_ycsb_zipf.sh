top_component_dram_ratio=0.8
EXEC=/ssd1/xinjing/2-Tree/build/frontend/ycsb_zipf
RUNTIME=800
SLEEP_COUNT=20
TUPLE_COUNT=100000000
WORKERS=20
RESULT_DIR_PREFIX=results/steady_state/

for dram_budget in 2.5 5 10 20
do
run_time=$RUNTIME
log_file="${RESULT_DIR_PREFIX}/1hash_ycsb_zipf_dram_${dram_budget}.log"
# empty log file
cat /dev/null > $log_file
for read_ratio in 100 50
do
update_ratio=$((100-read_ratio))
csv_file="${RESULT_DIR_PREFIX}/1hash_ycsb_zipf_dram_${dram_budget}_read_ratio_${read_ratio}.csv"
for zipf_factor in 0.9
do
$EXEC --trunc=1 --ycsb_tuple_count=$TUPLE_COUNT --dram_gib=$dram_budget --worker_threads=$WORKERS --index_type=Hash  --top_component_dram_ratio=$top_component_dram_ratio --ycsb_read_ratio=$read_ratio --ycsb_update_ratio=$update_ratio --ssd_path=/flash1/xinjing/2-Tree/build/leanstore_data/leanstore --run_for_seconds=$run_time --ycsb_request_dist=zipfian --zipf_factor=$zipf_factor --lsmt_db_path=/flash1/xinjing/2-Tree/build/rocksdb --results_csv_file_path=$csv_file >> $log_file 2>&1
sleep $SLEEP_COUNT
done
done
done
