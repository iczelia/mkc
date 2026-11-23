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

#ifndef MKC_SORT_H
#define MKC_SORT_H

#include "common.h"

#ifndef HSORT_LEAF
#define HSORT_LEAF 8
#endif
#ifndef HSORT_SMALL
#define HSORT_SMALL 64
#endif
#define HSORT_DUP_SLOTS 4096u

SI void hsort_t(u64 * t, u64 * a, sz n, u32 bits, const u64 * ahead);

SI void hsort_insert(u64 * a, sz n) {
  sz i, at;
  for (i = 1; i < n; i++) {
    u64 v = a[i];
    at = i;
    while (at && a[at - 1] > v) { a[at] = a[at - 1];  at--; }
    a[at] = v;
  }
}

#define HSORT_CE(i, j) { const u64 p_ = x##i, q_ = x##j;                      \
  x##i = p_ < q_ ? p_ : q_;  x##j = p_ < q_ ? q_ : p_; }

#define HSORT_IN2 x0 = from[0];  x1 = from[1];
#define HSORT_IN3 HSORT_IN2  x2 = from[2];
#define HSORT_IN4 HSORT_IN3  x3 = from[3];
#define HSORT_IN5 HSORT_IN4  x4 = from[4];
#define HSORT_IN6 HSORT_IN5  x5 = from[5];
#define HSORT_IN7 HSORT_IN6  x6 = from[6];
#define HSORT_IN8 HSORT_IN7  x7 = from[7];
#define HSORT_IN9 HSORT_IN8  x8 = from[8];
#define HSORT_IN10 HSORT_IN9  x9 = from[9];
#define HSORT_IN11 HSORT_IN10  x10 = from[10];
#define HSORT_IN12 HSORT_IN11  x11 = from[11];
#define HSORT_IN13 HSORT_IN12  x12 = from[12];
#define HSORT_IN14 HSORT_IN13  x13 = from[13];
#define HSORT_IN15 HSORT_IN14  x14 = from[14];
#define HSORT_IN16 HSORT_IN15  x15 = from[15];
#define HSORT_OUT2 to[0] = x0;  to[1] = x1;
#define HSORT_OUT3 HSORT_OUT2  to[2] = x2;
#define HSORT_OUT4 HSORT_OUT3  to[3] = x3;
#define HSORT_OUT5 HSORT_OUT4  to[4] = x4;
#define HSORT_OUT6 HSORT_OUT5  to[5] = x5;
#define HSORT_OUT7 HSORT_OUT6  to[6] = x6;
#define HSORT_OUT8 HSORT_OUT7  to[7] = x7;
#define HSORT_OUT9 HSORT_OUT8  to[8] = x8;
#define HSORT_OUT10 HSORT_OUT9  to[9] = x9;
#define HSORT_OUT11 HSORT_OUT10  to[10] = x10;
#define HSORT_OUT12 HSORT_OUT11  to[11] = x11;
#define HSORT_OUT13 HSORT_OUT12  to[12] = x12;
#define HSORT_OUT14 HSORT_OUT13  to[13] = x13;
#define HSORT_OUT15 HSORT_OUT14  to[14] = x14;
#define HSORT_OUT16 HSORT_OUT15  to[15] = x15;

