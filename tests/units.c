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

#include "common.h"
#include "mkc.h"
#include <stdarg.h>
#include <sys/stat.h>
#include "sort.h"
#include "spool.h"

static u64 s = 88172645463325252ull;

static u64 rnd(void) {
  s ^= s << 13;  s ^= s >> 7;  s ^= s << 17;  return s;
}

static int cmp(const void * a, const void * b) {
  u64 x = *(const u64 *) a, y = *(const u64 *) b;
  return (x > y) - (x < y);
}

#define CHECK(c, ...) do { \
  if (!(c)) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__);  printf(__VA_ARGS__); \
    putchar('\n');  exit(1); \
  } } while (0)

static void codes(void) {
  static u8 seen[1u << 12];
  u32 k, i, bits;
  u64 key, rc, canon, code;
  for (k = 1; k <= 32; k++) {
    bits = mkc_code_bits(k);
    Fi(20000,
      key = rnd() & mkc_mask(k);  rc = mkc_revcomp(key, k);
      canon = key < rc ? key : rc;
      code = mkc_code(key, rc, k);
      CHECK(code <= mkc_bits(bits), "k=%u code out of range", k);
      CHECK(code == mkc_code(rc, key, k), "k=%u orientation", k);
      CHECK(mkc_uncode(code, k) == canon, "k=%u uncode", k);
      CHECK(mkc_unhash(mkc_hash(code, bits), bits) == code, "k=%u unhash", k);
      CHECK(mkc_revcomp(rc, k) == key, "k=%u revcomp", k));
    if (k <= 6) {
      memset(seen, 0, sizeof seen);
      for (key = 0; key <= mkc_mask(k); key++) {
        rc = mkc_revcomp(key, k);
        if (rc < key) continue;
        code = mkc_code(key, rc, k);
        CHECK(!seen[code], "k=%u code collision", k);  seen[code] = 1;
      }
    }
  }
  puts("PASS codes");
}

static void division(void) {
  u64 cap, v;
  u32 i;
  for (cap = 1; cap <= 4096; cap++) {
    mkc_index d = { 0 };
    d.cap = cap;  d.recip = cap > 1 ? UINT64_MAX / cap + 1 : 0;
    Fi(2000,
      v = rnd() >> (rnd() % 62 + 2);
      CHECK(mg_div(&d, v) == v / cap, "cap=%llu v=%llu",
            (unsigned long long) cap, (unsigned long long) v));
    CHECK(mg_div(&d, (UINT64_C(1) << 62) - 1)
          == ((UINT64_C(1) << 62) - 1) / cap, "edge");
  }
  puts("PASS division");
}

static u32 naive(const u8 * p, u32 bits, u64 q, u32 sh) {
  u32 i;
  u64 r;
  Fi(512 / bits,
    r = mg_get(p, i, bits);
    if ((r & mkc_bits(sh)) == q && (r >> sh)) return (u32) (r >> sh));
  return 0;
}

static void put(u8 * p, u32 at, u32 bits, u64 r) {
  u32 b = mg_byte(at, bits), sh = at * bits - 8 * b;
  u64 x = mkc_load64(p + b), m = mkc_bits(bits) << sh;
  wr64le(p + b, (x & ~m) | (r << sh));
}

static void probes(void) {
  static const u32 ws[] = { 40, 42, 46, 48, 51, 56, 64 };
  u8 p[64 + 16];
  u32 bits, slots, qbits, sh, t, n, i, j, k, dup, e;
  u64 keys[16], lo, q;
  mg_probe_fn fn;
  Fk(ARRAY_LEN(ws),
    bits = ws[k];
    fn = mg_probe_for(bits);
    slots = 512 / bits;
    for (qbits = 8; qbits + 8 <= bits; qbits += 3) {
      sh = qbits + 1;
      for (t = 0; t < 20000; t++) {
        memset(p, 0, sizeof p);
        if ((t & 1) && slots * bits < 512) p[63] |= 0x80;
        n = (u32) (rnd() % (slots + 1));
        Fj(n,
          do {
            lo = rnd() & mkc_bits(sh);
            if (t % 5 == 0 && j && sh > 33)
              lo = (lo & ~UINT64_C(0xFFFFFFFF)) | (keys[0] & 0xFFFFFFFF);
            if (t % 7 == 0 && j == 0) lo = 0;
            dup = 0;
            Fi(j, dup |= keys[i] == lo);
            if (!dup) break;
          } while (1);
          keys[j] = lo;
          put(p, j, bits, lo | (u64) (1 + rnd() % 127) << sh));
        q = n && rnd() % 3 ? keys[rnd() % n] : rnd() & mkc_bits(sh);
        if (t % 11 == 0) q = 0;
        e = naive(p, bits, q, sh);
        CHECK(fn(p, q, sh) == e, "portable bits=%u qbits=%u used=%u",
              bits, qbits, n);
      }
    });
  puts("PASS probes");
}

