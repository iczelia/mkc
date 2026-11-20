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

SI void hsort_t(u64 * t, u64 * a, sz n, u32 bits);

SI void hsort_insert(u64 * a, sz n) {
  sz i, at;
  for (i = 1; i < n; i++) {
    u64 v = a[i];
    at = i;
    while (at && a[at - 1] > v) { a[at] = a[at - 1];  at--; }
    a[at] = v;
  }
}

SI u32 hsort_digit(sz n, u32 bits) {
  u32 d = 0;
  while ((n >> d) > HSORT_LEAF) d++;
  if (d > 11) d = 11;
  return d < bits ? d : bits;
}

static NOINLINE u32 hsort_scatter(const u64 * from, u64 * to,
                                  sz n, u32 bits, sz * off) {
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
      d = (from[i] >> shift) & mask;  stage[d][fill[d]++] = from[i];
      if (fill[d] == 8) {
        memcpy(to + cur[d], stage[d], 64);
        cur[d] += 8;  fill[d] = 0;
      });
    Fj(nb, memcpy(to + cur[j], stage[j], (sz) fill[j] * 8));
  } else Fi(n, to[cur[(from[i] >> shift) & mask]++] = from[i]);
  return shift;
}

SI void hsort_a(u64 * a, u64 * t, sz n, u32 bits) {
  while (n > HSORT_SMALL && bits) {
    sz j, nb, off[2049];
    u32 shift = hsort_scatter(a, t, n, bits, off);
    if (shift == bits) { bits -= hsort_digit(n, bits);  continue; }
    nb = (sz) 1 << (bits - shift);
    Fj(nb,
      if (off[j + 1] > off[j])
        hsort_t(t + off[j], a + off[j], off[j + 1] - off[j], shift));
    return;
  }
  if (bits) hsort_insert(a, n);
}

SI void hsort_t(u64 * t, u64 * a, sz n, u32 bits) {
  while (n > HSORT_SMALL && bits) {
    sz j, nb, off[2049];
    u32 shift = hsort_scatter(t, a, n, bits, off);
    if (shift == bits) { bits -= hsort_digit(n, bits);  continue; }
    nb = (sz) 1 << (bits - shift);
    Fj(nb,
      if (off[j + 1] > off[j])
        hsort_a(a + off[j], t + off[j], off[j + 1] - off[j], shift));
    return;
  }
  memcpy(a, t, n * 8);
  if (bits) hsort_insert(a, n);
}

SI void hsort_dup(u64 * t, u64 * a, sz n, u32 bits,
                  sz * ratio) {
  static MKC_TLS u64 keys[HSORT_DUP_SLOTS];
  static MKC_TLS u32 counts[HSORT_DUP_SLOTS];
  u64 found[HSORT_DUP_SLOTS / 2];
  u32 c, k, times[HSORT_DUP_SLOTS / 2];
  sz i, j, h, d, want, slots, mask, lim, m = 0;
  u32 shift, lg = 8;
  if (n < 256 || n > UINT32_MAX || bits < 12 || *ratio == SIZE_MAX) {
    hsort_t(t, a, n, bits);  return;
  }
  if (!*ratio) {
    hsort_t(t, a, n, bits);
    d = 1;
    for (i = 1; i < n; i++) d += a[i] != a[i - 1];
    *ratio = 2 * d <= n ? d * 1024 / n + 1 : SIZE_MAX;
    return;
  }
  want = n * *ratio / 1024 + 1;
  while (((sz) 1 << lg) < 4 * want) lg++;
  if (((sz) 1 << lg) > HSORT_DUP_SLOTS) {
    *ratio = SIZE_MAX;  hsort_t(t, a, n, bits);  return;
  }
  slots = (sz) 1 << lg;  mask = slots - 1;  lim = slots / 2;
  shift = bits - lg;  d = 0;
  memset(keys, 0xFF, slots * 8);
  Fi(n,
    u64 key = t[i];
    h = (key >> shift) & mask;
    while (keys[h] != key && keys[h] != UINT64_MAX) h = (h + 1) & mask;
    if (keys[h] == key) { counts[h]++;  continue; }
    if (++d > lim) { *ratio = SIZE_MAX;  hsort_t(t, a, n, bits);  return; }
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
