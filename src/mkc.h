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

#ifndef MKC_QUERY_H
#define MKC_QUERY_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#endif
#endif
#if (defined(__x86_64__) || defined(__i386__)) \
    && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#endif

enum { MKC_HASH = 1, MKC_DENSE = 2 };

#if defined(__GNUC__) || defined(__clang__)
#define MKC_HOT static inline __attribute__((always_inline))
#else
#define MKC_HOT static inline
#endif

typedef struct mkc_index {
  const unsigned char * map;
  size_t size;
  void * copy;  size_t copy_length;
  unsigned layout, k, hbits;
  uint64_t distinct, total;
  unsigned gbits, sbits, qbits, tagbits, bits, slots, flags;
  unsigned packed, lbits;
  unsigned char mp_win[8], mp_shuf[4][32];
  uint64_t mp_shift[4][4], mp_lmask, mp_tmask;
  unsigned fbase, fspan;
  uint64_t cap, recip, qmask, groups, over, stash, table_end, stash_off;
  uint64_t seed;
  unsigned (*probe)(const unsigned char *, uint64_t, unsigned);
  uint64_t (*count)(const struct mkc_index *, uint64_t);
  void (*codes)(const uint64_t *, uint64_t *, size_t, int, uint64_t);
  void (*block)(const struct mkc_index *, const uint64_t *, uint64_t *,
                size_t);
  unsigned width;
  uint64_t hcap, hash_off;
  unsigned inline_max;
  const unsigned char * direct_base, * direct_counts;
} mkc_index;

MKC_HOT uint64_t mkc_load64(const void * p) {
  const unsigned char * s = (const unsigned char *) p;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  uint64_t v;
  memcpy(&v, s, 8);  return v;
#else
  return (uint64_t) s[0] | (uint64_t) s[1] << 8 | (uint64_t) s[2] << 16
         | (uint64_t) s[3] << 24 | (uint64_t) s[4] << 32
         | (uint64_t) s[5] << 40 | (uint64_t) s[6] << 48
         | (uint64_t) s[7] << 56;
#endif
}

MKC_HOT uint64_t mkc_bits(unsigned n) {
  return n >= 64 ? UINT64_MAX : (UINT64_C(1) << n) - 1;
}

MKC_HOT uint64_t mkc_mask(unsigned k) { return mkc_bits(2 * k); }

MKC_HOT uint64_t mkc_loadn(const unsigned char * p, unsigned n) {
  return mkc_load64(p) & mkc_bits(8 * n);
}

static inline int mkc_base(unsigned char c) {
  switch (c) {
  case 'A': case 'a': return 0;
  case 'C': case 'c': return 1;
  case 'G': case 'g': return 2;
  case 'T': case 't': return 3;
  default: return -1;
  }
}

MKC_HOT uint64_t mkc_mix(uint64_t x) {
  x ^= x >> 30;  x *= UINT64_C(0xBF58476D1CE4E5B9);
  x ^= x >> 27;  x *= UINT64_C(0x94D049BB133111EB);
  return x ^ (x >> 31);
}

MKC_HOT uint64_t mkc_revcomp(uint64_t x, unsigned k) {
  x = ~x;
  x = ((x >> 2) & UINT64_C(0x3333333333333333))
      | ((x & UINT64_C(0x3333333333333333)) << 2);
  x = ((x >> 4) & UINT64_C(0x0F0F0F0F0F0F0F0F))
      | ((x & UINT64_C(0x0F0F0F0F0F0F0F0F)) << 4);
#if defined(__GNUC__) || defined(__clang__)
  x = __builtin_bswap64(x);
#else
  x = ((x >> 8) & UINT64_C(0x00FF00FF00FF00FF))
      | ((x & UINT64_C(0x00FF00FF00FF00FF)) << 8);
  x = ((x >> 16) & UINT64_C(0x0000FFFF0000FFFF))
      | ((x & UINT64_C(0x0000FFFF0000FFFF)) << 16);
  x = (x >> 32) | (x << 32);
#endif
  return x >> (64 - 2 * k);
}

MKC_HOT unsigned mkc_code_bits(unsigned k) {
  return k & 1 ? 2 * k - 1 : 2 * k;
}

MKC_HOT uint64_t mkc_code(uint64_t key, uint64_t rc, unsigned k) {
  uint64_t oriented, low;
  if (!(k & 1)) return key < rc ? key : rc;
  oriented = (key >> k) & 1 ? rc : key;  low = (UINT64_C(1) << k) - 1;
  return (oriented & low) | ((oriented >> (k + 1)) << k);
}

static inline uint64_t mkc_uncode(uint64_t code, unsigned k) {
  uint64_t low, key, rc;
  if (!(k & 1)) return code;
  low = (UINT64_C(1) << k) - 1;  key = (code & low) | ((code >> k) << (k + 1));
  rc = mkc_revcomp(key, k);
  return key < rc ? key : rc;
}

MKC_HOT uint64_t mkc_hash(uint64_t x, unsigned bits) {
  uint64_t mask = mkc_bits(bits);
  x = (x ^ UINT64_C(0x8A5CD789635D2DFF)) & mask;
  x ^= x >> 30;  x = (x * UINT64_C(0xBF58476D1CE4E5B9)) & mask;
  x ^= x >> 27;  x = (x * UINT64_C(0x94D049BB133111EB)) & mask;
  return x ^ (x >> 31);
}

static inline uint64_t mkc_seed_word(unsigned seed, unsigned bits) {
  return (seed * UINT64_C(0x9E3779B97F4A7C15)) & mkc_bits(bits);
}

static inline uint64_t mkc_unhash(uint64_t x, unsigned bits) {
  uint64_t mask = mkc_bits(bits);
  x ^= (x >> 31) ^ (x >> 62);  x = (x * UINT64_C(0x319642B2D24D8EC3)) & mask;
  x ^= (x >> 27) ^ (x >> 54);  x = (x * UINT64_C(0x96DE1B173F119089)) & mask;
  x ^= (x >> 30) ^ (x >> 60);
  return (x ^ UINT64_C(0x8A5CD789635D2DFF)) & mask;
}

static uint64_t mkc_exception_probe(const mkc_index * db, uint64_t id,
                                    uint64_t pos) {
  uint64_t i;
  for (i = 0; i < db->hcap; i++) {
    const unsigned char * p = db->map + db->hash_off + 16 * pos;
    uint64_t count = mkc_load64(p + 8);
    if (!count) return 0;
    if (mkc_load64(p) == id) return count;
    pos = (pos + 1) & (db->hcap - 1);
  }
  return 0;
}

MKC_HOT uint64_t mkc_exception(const mkc_index * db, uint64_t id) {
  uint64_t pos;
  const unsigned char * p;
  if (!db->hcap) return 0;
  pos = mkc_mix(id) & (db->hcap - 1);
  p = db->map + db->hash_off + 16 * pos;
  if (mkc_load64(p) == id) return mkc_load64(p + 8);
  return mkc_exception_probe(db, id, pos);
}

#define MG_MAX_SLOTS 12

MKC_HOT unsigned mg_byte(unsigned j, unsigned bits) {
  unsigned b = j * bits >> 3;
  return b < 56 ? b : 56;
}

MKC_HOT uint64_t mg_get(const unsigned char * bucket, unsigned j,
                        unsigned bits) {
  unsigned b = mg_byte(j, bits);
  return mkc_load64(bucket + b) >> (j * bits - 8 * b) & mkc_bits(bits);
}

MKC_HOT uint64_t mg_alt(uint64_t s, unsigned sbits, unsigned choice) {
  return choice ? mkc_hash(s, sbits) : s;
}

MKC_HOT uint64_t mg_div(const mkc_index * d, uint64_t v) {
#ifdef __SIZEOF_INT128__
  __extension__ typedef unsigned __int128 mkc_u128;
  if (d->recip) {
    uint64_t q = (uint64_t) (((mkc_u128) v * d->recip) >> 64);
    return q - (q * d->cap > v);
  }
#endif
  return v / d->cap;
}

#define MG_SLOT(W, J) \
  if ((J) < 512 / (W)) \
    m |= (unsigned) !(((mkc_load64(p + mg_byte(J, W)) \
                        >> ((J) * (W) - 8 * mg_byte(J, W))) ^ wanted) \
                      & qmask) << (J);
#define MG_PROBE(W) \
  static inline unsigned mg_probe##W(const unsigned char * p, \
                                     uint64_t wanted, unsigned shift) { \
    const uint64_t qmask = mkc_bits(shift); \
    unsigned m = 0; \
    MG_SLOT(W, 0) MG_SLOT(W, 1) MG_SLOT(W, 2) MG_SLOT(W, 3) MG_SLOT(W, 4) \
    MG_SLOT(W, 5) MG_SLOT(W, 6) MG_SLOT(W, 7) MG_SLOT(W, 8) MG_SLOT(W, 9) \
    MG_SLOT(W, 10) MG_SLOT(W, 11) MG_SLOT(W, 12) MG_SLOT(W, 13) \
    return m ? (unsigned) (mg_get(p, (unsigned) __builtin_ctz(m), W) \
                           >> shift) \
             : 0; \
  }