static void sorting(void) {
  sz ns[] = { 0, 1, 2, 31, 32, 33, 64, 100, 1000, 65536, 65537, 300000,
              2000000 };
  u32 ws[] = { 61, 64, 27 }, k, bits;
  int pat;
  sz j, n, i;
  u64 * a, * t, * r, m, v;
  Fk(3,
    for (pat = 0; pat < 6; pat++)
      Fj(ARRAY_LEN(ns),
        n = ns[j];  bits = ws[k];
        a = xmalloc(8 * n + 8);  t = xmalloc(8 * n + 8);
        r = xmalloc(8 * n + 8);  m = mkc_bits(bits);
        Fi(n,
          v = rnd() & m;
          if (pat == 1) v = 12345 & m;
          if (pat == 2) v = (i & 1) ? m : 0;
          if (pat == 3)
            v = (rnd() % 3) ? (UINT64_C(0x155555555555) & m) ^ (i % 2) : v;
          if (pat == 4) v = v >> (bits - 20);
          if (pat == 5) v = ((rnd() % 7) << (bits - 3) | (rnd() % 5)) & m;
          a[i] = r[i] = v);
        hsort_a(a, t, n, bits);  qsort(r, n, 8, cmp);
        Fi(n, CHECK(a[i] == r[i], "sort bits=%u pattern=%d n=%zu", bits,
                    pat, n));
        xfree(a);  xfree(t);  xfree(r)));
  puts("PASS sort");
}

static int roundtrip(const u64 * suf, const u64 * cnt, sz n,
                     u32 bits) {
  u32 l = spc_param(n, bits);
  u8 * buf = xcalloc(n * 40 + 64, 1);
  spc_writer w = { buf, 0, 0, 0 };
  spc_reader r;
  u64 prev = 0, a, c;
  sz i, words;
  int bad;
  Fi(n, spc_encode(&w, !i, i ? suf[i - 1] : 0, suf[i], cnt[i], l, bits));
  words = spc_finish(&w);
  r.base = buf;  r.pos = 0;  r.limit = 64 * words;
  Fi(n,
    if (spc_decode(&r, !i, prev, l, bits, &a, &c) || a != suf[i]
        || c != cnt[i]) { xfree(buf);  return 1; }
    prev = a);
  bad = r.limit - r.pos >= 64;
  if (n > 2 && words > 1) {
    spc_reader t = { buf, 0, 64 * (words - 1) };
    int fail = 0;
    prev = 0;
    for (i = 0; i < n && !fail; i++) {
      fail = spc_decode(&t, !i, prev, l, bits, &a, &c);  prev = a;
    }
    if (!fail && t.limit - t.pos < 64 && words * 64 - r.pos >= 64) bad = 1;
  }
  xfree(buf);
  return bad;
}

static void codec(void) {
  sz ns[] = { 1, 2, 3, 10, 1000, 10000, 100000 };
  u32 ws[] = { 17, 27, 51, 54 }, k, bits;
  int pat;
  sz j, n, m, i;
  u64 * suf, * cnt, top;
  Fk(4,
    for (pat = 0; pat < 6; pat++)
      Fj(ARRAY_LEN(ns),
        n = ns[j];  bits = ws[k];  top = mkc_bits(bits);
        if (n > top) continue;
        suf = xmalloc(8 * n);  cnt = xmalloc(8 * n);
        Fi(n, suf[i] = rnd() & top;
              cnt[i] = 1 + (rnd() % 3 == 0) * (rnd() % 5));
        if (pat == 1) Fi(n, suf[i] = i);
        if (pat == 2) Fi(n, suf[i] = top - (n - 1) + i);
        if (pat == 3) Fi(n, cnt[i] = i % 2 ? UINT64_MAX : (rnd() | 1));
        if (pat == 4 && n > 1) { Fi(n, suf[i] = i);  suf[n - 1] = top; }
        if (pat != 1 && pat != 2 && pat != 4) {
          qsort(suf, n, 8, cmp);
          m = 0;
          Fi(n,
            if (!m || suf[i] != suf[m - 1]) {
              suf[m] = suf[i];  cnt[m] = cnt[i];  m++;
            });
          n = m;
        }
        CHECK(!roundtrip(suf, cnt, n, bits), "codec bits=%u pattern=%d n=%zu",
              bits, pat, n);
        xfree(suf);  xfree(cnt)));
  puts("PASS codec");
}

int main(void) {
  codes();  division();  probes();  sorting();  codec();
  return 0;
}
