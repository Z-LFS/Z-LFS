#ifndef __CALCLOCK_H
#define __CALCLOCK_H

#include <linux/time.h>
#include "zoned.h"

struct bio;

#define BILLION 1000000000L
#define F2FS_STAT_INTERVAL_MS 1000
#define NS_TO_US 1000

extern unsigned long long wsdp_time, wsdp_cnt;
extern unsigned long long wsdp_in_time, wsdp_in_cnt;
extern unsigned long long fspw_time, fspw_cnt;
extern unsigned long long smb_time, smb_cnt;
extern unsigned long long bfp_time, bfp_cnt;
extern unsigned long long fdp_time, fdp_cnt;
extern unsigned long long sb_time, sb_cnt;

/* List of statistics to track. X(name, string) */
#define F2FS_STAT_LIST(X) \
	X(wsdp, "wsdp") \
	X(wsdp_in, "wsdp_in") \
  X(fspw, "fspw") \
  X(smb, "smb") \
  X(bfp, "bfp") \
  X(fdp, "fdp") \
  X(sb, "sb")

	
#define F2FS_TRACE_DEF_VARS(name, str) \
	unsigned long long name##_cnt, name##_time;

/* 1. Macro for declaring local variables for count and time */
#define F2FS_STAT_DEF_VARS(name, str) \
	unsigned long long name##_c, name##_t;

/* 2. Macro for printing the header string for each stat item */
#define F2FS_STAT_PRINT_HEADER(name, str) \
	str "[ cnt lat(ns) total(us) ], "

#define F2FS_STAT_PRINT_HEADER2(name, str) str ", "

/* 3. Macro for atomically exchanging the global stat counters */
#define F2FS_STAT_ATOMIC_EXCHANGE(name, str) \
	do { \
		name##_c = __atomic_exchange_n(&name##_cnt, 0, __ATOMIC_RELAXED); \
		name##_t = __atomic_exchange_n(&name##_time, 0, __ATOMIC_RELAXED); \
	} while (0);

/* 4. Macro for generating the format string for printk */
#define F2FS_STAT_PRINT_FMT(name, str) \
	"[ %llu, %llu, %llu ], "

/* 5. Macro for generating the arguments for the printk format string */
#define F2FS_STAT_PRINT_ARGS(name, str) \
	name##_c, name##_c ? name##_t / name##_c : 0, name##_t / NS_TO_US,

/* 6. Macros for columnar printing */
#define F2FS_STAT_PRINT_FMT_COLUMN(name, str) "%15s "
#define F2FS_STAT_PRINT_FMT_VALUE(name, str) "%15llu" 
#define F2FS_STAT_PRINT_NAME(name, str) str,
#define F2FS_STAT_PRINT_CNT(name, str) name##_c,
#define F2FS_STAT_PRINT_LAT(name, str) (name##_c ? div_u64(name##_t, name##_c) : 0),
#define F2FS_STAT_PRINT_TOTAL(name, str) div_u64(name##_t, NS_TO_US),

unsigned long long calclock(struct timespec64 *myclock, 
		    unsigned long long *total_time, unsigned long long *total_count);
void f2fs_trace_bio_flags(struct bio *bio);

#if PROFILING

#define F2FS_TIME_START(ts_var) \
  struct timespec64 ts_var[2]; \
	ktime_get_raw_ts64(&ts_var[0])

#define F2FS_TIME_END(ts_var, time_var, count_var) \
	do { \
		ktime_get_raw_ts64(&(ts_var)[1]); \
		calclock(ts_var, time_var, count_var); \
	} while (0)

#define F2FS_TIME_DEF(ts_var) \
  struct timespec64 ts_var[2]

#define F2FS_TIME_START_NODEF(ts_var) \
  ktime_get_raw_ts64(&ts_var[0])

#else // PROFILING

#define F2FS_TIME_START(ts_var) do {} while (0)
#define F2FS_TIME_END(ts_var, time_var, count_var) do {} while (0)
#define F2FS_TIME_DEF(ts_var) do {} while (0)
#define F2FS_TIME_START_NODEF(ts_var) do {} while (0)

#endif /* PROFILING */

#endif