MG_PROBE(36) MG_PROBE(39) MG_PROBE(40) MG_PROBE(42) MG_PROBE(46)
MG_PROBE(48) MG_PROBE(51) MG_PROBE(56) MG_PROBE(64)
#undef MG_PROBE
#undef MG_SLOT

typedef unsigned (*mg_probe_fn)(const unsigned char *, uint64_t, unsigned);

static inline mg_probe_fn mg_probe_for(unsigned bits) {
  switch (bits) {
  case 36: return mg_probe36;
  case 39: return mg_probe39;
  case 40: return mg_probe40;
  case 42: return mg_probe42;
  case 46: return mg_probe46;
  case 48: return mg_probe48;
  case 51: return mg_probe51;
  case 56: return mg_probe56;
  case 64: return mg_probe64;
  default: return NULL;
  }
}

MKC_HOT const unsigned char * mg_bucket(const mkc_index * d, uint64_t h,
                                        unsigned choice, uint64_t * wanted) {
  uint64_t v = mg_alt(h & mkc_bits(d->sbits), d->sbits, choice),
           q = mg_div(d, v);
  *wanted = q | ((uint64_t) choice << d->qbits);
  return d->map + 128 + 64 * (d->cap * (h >> d->sbits) + (v - q * d->cap));
}

static inline unsigned mg_filter_bit(const mkc_index * d, uint64_t quotient) {
  return d->fbase
         + (unsigned) (((quotient * UINT64_C(0x9E3779B97F4A7C15) >> 32)
                        * d->fspan) >> 32);
}

MKC_HOT int mg_overflowed(const mkc_index * d, const unsigned char * bucket,
                          uint64_t wanted) {
  return (int) (mkc_load64(bucket + 56) >> mg_filter_bit(d, wanted) & 1);
}

MKC_HOT int mg_spilled(const mkc_index * d, const unsigned char * p,
                       uint64_t wanted) {
  return d->flags ? mg_overflowed(d, p, wanted)
                  : (int) (mg_get(p, d->slots - 1, d->bits) >> (d->qbits + 1)
                           != 0);
}

static inline const unsigned char * mg_tag_where(const mkc_index * d,
                                                 uint64_t h, unsigned tag) {
  if (tag < (1u << d->tagbits) - 1)
    return d->direct_counts
           + 8 * (mkc_load64(d->direct_base + 8 * (h >> d->sbits)) + tag
                  - d->inline_max - 1);
  return d->map + d->hash_off + 16 * (mkc_mix(h) & (d->hcap - 1));
}

MKC_HOT uint64_t mg_tag_count(const mkc_index * d, uint64_t h, unsigned tag) {
  uint64_t count;
  if (tag <= d->inline_max) return tag;
  if (tag < (1u << d->tagbits) - 1)
    return mkc_load64(d->direct_counts
                      + 8 * (mkc_load64(d->direct_base + 8 * (h >> d->sbits))
                             + tag - d->inline_max - 1));
  count = mkc_exception(d, h);
  return count ? count : tag;
}

MKC_HOT uint64_t mg_resolve(const mkc_index * d, uint64_t h, unsigned tag) {
  if (tag) return mg_tag_count(d, h, tag);
  return d->stash ? mkc_exception(d, h) : 0;
}

MKC_HOT uint64_t mp_bits(const unsigned char * p, unsigned at, unsigned w) {
  unsigned b = at >> 3;
  b = b < 56 ? b : 56;
  return mkc_load64(p + b) >> (at - 8 * b) & mkc_bits(w);
}

MKC_HOT unsigned mp_select0(uint64_t u, unsigned r) {
  uint64_t z = ~u;
  while (r--) z &= z - 1;
  return (unsigned) __builtin_ctzll(z);
}

#define MP_HEADER 6u

MKC_HOT unsigned mp_base(const mkc_index * d) {
  return MP_HEADER + d->slots + 16;
}

MKC_HOT void mp_filter(const mkc_index * d, unsigned n, unsigned e,
                       unsigned * lo, unsigned * hi) {
  *lo = mp_base(d) + n * d->lbits;  *hi = 512 - e * (4 + d->tagbits);
}

MKC_HOT unsigned mp_tag(const mkc_index * d, const unsigned char * p,
                        unsigned n, unsigned e, unsigned i) {
  unsigned x;
  (void) n;
  for (x = 0; x < e; x++)
    if (mp_bits(p, 508 - 4 * x, 4) == i)
      return (unsigned) mp_bits(p, 512 - 4 * e - (x + 1) * d->tagbits,
                                d->tagbits);
  return 1;
}

MKC_HOT void mp_run(uint64_t upper, unsigned hi, unsigned * first,
                    unsigned * run) {
  const unsigned start = hi ? mp_select0(upper, hi - 1) + 1 : 0;
  *first = start - hi;  *run = (unsigned) __builtin_ctzll(~(upper >> start));
}

MKC_HOT unsigned mp_find(const mkc_index * d, const unsigned char * p,
                         uint64_t v) {
  const uint64_t w = mkc_load64(p);
  const unsigned n = (unsigned) (w & 15), e = (unsigned) (w >> 4) & 3,
                 L = d->lbits;
  const uint64_t upper = w >> MP_HEADER & mkc_bits(n + 16),
                 low = v & mkc_bits(L);
  unsigned i, run, at;
  mp_run(upper, (unsigned) (v >> L), &i, &run);
  for (at = mp_base(d) + i * L; run; run--, i++, at += L)
    if (mp_bits(p, at, L) == low) return mp_tag(d, p, n, e, i);
  return 0;
}

MKC_HOT unsigned mp_filter_bit(uint64_t quotient, unsigned lo, unsigned hi) {
  return lo + (unsigned) (((quotient * UINT64_C(0x9E3779B97F4A7C15) >> 32)
                           * (hi - lo)) >> 32);
}

MKC_HOT int mp_spilled(const mkc_index * d, const unsigned char * p,
                       uint64_t quotient) {
  const uint64_t w = mkc_load64(p);
  unsigned lo, hi;
  mp_filter(d, (unsigned) (w & 15), (unsigned) (w >> 4) & 3, &lo, &hi);
  if (lo >= hi) return 1;
  return (int) mp_bits(p, mp_filter_bit(quotient, lo, hi), 1);
}

static inline uint64_t mp_count(const mkc_index * d, uint64_t h) {
  uint64_t wanted;
  const unsigned char * p = mg_bucket(d, h, 0, &wanted);
  unsigned tag = mp_find(d, p, wanted);
  if (!tag && mp_spilled(d, p, wanted)) {
    p = mg_bucket(d, h, 1, &wanted);  tag = mp_find(d, p, wanted);
  }
  return mg_resolve(d, h, tag);
}

static inline uint64_t mg_count(const mkc_index * d, uint64_t h) {
  unsigned shift, tag;
  uint64_t wanted;
  const unsigned char * p;
  if (d->packed) return mp_count(d, h);
  shift = d->qbits + 1;
  p = mg_bucket(d, h, 0, &wanted);
  tag = d->probe(p, wanted, shift);
  if (!tag) {
    if (d->flags && !mg_overflowed(d, p, wanted)) return 0;
    if (mg_spilled(d, p, wanted)) {
      p = mg_bucket(d, h, 1, &wanted);  tag = d->probe(p, wanted, shift);
    }
  }
  return mg_resolve(d, h, tag);
}

static inline uint64_t mkc_dense_end(unsigned width, unsigned hbits) {
  return ((128 + ((uint64_t) width << hbits)) / 64 + 1) * 64;
}

MKC_HOT uint64_t md_count(const mkc_index * d, uint64_t code) {
  uint64_t v = mkc_loadn(d->map + 128 + (size_t) d->width * code, d->width),
           count;
  if (d->width == 8 || v != mkc_bits(8 * d->width)) return v;
  count = mkc_exception(d, code);
  return count ? count : v;
}

static inline uint64_t mkc_generic_hash(const mkc_index * db, uint64_t key) {
  key &= mkc_mask(db->k);
  return mg_count(db, mkc_hash(mkc_code(key, mkc_revcomp(key, db->k), db->k)
                               ^ db->seed, db->hbits));
}

static inline uint64_t mkc_generic_dense(const mkc_index * db, uint64_t key) {
  key &= mkc_mask(db->k);
  return md_count(db, mkc_code(key, mkc_revcomp(key, db->k), db->k));
}

static inline int mkc_cpu_avx2(int bmi) {
#if (defined(__x86_64__) || defined(__i386__)) \
    && (defined(__GNUC__) || defined(__clang__))
  unsigned a, b, c, d, lo, hi;
  if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 27))
      || !(c & (1u << 28)))
    return 0;
  __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  (void) hi;
  if ((lo & 6) != 6 || __get_cpuid_max(0, NULL) < 7) return 0;
  __cpuid_count(7, 0, a, b, c, d);
  return (b & (1u << 5)) && (!bmi || ((b & (1u << 3)) && (b & (1u << 8))));
#else
  (void) bmi;  return 0;
#endif
}

#if (defined(__x86_64__) || defined(__i386__)) \
    && (defined(__GNUC__) || defined(__clang__)) && !defined(MKC_NO_KERNELS)