SI void hsort_leaf(const u64 * from, u64 * to, sz n) {
  u64 x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15;
  switch (n) {
  case 0: return;
  case 1: to[0] = from[0];  return;
  case 2:
    HSORT_IN2
    HSORT_CE(0, 1)
    HSORT_OUT2
    return;
  case 3:
    HSORT_IN3
    HSORT_CE(0, 2) HSORT_CE(0, 1) HSORT_CE(1, 2)
    HSORT_OUT3
    return;
  case 4:
    HSORT_IN4
    HSORT_CE(0, 2) HSORT_CE(1, 3) HSORT_CE(0, 1) HSORT_CE(2, 3) HSORT_CE(1, 2)
    HSORT_OUT4
    return;
  case 5:
    HSORT_IN5
    HSORT_CE(0, 3) HSORT_CE(1, 4) HSORT_CE(0, 2) HSORT_CE(1, 3) HSORT_CE(0, 1)
    HSORT_CE(2, 4) HSORT_CE(1, 2) HSORT_CE(3, 4) HSORT_CE(2, 3)
    HSORT_OUT5
    return;
  case 6:
    HSORT_IN6
    HSORT_CE(0, 5) HSORT_CE(1, 3) HSORT_CE(2, 4) HSORT_CE(1, 2) HSORT_CE(3, 4)
    HSORT_CE(0, 3) HSORT_CE(2, 5) HSORT_CE(0, 1) HSORT_CE(2, 3) HSORT_CE(4, 5)
    HSORT_CE(1, 2) HSORT_CE(3, 4)
    HSORT_OUT6
    return;
  case 7:
    HSORT_IN7
    HSORT_CE(0, 6) HSORT_CE(2, 3) HSORT_CE(4, 5) HSORT_CE(0, 2) HSORT_CE(1, 4)
    HSORT_CE(3, 6) HSORT_CE(0, 1) HSORT_CE(2, 5) HSORT_CE(3, 4) HSORT_CE(1, 2)
    HSORT_CE(4, 6) HSORT_CE(2, 3) HSORT_CE(4, 5) HSORT_CE(1, 2) HSORT_CE(3, 4)
    HSORT_CE(5, 6)
    HSORT_OUT7
    return;
  case 8:
    HSORT_IN8
    HSORT_CE(0, 2) HSORT_CE(1, 3) HSORT_CE(4, 6) HSORT_CE(5, 7) HSORT_CE(0, 4)
    HSORT_CE(1, 5) HSORT_CE(2, 6) HSORT_CE(3, 7) HSORT_CE(0, 1) HSORT_CE(2, 3)
    HSORT_CE(4, 5) HSORT_CE(6, 7) HSORT_CE(2, 4) HSORT_CE(3, 5) HSORT_CE(1, 4)
    HSORT_CE(3, 6) HSORT_CE(1, 2) HSORT_CE(3, 4) HSORT_CE(5, 6)
    HSORT_OUT8
    return;
  case 9:
    HSORT_IN9
    HSORT_CE(0, 3) HSORT_CE(1, 7) HSORT_CE(2, 5) HSORT_CE(4, 8) HSORT_CE(0, 7)
    HSORT_CE(2, 4) HSORT_CE(3, 8) HSORT_CE(5, 6) HSORT_CE(0, 2) HSORT_CE(1, 3)
    HSORT_CE(4, 5) HSORT_CE(7, 8) HSORT_CE(1, 4) HSORT_CE(3, 6) HSORT_CE(5, 7)
    HSORT_CE(0, 1) HSORT_CE(2, 4) HSORT_CE(3, 5) HSORT_CE(6, 8) HSORT_CE(2, 3)
    HSORT_CE(4, 5) HSORT_CE(6, 7) HSORT_CE(1, 2) HSORT_CE(3, 4) HSORT_CE(5, 6)
    HSORT_OUT9
    return;
  case 10:
    HSORT_IN10
    HSORT_CE(0, 8) HSORT_CE(1, 9) HSORT_CE(2, 7) HSORT_CE(3, 5) HSORT_CE(4, 6)
    HSORT_CE(0, 2) HSORT_CE(1, 4) HSORT_CE(5, 8) HSORT_CE(7, 9) HSORT_CE(0, 3)
    HSORT_CE(2, 4) HSORT_CE(5, 7) HSORT_CE(6, 9) HSORT_CE(0, 1) HSORT_CE(3, 6)
    HSORT_CE(8, 9) HSORT_CE(1, 5) HSORT_CE(2, 3) HSORT_CE(4, 8) HSORT_CE(6, 7)
    HSORT_CE(1, 2) HSORT_CE(3, 5) HSORT_CE(4, 6) HSORT_CE(7, 8) HSORT_CE(2, 3)
    HSORT_CE(4, 5) HSORT_CE(6, 7) HSORT_CE(3, 4) HSORT_CE(5, 6)
    HSORT_OUT10
    return;
  case 11:
    HSORT_IN11
    HSORT_CE(4, 8) HSORT_CE(5, 6) HSORT_CE(9, 10) HSORT_CE(0, 5)
    HSORT_CE(1, 7) HSORT_CE(2, 9) HSORT_CE(3, 4) HSORT_CE(0, 1) HSORT_CE(2, 3)
    HSORT_CE(4, 5) HSORT_CE(6, 8) HSORT_CE(7, 9) HSORT_CE(0, 2) HSORT_CE(1, 3)
    HSORT_CE(4, 10) HSORT_CE(6, 7) HSORT_CE(8, 9) HSORT_CE(1, 2)
    HSORT_CE(4, 6) HSORT_CE(5, 7) HSORT_CE(8, 10) HSORT_CE(1, 4)
    HSORT_CE(2, 6) HSORT_CE(5, 8) HSORT_CE(7, 10) HSORT_CE(2, 4)
    HSORT_CE(3, 6) HSORT_CE(3, 5) HSORT_CE(6, 8) HSORT_CE(7, 9) HSORT_CE(3, 4)
    HSORT_CE(5, 6) HSORT_CE(7, 8) HSORT_CE(9, 10) HSORT_CE(6, 7)
    HSORT_CE(8, 9)
    HSORT_OUT11
    return;
  case 12:
    HSORT_IN12
    HSORT_CE(4, 8) HSORT_CE(5, 6) HSORT_CE(7, 11) HSORT_CE(9, 10)
    HSORT_CE(0, 5) HSORT_CE(1, 7) HSORT_CE(2, 9) HSORT_CE(3, 4) HSORT_CE(0, 1)
    HSORT_CE(2, 3) HSORT_CE(4, 5) HSORT_CE(6, 8) HSORT_CE(7, 9)
    HSORT_CE(10, 11) HSORT_CE(0, 2) HSORT_CE(1, 3) HSORT_CE(4, 10)
    HSORT_CE(5, 11) HSORT_CE(6, 7) HSORT_CE(8, 9) HSORT_CE(1, 2)
    HSORT_CE(4, 6) HSORT_CE(5, 7) HSORT_CE(8, 10) HSORT_CE(9, 11)
    HSORT_CE(1, 4) HSORT_CE(2, 6) HSORT_CE(5, 8) HSORT_CE(7, 10)
    HSORT_CE(2, 4) HSORT_CE(3, 6) HSORT_CE(3, 5) HSORT_CE(6, 8) HSORT_CE(7, 9)
    HSORT_CE(3, 4) HSORT_CE(5, 6) HSORT_CE(7, 8) HSORT_CE(9, 10)
    HSORT_CE(6, 7) HSORT_CE(8, 9)
    HSORT_OUT12
    return;
  case 13:
    HSORT_IN13
    HSORT_CE(1, 12) HSORT_CE(4, 8) HSORT_CE(5, 6) HSORT_CE(7, 11)
    HSORT_CE(9, 10) HSORT_CE(0, 5) HSORT_CE(1, 7) HSORT_CE(2, 9)
    HSORT_CE(3, 4) HSORT_CE(11, 12) HSORT_CE(0, 1) HSORT_CE(2, 3)
    HSORT_CE(4, 5) HSORT_CE(6, 8) HSORT_CE(7, 9) HSORT_CE(10, 11)
    HSORT_CE(0, 2) HSORT_CE(1, 3) HSORT_CE(4, 10) HSORT_CE(5, 11)
    HSORT_CE(6, 7) HSORT_CE(8, 9) HSORT_CE(1, 2) HSORT_CE(3, 12)
    HSORT_CE(4, 6) HSORT_CE(5, 7) HSORT_CE(8, 10) HSORT_CE(9, 11)
    HSORT_CE(1, 4) HSORT_CE(2, 6) HSORT_CE(5, 8) HSORT_CE(7, 10)
    HSORT_CE(2, 4) HSORT_CE(3, 6) HSORT_CE(9, 12) HSORT_CE(3, 5)
    HSORT_CE(6, 8) HSORT_CE(7, 9) HSORT_CE(10, 12) HSORT_CE(3, 4)
    HSORT_CE(5, 6) HSORT_CE(7, 8) HSORT_CE(9, 10) HSORT_CE(11, 12)
    HSORT_CE(6, 7) HSORT_CE(8, 9)
    HSORT_OUT13
    return;
  case 14:
    HSORT_IN14
    HSORT_CE(0, 13) HSORT_CE(1, 12) HSORT_CE(4, 8) HSORT_CE(5, 6)
    HSORT_CE(7, 11) HSORT_CE(9, 10) HSORT_CE(0, 5) HSORT_CE(1, 7)
    HSORT_CE(2, 9) HSORT_CE(3, 4) HSORT_CE(6, 13) HSORT_CE(11, 12)
    HSORT_CE(0, 1) HSORT_CE(2, 3) HSORT_CE(4, 5) HSORT_CE(6, 8) HSORT_CE(7, 9)
    HSORT_CE(10, 11) HSORT_CE(12, 13) HSORT_CE(0, 2) HSORT_CE(1, 3)
    HSORT_CE(4, 10) HSORT_CE(5, 11) HSORT_CE(6, 7) HSORT_CE(8, 9)
    HSORT_CE(1, 2) HSORT_CE(3, 12) HSORT_CE(4, 6) HSORT_CE(5, 7)
    HSORT_CE(8, 10) HSORT_CE(9, 11) HSORT_CE(1, 4) HSORT_CE(2, 6)
    HSORT_CE(5, 8) HSORT_CE(7, 10) HSORT_CE(9, 13) HSORT_CE(2, 4)
    HSORT_CE(3, 6) HSORT_CE(9, 12) HSORT_CE(11, 13) HSORT_CE(3, 5)
    HSORT_CE(6, 8) HSORT_CE(7, 9) HSORT_CE(10, 12) HSORT_CE(3, 4)
    HSORT_CE(5, 6) HSORT_CE(7, 8) HSORT_CE(9, 10) HSORT_CE(11, 12)
    HSORT_CE(6, 7) HSORT_CE(8, 9)
    HSORT_OUT14
    return;
  case 15:
    HSORT_IN15
    HSORT_CE(0, 13) HSORT_CE(1, 12) HSORT_CE(3, 14) HSORT_CE(4, 8)
    HSORT_CE(5, 6) HSORT_CE(7, 11) HSORT_CE(9, 10) HSORT_CE(0, 5)
    HSORT_CE(1, 7) HSORT_CE(2, 9) HSORT_CE(3, 4) HSORT_CE(6, 13)
    HSORT_CE(8, 14) HSORT_CE(11, 12) HSORT_CE(0, 1) HSORT_CE(2, 3)
    HSORT_CE(4, 5) HSORT_CE(6, 8) HSORT_CE(7, 9) HSORT_CE(10, 11)
    HSORT_CE(12, 13) HSORT_CE(0, 2) HSORT_CE(1, 3) HSORT_CE(4, 10)
    HSORT_CE(5, 11) HSORT_CE(6, 7) HSORT_CE(8, 9) HSORT_CE(12, 14)
    HSORT_CE(1, 2) HSORT_CE(3, 12) HSORT_CE(4, 6) HSORT_CE(5, 7)
    HSORT_CE(8, 10) HSORT_CE(9, 11) HSORT_CE(13, 14) HSORT_CE(1, 4)
    HSORT_CE(2, 6) HSORT_CE(5, 8) HSORT_CE(7, 10) HSORT_CE(9, 13)
    HSORT_CE(11, 14) HSORT_CE(2, 4) HSORT_CE(3, 6) HSORT_CE(9, 12)
    HSORT_CE(11, 13) HSORT_CE(3, 5) HSORT_CE(6, 8) HSORT_CE(7, 9)
    HSORT_CE(10, 12) HSORT_CE(3, 4) HSORT_CE(5, 6) HSORT_CE(7, 8)
    HSORT_CE(9, 10) HSORT_CE(11, 12) HSORT_CE(6, 7) HSORT_CE(8, 9)
    HSORT_OUT15
    return;
  case 16:
    HSORT_IN16
    HSORT_CE(0, 13) HSORT_CE(1, 12) HSORT_CE(2, 15) HSORT_CE(3, 14)
    HSORT_CE(4, 8) HSORT_CE(5, 6) HSORT_CE(7, 11) HSORT_CE(9, 10)
    HSORT_CE(0, 5) HSORT_CE(1, 7) HSORT_CE(2, 9) HSORT_CE(3, 4)
    HSORT_CE(6, 13) HSORT_CE(8, 14) HSORT_CE(10, 15) HSORT_CE(11, 12)
    HSORT_CE(0, 1) HSORT_CE(2, 3) HSORT_CE(4, 5) HSORT_CE(6, 8) HSORT_CE(7, 9)
    HSORT_CE(10, 11) HSORT_CE(12, 13) HSORT_CE(14, 15) HSORT_CE(0, 2)
    HSORT_CE(1, 3) HSORT_CE(4, 10) HSORT_CE(5, 11) HSORT_CE(6, 7)
    HSORT_CE(8, 9) HSORT_CE(12, 14) HSORT_CE(13, 15) HSORT_CE(1, 2)
    HSORT_CE(3, 12) HSORT_CE(4, 6) HSORT_CE(5, 7) HSORT_CE(8, 10)
    HSORT_CE(9, 11) HSORT_CE(13, 14) HSORT_CE(1, 4) HSORT_CE(2, 6)
    HSORT_CE(5, 8) HSORT_CE(7, 10) HSORT_CE(9, 13) HSORT_CE(11, 14)
    HSORT_CE(2, 4) HSORT_CE(3, 6) HSORT_CE(9, 12) HSORT_CE(11, 13)
    HSORT_CE(3, 5) HSORT_CE(6, 8) HSORT_CE(7, 9) HSORT_CE(10, 12)
    HSORT_CE(3, 4) HSORT_CE(5, 6) HSORT_CE(7, 8) HSORT_CE(9, 10)
    HSORT_CE(11, 12) HSORT_CE(6, 7) HSORT_CE(8, 9)
    HSORT_OUT16
    return;
  }
  if (to != from) memcpy(to, from, n * 8);
  hsort_insert(to, n);
}

