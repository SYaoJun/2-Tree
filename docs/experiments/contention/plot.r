library(ggplot2)
library(sqldf)
library(Cairo)
library(stringr)
library(scales)
# Rome

dev.set(0)
df=read.csv('/home/adnan/rome/dev/leanstore/docs/experiments/contention/results.csv')
d=sqldf("select * from df")
tx <- ggplot(d, aes(t, tx, color=c_cm_split, group=c_cm_split)) + geom_line()
tx <- tx + facet_grid (row=vars(cm_threads_pro_page), cols=vars())
print(tx)

aux =sqldf("select t, max(GHz) GHz, min(instr) instr,
 max(space_usage_gib) space_usage_gib,
 sum(dt_restarts_update_same_size) restarts,
 sum(dt_researchy_0) splits,
 sum(dt_researchy_1) merge_succ,
 sum(dt_researchy_2) merge_fail,
c_cm_split,c_su_merge,cm_threads_pro_page,
  c_backoff_strategy, c_dram_gib, c_zipf_factor, c_worker_threads,c_cm_update_tracker_pct from d  group by t, c_dram_gib, c_zipf_factor, c_worker_threads, c_backoff_strategy,c_cm_update_tracker_pct, c_cm_split, c_su_merge")
plot <- ggplot(aux, aes(t)) + geom_line(aes(y=splits), color="red") + geom_line(aes(y=merge_succ), colour="blue")  + geom_line(aes(y=merge_fail), colour="green") #+ geom_line(aes(y=space_usage_gib * 1024), color="purple")
#plot <- ggplot(aux, aes(t))
plot <- plot + facet_grid (row=vars(), cols=vars(cm_threads_pro_page))
print(plot)