#define MKC_X86 1
#include <immintrin.h>
#define MG_WIN(W, a) (mg_byte(a, W) < 48 ? mg_byte(a, W) : 48)
#define MG_LANE(W, a) \
  (char) (mg_byte(a, W) - MG_WIN(W, a) + 0), \
  (char) (mg_byte(a, W) - MG_WIN(W, a) + 1), \
  (char) (mg_byte(a, W) - MG_WIN(W, a) + 2), \
  (char) (mg_byte(a, W) - MG_WIN(W, a) + 3), \
  (char) (mg_byte(a, W) - MG_WIN(W, a) + 4), \
  (char) (mg_byte(a, W) - MG_WIN(W, a) + 5), \
  (char) (mg_byte(a, W) - MG_WIN(W, a) + 6), \
  (char) (mg_byte(a, W) - MG_WIN(W, a) + 7), \
  (char) (mg_byte((a) + 1, W) - MG_WIN(W, a) + 0), \
  (char) (mg_byte((a) + 1, W) - MG_WIN(W, a) + 1), \
  (char) (mg_byte((a) + 1, W) - MG_WIN(W, a) + 2), \
  (char) (mg_byte((a) + 1, W) - MG_WIN(W, a) + 3), \
  (char) (mg_byte((a) + 1, W) - MG_WIN(W, a) + 4), \
  (char) (mg_byte((a) + 1, W) - MG_WIN(W, a) + 5), \
  (char) (mg_byte((a) + 1, W) - MG_WIN(W, a) + 6), \
  (char) (mg_byte((a) + 1, W) - MG_WIN(W, a) + 7)
#define MG_SHIFT(W, a) (long long) ((a) * (W) - 8 * mg_byte(a, W))
#define MG_VEC(W, v) \
  m |= (unsigned) _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64( \
         _mm256_and_si256(_mm256_xor_si256(_mm256_srlv_epi64( \
           _mm256_shuffle_epi8(_mm256_loadu2_m128i( \
               (const __m128i *) (p + MG_WIN(W, 4 * (v) + 2)), \
               (const __m128i *) (p + MG_WIN(W, 4 * (v)))), \
             _mm256_setr_epi8(MG_LANE(W, 4 * (v)), \
                              MG_LANE(W, 4 * (v) + 2))), \
           _mm256_setr_epi64x(MG_SHIFT(W, 4 * (v)), \
                              MG_SHIFT(W, 4 * (v) + 1), \
                              MG_SHIFT(W, 4 * (v) + 2), \
                              MG_SHIFT(W, 4 * (v) + 3))), \
         want), qmask), zero))) << 4 * (v);
#define MG_MATCH(W) \
  __attribute__((target("avx2,bmi,bmi2"), always_inline)) \
  static inline unsigned mg_match##W(const unsigned char * p, \
                                     uint64_t wanted, __m256i qmask) { \
    const __m256i want = _mm256_set1_epi64x((long long) wanted), \
                  zero = _mm256_setzero_si256(); \
    unsigned m = 0; \
    MG_VEC(W, 0) MG_VEC(W, 1) if (512 / (W) > 8) { MG_VEC(W, 2) } \
    if (512 / (W) > 12) { \
      const uint64_t q = (uint64_t) _mm256_extract_epi64(qmask, 0); \
      m |= (unsigned) !(((mkc_load64(p + mg_byte(12, W)) \
                          >> (12 * (W) - 8 * mg_byte(12, W))) ^ wanted) \
                        & q) << 12; \
      if (512 / (W) > 13) \
        m |= (unsigned) !(((mkc_load64(p + mg_byte(13, W)) \
                            >> (13 * (W) - 8 * mg_byte(13, W))) ^ wanted) \
                          & q) << 13; \
    } \
    return m & ((1u << 512 / (W)) - 1); \
  }
MG_MATCH(36) MG_MATCH(39) MG_MATCH(42) MG_MATCH(46) MG_MATCH(51)
MG_MATCH(56) MG_MATCH(64)
#define MG_SLOW(W) \
  __attribute__((target("avx2,bmi,bmi2"), noinline, cold)) \
  static uint64_t mg_slow##W(const mkc_index * d, uint64_t h) { \
    const unsigned shift = d->qbits + 1; \
    const __m256i qmask = _mm256_set1_epi64x((long long) d->qmask); \
    uint64_t wanted; \
    const unsigned char * p = mg_bucket(d, h, 0, &wanted); \
    unsigned m = mg_match##W(p, wanted, qmask); \
    if (!m) { \
      if (d->flags && !mg_overflowed(d, p, wanted)) return 0; \
      if (mg_spilled(d, p, wanted)) { \
        p = mg_bucket(d, h, 1, &wanted); \
        m = mg_match##W(p, wanted, qmask); \
      } \
    } \
    return mg_resolve(d, h, m ? (unsigned) (mg_get(p, \
                                              (unsigned) __builtin_ctz(m), \
                                              W) >> shift) \
                              : 0); \
  }
MG_SLOW(36) MG_SLOW(39) MG_SLOW(42) MG_SLOW(46) MG_SLOW(51) MG_SLOW(56)
MG_SLOW(64)
#define MKC_HASH_KERNEL(K, W) \
  __attribute__((target("avx2,bmi,bmi2"))) \
  static uint64_t mkc_kernel_##K##_##W(const mkc_index * d, uint64_t key) { \
    uint64_t h, wanted; \
    const unsigned char * p; \
    unsigned m; \
    key &= mkc_mask(K); \
    h = mkc_hash(mkc_code(key, mkc_revcomp(key, K), K) ^ d->seed, \
                 mkc_code_bits(K)); \
    p = mg_bucket(d, h, 0, &wanted); \
    m = mg_match##W(p, wanted, _mm256_set1_epi64x((long long) d->qmask)); \
    if (__builtin_expect(m != 0, 1)) { \
      unsigned tag = (unsigned) (mg_get(p, (unsigned) __builtin_ctz(m), W) \
                                 >> (d->qbits + 1)); \
      if (__builtin_expect(tag <= d->inline_max, 1)) return tag; \
      return mg_tag_count(d, h, tag); \
    } else if (d->flags && !mg_overflowed(d, p, wanted)) return 0; \
    return mg_slow##W(d, h); \
  }
#define MKC_BLOCK 64
#define MG_BLOCK(W) \
  __attribute__((target("avx2,bmi,bmi2"))) \
  static void mg_block##W(const mkc_index * d, const uint64_t * h, \
                          uint64_t * out, size_t n) { \
    const unsigned char * p[MKC_BLOCK]; \
    uint64_t wanted[MKC_BLOCK]; \
    const __m256i qmask = _mm256_set1_epi64x((long long) d->qmask); \
    const unsigned shift = d->qbits + 1; \
    unsigned char late[MKC_BLOCK]; \
    unsigned tags[MKC_BLOCK]; \
    size_t nlate = 0, i, j; \
    for (i = 0; i < n; i++) { \
      p[i] = mg_bucket(d, h[i], 0, &wanted[i]); \
      __builtin_prefetch(p[i], 0, 0); \
    } \
    for (i = 0; i < n; i++) { \
      unsigned m = mg_match##W(p[i], wanted[i], qmask); \
      uint64_t unused; \
      if (m) { \
        unsigned tag = (unsigned) (mg_get(p[i], (unsigned) __builtin_ctz(m), \
                                          W) >> shift); \
        if (__builtin_expect(tag <= d->inline_max, 1)) { \
          out[i] = tag;  continue; \
        } \
        __builtin_prefetch(mg_tag_where(d, h[i], tag), 0, 0); \
        late[nlate] = (unsigned char) i;  tags[nlate++] = tag;  continue; \
      } else if (d->flags && !mg_overflowed(d, p[i], wanted[i])) { \
        out[i] = 0;  continue; \
      } \
      __builtin_prefetch(mg_bucket(d, h[i], 1, &unused), 0, 0); \
      late[nlate] = (unsigned char) i;  tags[nlate++] = 0; \
    } \
    for (j = 0; j < nlate; j++) { \
      i = late[j]; \
      out[i] = tags[j] ? mg_tag_count(d, h[i], tags[j]) \
                       : mg_slow##W(d, h[i]); \
    } \
  }
MG_BLOCK(36) MG_BLOCK(39) MG_BLOCK(42) MG_BLOCK(46) MG_BLOCK(51)
MG_BLOCK(56) MG_BLOCK(64)

__attribute__((target("avx2,bmi,bmi2")))
static void md_block(const mkc_index * d, const uint64_t * code,
                     uint64_t * out, size_t n) {
  size_t i;
  for (i = 0; i < n; i++)
    __builtin_prefetch(d->map + 128 + (size_t) d->width * code[i], 0, 0);
  for (i = 0; i < n; i++) out[i] = md_count(d, code[i]);
}

MKC_HOT uint64_t mp_field(const unsigned char * p, unsigned at,
                          uint64_t mask) {
  unsigned b = at >> 3;
  b = b < 56 ? b : 56;
  return mkc_load64(p + b) >> (at - 8 * b) & mask;
}