#undef HSORT_CE
#undef HSORT_IN2
#undef HSORT_OUT2
#undef HSORT_IN3
#undef HSORT_OUT3
#undef HSORT_IN4
#undef HSORT_OUT4
#undef HSORT_IN5
#undef HSORT_OUT5
#undef HSORT_IN6
#undef HSORT_OUT6
#undef HSORT_IN7
#undef HSORT_OUT7
#undef HSORT_IN8
#undef HSORT_OUT8
#undef HSORT_IN9
#undef HSORT_OUT9
#undef HSORT_IN10
#undef HSORT_OUT10
#undef HSORT_IN11
#undef HSORT_OUT11
#undef HSORT_IN12
#undef HSORT_OUT12
#undef HSORT_IN13
#undef HSORT_OUT13
#undef HSORT_IN14
#undef HSORT_OUT14
#undef HSORT_IN15
#undef HSORT_OUT15
#undef HSORT_IN16
#undef HSORT_OUT16

SI u32 hsort_digit(sz n, u32 bits) {
  u32 d = 0;
  while ((n >> d) > HSORT_LEAF) d++;
  if (d > 11) d = 11;
  return d < bits ? d : bits;
}

static NOINLINE u32 hsort_scatter(const u64 * restrict from,
                                  u64 * restrict to, sz n, u32 bits,
                                  sz * restrict off, const u64 * ahead) {
  u32 digit = hsort_digit(n, bits), shift = bits - digit;
  sz i, j, d, mask = ((sz) 1 << digit) - 1, nb = mask + 1;
  sz cur[2048];
  memset(off, 0, (nb + 1) * sizeof *off);
  Fi(n, off[((from[i] >> shift) & mask) + 1]++);
  Fj(nb, if (off[j + 1] == n) return bits);
  Fj(nb, off[j + 1] += off[j]);
  memcpy(cur, off, nb * sizeof *cur);
  if (n > 65536) {
    static MKC_TLS u64 stage[2048][8];
    u8 fill[2048] = {0};
    Fi(n,
      u64 v = from[i];
      u32 f;
      d = (v >> shift) & mask;  f = fill[d];  stage[d][f] = v;
      if (++f == 8) {
        memcpy(to + cur[d], stage[d], 64);
        cur[d] += 8;  f = 0;
      }
      fill[d] = (u8) f);
    Fj(nb, memcpy(to + cur[j], stage[j], (sz) fill[j] * 8));
  } else {
    const uintptr_t next = (uintptr_t) (ahead ? ahead : from);
    sz end[2048], k = n - 1, pa, pb;
    u64 pv = from[0], pw = from[k];
    memcpy(end, off + 1, nb * sizeof *end);
    pa = cur[(pv >> shift) & mask]++;  pb = --end[(pw >> shift) & mask];
    for (i = 1; i + 1 < k; i++, k--) {
      const u64 v = from[i], w = from[k - 1];
      const sz a = cur[(v >> shift) & mask]++, b = --end[(w >> shift) & mask];
      if (!(i & 3))
        __builtin_prefetch((const void *) (next + 16 * i - 64), 0, 2);
      to[pa] = pv;  to[pb] = pw;
      pa = a;  pb = b;  pv = v;  pw = w;
    }
    to[pa] = pv;  to[pb] = pw;
    if (i < k) to[cur[(from[i] >> shift) & mask]] = from[i];
  }
  return shift;
}

