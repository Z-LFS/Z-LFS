#include "calclock.h"
#include <linux/kernel.h>
#include <linux/bio.h>
#include <linux/blk_types.h>
#include <linux/blkdev.h>
#include <linux/atomic.h>

F2FS_STAT_LIST(F2FS_TRACE_DEF_VARS)


unsigned long long calclock(struct timespec64 *myclock, unsigned long long *total_time,
		unsigned long long *total_count)
{
	  unsigned long long timedelay = 0, temp = 0, temp_n = 0;
	    if (myclock[1].tv_nsec >= myclock[0].tv_nsec) {
			    temp = myclock[1].tv_sec - myclock[0].tv_sec;
				    temp_n = myclock[1].tv_nsec - myclock[0].tv_nsec;
					    timedelay = BILLION * temp + temp_n;
		} else {
			    temp = myclock[1].tv_sec - myclock[0].tv_sec - 1;
				    temp_n = BILLION + myclock[1].tv_nsec - myclock[0].tv_nsec;
					    timedelay = BILLION * temp + temp_n;
		}
		  __sync_fetch_and_add(total_time, timedelay);
		    __sync_fetch_and_add(total_count, 1);
			  return timedelay;
}

void f2fs_trace_bio_flags(struct bio *bio)
{
	static atomic_t count = ATOMIC_INIT(0);
	char buf[256];
	char *p = buf;
	char *end = buf + sizeof(buf);
	int flags;

	if (atomic_inc_return(&count) % 1000 != 1)
		return;

	flags = bio->bi_opf;
	p += scnprintf(p, end - p, "f2fs_submit_bio: op=%s, op_flags=0x%x {",
		       blk_op_str(bio_op(bio)), flags);

#define PFLAG(f) if (flags & f) p += scnprintf(p, end - p, "%s|", #f)
	PFLAG(REQ_PREFLUSH);
	PFLAG(REQ_FUA);
	PFLAG(REQ_SYNC);
	PFLAG(REQ_META);
	PFLAG(REQ_PRIO);
	PFLAG(REQ_NOMERGE);
	PFLAG(REQ_IDLE);
	PFLAG(REQ_RAHEAD);
	PFLAG(REQ_BACKGROUND);
	PFLAG(REQ_SWAP);
	PFLAG(REQ_INTEGRITY);
	PFLAG(REQ_IDLE);
	PFLAG(REQ_NOWAIT);
	PFLAG(REQ_FAILFAST_DEV);
	PFLAG(REQ_FAILFAST_TRANSPORT);
	PFLAG(REQ_FAILFAST_DRIVER);
#undef PFLAG

	if (p > buf && *(p - 1) == '|')
		*(p - 1) = '\0';

	p += scnprintf(p, end - p, "}");
	printk(KERN_INFO "%s\n", buf);
}