__attribute__((target("avx2,bmi,bmi2"), always_inline))
static inline unsigned mp_finish(const mkc_index * d, const unsigned char * p,
                                 uint64_t v, unsigned m, const unsigned L,
                                 const unsigned S) {
  const unsigned hdr = MP_HEADER;
  uint64_t w, upper, tail;
  unsigned hi, j, t, e, x, zero, entry;
  if (!m) return 0;
  w = mkc_load64(p);  upper = _bzhi_u64(w >> hdr, S + 16);
  hi = (unsigned) (v >> L);
  j = (unsigned) __builtin_ctz(m);
  while (__builtin_expect((unsigned) _tzcnt_u64(_pdep_u64(UINT64_C(1) << j,
                                                          upper)) - j != hi,
                          0)) {
    m &= m - 1;
    if (!m) return 0;
    j = (unsigned) __builtin_ctz(m);
  }
  t = d->tagbits;
  e = (unsigned) (w >> 4) & 3;
  tail = mkc_load64(p + 56);
  x = ((unsigned) (tail >> 52) ^ (j * 0x111u)) | ((0xFFFu >> 4 * e) & 0x888u);
  zero = (x - 0x111u) & ~x & 0x888u;
  if (__builtin_expect(!zero, 1)) return 1;
  entry = 2 - ((unsigned) __builtin_ctz(zero) >> 2);
  return (unsigned) (tail >> (64 - 4 * e - (entry + 1) * t) & d->mp_tmask);
}

__attribute__((target("avx2,bmi,bmi2"), always_inline))
static inline unsigned mp_find_fast(const mkc_index * d,
                                    const unsigned char * p, uint64_t v) {
  const unsigned L = d->lbits;
  const __m256i low = _mm256_set1_epi64x((long long) (v & d->mp_lmask)),
                mask = _mm256_set1_epi64x((long long) d->mp_lmask);
  unsigned m = 0, x;
  for (x = 0; x < 4; x++) {
    __m256i y = _mm256_loadu2_m128i(
        (const __m128i *) (p + d->mp_win[2 * x + 1]),
        (const __m128i *) (p + d->mp_win[2 * x]));
    y = _mm256_shuffle_epi8(
        y, _mm256_loadu_si256((const __m256i *) d->mp_shuf[x]));
    y = _mm256_and_si256(
        _mm256_srlv_epi64(
            y, _mm256_loadu_si256((const __m256i *) d->mp_shift[x])),
        mask);
    m |= (unsigned) _mm256_movemask_pd(_mm256_castsi256_pd(
             _mm256_cmpeq_epi64(y, low))) << 4 * x;
  }
  return mp_finish(d, p, v, m, L, d->slots);
}

__attribute__((target("avx2,bmi,bmi2"), noinline, cold))
static uint64_t mp_slow(const mkc_index * d, uint64_t h) {
  uint64_t wanted;
  const unsigned char * p = mg_bucket(d, h, 1, &wanted);
  return mg_resolve(d, h, mp_find_fast(d, p, wanted));
}

__attribute__((target("avx2,bmi,bmi2"), always_inline))
static inline uint64_t mp_count_fast(const mkc_index * d, uint64_t h) {
  uint64_t wanted;
  const unsigned char * p = mg_bucket(d, h, 0, &wanted);
  const unsigned tag = mp_find_fast(d, p, wanted);
  if (__builtin_expect(tag != 0, 1))
    return __builtin_expect(tag <= d->inline_max, 1)
           ? tag : mg_tag_count(d, h, tag);
  if (!mp_spilled(d, p, wanted)) return 0;
  return mp_slow(d, h);
}

#define MP_S(L) (484 / ((L) + 1) < 15 ? 484 / ((L) + 1) : 15)
#define MP_AT(L, j) \
  (6 + MP_S(L) + 16 + (j) * (L) > 512 - (L) \
   ? 512 - (L) : 6 + MP_S(L) + 16 + (j) * (L))
#define MP_WIN0(L, pr) \
  (6 + MP_S(L) + 16 + 2 * (pr) * (L) > 512 - (L) \
   ? (512 - (L)) / 8 : (6 + MP_S(L) + 16 + 2 * (pr) * (L)) / 8)
#define MP_WIN(L, pr) (MP_WIN0(L, pr) > 48 ? 48 : MP_WIN0(L, pr))
#define MP_BYTE0(L, j) (MP_AT(L, j) / 8 > 56 ? 56 : MP_AT(L, j) / 8)
#define MP_BYTE(L, j) \
  (MP_BYTE0(L, j) > MP_WIN(L, (j) / 2) + 8 \
   ? MP_WIN(L, (j) / 2) + 8 : MP_BYTE0(L, j))
#define MP_SJ(v, b) (4 * (v) + 2 * ((b) >> 4) + (((b) >> 3) & 1))
#define MP_SB(L, v, b) \
  (char) (MP_BYTE(L, MP_SJ(v, b)) - MP_WIN(L, MP_SJ(v, b) / 2) + ((b) & 7))
#define MP_SB8(L, v, b) \
  MP_SB(L, v, b), MP_SB(L, v, (b) + 1), MP_SB(L, v, (b) + 2), \
  MP_SB(L, v, (b) + 3), MP_SB(L, v, (b) + 4), MP_SB(L, v, (b) + 5), \
  MP_SB(L, v, (b) + 6), MP_SB(L, v, (b) + 7)
#define MP_SHIFT(L, j) (long long) (MP_AT(L, j) - 8 * MP_BYTE(L, j))
#define MP_VEC(L, v) \
  m |= (unsigned) _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64( \
         _mm256_and_si256(_mm256_srlv_epi64(_mm256_shuffle_epi8( \
             _mm256_loadu2_m128i( \
               (const __m128i *) (p + MP_WIN(L, 2 * (v) + 1)), \
               (const __m128i *) (p + MP_WIN(L, 2 * (v)))), \
             _mm256_setr_epi8(MP_SB8(L, v, 0), MP_SB8(L, v, 8), \
                              MP_SB8(L, v, 16), MP_SB8(L, v, 24))), \
           _mm256_setr_epi64x(MP_SHIFT(L, 4 * (v)), \
                              MP_SHIFT(L, 4 * (v) + 1), \
                              MP_SHIFT(L, 4 * (v) + 2), \
                              MP_SHIFT(L, 4 * (v) + 3))), mask), \
         low))) << 4 * (v);
#define MP_FIXED(L) \
  __attribute__((target("avx2,bmi,bmi2"), always_inline)) \
  static inline unsigned mp_find_##L(const mkc_index * d, \
                                     const unsigned char * p, uint64_t v) { \
    const __m256i low = _mm256_set1_epi64x((long long) (v & mkc_bits(L))), \
                  mask = _mm256_set1_epi64x((long long) mkc_bits(L)); \
    unsigned m = 0; \
    MP_VEC(L, 0) MP_VEC(L, 1) MP_VEC(L, 2) MP_VEC(L, 3) \
    return mp_finish(d, p, v, m, L, MP_S(L)); \
  } \
  __attribute__((target("avx2,bmi,bmi2"))) \
  static uint64_t mp_kernel_l##L(const mkc_index * d, uint64_t key) { \
    const unsigned k = d->k; \
    uint64_t h, wanted; \
    const unsigned char * p; \
    unsigned tag; \
    key &= mkc_mask(k); \
    h = mkc_hash(mkc_code(key, mkc_revcomp(key, k), k) ^ d->seed, d->hbits); \
    p = mg_bucket(d, h, 0, &wanted); \
    tag = mp_find_##L(d, p, wanted); \
    if (__builtin_expect(tag != 0, 1)) \
      return __builtin_expect(tag <= d->inline_max, 1) \
             ? tag : mg_tag_count(d, h, tag); \
    if (!mp_spilled(d, p, wanted)) return 0; \
    return mp_slow(d, h); \
  } \
  __attribute__((target("avx2,bmi,bmi2"))) \
  static void mp_block_l##L(const mkc_index * d, const uint64_t * h, \
                            uint64_t * out, size_t n) { \
    const unsigned char * p[MKC_BLOCK]; \
    uint64_t wanted[MKC_BLOCK]; \
    unsigned char late[MKC_BLOCK]; \
    unsigned tags[MKC_BLOCK]; \
    size_t nlate = 0, i, j; \
    const unsigned inline_max = d->inline_max; \
    for (i = 0; i < n; i++) { \
      p[i] = mg_bucket(d, h[i], 0, &wanted[i]); \
      __builtin_prefetch(p[i], 0, 0); \
    } \
    for (i = 0; i < n; i++) { \
      const unsigned tag = mp_find_##L(d, p[i], wanted[i]); \
      uint64_t unused; \
      if (tag) { \
        if (__builtin_expect(tag <= inline_max, 1)) { \
          out[i] = tag;  continue; \
        } \
        __builtin_prefetch(mg_tag_where(d, h[i], tag), 0, 0); \
        late[nlate] = (unsigned char) i;  tags[nlate++] = tag;  continue; \
      } \
      if (!mp_spilled(d, p[i], wanted[i])) { out[i] = 0;  continue; } \
      __builtin_prefetch(mg_bucket(d, h[i], 1, &unused), 0, 0); \
      late[nlate] = (unsigned char) i;  tags[nlate++] = 0; \
    } \
    for (j = 0; j < nlate; j++) { \
      i = late[j]; \
      out[i] = tags[j] ? mg_tag_count(d, h[i], tags[j]) : mp_slow(d, h[i]); \
    } \
  }