SI u32 hsort_same(const u64 * a, sz n) {
  u64 x = 0;
  sz i;
  Fi(n, x |= a[i] ^ a[0]);
  return x ? 64 - (u32) __builtin_clzll(x) : 0;
}

SI void hsort_a(u64 * a, u64 * t, sz n, u32 bits) {
  while (n > HSORT_SMALL && bits) {
    sz j, nb, off[2049];
    u32 shift = hsort_scatter(a, t, n, bits, off, NULL);
    if (shift == bits) { bits = hsort_same(a, n);  continue; }
    nb = (sz) 1 << (bits - shift);
    Fj(nb,
      if (off[j + 1] > off[j])
        hsort_t(t + off[j], a + off[j], off[j + 1] - off[j], shift, NULL));
    return;
  }
  if (bits) hsort_leaf(a, a, n);
}

static void hsort_buckets(u64 * a, u64 * t, const sz * off, sz nb,
                          u32 shift) {
  u64 of[18][32], m;
  const sz words = (nb + 63) / 64;
  sz j, w;
  u32 c;
  Fj(18, for (w = 0; w < words; w++) of[j][w] = 0);
  Fj(nb,
    const sz size = off[j + 1] - off[j];
    of[size < 17 ? size : 17][j >> 6] |= UINT64_C(1) << (j & 63));
  for (c = 2; c <= 16 && shift; c++)
    for (w = 0; w < words; w++)
      for (m = of[c][w]; m; m &= m - 1) {
        u64 * const at = a + off[64 * w + (sz) __builtin_ctzll(m)];
        hsort_leaf(at, at, c);
      }
  for (w = 0; w < words; w++)
    for (m = of[17][w]; m; m &= m - 1) {
      j = 64 * w + (sz) __builtin_ctzll(m);
      hsort_a(a + off[j], t + off[j], off[j + 1] - off[j], shift);
    }
}

