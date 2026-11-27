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

#include "seed.h"
#include "simd.h"

#include <immintrin.h>

static INLINE u64 bits(u32 n) {
  return n >= 64 ? UINT64_MAX : ((u64) 1 << n) - 1;
}

static INLINE u64 load64(const u8 * p) {
  u64 v;  memcpy(&v, p, 8);  return v;
}

static INLINE u8 base(u8 c) {
  switch (c) {
  case 'A': case 'a': return 0;
  case 'C': case 'c': return 1;
  case 'G': case 'g': return 2;
  case 'T': case 't': return 3;
  default: return 255;
  }
}

void dec_avx2(u8 * p, sz n) {
  sz i = 0;
  const __m256i upper = _mm256_set1_epi8((char) 0xDF),
                three = _mm256_set1_epi8(3),
                a = _mm256_set1_epi8('A'), c = _mm256_set1_epi8('C'),
                g = _mm256_set1_epi8('G'), t = _mm256_set1_epi8('T'),
                ones = _mm256_set1_epi8((char) 0xFF);
  for (; n - i >= 32; i += 32) {
    __m256i x = _mm256_loadu_si256((const __m256i *) (const void *) (p + i)), valid, code;
    x = _mm256_and_si256(x, upper);
    valid = _mm256_or_si256(_mm256_cmpeq_epi8(x, a), _mm256_cmpeq_epi8(x, c));
    valid = _mm256_or_si256(valid, _mm256_cmpeq_epi8(x, g));
    valid = _mm256_or_si256(valid, _mm256_cmpeq_epi8(x, t));
    code = _mm256_xor_si256(_mm256_srli_epi16(x, 1), _mm256_srli_epi16(x, 2));
    code = _mm256_and_si256(code, three);
    code = _mm256_or_si256(code, _mm256_andnot_si256(valid, ones));
    _mm256_storeu_si256((__m256i *) (void *) (p + i), code);
  }
  for (; i < n; i++) p[i] = base(p[i]);
}

static INLINE __m256i pack32(__m256i lo, __m256i hi, __m256i lows) {
  return _mm256_set_m128i(
    _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(hi, lows)),
    _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(lo, lows)));
}

