/*  Copyright (C) 2026 Kamila Szewczyk

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#ifndef MKC_COMMON_H
#define MKC_COMMON_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t  u8;  typedef int8_t  i8;
typedef uint16_t u16;  typedef int16_t i16;
typedef uint32_t u32;  typedef int32_t i32;
typedef uint64_t u64;  typedef int64_t i64;
typedef size_t   sz;
__extension__ typedef unsigned __int128 u128;

#if CHAR_BIT != 8 || INT_MAX != 2147483647 || SIZE_MAX < UINT64_MAX
#error "mkc requires 8-bit bytes, 32-bit int, and a 64-bit size_t"
#endif

#define SI          static inline
#define INLINE      __inline__ __attribute__((__always_inline__))
#define NOINLINE    __attribute__((__noinline__))
#define HOT         __attribute__((__hot__))
#define COLD        __attribute__((__cold__))
#define NORETURN    __attribute__((__noreturn__))
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define PREFETCH(p) __builtin_prefetch(p)
#define MKC_PRINTF(f, a) __attribute__((__format__(__printf__, f, a)))
#define MKC_TLS     __thread

static INLINE u32 rd16le(const u8 * p) { return p[0] | (u32) p[1] << 8; }
static INLINE u32 rd32le(const u8 * p) {
  return p[0] | (u32) p[1] << 8 | (u32) p[2] << 16 | (u32) p[3] << 24;
}
static INLINE u64 rd64le(const u8 * p) {
  return rd32le(p) | (u64) rd32le(p + 4) << 32;
}
static INLINE void wr16le(u8 * p, u32 v) {
  p[0] = (u8) v;  p[1] = (u8) (v >> 8);
}
static INLINE void wr32le(u8 * p, u32 v) {
  p[0] = (u8) v;  p[1] = (u8) (v >> 8);
  p[2] = (u8) (v >> 16);  p[3] = (u8) (v >> 24);
}
static INLINE void wr64le(u8 * p, u64 v) {
  wr32le(p, (u32) v);  wr32le(p + 4, (u32) (v >> 32));
}

#define MKC_EXIT_OK    0
#define MKC_EXIT_FAIL  1

#define FATAL         mkc_fatal
#define FATAL_UNLESS  mkc_fatal_unless

NORETURN void mkc_fatal(const char * fmt, ...) MKC_PRINTF(1, 2);
void mkc_fatal_unless(int cond, const char * fmt, ...) MKC_PRINTF(2, 3);

#define FATAL_IF_HOT(cond)  if (UNLIKELY(cond)) FATAL

#define Fi(n, ...) for (i = 0; i < (n); i++) {  __VA_ARGS__; }
#define Fj(n, ...) for (j = 0; j < (n); j++) {  __VA_ARGS__; }
#define Fk(n, ...) for (k = 0; k < (n); k++) {  __VA_ARGS__; }

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define ABS(x) ((x) < 0 ? -(x) : (x))
#define ARRAY_LEN(a) (sizeof (a) / sizeof *(a))

void * xmalloc(sz n);
void * xcalloc(sz n, sz size);
void * xrealloc(void * p, sz n);
void xfree(void * p);

#endif