SI void hsort_t(u64 * t, u64 * a, sz n, u32 bits, const u64 * ahead) {
  while (n > HSORT_SMALL && bits) {
    sz nb, off[2049];
    u32 shift = hsort_scatter(t, a, n, bits, off, ahead);
    if (shift == bits) { bits = hsort_same(t, n);  continue; }
    nb = (sz) 1 << (bits - shift);
    hsort_buckets(a, t, off, nb, shift);
    return;
  }
  if (bits) hsort_leaf(t, a, n);
  else memcpy(a, t, n * 8);
}

SI void hsort_dup(u64 * t, u64 * a, sz n, u32 bits,
                  sz * ratio, const u64 * ahead) {
  static MKC_TLS u64 keys[HSORT_DUP_SLOTS];
  static MKC_TLS u32 counts[HSORT_DUP_SLOTS];
  u64 found[HSORT_DUP_SLOTS / 2];
  u32 c, k, times[HSORT_DUP_SLOTS / 2];
  sz i, j, h, d, want, slots, mask, lim, m = 0;
  u32 shift, lg = 8;
  if (n < 256 || n > UINT32_MAX || bits < 12 || *ratio == SIZE_MAX) {
    hsort_t(t, a, n, bits, ahead);  return;
  }
  if (!*ratio) {
    hsort_t(t, a, n, bits, ahead);
    d = 1;
    for (i = 1; i < n; i++) d += a[i] != a[i - 1];
    *ratio = 2 * d <= n ? d * 1024 / n + 1 : SIZE_MAX;
    return;
  }
  want = n * *ratio / 1024 + 1;
  while (((sz) 1 << lg) < 4 * want) lg++;
  if (((sz) 1 << lg) > HSORT_DUP_SLOTS) {
    *ratio = SIZE_MAX;  hsort_t(t, a, n, bits, ahead);  return;
  }
  slots = (sz) 1 << lg;  mask = slots - 1;  lim = slots / 2;
  shift = bits - lg;  d = 0;
  memset(keys, 0xFF, slots * 8);
  Fi(n,
    u64 key = t[i];
    h = (key >> shift) & mask;
    while (keys[h] != key && keys[h] != UINT64_MAX) h = (h + 1) & mask;
    if (keys[h] == key) { counts[h]++;  continue; }
    if (++d > lim) {
      *ratio = SIZE_MAX;  hsort_t(t, a, n, bits, ahead);  return;
    }
    keys[h] = key;  counts[h] = 1);
  Fj(slots,
    if (keys[j] != UINT64_MAX) {
      u64 key = keys[j];
      sz at = m++;
      c = counts[j];
      while (at && found[at - 1] > key) {
        found[at] = found[at - 1];  times[at] = times[at - 1];  at--;
      }
      found[at] = key;  times[at] = c;
    });
  Fj(m, Fk(times[j], *a++ = found[j]));
  *ratio = m * 1024 / n + 1;
}

#endif