MP_FIXED(20) MP_FIXED(21) MP_FIXED(22) MP_FIXED(23) MP_FIXED(24)
MP_FIXED(25) MP_FIXED(26) MP_FIXED(27) MP_FIXED(28) MP_FIXED(29)
MP_FIXED(30) MP_FIXED(31) MP_FIXED(32) MP_FIXED(33) MP_FIXED(34)
MP_FIXED(35) MP_FIXED(36) MP_FIXED(37) MP_FIXED(38) MP_FIXED(39)
MP_FIXED(40)
#define MP_FIXED_FIRST 20
#define MP_FIXED_LAST 40
#define MP_KERNEL(K) \
  __attribute__((target("avx2,bmi,bmi2"))) \
  static uint64_t mp_kernel_##K(const mkc_index * d, uint64_t key) { \
    key &= mkc_mask(K); \
    return mp_count_fast(d, mkc_hash(mkc_code(key, mkc_revcomp(key, K), K) \
                                     ^ d->seed, mkc_code_bits(K))); \
  }

__attribute__((target("avx2,bmi,bmi2")))
static void mp_block(const mkc_index * d, const uint64_t * h, uint64_t * out,
                     size_t n) {
  const unsigned char * p[MKC_BLOCK];
  uint64_t wanted[MKC_BLOCK];
  unsigned char late[MKC_BLOCK];
  unsigned tags[MKC_BLOCK];
  size_t nlate = 0, i, j;
  const unsigned inline_max = d->inline_max;
  for (i = 0; i < n; i++) {
    p[i] = mg_bucket(d, h[i], 0, &wanted[i]);
    __builtin_prefetch(p[i], 0, 0);
  }
  for (i = 0; i < n; i++) {
    const unsigned tag = mp_find_fast(d, p[i], wanted[i]);
    uint64_t unused;
    if (tag) {
      if (__builtin_expect(tag <= inline_max, 1)) { out[i] = tag;  continue; }
      __builtin_prefetch(mg_tag_where(d, h[i], tag), 0, 0);
      late[nlate] = (unsigned char) i;  tags[nlate++] = tag;  continue;
    }
    if (!mp_spilled(d, p[i], wanted[i])) { out[i] = 0;  continue; }
    __builtin_prefetch(mg_bucket(d, h[i], 1, &unused), 0, 0);
    late[nlate] = (unsigned char) i;  tags[nlate++] = 0;
  }
  for (j = 0; j < nlate; j++) {
    i = late[j];
    out[i] = tags[j] ? mg_tag_count(d, h[i], tags[j]) : mp_slow(d, h[i]);
  }
}

#define MKC_CODES(K) \
  __attribute__((target("avx2,bmi,bmi2"))) \
  static void mkc_codes_##K(const uint64_t * keys, uint64_t * out, size_t n, \
                            int hashed, uint64_t seed) { \
    size_t i; \
    for (i = 0; i < n; i++) { \
      uint64_t key = keys[i] & mkc_mask(K), \
               code = mkc_code(key, mkc_revcomp(key, K), K); \
      out[i] = hashed ? mkc_hash(code ^ seed, mkc_code_bits(K)) : code; \
    } \
  }
#define MKC_DENSE_KERNEL(K) \
  __attribute__((target("avx2,bmi,bmi2"))) \
  static uint64_t mkc_dense_##K(const mkc_index * d, uint64_t key) { \
    key &= mkc_mask(K); \
    return md_count(d, mkc_code(key, mkc_revcomp(key, K), K)); \
  }
#define MKC_KERNELS(K) \
  MP_KERNEL(K) MKC_HASH_KERNEL(K, 36) MKC_HASH_KERNEL(K, 39) \
  MKC_HASH_KERNEL(K, 42) MKC_HASH_KERNEL(K, 46) MKC_HASH_KERNEL(K, 51) \
  MKC_HASH_KERNEL(K, 56) MKC_HASH_KERNEL(K, 64) MKC_DENSE_KERNEL(K) \
  MKC_CODES(K)
