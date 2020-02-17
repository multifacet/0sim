#ifndef _MM_STATS_H_
#define _MM_STATS_H_

#include <linux/proc_fs.h>
#include <linux/types.h>

struct mm_hist;

void mm_stats_init(void);

// Add the measurement `val` to the histogram `hist`.
void mm_stats_hist_measure(struct mm_hist *hist, u64 val);

// Externed stats...
extern struct mm_hist mm_page_fault_cycles;
extern struct mm_hist mm_direct_compaction_cycles;
extern struct mm_hist mm_indirect_compaction_cycles;

#endif
