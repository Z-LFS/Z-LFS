#ifndef _LINUX_ZONED_H
#define _LINUX_ZONED_H

#define NPO2 0 //support NPO2 ZNS SSD

#ifndef META_FOR_ZNS
  #define META_FOR_ZNS 1
#endif

#define DEBUG_GC 0

#if META_FOR_ZNS
  //for evaluation - have to change META_LOG_STRIPE of mkfs
  #define NAIVE_MFZ 0
  #if !NAIVE_MFZ
    #define DELAYED_MERGE 1
    #define META_LOG_STRIPE 1
    
    #if META_LOG_STRIPE
      #define META_STRIPE_CNT 2
    #else //META_LOG_STRIPE
      #define META_STRIPE_CNT 1
    #endif //META_LOG_STRIPE

  #else //NAIVE_MFZ
    #define DELAYED_MERGE 1
    #define META_LOG_STRIPE 0
  #endif //NAIVE_MFZ

#else //META_FOR_ZNS
  #define DELAYED_MERGE 0
  #define META_LOG_STRIPE 0
#endif//META_FOR_ZNS

#define OPT 2

#define ZF2FS_MONITOR 1
#define STRIPE 1

#if STRIPE
  #define GRID_STRIPE 1
  #define DYNAMIC_STRIPE 1

  #if GRID_STRIPE
    #define GRID_CNT 8
  #endif

  #define STRIPE_SMALL 0
  #define STRIPE_MAX_CNT 16
  #define STRIPE_CNT 8
  #define STRIPE_MIN_CNT 4
  #define NODE_STRIPE 1
#else // STRIPE 
  #define GRID_STRIPE 0
  #define STRIPE_MAX_CNT 1
  #define STRIPE_CNT 1
  #define STRIPE_MIN_CNT 1
  #define NODE_STRIPE 0
#endif // STRIPE

#endif //_LINUX_ZONED_H