MKC_KERNELS(1) MKC_KERNELS(2) MKC_KERNELS(3) MKC_KERNELS(4) MKC_KERNELS(5)
MKC_KERNELS(6) MKC_KERNELS(7) MKC_KERNELS(8) MKC_KERNELS(9) MKC_KERNELS(10)
MKC_KERNELS(11) MKC_KERNELS(12) MKC_KERNELS(13) MKC_KERNELS(14)
MKC_KERNELS(15) MKC_KERNELS(16) MKC_KERNELS(17) MKC_KERNELS(18)
MKC_KERNELS(19) MKC_KERNELS(20) MKC_KERNELS(21) MKC_KERNELS(22)
MKC_KERNELS(23) MKC_KERNELS(24) MKC_KERNELS(25) MKC_KERNELS(26)
MKC_KERNELS(27) MKC_KERNELS(28) MKC_KERNELS(29) MKC_KERNELS(30)
MKC_KERNELS(31) MKC_KERNELS(32)
#define MKC_KERNEL_ROW(K) \
  { mkc_kernel_##K##_36, mkc_kernel_##K##_39, mkc_kernel_##K##_42, \
    mkc_kernel_##K##_46, mkc_kernel_##K##_51, mkc_kernel_##K##_56, \
    mkc_kernel_##K##_64, mkc_dense_##K },
#define MKC_CODES_ENTRY(K) mkc_codes_##K,
#define MP_KERNEL_ENTRY(K) mp_kernel_##K,

typedef uint64_t (*mkc_count_fn)(const mkc_index *, uint64_t);

static inline mkc_count_fn mkc_kernel_for(mkc_index * d) {
  static void (*const codes[32])(const uint64_t *, uint64_t *, size_t, int,
                                 uint64_t) = {
    MKC_CODES_ENTRY(1) MKC_CODES_ENTRY(2) MKC_CODES_ENTRY(3)
    MKC_CODES_ENTRY(4) MKC_CODES_ENTRY(5) MKC_CODES_ENTRY(6)
    MKC_CODES_ENTRY(7) MKC_CODES_ENTRY(8) MKC_CODES_ENTRY(9)
    MKC_CODES_ENTRY(10) MKC_CODES_ENTRY(11) MKC_CODES_ENTRY(12)
    MKC_CODES_ENTRY(13) MKC_CODES_ENTRY(14) MKC_CODES_ENTRY(15)
    MKC_CODES_ENTRY(16) MKC_CODES_ENTRY(17) MKC_CODES_ENTRY(18)
    MKC_CODES_ENTRY(19) MKC_CODES_ENTRY(20) MKC_CODES_ENTRY(21)
    MKC_CODES_ENTRY(22) MKC_CODES_ENTRY(23) MKC_CODES_ENTRY(24)
    MKC_CODES_ENTRY(25) MKC_CODES_ENTRY(26) MKC_CODES_ENTRY(27)
    MKC_CODES_ENTRY(28) MKC_CODES_ENTRY(29) MKC_CODES_ENTRY(30)
    MKC_CODES_ENTRY(31) MKC_CODES_ENTRY(32)
  };
  static const mkc_count_fn kernels[32][8] = {
    MKC_KERNEL_ROW(1) MKC_KERNEL_ROW(2) MKC_KERNEL_ROW(3) MKC_KERNEL_ROW(4)
    MKC_KERNEL_ROW(5) MKC_KERNEL_ROW(6) MKC_KERNEL_ROW(7) MKC_KERNEL_ROW(8)
    MKC_KERNEL_ROW(9) MKC_KERNEL_ROW(10) MKC_KERNEL_ROW(11)
    MKC_KERNEL_ROW(12) MKC_KERNEL_ROW(13) MKC_KERNEL_ROW(14)
    MKC_KERNEL_ROW(15) MKC_KERNEL_ROW(16) MKC_KERNEL_ROW(17)
    MKC_KERNEL_ROW(18) MKC_KERNEL_ROW(19) MKC_KERNEL_ROW(20)
    MKC_KERNEL_ROW(21) MKC_KERNEL_ROW(22) MKC_KERNEL_ROW(23)
    MKC_KERNEL_ROW(24) MKC_KERNEL_ROW(25) MKC_KERNEL_ROW(26)
    MKC_KERNEL_ROW(27) MKC_KERNEL_ROW(28) MKC_KERNEL_ROW(29)
    MKC_KERNEL_ROW(30) MKC_KERNEL_ROW(31) MKC_KERNEL_ROW(32)
  };
  static const mkc_count_fn packed[32] = {
    MP_KERNEL_ENTRY(1) MP_KERNEL_ENTRY(2) MP_KERNEL_ENTRY(3)
    MP_KERNEL_ENTRY(4) MP_KERNEL_ENTRY(5) MP_KERNEL_ENTRY(6)
    MP_KERNEL_ENTRY(7) MP_KERNEL_ENTRY(8) MP_KERNEL_ENTRY(9)
    MP_KERNEL_ENTRY(10) MP_KERNEL_ENTRY(11) MP_KERNEL_ENTRY(12)
    MP_KERNEL_ENTRY(13) MP_KERNEL_ENTRY(14) MP_KERNEL_ENTRY(15)
    MP_KERNEL_ENTRY(16) MP_KERNEL_ENTRY(17) MP_KERNEL_ENTRY(18)
    MP_KERNEL_ENTRY(19) MP_KERNEL_ENTRY(20) MP_KERNEL_ENTRY(21)
    MP_KERNEL_ENTRY(22) MP_KERNEL_ENTRY(23) MP_KERNEL_ENTRY(24)
    MP_KERNEL_ENTRY(25) MP_KERNEL_ENTRY(26) MP_KERNEL_ENTRY(27)
    MP_KERNEL_ENTRY(28) MP_KERNEL_ENTRY(29) MP_KERNEL_ENTRY(30)
    MP_KERNEL_ENTRY(31) MP_KERNEL_ENTRY(32)
  };
  if (!mkc_cpu_avx2(1)) return NULL;
  d->codes = codes[d->k - 1];
  if (d->packed) {
#define MP_FIXED_ENTRY(L) { mp_kernel_l##L, mp_block_l##L },
    static const struct {
      mkc_count_fn count;
      void (*block)(const mkc_index *, const uint64_t *, uint64_t *, size_t);
    } fixed[] = {
      MP_FIXED_ENTRY(20) MP_FIXED_ENTRY(21) MP_FIXED_ENTRY(22)
      MP_FIXED_ENTRY(23) MP_FIXED_ENTRY(24) MP_FIXED_ENTRY(25)
      MP_FIXED_ENTRY(26) MP_FIXED_ENTRY(27) MP_FIXED_ENTRY(28)
      MP_FIXED_ENTRY(29) MP_FIXED_ENTRY(30) MP_FIXED_ENTRY(31)
      MP_FIXED_ENTRY(32) MP_FIXED_ENTRY(33) MP_FIXED_ENTRY(34)
      MP_FIXED_ENTRY(35) MP_FIXED_ENTRY(36) MP_FIXED_ENTRY(37)
      MP_FIXED_ENTRY(38) MP_FIXED_ENTRY(39) MP_FIXED_ENTRY(40)
    };
#undef MP_FIXED_ENTRY
    if (d->lbits >= MP_FIXED_FIRST && d->lbits <= MP_FIXED_LAST
        && d->slots == MP_S(d->lbits)) {
      d->block = fixed[d->lbits - MP_FIXED_FIRST].block;
      return fixed[d->lbits - MP_FIXED_FIRST].count;
    }
    d->block = mp_block;  return packed[d->k - 1];
  }
  if (d->layout == MKC_DENSE) {
    d->block = md_block;  return kernels[d->k - 1][7];
  }
  switch (d->bits) {
  case 36: d->block = mg_block36;  return kernels[d->k - 1][0];
  case 39: d->block = mg_block39;  return kernels[d->k - 1][1];
  case 42: d->block = mg_block42;  return kernels[d->k - 1][2];
  case 46: d->block = mg_block46;  return kernels[d->k - 1][3];
  case 51: d->block = mg_block51;  return kernels[d->k - 1][4];
  case 56: d->block = mg_block56;  return kernels[d->k - 1][5];
  case 64: d->block = mg_block64;  return kernels[d->k - 1][6];
  default: d->codes = NULL;  return NULL;
  }
}

#undef MKC_KERNEL_ROW
#undef MKC_CODES_ENTRY
#undef MP_KERNEL_ENTRY
#undef MP_KERNEL
#undef MKC_CODES
#undef MG_BLOCK
#undef MKC_KERNELS
#undef MKC_DENSE_KERNEL
#undef MKC_HASH_KERNEL
#undef MG_SLOW
#undef MG_MATCH
#undef MG_VEC
#undef MG_SHIFT
#undef MG_LANE
#undef MG_WIN
#endif

static inline uint64_t mkc_count_key(const mkc_index * db, uint64_t key) {
  return db->count(db, key);
}

static inline int mkc_count(const mkc_index * db, const char * dna, size_t len,
                            uint64_t * count) {
  uint64_t key = 0;
  size_t i;
  *count = 0;
  if (len != db->k) return -1;
  for (i = 0; i < len; i++) {
    int b = mkc_base((unsigned char) dna[i]);
    if (b < 0) return -1;
    key = (key << 2) | (unsigned) b;
  }
  *count = mkc_count_key(db, key);
  return 0;
}

static inline void mkc_count_batch(const mkc_index * db, const uint64_t * keys,
                                   uint64_t * out, size_t n) {
  enum { AHEAD = 16 };
  uint64_t h[AHEAD];
  const unsigned k = db->k;
  size_t i;
  if (db->codes && db->block) {
    uint64_t hb[2][64];
    const int hash = db->layout == MKC_HASH;
    size_t len = n < 64 ? n : 64, at, cur;
    if (!n) return;
    db->codes(keys, hb[0], len, hash, db->seed);
    for (at = 0, cur = 0; at < n; at += 64, cur ^= 1) {
      size_t next = at + 64,
             nlen = n > next ? (n - next < 64 ? n - next : 64) : 0;
      if (nlen) {
        db->codes(keys + next, hb[cur ^ 1], nlen, hash, db->seed);
        for (i = 0; i < nlen; i++) {
          uint64_t w;
          const void * q = hash ? (const void *) mg_bucket(db, hb[cur ^ 1][i],
                                                           0, &w)
                                : (const void *) (db->map + 128
                                                  + (size_t) db->width
                                                    * hb[cur ^ 1][i]);
#if defined(__GNUC__) || defined(__clang__)
          __builtin_prefetch(q, 0, 0);
#else
          (void) q;
#endif
        }
      }
      db->block(db, hb[cur], out + at, len);
      len = nlen;
    }
    return;
  }
  for (i = 0; i < n + AHEAD; i++) {
    if (i >= AHEAD) {
      size_t j = i - AHEAD;
      out[j] = db->layout == MKC_DENSE ? md_count(db, h[j % AHEAD])
                                       : mg_count(db, h[j % AHEAD]);
    }
    if (i < n) {
      uint64_t key = keys[i] & mkc_mask(k),
               code = mkc_code(key, mkc_revcomp(key, k), k);
      const void * p;
      if (db->layout == MKC_DENSE) {
        h[i % AHEAD] = code;  p = db->map + 128 + (size_t) db->width * code;
      } else {
        uint64_t w;
        h[i % AHEAD] = mkc_hash(code ^ db->seed, db->hbits);
        p = mg_bucket(db, h[i % AHEAD], 0, &w);
      }
#if defined(__GNUC__) || defined(__clang__)
      __builtin_prefetch(p, 0, 0);
#else
      (void) p;
#endif
    }
  }
}

static inline size_t mkc_count_sequence(const mkc_index * db, const char * seq,
                                        size_t len, uint64_t * counts) {
  enum { MKC_SEQ_BLOCK = 1024 };
  const unsigned k = db->k;
  uint64_t keys[MKC_SEQ_BLOCK];
  unsigned char valid[MKC_SEQ_BLOCK];
  size_t windows, fill = 0, done = 0, i, w;
  const uint64_t mask = mkc_mask(k);
  uint64_t key = 0;
  unsigned run = 0;
  if (len < k) return 0;
  windows = len - k + 1;
  for (i = 0; i < len; i++) {
    int b = mkc_base((unsigned char) seq[i]);
    if (b < 0) run = 0;
    else {
      key = (key << 2 | (unsigned) b) & mask;
      if (run < k) run++;
    }
    if (i + 1 < k) continue;
    keys[fill] = key;  valid[fill++] = run == k;
    if (fill == MKC_SEQ_BLOCK || i + 1 == len) {
      mkc_count_batch(db, keys, counts + done, fill);
      for (w = 0; w < fill; w++)
        if (!valid[w]) counts[done + w] = 0;
      done += fill;  fill = 0;
    }
  }
  return windows;
}

typedef struct { uint64_t at; } mkc_iterator;

static inline uint64_t mkc_iter_end(const mkc_index * db) {
  if (db->layout == MKC_DENSE) return UINT64_C(1) << db->hbits;
  return db->groups * db->cap * (db->packed ? 16 : db->slots) + db->stash;
}

static inline int mkc_next(const mkc_index * db, mkc_iterator * it,
                           uint64_t * key, uint64_t * count) {
  uint64_t n, idx;
  const unsigned char * rec;
  if (db->layout == MKC_DENSE) {
    n = UINT64_C(1) << db->hbits;
    while (it->at < n) {
      uint64_t code = it->at++, c = md_count(db, code);
      if (c) { *key = mkc_uncode(code, db->k);  *count = c;  return 1; }
    }
    return 0;
  }
  n = db->groups * db->cap * db->slots;
  if (db->packed) n = db->groups * db->cap * 16;
  while (db->packed && it->at < n) {
    uint64_t at = it->at++, bucket = at >> 4;
    const unsigned char * p = db->map + 128 + 64 * bucket;
    const uint64_t w = mkc_load64(p);
    const unsigned i = (unsigned) (at & 15), m = (unsigned) (w & 15),
                   e = (unsigned) (w >> 4) & 3, hdr = MP_HEADER;
    uint64_t upper, ones, v, local, x, s, h;
    unsigned j;
    if (i >= m) { it->at = (bucket + 1) << 4;  continue; }
    upper = w >> hdr & mkc_bits(m + 16);
    ones = upper;
    for (j = 0; j < i; j++) ones &= ones - 1;
    v = (uint64_t) ((unsigned) __builtin_ctzll(ones) - i) << db->lbits
        | mp_bits(p, mp_base(db) + i * db->lbits, db->lbits);
    local = bucket % db->cap;  x = (v & mkc_bits(db->qbits)) * db->cap + local;
    s = v >> db->qbits ? mkc_unhash(x, db->sbits) : x;
    h = (bucket / db->cap) << db->sbits | s;
    *key = mkc_uncode(mkc_unhash(h, db->hbits) ^ db->seed, db->k);
    *count = mg_tag_count(db, h, mp_tag(db, p, m, e, i));
    return 1;
  }
  while (!db->packed && it->at < n) {
    uint64_t at = it->at++, bucket = at / db->slots;
    uint64_t r = mg_get(db->map + 128 + 64 * bucket,
                        (unsigned) (at % db->slots), db->bits);
    unsigned tag = (unsigned) (r >> (db->qbits + 1));
    uint64_t local, v, s, h;
    if (!tag) continue;
    local = bucket % db->cap;  v = (r & mkc_bits(db->qbits)) * db->cap + local;
    s = (r >> db->qbits) & 1 ? mkc_unhash(v, db->sbits) : v;
    h = (bucket / db->cap) << db->sbits | s;
    *key = mkc_uncode(mkc_unhash(h, db->hbits) ^ db->seed, db->k);
    *count = mg_tag_count(db, h, tag);
    return 1;
  }
  idx = it->at - n;
  if (idx >= db->stash) return 0;
  it->at++;
  rec = db->map + db->stash_off + 16 * idx;
  *key = mkc_uncode(mkc_unhash(mkc_load64(rec), db->hbits) ^ db->seed, db->k);
  *count = mkc_load64(rec + 8);
  return 1;
}

static inline void mkc_choose_kernel(mkc_index * d,
                                     uint64_t (*generic)(const mkc_index *,
                                                         uint64_t)) {
#ifdef MKC_X86
  mkc_count_fn kernel;
  d->count = generic;
  kernel = mkc_kernel_for(d);
  if (kernel) d->count = kernel;
#else
  d->count = generic;
#endif
}

static inline unsigned mkc_bitlen(uint64_t n) {
  unsigned b = 0;
  while (n) { b++;  n >>= 1; }
  return b;
}

static inline int mkc_check_dictionary(mkc_index * d, uint64_t entries,
                                       uint64_t hcap, uint64_t hash_off,
                                       uint64_t end) {
  if ((hcap & (hcap - 1)) || (entries ? hcap <= entries : hcap != 0))
    return -1;
  if (hash_off > end || end > d->size || hcap > (end - hash_off) / 16
      || hash_off + 16 * hcap != end)
    return -1;
  d->hcap = hcap;  d->hash_off = hash_off;
  return 0;
}

static inline void mp_prepare(mkc_index * d) {
  const unsigned base = mp_base(d), L = d->lbits;
  unsigned pair;
  d->mp_lmask = mkc_bits(L);  d->mp_tmask = mkc_bits(d->tagbits);
  for (pair = 0; pair < 8; pair++) {
    unsigned first = base + 2 * pair * L, win, half;
    if (first > 512 - L) first = 512 - L;
    win = first >> 3;
    if (win > 48) win = 48;
    d->mp_win[pair] = (unsigned char) win;
    for (half = 0; half < 2; half++) {
      const unsigned j = 2 * pair + half, v = j / 4, lane = (j % 4) / 2,
                     q = j % 4;
      unsigned at = base + j * L, byte, x;
      if (at > 512 - L) at = 512 - L;
      byte = at >> 3;
      if (byte > 56) byte = 56;
      if (byte > win + 8) byte = win + 8;
      for (x = 0; x < 8; x++)
        d->mp_shuf[v][16 * lane + 8 * (q % 2) + x] =
          (unsigned char) (byte - win + x);
      d->mp_shift[v][q] = at - 8 * byte;
    }
  }
}

static inline int mkc_map(mkc_index * d, const unsigned char * p, size_t size) {
  memset(d, 0, sizeof *d);
  d->map = p;  d->size = size;
  if (size < 128) goto bad;
  d->k = p[16];
  if (d->k < 1 || d->k > 32 || mkc_load64(p + 8)) goto bad;
  d->hbits = mkc_code_bits(d->k);
  d->distinct = mkc_load64(p + 24);  d->total = mkc_load64(p + 32);
  if (d->total < d->distinct) goto bad;
  if (!memcmp(p, "MKCGLOB1", 8) || !memcmp(p, "MKCGLOB2", 8)
      || !memcmp(p, "MKCGLOB3", 8)) {
    uint64_t base_off, counts_off, inline_max, dict_end;
    d->layout = MKC_HASH;
    d->gbits = p[17];  d->tagbits = p[18];  d->qbits = p[19];
    d->slots = p[21];
    d->packed = p[7] == '3';
    d->bits = d->packed ? 0 : p[7] == '1' ? 8u * p[20] : p[20];
    d->flags = p[22];
    if (d->gbits >= d->hbits || d->gbits > 30 || d->flags > 2) goto bad;
    d->seed = mkc_seed_word(p[23], d->hbits);
    d->sbits = d->hbits - d->gbits;
    d->cap = mkc_load64(p + 40);  d->groups = UINT64_C(1) << d->gbits;
    if (d->sbits > 62 || d->tagbits < 2 || d->tagbits > 24 || !d->cap
        || d->cap > 4096
        || d->qbits != mkc_bitlen(mkc_bits(d->sbits) / d->cap))
      goto bad;
    if (d->packed) {
      d->lbits = d->qbits + 1 - 4;
      if (p[20] || d->qbits < 8 || d->lbits > 57 || !d->slots
          || d->slots > 15 || d->flags != 2 || d->tagbits > 12
          || mp_base(d) + d->slots * d->lbits > 512)
        goto bad;
      mp_prepare(d);
    } else if (!(d->probe = mg_probe_for(d->bits))
               || d->bits < d->qbits + 1 + d->tagbits
               || d->slots != 512 / d->bits
               || (d->flags && d->slots * d->bits >= 512))
      goto bad;
    if (d->groups > (size - 128) / 64 / d->cap) goto bad;
    d->recip = d->cap > 1 ? UINT64_MAX / d->cap + 1 : 0;
    d->qmask = mkc_bits(d->qbits + 1);
    d->fspan = d->flags == 2 ? 512 - d->slots * d->bits : 1;
    d->fbase = 64 - d->fspan;
    d->table_end = 128 + 64 * d->groups * d->cap;
    d->over = mkc_load64(p + 48);  d->stash = mkc_load64(p + 56);
    if (d->over > d->distinct || d->stash > d->distinct
        || d->over > (size - d->table_end) / 16)
      goto bad;
    d->stash_off = d->table_end + 16 * d->over;
    if (d->stash > (size - d->stash_off) / 16) goto bad;
    if (mkc_load64(p + 72) != d->table_end
        || mkc_load64(p + 80) != d->stash_off
        || mkc_load64(p + 88) != d->stash_off + 16 * d->stash
        || mkc_load64(p + 96) != size)
      goto bad;
    base_off = mkc_load64(p + 104);  counts_off = mkc_load64(p + 112);
    inline_max = mkc_load64(p + 120);
    dict_end = base_off ? base_off : size;
    if (mkc_check_dictionary(d, d->over > d->stash ? d->over : d->stash,
                             mkc_load64(p + 64), d->stash_off + 16 * d->stash,
                             dict_end))
      goto bad;
    if (base_off) {
      uint64_t direct;
      if (!inline_max || inline_max >= (UINT64_C(1) << d->tagbits) - 2
          || base_off % 8 || (size - base_off) / 8 < d->groups + 1
          || counts_off != base_off + 8 * (d->groups + 1))
        goto bad;
      direct = mkc_load64(p + base_off + 8 * d->groups);
      if (direct > (size - counts_off) / 8 || counts_off + 8 * direct != size
          || mkc_load64(p + base_off))
        goto bad;
      d->inline_max = (unsigned) inline_max;
      d->direct_base = p + base_off;  d->direct_counts = p + counts_off;
    } else if (counts_off || inline_max) goto bad;
    else d->inline_max = (1u << d->tagbits) - 2;
    if (d->distinct - d->stash > d->groups * d->cap * d->slots) goto bad;
    mkc_choose_kernel(d, mkc_generic_hash);
    return 0;
  }
  if (!memcmp(p, "MKCDENS1", 8)) {
    unsigned i;
    uint64_t entries, data, exceptions;
    d->layout = MKC_DENSE;  d->width = p[17];
    if ((d->width != 1 && d->width != 2 && d->width != 4 && d->width != 8)
        || d->hbits > 40)
      goto bad;
    for (i = 18; i < 24; i++)
      if (p[i]) goto bad;
    entries = UINT64_C(1) << d->hbits;
    data = mkc_dense_end(d->width, d->hbits);
    if (mkc_load64(p + 40) != entries || mkc_load64(p + 64) != 128
        || mkc_load64(p + 72) != data || mkc_load64(p + 80) != size
        || data > size || d->distinct > entries)
      goto bad;
    for (i = 104; i < 128; i++)
      if (p[i]) goto bad;
    exceptions = mkc_load64(p + 48);
    if (d->width == 8 ? exceptions != 0 : exceptions > d->distinct) goto bad;
    if (mkc_check_dictionary(d, exceptions, mkc_load64(p + 56), data, size))
      goto bad;
    if (!d->hcap && size - data != 0) goto bad;
    mkc_choose_kernel(d, mkc_generic_dense);
    return 0;
  }
bad:
  memset(d, 0, sizeof *d);
  errno = EINVAL;
  return -1;
}

static inline uint64_t mkc_available_memory(void) {
#if defined(_WIN32)
  MEMORYSTATUSEX s;
  s.dwLength = sizeof s;
  return GlobalMemoryStatusEx(&s) ? (uint64_t) s.ullAvailPhys : 0;
#elif defined(__linux__)
  char buf[4096];
  int fd = open("/proc/meminfo", O_RDONLY);
  ssize_t n;
  const char * line;
  if (fd < 0) return 0;
  n = read(fd, buf, sizeof buf - 1);  close(fd);
  if (n <= 0) return 0;
  buf[n] = 0;
  line = strstr(buf, "MemAvailable:");
  return line ? (uint64_t) strtoull(line + 13, NULL, 10) * 1024 : 0;
#elif defined(__APPLE__)
  vm_statistics64_data_t v;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                        (host_info64_t) &v, &count) != KERN_SUCCESS)
    return 0;
  return ((uint64_t) v.free_count + v.inactive_count + v.purgeable_count)
         * (uint64_t) vm_page_size;
