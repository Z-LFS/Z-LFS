#ifndef _LINUX_ZONED_H
#define _LINUX_ZONED_H

#ifndef META_FOR_ZNS
  #define META_FOR_ZNS 1
#endif

#if META_FOR_ZNS
  #define META_LOG_STRIPE 1
  
  #if META_LOG_STRIPE
    #define META_STRIPE_CNT 2
  #else //META_LOG_STRIPE
    #define META_STRIPE_CNT 1
  #endif //META_LOG_STRIPE

#else //META_FOR_ZNS
  #define META_LOG_STRIPE 0
#endif//META_FOR_ZNS

#define ZF2FS_MONITOR 1
#define STRIPE 1

#define IGZO 1
#define IG_SIZE 16
#define IG_NR 8
#define DEFAULT_MIN_FREE_SECS_PER_IG_HARD	1
#define DEFAULT_MIN_FREE_SECS_PER_IG_SOFT	2
#define NUM_SZ_FOR_GC (IG_NR / 4)
#define NUM_SZ_FOR_GC_COLD (IG_NR / 2)

#define SEP_SSA 1
#define ZLFS_DISPATCH 1

#define PROFILING 0
#define DEBUG 0

#endif //_LINUX_ZONED_H
