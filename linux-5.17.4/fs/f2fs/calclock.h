#ifndef __CALCLOCK_H
#define __CALCLOCK_H

#include <linux/time64.h>

#define BILLION 1000000000UL

unsigned long long calclock(struct timespec64 *myclock, 
		unsigned long long *total_time, unsigned long long *total_clock);

#endif