#else
  return 0;
#endif
}

static inline int mkc_resident_wanted(size_t size) {
  return size >= ((size_t) 64 << 20)
         && (uint64_t) size <= mkc_available_memory() / 2;
}

#ifdef _WIN32
typedef HANDLE mkc_handle_;

static inline wchar_t * mkc_wide_path_(const char * s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t * w = n > 0 ? (wchar_t *) malloc((size_t) n * sizeof (wchar_t))
                      : NULL;
  if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
    free(w);  w = NULL;
  }
  return w;
}

static inline int mkc_read_at_(mkc_handle_ f, unsigned char * p, size_t n,
                               uint64_t at) {
  OVERLAPPED o;
  DWORD got = 0;
  memset(&o, 0, sizeof o);
  o.Offset = (DWORD) at;  o.OffsetHigh = (DWORD) (at >> 32);
  return ReadFile(f, p, (DWORD) n, &got, &o) && got ? (int) got : -1;
}

static inline int mkc_lock_memory_right_(void) {
  HANDLE token;
  TOKEN_PRIVILEGES tp;
  int ok;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
    return 0;
  tp.PrivilegeCount = 1;  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  ok = LookupPrivilegeValueA(NULL, "SeLockMemoryPrivilege",
                             &tp.Privileges[0].Luid)
       && AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL)
       && GetLastError() == ERROR_SUCCESS;
  CloseHandle(token);
  return ok;
}
#else
typedef int mkc_handle_;

