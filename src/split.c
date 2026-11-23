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

#define BIN_BUFFER 16384u
#ifndef MKC_BINBUF
#define MKC_BINBUF 4096
#endif

typedef struct {
  sz n;
  u64 acc;
  u32 used;
  u8 bytes[MKC_BINBUF];
} binbuf;

typedef struct {
  binbuf * bins;
  u64 total, runs, bytes, * occ;
  char pad[64];
} producer;

typedef struct {
  mkc_file f;
  u64 off;
  sz n;
  u8 * buf;
} bin_file;

static u32 nbins = MIN_BINS;
static bin_file bin_files[MAX_BINS];
static mkc_mutex bin_locks[MAX_BINS];
static producer * producers;
static int rank32;

static void bin_write(bin_file * b, const u8 * p, sz n) {
  if (os_pwrite_all(b->f, p, n, b->off))
    FATAL("temporary write failed: %s", os_error());
  b->off += n;
}

static void flush_buffer(producer * p, u32 b) {
  binbuf * buf = &p->bins[b];
  bin_file * f = &bin_files[b];
  u32 i;
  if (!buf->n) return;
  Fi((buf->used + 7) / 8, buf->bytes[buf->n++] = (u8) (buf->acc >> (8 * i)));
  buf->acc = 0;  buf->used = 0;
  buf->bytes[0] = (u8) (buf->n - 2);  buf->bytes[1] = (u8) ((buf->n - 2) >> 8);
  p->bytes += buf->n;
  mx_lock(&bin_locks[b]);
  if (f->n + buf->n > BIN_BUFFER) { bin_write(f, f->buf, f->n);  f->n = 0; }
  memcpy(f->buf + f->n, buf->bytes, buf->n);  f->n += buf->n;
  mx_unlock(&bin_locks[b]);
  buf->n = 0;
}

typedef struct {
  producer * owner;
  u64 first, suffix, minrank;
  u32 bin, extra, active, cached, target;
} scanner;

SI void binbuf_put(binbuf * b, u64 v, u32 n) {
  u32 i;
  b->acc |= v << b->used;  b->used += n;
  if (b->used >= 32) {
    Fi(4, b->bytes[b->n++] = (u8) (b->acc >> (8 * i)));
    b->acc >>= 32;  b->used -= 32;
  }
}

static void flush_run(scanner * s) {
  producer * o = s->owner;
  binbuf * b = &o->bins[s->bin];
  const u32 e = s->extra, kb = 2 * K;
  if (!s->active) return;
  if (b->n + (b->used + 6 + kb + 2 * e + 7) / 8 > sizeof b->bytes)
    flush_buffer(o, s->bin);
  if (!b->n) b->n = 2;
  binbuf_put(b, e, 6);
  binbuf_put(b, s->first & 0xFFFFFFFFu, kb < 32 ? kb : 32);
  if (kb > 32) binbuf_put(b, s->first >> 32, kb - 32);
  if (e) {
    u32 eb = 2 * e;
    binbuf_put(b, s->suffix & mkc_bits(eb < 32 ? eb : 32), eb < 32 ? eb : 32);
    if (eb > 32) binbuf_put(b, s->suffix >> 32, eb - 32);
  }
  o->runs++;  s->active = 0;
  o->occ[s->bin] += e + 1;
}

#define SCAN_TILE 2048u

static int scan_simd;

