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

#ifndef MKC_SPOOL_H
#define MKC_SPOOL_H

#include "common.h"
#include "os.h"
#include "mkc.h"

#define SPOOL_BITS 10u
#define SPOOL_PARTS (1u << SPOOL_BITS)
#define SPOOL_ABOVE 10

typedef struct {
  u8 * out;
  u64 acc;
  u32 used;
  sz bytes;
} spc_writer;

typedef struct {
  const u8 * base;
  u64 pos, limit;
} spc_reader;

typedef struct {
  u64 distinct, checksum, above[SPOOL_ABOVE];
  u128 total;
  char pad[64];
} spool_stats;

static const u64 inl_max[SPOOL_ABOVE] = {
  127, 255, 65535, UINT32_MAX, 2, 6, 14, 30, 62, 1
};

static struct {
  char dir[4096];
  u32 hbits, low;
  u64 sizes[SPOOL_PARTS], records[SPOOL_PARTS];
  mkc_file fds[SPOOL_PARTS];
} spool;

SI void spc_word(spc_writer * w, u64 v) {
  u32 i;
  Fi(8, w->out[w->bytes++] = (u8) (v >> (8 * i)));
}

SI void spc_put(spc_writer * w, u64 v, u32 n) {
  if (!n) return;
  v &= mkc_bits(n);
  w->acc |= v << w->used;
  if (w->used + n >= 64) {
    u32 spill;
    spc_word(w, w->acc);
    spill = 64 - w->used;
    w->acc = spill < 64 ? v >> spill : 0;  w->used = w->used + n - 64;
  } else w->used += n;
}

SI void spc_unary(spc_writer * w, u64 q) {
  while (q >= 63) { spc_put(w, 0, 63);  q -= 63; }
  spc_put(w, UINT64_C(1) << q, (u32) q + 1);
}

SI void spc_gamma(spc_writer * w, u64 v) {
  u32 bits = 64 - (u32) __builtin_clzll(v);
  spc_unary(w, bits - 1);  spc_put(w, v, bits - 1);
}

SI sz spc_finish(spc_writer * w) {
  if (w->used) { spc_word(w, w->acc);  w->acc = 0;  w->used = 0; }
  return w->bytes / 8;
}

SI u32 spc_param(u64 n, u32 bits) {
  u64 g = (UINT64_C(1) << bits) / (n ? n : 1) / 100 * 69;
  u32 l = 0;
  while (l + 1 < bits && (UINT64_C(2) << l) <= g) l++;
  return g ? l : 0;
}

SI void spc_encode(spc_writer * w, int first, u64 prev,
                   u64 suffix, u64 count, u32 l,
                   u32 bits) {
  u64 gap, q;
  if (!first && count == 1) {
    gap = suffix - prev - 1;  q = gap >> l;
    if (q + l + 2 <= 64) {
      spc_put(w, (UINT64_C(1) << q) | ((gap & mkc_bits(l)) << (q + 1)),
              (u32) (q + l + 2));
      return;
    }
  }
  if (first) spc_put(w, suffix, bits);
  else {
    gap = suffix - prev - 1;
    spc_unary(w, gap >> l);  spc_put(w, gap, l);
  }
  if (count == 1) spc_put(w, 0, 1);
  else { spc_put(w, 1, 1);  spc_gamma(w, count - 1); }
}

SI u64 spc_peek(const spc_reader * r) {
  return mkc_load64(r->base + (r->pos >> 3)) >> (r->pos & 7);
}

SI u64 spc_get(spc_reader * r, u32 n) {
  u64 v = 0;
  u32 take, at = 0;
  while (n) {
    take = n > 56 ? 56 : n;
    v |= (spc_peek(r) & mkc_bits(take)) << at;
    r->pos += take;  at += take;  n -= take;
  }
  return v;
}

SI u64 spc_unary_get(spc_reader * r) {
  u64 q = 0;
  for (;;) {
    u64 v = spc_peek(r) & mkc_bits(56);
    if (v) {
      u32 z = (u32) __builtin_ctzll(v);
      r->pos += z + 1;
      return q + z;
    }
    r->pos += 56;  q += 56;
    if (r->pos > r->limit) return UINT64_MAX;
  }
}

SI int spc_decode(spc_reader * r, int first, u64 prev,
                  u32 l, u32 bits, u64 * suffix,
                  u64 * count) {
  if (!first) {
    u64 v = spc_peek(r), m = v & mkc_bits(56);
    if (m) {
      u32 z = (u32) __builtin_ctzll(m);
      if (z + l + 2 <= 57 && !((v >> (z + 1 + l)) & 1)) {
        u64 gap = ((u64) z << l) | ((v >> (z + 1)) & mkc_bits(l));
        if ((u64) z > mkc_bits(bits) >> l || gap >= mkc_bits(bits) - prev)
          return -1;
        *suffix = prev + 1 + gap;  *count = 1;  r->pos += z + l + 2;
        return r->pos <= r->limit ? 0 : -1;
      }
    }
  }
  if (first) *suffix = spc_get(r, bits);
  else {
    u64 gap, q = spc_unary_get(r);
    if (q == UINT64_MAX || q > mkc_bits(bits) >> l) return -1;
    gap = (q << l) | spc_get(r, l);
    if (gap >= mkc_bits(bits) - prev) return -1;
    *suffix = prev + 1 + gap;
  }
  if (!spc_get(r, 1)) *count = 1;
  else {
    u64 v, z = spc_unary_get(r);
    if (z > 63) return -1;
    v = (UINT64_C(1) << z) | spc_get(r, (u32) z);
    if (v == UINT64_MAX) return -1;
    *count = v + 1;
  }
  return r->pos <= r->limit ? 0 : -1;
}

