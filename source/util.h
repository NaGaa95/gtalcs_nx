/* util.h -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>

// debugPrintf is the game's `printf` import target, so it stays a real function;
// with DEBUG_LOG off (config.h) its body compiles away to a no-op (no file, no
// output) -- see util.c.
int debugPrintf(char *text, ...);

void cpu_boost(int on);

int ret0(void);
int retm1(void);

static inline void armSetTlsRw(void *addr) {
  __asm__  ("msr s3_3_c13_c0_2, %0" : : "r"(addr));
}

static inline uint64_t umin(uint64_t a, uint64_t b) {
  return (a < b) ? a : b;
}

#endif