void scan_ranks_avx2(const u8 * p, sz count, u32 m, u32 * out) {
  const u64 mask = bits(2 * m);
  const long long b = 64 - 2 * (long long) m;
  const __m256i sxl = _mm256_setr_epi64x(14, 12, 10, 8),
                sxh = _mm256_setr_epi64x(6, 4, 2, 0),
                syl = _mm256_setr_epi64x(b - 14, b - 12, b - 10, b - 8),
                syh = _mm256_setr_epi64x(b - 6, b - 4, b - 2, b),
                vmask = _mm256_set1_epi64x((long long) mask),
                lows = _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6),
                c1 = _mm256_set1_epi32(0x7FEB352D),
                c2 = _mm256_set1_epi32((int) 0x846CA68Bu),
                byte = _mm256_set1_epi32(255),
                eps = _mm256_set1_epi32(DEC_EPS),
                neps = _mm256_set1_epi32(-DEC_EPS),
                low16 = _mm256_set1_epi32(0xFFFF),
                bias = _mm256_set1_epi32((int) dec_chunks * DEC_BIAS);
  const u32 chunks = dec_chunks;
  u64 X = 0, Y = 0, f, r;
  u32 i, c;
  sz j = 0;
  for (i = m - 1; i > 0; i--) {
    c = p[-(ptrdiff_t) i];
    X = (X << 2) | c;  Y = (Y >> 2) | ((u64) (3 - c) << 62);
  }
  for (; j + 8 <= count; j += 8) {
    u64 P = _pext_u64(load64(p + j), UINT64_C(0x0303030303030303)), Q = P;
    __m256i bx, by, s, sum, wa, wb, t0, t1, tier, x;
    Q = ((Q & 0x3333) << 2) | ((Q >> 2) & 0x3333);
    Q = ((Q & 0x0F0F) << 4) | ((Q >> 4) & 0x0F0F);
    Q = ((Q & 0x00FF) << 8) | ((Q >> 8) & 0x00FF);
    X = (X << 16) | Q;  Y = (Y >> 16) | ((P ^ 0xFFFF) << 48);
    bx = _mm256_set1_epi64x((long long) X);
    by = _mm256_set1_epi64x((long long) Y);
    s = _mm256_min_epu32(
      pack32(_mm256_and_si256(_mm256_srlv_epi64(bx, sxl), vmask),
             _mm256_and_si256(_mm256_srlv_epi64(bx, sxh), vmask), lows),
      pack32(_mm256_and_si256(_mm256_srlv_epi64(by, syl), vmask),
             _mm256_and_si256(_mm256_srlv_epi64(by, syh), vmask), lows));
    sum = _mm256_setzero_si256();
    Fi(chunks,
      __m256i v = _mm256_and_si256(
        _mm256_srl_epi32(s, _mm_cvtsi32_si128((int) (8 * i))), byte);
      sum = _mm256_add_epi32(sum,
        _mm256_i32gather_epi32((const int *) dec_pk[i], v, 4)));
    wa = _mm256_sub_epi32(_mm256_and_si256(sum, low16), bias);
    wb = _mm256_sub_epi32(_mm256_srli_epi32(sum, 16), bias);
    t0 = _mm256_andnot_si256(_mm256_cmpgt_epi32(wb, eps),
                             _mm256_cmpgt_epi32(wa, eps));
    t1 = _mm256_andnot_si256(_mm256_cmpgt_epi32(neps, wb),
                             _mm256_cmpgt_epi32(neps, wa));
    tier = _mm256_andnot_si256(t1, _mm256_andnot_si256(t0,
                                     _mm256_set1_epi32(INT32_MIN)));
    tier = _mm256_or_si256(tier,
                           _mm256_and_si256(t1, _mm256_set1_epi32(1 << 30)));
    x = s;
    x = _mm256_xor_si256(x, _mm256_srli_epi32(x, 16));
    x = _mm256_mullo_epi32(x, c1);
    x = _mm256_xor_si256(x, _mm256_srli_epi32(x, 15));
    x = _mm256_mullo_epi32(x, c2);
    x = _mm256_xor_si256(x, _mm256_srli_epi32(x, 16));
    _mm256_storeu_si256((__m256i *) (void *) (out + j),
                        _mm256_or_si256(_mm256_srli_epi32(x, 2), tier));
  }
  for (; j < count; j++) {
    c = p[j];
    X = (X << 2) | c;  Y = (Y >> 2) | ((u64) (3 - c) << 62);
    f = X & mask;  r = (Y >> (64 - 2 * m)) & mask;
    out[j] = seed_order((u32) (f < r ? f : r));
  }
}

static INLINE __m256i min8(const u32 * x, const u32 * y) {
  return _mm256_min_epu32(_mm256_loadu_si256((const __m256i *) (const void *) x),
                          _mm256_loadu_si256((const __m256i *) (const void *) y));
}

void scan_minima_avx2(const u32 * r, sz len, u32 w, u32 * a, u32 * out) {
  sz valid = len, n, j, d;
  u32 span = 1;
  if (len < w) return;
  memcpy(a, r, len * 4);
  while (2 * span <= w) {
    n = valid - span;
    for (j = 0; j + 8 <= n; j += 8)
      _mm256_storeu_si256((__m256i *) (void *) (a + j), min8(a + j, a + j + span));
    for (; j < n; j++) a[j] = a[j] < a[j + span] ? a[j] : a[j + span];
    valid = n;  span *= 2;
  }
  n = len - w + 1;  d = w - span;
  for (j = 0; j + 8 <= n; j += 8)
    _mm256_storeu_si256((__m256i *) (void *) (out + j), min8(a + j, a + j + d));
  for (; j < n; j++) out[j] = a[j] < a[j + d] ? a[j] : a[j + d];
}