SI void spool_note(spool_stats * s, u64 h, u64 count) {
  u32 i;
  s->distinct++;  s->total += count;
  s->checksum += mkc_mix(h ^ (count == 1 ? mkc_mix(1) : mkc_mix(count)));
  if (count > 1) Fi(SPOOL_ABOVE, s->above[i] += count > inl_max[i]);
}

SI void spool_tally(spool_stats * s, const spool_stats * t) {
  u32 i;
  s->distinct += t->distinct;  s->total += t->total;
  s->checksum += t->checksum;
  Fi(SPOOL_ABOVE, s->above[i] += t->above[i]);
}

SI void spool_path(char * p, u32 part) {
  if (snprintf(p, 4096, "%s/%04u.part", spool.dir, part) >= 4096)
    FATAL("spool path too long");
}

SI void spool_begin(const char * dir, u32 hbits) {
  u32 i;
  if (snprintf(spool.dir, sizeof spool.dir, "%s", dir)
      >= (int) sizeof spool.dir)
    FATAL("spool path too long");
  if (os_make_dir(spool.dir)) FATAL("create spool: %s", os_error());
  spool.hbits = hbits;  spool.low = hbits - SPOOL_BITS;
  Fi(SPOOL_PARTS,
    spool.sizes[i] = spool.records[i] = 0;  spool.fds[i] = MKC_NO_FILE);
}

SI mkc_file spool_fd(u32 part) {
  mkc_file old, fd = ld_acq(&spool.fds[part]);
  char p[4096];
  if (fd != MKC_NO_FILE) return fd;
  spool_path(p, part);
  fd = os_open(p, MKC_WRITE | MKC_CREATE | MKC_SCRATCH);
  if (fd == MKC_NO_FILE) FATAL("open spool partition: %s", os_error());
  old = MKC_NO_FILE;
  if (!cas(&spool.fds[part], &old, fd)) { os_close(fd);  fd = old; }
  return fd;
}

SI void spool_writeback(mkc_file fd, u64 off, sz len) {
  const u64 win = UINT64_C(32) << 20;
  u64 lo = off / win, hi = (off + len) / win;
  if (hi > lo) os_writeback(fd, lo * win, (hi - lo) * win);
}

SI void spool_slice(u32 part, u64 source, u32 rice,
                    u64 records, const void * payload,
                    sz words) {
  u8 hdr[24];
  sz len;
  u64 off;
  mkc_file fd;
  wr64le(hdr, source | (u64) rice << 32);
  wr64le(hdr + 8, records);  wr64le(hdr + 16, words);
  len = 24 + 8 * words;
  off = fetch_add(&spool.sizes[part], (u64) len);
  fetch_add(&spool.records[part], records);
  fd = spool_fd(part);
  if (os_pwrite2(fd, hdr, 24, payload, 8 * words, off))
    FATAL("spool write: %s", os_error());
  spool_writeback(fd, off, len);
}

SI void spool_close(void) {
  u32 i;
  Fi(SPOOL_PARTS,
    mkc_file fd = spool.fds[i];
    if (fd != MKC_NO_FILE && os_close(fd))
      FATAL("close spool partition: %s", os_error());
    spool.fds[i] = MKC_NO_FILE);
}

SI void spool_sorted(u32 bin, u32 piece,
                     const u64 * keys, sz n,
                     u64 * scratch, spool_stats * st) {
  const u32 low = spool.low;
  const u64 mask = mkc_bits(low);
  spool_stats tally = {0, 0, {0, 0, 0, 0}, 0, {0}};
  sz at, p;
  for (at = 0; at < n;) {
    u32 rice, part = (u32) (keys[at] >> low);
    sz end = at;
    u64 prev = 0, records = 0;
    spc_writer w = {(u8 *) scratch, 0, 0, 0};
    while (end < n && (u32) (keys[end] >> low) == part) {
      u64 h = keys[end];
      records++;
      while (end < n && keys[end] == h) end++;
    }
    rice = spc_param(records, low);
    for (p = at; p < end;) {
      u64 h = keys[p];
      sz q = p + 1;
      while (q < end && keys[q] == h) q++;
      spc_encode(&w, p == at, prev, h & mask, q - p, rice, low);
      spool_note(&tally, h, q - p);  prev = h & mask;  p = q;
    }
    spool_slice(part, (u64) bin << 18 | piece, rice, records, scratch,
                spc_finish(&w));
    at = end;
  }
  spool_tally(st, &tally);
}

SI void spool_pairs(u32 bin, u32 piece,
                    const u64 * h, const u64 * c, sz n,
                    u64 * scratch, spool_stats * st) {
  const u32 low = spool.low;
  const u64 mask = mkc_bits(low);
  spool_stats tally = {0, 0, {0, 0, 0, 0}, 0, {0}};
  sz at, j;
  for (at = 0; at < n;) {
    u32 rice, part = (u32) (h[at] >> low);
    sz end = at;
    spc_writer w = {(u8 *) scratch, 0, 0, 0};
    while (end < n && (u32) (h[end] >> low) == part) end++;
    rice = spc_param(end - at, low);
    for (j = at; j < end; j++) {
      spc_encode(&w, j == at, j > at ? h[j - 1] & mask : 0, h[j] & mask, c[j],
                 rice, low);
      spool_note(&tally, h[j], c[j]);
    }
    spool_slice(part, (u64) bin << 18 | piece, rice, end - at, scratch,
                spc_finish(&w));
    at = end;
  }
  spool_tally(st, &tally);
}

#endif