static INLINE void scan_segment_k(scanner * s, const u8 * p, sz n,
                                  const u32 kl) {
  const u32 w = kl - M + 1;
  const u64 kmask = mkc_mask(kl), mmask = mkc_mask(M);
  u64 rank[SCAN_TILE + 64], pre[SCAN_TILE + 64], suf[SCAN_TILE + 64];
  u64 mf = 0, mr = 0, f = 0;
  sz at = 0;
  u32 halo = 0;
  for (; at < M - 1 && at < n; at++) {
    u32 c = p[at];
    mf = ((mf << 2) | c) & mmask;
    mr = (mr >> 2) | ((u64) (3 - c) << (2 * M - 2));
    f = ((f << 2) | c) & kmask;
  }
#ifdef HAVE_AVX2
  if (scan_simd) {
    u32 r32[SCAN_TILE + 64], a32[SCAN_TILE + 64], w32[SCAN_TILE + 64];
    while (at < n) {
      sz end = n - at < SCAN_TILE ? n : at + SCAN_TILE, count = end - at,
         total = halo + count, t = halo;
      u64 first = s->first, suffix = s->suffix, cur = s->minrank;
      u32 extra = s->extra, bin = s->bin, active = s->active,
          cached = s->cached, target = s->target, keep;
      const u8 * q = p + at - halo;
      scan_ranks_avx2(p + at, count, M, r32 + halo);
      scan_minima_avx2(r32, total, w, a32, w32);
      for (; t < total; t++) {
        sz base = at + t - halo;
        if (base >= kl - 1 && t + 1 >= w) break;
        f = ((f << 2) | p[base]) & kmask;
      }
      for (; t < total; t++) {
        u32 c = q[t];
        u64 minrank = w32[t + 1 - w];
        f = ((f << 2) | c) & kmask;
        if (UNLIKELY(!cached || cur != minrank)) {
          cur = minrank;  cached = 1;
          target = (u32) (mkc_mix(minrank ^ UINT64_C(0x9E3779B97F4A7C15))
                          & (nbins - 1));
        }
        if (active && target == bin && extra < 32) {
          suffix |= (u64) c << (2 * extra);  extra++;  continue;
        }
        if (active) {
          s->first = first;  s->suffix = suffix;  s->extra = extra;
          s->bin = bin;  s->active = 1;  flush_run(s);
        }
        active = 1;  bin = target;  first = f;  suffix = 0;  extra = 0;
      }
      s->first = first;  s->suffix = suffix;  s->extra = extra;  s->bin = bin;
      s->active = active;  s->minrank = cur;  s->cached = cached;
      s->target = target;
      s->owner->total += end > kl - 1 ? end - (at > kl - 1 ? at : kl - 1) : 0;
      keep = (u32) (total < w - 1 ? total : w - 1);
      memmove(r32, r32 + total - keep, keep * sizeof *r32);  halo = keep;
      at = end;
    }
    return;
  }
#endif
  while (at < n) {
    sz end = n - at < SCAN_TILE ? n : at + SCAN_TILE, count = end - at,
       total = halo + count, i, j, b, t;
    u32 keep;
    Fi(count,
      u32 c = p[at + i];
      u64 seed;
      mf = ((mf << 2) | c) & mmask;
      mr = (mr >> 2) | ((u64) (3 - c) << (2 * M - 2));
      seed = mf < mr ? mf : mr;
      rank[halo + i] = rank32 ? seed_order((u32) seed) : mkc_mix(seed));
    for (b = 0; b < total; b += w) {
      sz e = b + w < total ? b + w : total;
      u64 m = UINT64_MAX;
      for (j = b; j < e; j++) { m = rank[j] < m ? rank[j] : m;  pre[j] = m; }
      m = UINT64_MAX;
      for (j = e; j-- > b;) { m = rank[j] < m ? rank[j] : m;  suf[j] = m; }
    }
    for (t = halo; t < total; t++) {
      sz base = at + t - halo, l;
      u32 c = p[base], bin;
      u64 minrank;
      f = ((f << 2) | c) & kmask;
      if (base < kl - 1 || t + 1 < w) continue;
      l = t + 1 - w;  minrank = suf[l] < pre[t] ? suf[l] : pre[t];
      if (!s->cached || s->minrank != minrank) {
        s->minrank = minrank;  s->cached = 1;
        s->target = (u32) (mkc_mix(minrank ^ UINT64_C(0x9E3779B97F4A7C15))
                           & (nbins - 1));
      }
      bin = s->target;
      if (s->active && (bin != s->bin || s->extra == 32)) flush_run(s);
      if (!s->active) {
        s->active = 1;  s->bin = bin;  s->first = f;  s->suffix = 0;
        s->extra = 0;
      } else { s->suffix |= (u64) c << (2 * s->extra);  s->extra++; }
    }
    s->owner->total += end > kl - 1 ? end - (at > kl - 1 ? at : kl - 1) : 0;
    keep = (u32) (total < w - 1 ? total : w - 1);
    memmove(rank, rank + total - keep, keep * sizeof *rank);  halo = keep;
    at = end;
  }
}

static INLINE void split_scan_k(chunk_job * job, u32 tid,
                                const u32 kl) {
  const u8 * p;
  sz n, lo, hi;
  while (chunk_next(job, &p, &n)) {
    scanner s;
    memset(&s, 0, sizeof s);  s.owner = &producers[tid];
    for (lo = 0; lo < n;) {
      if (p[lo] > 3) { lo++;  continue; }
      hi = lo;
      while (hi + 8 <= n) {
        u64 x = mkc_load64(p + hi) & UINT64_C(0xFCFCFCFCFCFCFCFC);
        if (x) { hi += (sz) __builtin_ctzll(x) >> 3;  break; }
        hi += 8;
      }
      while (hi < n && p[hi] <= 3) hi++;
      if (hi - lo >= kl) {
        scan_segment_k(&s, p + lo, hi - lo, kl);  flush_run(&s);  s.cached = 0;
      }
      lo = hi;
    }
  }
}

#define SPLIT_INSTANCE(kl)                                                    \
  static void split_scan_##kl(void * q, u32 id, u32 m) {                      \
    split_scan_k((chunk_job *) q, id, kl);                                    \
  }
EACH_K(SPLIT_INSTANCE)
#define SPLIT_ENTRY(kl) split_scan_##kl,
static const mkc_task split_scans[33] = {NULL, EACH_K(SPLIT_ENTRY)};
