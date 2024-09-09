#ifndef __ZONED_H__
#define __ZONED_H__
#define META_FOR_ZNS 1 // 1
#if META_FOR_ZNS
  #define META_STRIPE_CNT 2 //1
#endif
#define GRID_STRIPE 1
#if GRID_STRIPE
  #define GRID_CNT 8
#endif

#endif