static inline int mkc_read_at_(mkc_handle_ f, unsigned char * p, size_t n,
                               uint64_t at) {
  if (!at && lseek(f, 0, SEEK_SET)) return -1;
  for (;;) {
    ssize_t got = read(f, p, n);
    if (got < 0 && errno == EINTR) continue;
    return got > 0 ? (int) got : -1;
  }
}
#endif

static inline void mkc_resident_free_(void * p, size_t length) {
#ifdef _WIN32
  (void) length;  VirtualFree(p, 0, MEM_RELEASE);
#else
  munmap(p, length);
#endif
}

static inline unsigned char * mkc_resident_read(mkc_index * db,
                                                mkc_handle_ fd, size_t size) {
  unsigned char * base = NULL, * p = NULL;
  size_t length = 0, at;
#if defined(_WIN32)
  size_t large = GetLargePageMinimum();
  if (!large || !mkc_lock_memory_right_()) return NULL;
  length = (size + large - 1) / large * large;
  base = p = (unsigned char *) VirtualAlloc(NULL, length,
                                            MEM_RESERVE | MEM_COMMIT
                                            | MEM_LARGE_PAGES,
                                            PAGE_READWRITE);
  if (!base) return NULL;
#elif defined(__linux__) && defined(MADV_HUGEPAGE) && defined(MAP_ANONYMOUS)
  size_t align = (size_t) 2 << 20;
  void * m;
  length = (size + 2 * align - 1) / align * align;
  m = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
           -1, 0);
  if (m == MAP_FAILED) return NULL;
  base = (unsigned char *) m;
  p = (unsigned char *) (((uintptr_t) base + align - 1)
                         & ~(uintptr_t) (align - 1));
  (void) madvise(p, length - (size_t) (p - base), MADV_HUGEPAGE);
#elif defined(__APPLE__) && defined(__x86_64__) \
      && defined(VM_FLAGS_SUPERPAGE_SIZE_2MB)
  size_t align = (size_t) 2 << 20;
  void * m;
  length = (size + align - 1) / align * align;
  m = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
           VM_FLAGS_SUPERPAGE_SIZE_2MB, 0);
  if (m == MAP_FAILED) return NULL;
  base = p = (unsigned char *) m;
#else
  (void) db;  (void) fd;  (void) size;  (void) base;  (void) length;
  return NULL;
#endif
  for (at = 0; at < size;) {
    int got = mkc_read_at_(fd, p + at,
                           size - at < ((size_t) 64 << 20)
                           ? size - at : ((size_t) 64 << 20), at);
    if (got < 0) { mkc_resident_free_(base, length);  return NULL; }
#ifdef POSIX_FADV_DONTNEED
    (void) posix_fadvise(fd, (off_t) at, got, POSIX_FADV_DONTNEED);
#endif
    at += (size_t) got;
  }
  db->copy = base;  db->copy_length = length;
  return p;
}

static inline void mkc_unmap_(const mkc_index * db) {
  if (db->copy) mkc_resident_free_(db->copy, db->copy_length);
#ifdef _WIN32
  else if (db->map) UnmapViewOfFile((void *) db->map);
#else
  else if (db->map) munmap((void *) db->map, db->size);
#endif
}

static inline int mkc_open_with(mkc_index * db, const char * path,
                                int resident) {
  size_t size, copy_length;
  const unsigned char * p = NULL;
  void * copy;
  int e;
  mkc_index gone;
#ifdef _WIN32
  wchar_t * w;
  HANDLE fd;
  LARGE_INTEGER st;
#else
  int fd;
  struct stat st;
#endif
  memset(db, 0, sizeof *db);
#ifdef _WIN32
  w = mkc_wide_path_(path);
  if (!w) { errno = EINVAL;  return -1; }
  fd = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  free(w);
  if (fd == INVALID_HANDLE_VALUE) {
    errno = GetLastError() == ERROR_FILE_NOT_FOUND
            || GetLastError() == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
    return -1;
  }
  if (!GetFileSizeEx(fd, &st) || st.QuadPart < 128
      || (uint64_t) st.QuadPart > SIZE_MAX) {
    CloseHandle(fd);  errno = EINVAL;  return -1;
  }
  size = (size_t) st.QuadPart;
  if (resident && mkc_resident_wanted(size))
    p = mkc_resident_read(db, fd, size);
  if (!p) {
    HANDLE m = CreateFileMappingW(fd, NULL, PAGE_READONLY, 0, 0, NULL);
    void * v = m ? MapViewOfFile(m, FILE_MAP_READ, 0, 0, size) : NULL;
    if (m) CloseHandle(m);
    if (!v) { CloseHandle(fd);  errno = ENOMEM;  return -1; }
    p = (const unsigned char *) v;
  }
  CloseHandle(fd);
#else
  fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  if (fstat(fd, &st)) { e = errno;  close(fd);  errno = e;  return -1; }
  if (st.st_size < 128 || (uintmax_t) st.st_size > SIZE_MAX) {
    close(fd);  errno = EINVAL;  return -1;
  }
  size = (size_t) st.st_size;
  if (resident && mkc_resident_wanted(size))
    p = mkc_resident_read(db, fd, size);
  if (!p) {
    void * m = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { e = errno;  close(fd);  errno = e;  return -1; }
    p = (const unsigned char *) m;
  }
  close(fd);
#endif
  copy = db->copy;  copy_length = db->copy_length;
  if (!mkc_map(db, p, size)) {
    db->copy = copy;  db->copy_length = copy_length;  return 0;
  }
  e = errno;
  memset(&gone, 0, sizeof gone);
  gone.map = p;  gone.size = size;  gone.copy = copy;
  gone.copy_length = copy_length;
  mkc_unmap_(&gone);
  errno = e;
  return -1;
}

static inline int mkc_open(mkc_index * db, const char * path) {
  return mkc_open_with(db, path, 1);
}

static inline int mkc_open_mapped(mkc_index * db, const char * path) {
  return mkc_open_with(db, path, 0);
}

static inline void mkc_close(mkc_index * db) {
  mkc_unmap_(db);
  memset(db, 0, sizeof *db);
}

#endif
