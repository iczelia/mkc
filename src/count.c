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

static void bin_path(char * p, u32 bin, const char * tag) {
  if (snprintf(p, 4096, "%s/%s%u", workdir, tag, bin) >= 4096)
    FATAL("temporary path too long");
}

typedef struct {
  u64 first, suffix;
  u32 ext;
} run_view;

#define RUN_HASHES(r, kl, ...) do {                                           \
  u64 key_ = (r).first, rc_ = mkc_revcomp(key_, kl), more_ = (r).suffix;      \
  const u64 seed_ = mkc_seed_word(table_seed, mkc_code_bits(kl));             \
  u32 left_ = (r).ext;                                                        \
  for (;;) {                                                                  \
    u64 h = mkc_hash(mkc_code(key_, rc_, kl) ^ seed_, mkc_code_bits(kl));     \
    __VA_ARGS__;                                                              \
    if (!left_--) break;                                                      \
    key_ = ((key_ << 2) | (more_ & 3)) & mkc_mask(kl);                        \
    rc_ = (rc_ >> 2) | ((3 - (more_ & 3)) << (2 * kl - 2));                   \
    more_ >>= 2;                                                              \
  }                                                                           \
} while (0)

typedef struct {
  const u8 * in;
  sz size, next;
  u64 bit, end;
} run_cursor;

static INLINE u64 run_bits(const u8 * in, u64 bit, u32 n) {
  return (mkc_load64(in + (bit >> 3)) >> (bit & 7)) & mkc_bits(n);
}

static INLINE u64 run_decode(const u8 * in, u64 bit, u32 kl,
                             run_view * r) {
  const u32 kb = 2 * kl;
  u32 eb;
  r->ext = (u32) run_bits(in, bit, 6);  bit += 6;
  if (r->ext > 32) FATAL("corrupt temporary run");
  if (kb <= 32) { r->first = run_bits(in, bit, kb);  bit += kb; }
  else {
    r->first = run_bits(in, bit, 32) | run_bits(in, bit + 32, kb - 32) << 32;
    bit += kb;
  }
  eb = 2 * r->ext;
  if (eb <= 32) r->suffix = run_bits(in, bit, eb);
  else r->suffix = run_bits(in, bit, 32)
                   | run_bits(in, bit + 32, eb - 32) << 32;
  return bit + eb;
}

static INLINE int run_frame(const u8 * in, sz size, sz pos, u64 * bit,
                            u64 * end, sz * next) {
  sz len;
  if (size - pos < 2) FATAL("truncated temporary bin");
  len = (sz) in[pos] | (sz) in[pos + 1] << 8;
  if (!len) return 0;
  if (size - pos - 2 < len + 8) FATAL("truncated temporary bin");
  *bit = 8 * (u64) (pos + 2);  *end = 8 * (u64) (pos + 2 + len);
  *next = pos + 2 + len;
  return 1;
}

static INLINE int run_next_of(run_cursor * c, u32 kl, run_view * r) {
  while (c->end - c->bit < 6 + 2 * (u64) kl)
    if (!run_frame(c->in, c->size, c->next, &c->bit, &c->end, &c->next))
      return 0;
  c->bit = run_decode(c->in, c->bit, kl, r);
  if (c->bit > c->end) FATAL("corrupt temporary run");
  return 1;
}

#define RUN_CURSOR(in, size) {(in), (size), 0, 0, 0}

static INLINE void stream_line(u64 * to, const u64 * from) {
#if defined(__SSE2__) && defined(__x86_64__)
  u32 i;
  Fi(8, _mm_stream_si64((long long *) (to + i), (long long) from[i]));
#else
  memcpy(to, from, 64);
#endif
}

#ifndef FIT_KEYS
#define FIT_KEYS 32768
#endif
#define FIT_SCRATCH ((sz) FIT_KEYS + 16)
#define FIT_BUFFER  ((sz) FIT_KEYS)

static MKC_TLS u64 * fit_area;
static MKC_TLS u64 fit_staging[2048][8];

static INLINE int fit_buf(u32 bin, const u64 * src0, sz n0,
                          const u64 * src1, sz n1, const u64 * next,
                          u32 shift, sz * reps, u64 * buf, u64 * scratch,
                          spool_stats * st) {
  if (n0 + n1 > FIT_BUFFER) return 0;
  if (n0) hsort_dup((u64 *) src0, buf, n0, shift, reps, src1);
  if (n1) hsort_dup((u64 *) src1, buf + n0, n1, shift, reps, next);
  if (n0 + n1) spool_sorted(bin, 0, buf, n0 + n1, scratch, st);
  return 1;
}

static INLINE int fit_emit(u32 bin, const u64 * a, const sz * at,
                           const sz * hist, u32 p, u64 * scratch,
                           spool_stats * st) {
  sz from = at[2 * p], len = hist[2 * p] + hist[2 * p + 1];
  if (!len) return 1;
  if (len + 16 > FIT_SCRATCH) return 0;
  spool_sorted(bin, 0, a + from, len, scratch, st);
  return 1;
}

static void fit_pack(sz * at, const sz * off, const sz * hist,
                     const u8 * fill, int fused) {
  sz used = 0;
  u32 d;
  if (!fused) { memcpy(at, off, 2049 * sizeof *at);  return; }
  for (d = 0; d < 2048; d += 2) {
    if (fill[d] || fill[d + 1] || hist[d] + hist[d + 1] > FIT_BUFFER) {
      at[d] = used;  at[d + 1] = used + hist[d];
      used += hist[d] + hist[d + 1];
    } else at[d] = at[d + 1] = 0;
  }
  at[2048] = used;
}

static INLINE sz count_fit_k(const u8 * in, sz size, sz expect, u32 lo,
                             u32 hi, u64 * a, u64 * tmp, sz cap,
                             u32 bin, spool_stats * st, const u32 kl) {
  const u32 hbits = mkc_code_bits(kl), digit = hbits < 11 ? hbits : 11,
            shift = hbits - digit, span = hi - lo;
  u64 (*stg)[8] = fit_staging;
  u64 * buf = fit_area, * scratch = fit_area + FIT_BUFFER;
  u8 fill[2048] = {0}, later[1024] = {0};
  sz hist[2048] = {0}, off[2049], pos[2048], at[2049];
  run_view r;
  const int fused = digit == 11 && hbits - 11 == spool.low - 1;
  sz room = 0, reps = 0, i;
  u32 d, q, j;
  if (digit == 11 && expect >= 65536) {
    sz mean = expect / span, sd = 1;
    while (sd * sd < mean) sd++;
    room = (mean + 8 * sd + 64 + 7) & ~(sz) 7;
    if (span * room + expect / 8 + 4096 > cap) room = 0;
  }
  if (room) {
    u64 * spill = tmp + span * room;
    const sz scap = cap - span * room;
    sz ns = 0;
    int fits = 1;
    run_cursor cur = RUN_CURSOR(in, size);
    for (d = lo; d < hi; d++) pos[d] = (d - lo) * room;
    while (fits && run_next_of(&cur, kl, &r)) {
      RUN_HASHES(r, kl, {
        d = (u32) (h >> shift);
        if (d - lo < span) {
          u32 f = fill[d];
          stg[d][f] = h;
          fill[d] = (u8) (++f & 7);
          if (f == 8) {
            if (pos[d] < ((sz) d - lo + 1) * room) {
              stream_line(tmp + pos[d], stg[d]);  pos[d] += 8;
            } else if (scap - ns >= 8) {
              memcpy(spill + ns, stg[d], 64);  ns += 8;
            } else { fits = 0;  break; }
          }
        }
      });
    }
    for (d = lo; d < hi && fits; d++) {
      sz f = fill[d];
      if (pos[d] + f <= (d - lo + 1) * room) {
        memcpy(tmp + pos[d], stg[d], f * 8);  pos[d] += f;
      } else if (scap - ns >= f) {
        memcpy(spill + ns, stg[d], f * 8);  ns += f;
      } else fits = 0;
    }
#if defined(__SSE2__) && defined(__x86_64__)
    _mm_sfence();
#endif
    if (fits) {
      for (d = lo; d < hi; d++) hist[d] = pos[d] - (d - lo) * room;
      Fi(ns, hist[spill[i] >> shift]++);
      off[0] = 0;
      Fj(2048, off[j + 1] = off[j] + hist[j]);
      for (d = lo; d < hi; d++)
        fill[d] = pos[d] - (d - lo) * room != hist[d];
      fit_pack(at, off, hist, fill, fused);
      for (d = lo; d < hi; d++) {
        sz held = pos[d] - (d - lo) * room;
        if (fused && !(d & 1)) {
          sz held1 = pos[d + 1] - (d - lo + 1) * room;
          if (held == hist[d] && held1 == hist[d + 1]
              && fit_buf(bin, tmp + (d - lo) * room, held,
                         tmp + (d - lo + 1) * room, held1,
                         tmp + (d - lo + 2) * room, shift, &reps, buf,
                         scratch, st)) {
            fill[d + 1] = 0;  d++;  continue;
          }
        }
        if (!fill[d]) {
          if (held)
            hsort_dup(tmp + (d - lo) * room, a + at[d], held, shift, &reps,
                      tmp + (d - lo + 1) * room);
        } else {
          memcpy(a + at[d], tmp + (d - lo) * room, held * 8);
          pos[d] = at[d] + held;
        }
        if (fused && (d & 1))
          later[d >> 1] = fill[d - 1] || fill[d]
                          || !fit_emit(bin, a, at, hist, (u32) (d >> 1),
                                       scratch, st);
      }
      Fi(ns, a[pos[spill[i] >> shift]++] = spill[i]);
      for (d = lo; d < hi && ns; d++)
        if (fill[d]) hsort_a(a + at[d], tmp, hist[d], shift);
      if (!fused) {
        spool_sorted(bin, 0, a, off[2048], tmp, st);  return off[2048];
      }
      for (q = lo / 2; q < hi / 2; q++)
        if (later[q])
          spool_sorted(bin, 0, a + at[2 * q],
                       at[2 * q + 1] - at[2 * q] + hist[2 * q + 1], tmp, st);
      return off[2048];
    }
    memset(fill, 0, sizeof fill);  memset(hist, 0, sizeof hist);
  }
  {
    run_cursor cur = RUN_CURSOR(in, size);
    while (run_next_of(&cur, kl, &r))
      RUN_HASHES(r, kl, {
        d = (u32) (h >> shift);  hist[d] += d - lo < span;
      });
  }
  off[0] = 0;
  Fj(2048, off[j + 1] = off[j] + hist[j]);
  if (off[2048] > cap) FATAL("temporary bin range exceeds the workspace");
  memcpy(pos, off, sizeof pos);
  {
    run_cursor cur = RUN_CURSOR(in, size);
    while (run_next_of(&cur, kl, &r)) {
      RUN_HASHES(r, kl, {
        d = (u32) (h >> shift);
        if (d - lo < span) {
          u32 f = fill[d];
          stg[d][f] = h;
          fill[d] = (u8) (++f & 7);
          if (f == 8) {
            stream_line(tmp + pos[d], stg[d]);
            pos[d] += 8;
          }
        }
      });
    }
  }
  for (d = lo; d < hi; d++) {
    memcpy(tmp + pos[d], stg[d], (sz) fill[d] * 8);
    pos[d] += fill[d];
  }
#if defined(__SSE2__) && defined(__x86_64__)
  _mm_sfence();
#endif
  for (d = lo; d < hi; d++)
    if (pos[d] != off[d + 1]) FATAL("hashed scatter mismatch");
  memset(fill, 0, sizeof fill);
  fit_pack(at, off, hist, fill, fused);
  for (d = lo; d < hi; d++) {
    if (fused && !(d & 1)
        && fit_buf(bin, tmp + off[d], hist[d], tmp + off[d + 1],
                   hist[d + 1], tmp + off[d + 2], shift, &reps, buf, scratch,
                   st)) {
      d++;  continue;
    }
    if (hist[d])
      hsort_dup(tmp + off[d], a + at[d], hist[d], shift, &reps,
                tmp + off[d + 1]);
    if (fused && (d & 1))
      later[d >> 1] = !fit_emit(bin, a, at, hist, d >> 1, scratch, st);
  }
  if (!fused) {
    spool_sorted(bin, 0, a, off[2048], tmp, st);  return off[2048];
  }
  for (q = lo / 2; q < hi / 2; q++)
    if (later[q])
      spool_sorted(bin, 0, a + at[2 * q],
                   at[2 * q + 1] - at[2 * q] + hist[2 * q + 1], tmp, st);
  return off[2048];
}

#define FIT_INSTANCE(kl)                                                      \
  static sz count_fit_##kl(const u8 * in, sz size, sz n, u32 lo,              \
                           u32 hi, u64 * a, u64 * t, sz cap,                  \
                           u32 b, spool_stats * st) {                         \
    return count_fit_k(in, size, n, lo, hi, a, t, cap, b, st, kl);            \
  }
EACH_K(FIT_INSTANCE)
#define FIT_ENTRY(kl) count_fit_##kl,
static sz (*const count_fits[33])(const u8 *, sz, sz, u32, u32,
                                  u64 *, u64 *, sz, u32,
                                  spool_stats *) = {
  NULL, EACH_K(FIT_ENTRY)
};

static INLINE int count_passes_k(const u8 * in, sz size, sz n, u64 * a,
                                 u64 * tmp, sz cap, sz pass, u32 bin,
                                 spool_stats * st, const u32 kl) {
  const u32 shift = mkc_code_bits(kl) - 11;
  sz hist[2048] = {0}, seen = 0;
  run_view r;
  u32 i, d, lo, hi;
  if (n <= cap) {
    u32 ranges = (u32) ((n + pass / 2) / pass);
    if (ranges > 1024) ranges = 1024;
    Fi(ranges,
      lo = 2 * (1024 * i / ranges);  hi = 2 * (1024 * (i + 1) / ranges);
      seen += count_fits[kl](in, size, (sz) ((u64) n * (hi - lo) / 2048),
                            lo, hi, a, tmp, cap, bin, st));
    if (seen != n) FATAL("temporary bin size mismatch");
    return 1;
  }
  {
    run_cursor cur = RUN_CURSOR(in, size);
    while (run_next_of(&cur, kl, &r)) RUN_HASHES(r, kl, hist[h >> shift]++);
  }
  for (d = 0; d < 2048; d += 2) {
    seen += hist[d] + hist[d + 1];
    if (hist[d] + hist[d + 1] > cap) return 0;
  }
  if (seen != n) FATAL("temporary bin size mismatch");
  for (lo = 0; lo < 2048; lo = hi) {
    sz held = hist[lo] + hist[lo + 1];
    for (hi = lo + 2;
         hi < 2048 && held + hist[hi] + hist[hi + 1] <= pass; hi += 2)
      held += hist[hi] + hist[hi + 1];
    if (count_fits[kl](in, size, held, lo, hi, a, tmp, cap, bin, st) != held)
      FATAL("temporary bin size mismatch");
  }
  return 1;
}

#define PASS_INSTANCE(kl)                                                     \
  static int count_passes_##kl(const u8 * in, sz size, sz n, u64 * a,         \
                               u64 * t, sz cap, sz pass, u32 b,               \
                               spool_stats * st) {                            \
    return count_passes_k(in, size, n, a, t, cap, pass, b, st, kl);           \
  }
EACH_K(PASS_INSTANCE)
#define PASS_ENTRY(kl) count_passes_##kl,
static int (*const count_passes[33])(const u8 *, sz, sz, u64 *,
                                     u64 *, sz, sz, u32,
                                     spool_stats *) = {
  NULL, EACH_K(PASS_ENTRY)
};

typedef struct {
  mkc_file f;
  u64 off;
  sz pos, end;
  u64 bit, fend;
  u8 buf[65536 + MKC_BINBUF + 64];
} file_reader;

static sz reader_more(file_reader * s) {
  ptrdiff_t got = os_pread(s->f, s->buf + s->end, 65536, s->off);
  if (got < 0) FATAL("temporary read failed: %s", os_error());
  s->off += (u64) got;  s->end += (sz) got;
  return (sz) got;
}

typedef struct {
  mkc_file f;
  u64 off;
  sz n;
  u8 buf[65536];
} file_writer;

static void writer_put(file_writer * w, const void * p, sz n) {
  if (w->n + n > sizeof w->buf) {
    if (os_pwrite_all(w->f, w->buf, w->n, w->off))
      FATAL("temporary write failed: %s", os_error());
    w->off += w->n;  w->n = 0;
  }
  memcpy(w->buf + w->n, p, n);  w->n += n;
}

static void writer_close(file_writer * w) {
  if (w->n && os_pwrite_all(w->f, w->buf, w->n, w->off))
    FATAL("temporary write failed: %s", os_error());
  if (os_close(w->f)) FATAL("temporary write failed: %s", os_error());
}

typedef struct {
  file_reader in;
  u64 h, c;
  int live;
} run_reader;

static void run_next(run_reader * r) {
  file_reader * s = &r->in;
  if (s->end - s->pos < 16) {
    memmove(s->buf, s->buf + s->pos, s->end - s->pos);
    s->end -= s->pos;  s->pos = 0;
    reader_more(s);
    if (s->end < 16) {
      if (s->end) FATAL("temporary read failed: truncated run");
      r->live = 0;  return;
    }
  }
  r->h = mkc_load64(s->buf + s->pos);  r->c = mkc_load64(s->buf + s->pos + 8);
  s->pos += 16;
}

static void heap_down(run_reader ** heap, sz n, sz at) {
  for (;;) {
    sz l = 2 * at + 1, m = at;
    run_reader * t;
    if (l < n && heap[l]->h < heap[m]->h) m = l;
    if (l + 1 < n && heap[l + 1]->h < heap[m]->h) m = l + 1;
    if (m == at) return;
    t = heap[at];  heap[at] = heap[m];  heap[m] = t;  at = m;
  }
}

static void bin_open(file_reader * s, u32 bin) {
  char path[4096];
  bin_path(path, bin, "b");
  if ((s->f = os_open(path, MKC_READ)) == MKC_NO_FILE)
    FATAL("temporary read failed: %s", os_error());
  s->off = 0;  s->pos = s->end = 0;  s->bit = s->fend = 0;
}

static int bin_next(file_reader * s, u32 kl, run_view * r) {
  while (s->fend - s->bit < 6 + 2 * (u64) kl) {
    if (s->end - s->pos < 2 + MKC_BINBUF + 8) {
      sz left = s->end - s->pos;
      memmove(s->buf, s->buf + s->pos, left);
      s->end = left;  s->pos = 0;
      while (s->end < 2 + MKC_BINBUF + 8 && reader_more(s)) {}
    }
    if (!run_frame(s->buf, s->end, s->pos, &s->bit, &s->fend, &s->pos))
      return 0;
  }
  s->bit = run_decode(s->buf, s->bit, kl, r);
  if (s->bit > s->fend) FATAL("corrupt temporary run");
  return 1;
}

#define TABLE_MAX (1u << 18)

#define REPEAT_SLOTS (1u << 14)
typedef struct {
  u64 first, suffix;
  u32 ext, count;
} repeat_slot;

/* On a collision, replay the previous run with its accumulated weight. */
static INLINE u32 repeat_put(repeat_slot * cache, run_view * r) {
  u64 h = mkc_mix(r->first ^ (r->suffix * UINT64_C(0x9e3779b97f4a7c15)) ^ r->ext);
  repeat_slot * slot = &cache[h & (REPEAT_SLOTS - 1)];
  u32 weight = slot->count;
  if (weight && weight < UINT32_MAX && slot->first == r->first
      && slot->suffix == r->suffix && slot->ext == r->ext) {
    slot->count++;
    return 0;
  }
  run_view old = {slot->first, slot->suffix, slot->ext};
  *slot = (repeat_slot) {r->first, r->suffix, r->ext, 1};
  *r = old;
  return weight;
}

static MKC_TLS sz previous_distinct;

static INLINE int count_table_k(u32 bin, sz cap, u64 * a,
                                u64 * tmp, file_reader * s,
                                spool_stats * st, const u32 kl) {
  const u32 hbits = mkc_code_bits(kl);
  sz limit = cap / 4 < TABLE_MAX ? cap / 4 : TABLE_MAX, size = 4096, used = 0,
     n = 0, i;
  u64 * table = a, * other = tmp, seen = 0;
  run_view r;
  int ok = 1;
  char path[4096];
  sz drain = 0;
  repeat_slot * cache = NULL;
  int eof = 0;
  if (limit < size) return 0;
  while (size * 2 <= limit && size < 2 * previous_distinct) size *= 2;
  /* Both hash tables occupy at most cap / 2 words during accumulation. */
  if ((cap / 2) * sizeof *tmp >= REPEAT_SLOTS * sizeof *cache) {
    cache = (repeat_slot *) (tmp + cap / 2);
    memset(cache, 0, REPEAT_SLOTS * sizeof *cache);
  }
  memset(table, 0, 2 * size * 8);
  bin_open(s, bin);
  while (ok) {
    u32 weight = 1;
    if (!eof && !bin_next(s, kl, &r)) eof = 1;
    if (eof) {
      if (!cache) break;
      while (drain < REPEAT_SLOTS && !cache[drain].count) drain++;
      if (drain == REPEAT_SLOTS) break;
      repeat_slot * slot = &cache[drain++];
      r = (run_view) {slot->first, slot->suffix, slot->ext};
      weight = slot->count;
    } else if (cache) {
      weight = repeat_put(cache, &r);
      if (!weight) continue;
    }
    RUN_HASHES(r, kl, {
      sz x = h & (size - 1);
      while (table[2 * x + 1] && table[2 * x] != h) x = (x + 1) & (size - 1);
      if (!table[2 * x + 1]) { table[2 * x] = h;  used++; }
      table[2 * x + 1] += weight;  seen += weight;
      if (2 * used > size) {
        u64 * t;
        if (2 * size > limit) { ok = 0;  break; }
        memset(other, 0, 4 * size * 8);
        Fi(size,
          if (table[2 * i + 1]) {
            sz m = table[2 * i] & (2 * size - 1);
            while (other[2 * m + 1]) m = (m + 1) & (2 * size - 1);
            other[2 * m] = table[2 * i];  other[2 * m + 1] = table[2 * i + 1];
          });
        t = table;  table = other;  other = t;  size *= 2;
      }
      if (seen >= 65536 && 4 * used > seen) { ok = 0;  break; }
    });
  }
  os_close(s->f);
  if (!ok) return 0;
  previous_distinct = used;
  bin_path(path, bin, "b");  os_remove(path);
  Fi(size, if (table[2 * i + 1]) other[n++] = table[2 * i]);
  hsort_a(other, other + n, n, hbits);
  Fi(n,
    sz x = other[i] & (size - 1);
    while (table[2 * x] != other[i]) x = (x + 1) & (size - 1);
    other[n + i] = table[2 * x + 1]);
  spool_pairs(bin, 0, other, other + n, n, table, st);
  return 1;
}

#define TABLE_INSTANCE(kl)                                                    \
  static int count_table_##kl(u32 b, sz cap, u64 * a, u64 * t,                \
                              file_reader * s, spool_stats * st) {            \
    return count_table_k(b, cap, a, t, s, st, kl);                            \
  }
EACH_K(TABLE_INSTANCE)
#define TABLE_ENTRY(kl) count_table_##kl,
static int (*const count_tables[33])(u32, sz, u64 *, u64 *,
                                     file_reader *, spool_stats *) = {
  NULL, EACH_K(TABLE_ENTRY)
};

#define HEAVY_FANIN 64u

static u32 heavy_fanin = 2;

static file_writer * run_create(u32 bin, u32 id) {
  char rp[4096];
  file_writer * out = xmalloc(sizeof *out);
  snprintf(rp, sizeof rp, "%s/r%u-%u", workdir, bin, id);
  out->off = 0;  out->n = 0;
  out->f = os_open(rp, MKC_WRITE | MKC_CREATE | MKC_TRUNCATE | MKC_SCRATCH);
  if (out->f == MKC_NO_FILE) FATAL("temporary write failed: %s", os_error());
  return out;
}

static void run_put(file_writer * out, u64 h, u64 c) {
  u8 rec[16];
  wr64le(rec, h);  wr64le(rec + 8, c);
  writer_put(out, rec, 16);
}

static void heavy_spill(u32 bin, u32 * nruns, u64 * a,
                        u64 * tmp, sz n, u32 hbits) {
  file_writer * out;
  sz x;
  hsort_a(a, tmp, n, hbits);
  out = run_create(bin, (*nruns)++);
  for (x = 0; x < n;) {
    sz y = x + 1;
    while (y < n && a[y] == a[x]) y++;
    run_put(out, a[x], y - x);
    x = y;
  }
  writer_close(out);  xfree(out);
}

typedef struct { run_reader * readers, ** heap;  sz live;  u32 n; } run_merge;

static void merge_open(run_merge * m, u32 bin, u32 from, u32 n) {
  sz i;
  u32 j;
  m->readers = xmalloc(n * sizeof *m->readers);
  m->heap = xmalloc(n * sizeof *m->heap);
  m->live = 0;  m->n = n;
  Fj(n,
    char rp[4096];
    run_reader * r = &m->readers[j];
    snprintf(rp, sizeof rp, "%s/r%u-%u", workdir, bin, from + j);
    if ((r->in.f = os_open(rp, MKC_READ)) == MKC_NO_FILE)
      FATAL("temporary read failed: %s", os_error());
    os_remove(rp);
    r->in.off = 0;  r->in.pos = r->in.end = 0;
    r->live = 1;  run_next(r);
    if (r->live) m->heap[m->live++] = r);
  for (i = m->live / 2; i-- > 0;) heap_down(m->heap, m->live, i);
}

static int merge_next(run_merge * m, u64 * h, u64 * c) {
  if (!m->live) return 0;
  *h = m->heap[0]->h;  *c = 0;
  while (m->live && m->heap[0]->h == *h) {
    *c += m->heap[0]->c;  run_next(m->heap[0]);
    if (!m->heap[0]->live) m->heap[0] = m->heap[--m->live];
    heap_down(m->heap, m->live, 0);
  }
  return 1;
}

static void merge_close(run_merge * m) {
  u32 j;
  Fj(m->n, os_close(m->readers[j].in.f));
  xfree(m->readers);  xfree(m->heap);
}

static INLINE void count_heavy_k(u32 bin, sz cap, u64 * a,
                                 u64 * tmp, file_reader * s,
                                 spool_stats * st, const u32 kl) {
  const u32 hbits = mkc_code_bits(kl);
  u32 nruns = 0, piece = 0, first;
  sz n = 0, limit = cap / 3 - 16, held = 0;
  u64 * hs = a, * cs = a + cap / 2, key, count;
  run_view r;
  char path[4096];
  run_merge mg;
  bin_open(s, bin);
  while (bin_next(s, kl, &r))
    RUN_HASHES(r, kl, {
      a[n++] = h;
      if (n == cap) { heavy_spill(bin, &nruns, a, tmp, n, hbits);  n = 0; }
    });
  if (n) heavy_spill(bin, &nruns, a, tmp, n, hbits);
  os_close(s->f);
  bin_path(path, bin, "b");  os_remove(path);
  for (first = 0; nruns - first > heavy_fanin; first += heavy_fanin) {
    file_writer * out = run_create(bin, nruns++);
    merge_open(&mg, bin, first, heavy_fanin);
    while (merge_next(&mg, &key, &count)) run_put(out, key, count);
    merge_close(&mg);
    writer_close(out);  xfree(out);
  }
  merge_open(&mg, bin, first, nruns - first);
  while (merge_next(&mg, &key, &count)) {
    hs[held] = key;  cs[held] = count;
    if (++held == limit) {
      if (piece + 1 >= 1u << 18)
        FATAL("bin %u has too many distinct k-mers for -M", bin);
      spool_pairs(bin, piece++, hs, cs, held, tmp, st);  held = 0;
    }
  }
  if (held) spool_pairs(bin, piece, hs, cs, held, tmp, st);
  merge_close(&mg);
}

#define HEAVY_INSTANCE(kl)                                                    \
  static void count_heavy_##kl(u32 b, sz cap, u64 * a, u64 * t,               \
                               file_reader * s, spool_stats * st) {           \
    count_heavy_k(b, cap, a, t, s, st, kl);                                   \
  }
EACH_K(HEAVY_INSTANCE)
#define HEAVY_ENTRY(kl) count_heavy_##kl,
static void (*const count_heavies[33])(u32, sz, u64 *, u64 *,
                                       file_reader *, spool_stats *) = {
  NULL, EACH_K(HEAVY_ENTRY)
};

static const u64 * occ_sort;

static int bin_cmp(const void * a, const void * b) {
  const u32 p = *(const u32 *) a, q = *(const u32 *) b;
  u64 x = occ_sort[p], y = occ_sort[q];
  int by_size = (x < y) - (x > y);
  return by_size ? by_size : (p > q) - (p < q);
}

static u64 occ_guess(int argc, char ** argv, int first) {
  u64 est = 0;
  int i;
  for (i = first; i < argc; i++) {
    mkc_file f;
    i64 size;
    u8 head[IN_HEAD] = {0};
    ptrdiff_t got;
    u64 bytes;
    u32 scale;
    if (!strcmp(argv[i], "-") || !os_is_regular(argv[i])) continue;
    f = os_open(argv[i], MKC_READ);
    if (f == MKC_NO_FILE) continue;
    size = os_file_size(f);
    got = size > 0 ? os_pread(f, head, IN_HEAD, 0) : 0;
    os_close(f);
    if (size <= 0 || got < 0) continue;
    bytes = (u64) size;  scale = in_ratio(head, (sz) got);
    est += scale > 1 ? scale * bytes : head[0] == '@' ? bytes / 5 * 2 : bytes;
  }
  return est;
}

typedef struct {
  const u32 * order;
  const u64 * occ;
  sz cap, pass;
  int batch_slices;
  spool_stats * stats;
  u32 next, tabled, misses;
} count_job;

static void count_worker(void * arg, u32 id, u32 m) {
  count_job * J = (count_job *) arg;
  const sz cap = J->cap;
  u64 * a = xpages(cap * 8), * tmp = xpages(cap * 8);
  u8 * in = NULL;
  sz in_cap = 0;
  file_reader * s;
  spool_stats * st = &J->stats[id];
  u32 x;
  previous_distinct = 0;
  fit_area = xpages((FIT_BUFFER + FIT_SCRATCH) * 8);
  s = xmalloc(sizeof *s);
  while ((x = fetch_add(&J->next, 1u)) < nbins) {
    u32 b = J->order[x];
    const u64 occ = J->occ[b];
    const int ranged = mkc_code_bits(K) >= 11;
    char path[4096];
    mkc_file f;
    i64 size;
    u8 * view, * data;
    int done = 1;
    if (!occ) { bin_path(path, b, "b");  os_remove(path);  continue; }
    if (ld_acq(&J->misses) < 32 || x % 64 == 0) {
      if (count_tables[K](b, cap, a, tmp, s, st)) {
        if (J->batch_slices && !spool_pages)
          spool_pages = xcalloc(SPOOL_PARTS, sizeof *spool_pages);
        fetch_add(&J->tabled, 1u);  st_rel(&J->misses, 0u);
        continue;
      }
      fetch_add(&J->misses, 1u);
    }
    if (!ranged && occ > cap) {
      count_heavies[K](b, cap, a, tmp, s, st);  continue;
    }
    bin_path(path, b, "b");
    f = os_open(path, MKC_READ);
    size = f == MKC_NO_FILE ? -1 : os_file_size(f);
    if (size < 0) FATAL("temporary read failed: %s", os_error());
    view = size > 0 ? (u8 *) os_map_all(f, (sz) size) : NULL;  data = view;
    if (!view) {
      if ((sz) size > in_cap) {
        os_pages_free(in, in_cap);
        in_cap = (sz) size + (sz) size / 4 + 4096;  in = xpages(in_cap);
      }
      if (os_pread_all(f, in, (sz) size, 0))
        FATAL("temporary read failed: %s", os_error());
      data = in;
    }
    os_close(f);
    if (ranged && (occ > cap || occ > J->pass + J->pass / 2))
      done = count_passes[K](data, (sz) size, (sz) occ, a, tmp, cap, J->pass,
                             b, st);
    else if (count_fits[K](data, (sz) size, (sz) occ, 0, 2048, a, tmp, cap, b,
                           st) != occ)
      FATAL("temporary bin size mismatch");
    if (view && os_unmap(view, (sz) size)) FATAL("unmap bin: %s", os_error());
    if (done) os_remove(path);
    else count_heavies[K](b, cap, a, tmp, s, st);
  }
  if (spool_pages) {
    for (x = 0; x < SPOOL_PARTS; x++)
      if (spool_pages[x].used) spool_flush_page(x);
    xfree(spool_pages);  spool_pages = NULL;
  }
  os_pages_free(in, in_cap);  xfree(s);
  os_pages_free(a, cap * 8);  os_pages_free(tmp, cap * 8);
  os_pages_free(fit_area, (FIT_BUFFER + FIT_SCRATCH) * 8);  fit_area = NULL;
}

static u64 spool_build(int argc, char ** argv, int first, u64 memory,
                       mkc_file fd, const char * out,
                       u64 * occurrences, int * dense) {
  sz cap, pass;
  u64 expected, * occ, total = 0, runs = 0, bytes = 0, largest = 0,
      used = 0;
  u32 scanners, i, j, heavy = 0;
  u8 * bufs;
  double start, split, counted;
  char dir[4096];
  spool_stats * stats;
  u32 * order;
  count_job job;
  assembly as;
  u128 sum = 0;
  rank32 = M <= 16;
  if (rank32) seed_init(M);
#ifdef HAVE_AVX2
  scan_simd = rank32 && mkc_simd_avx2();
#endif
  cap = (sz) (memory / (16 * threads));
  if (cap < 4096) FATAL("counting workspace too small; increase -M");
  expected = occ_guess(argc, argv, first);
  while (nbins < MAX_BINS && expected > (u64) (0.85 * cap) * nbins)
    nbins *= 2;
  {
    u32 few = FEW_BINS;
    while (few < nbins && expected > (u64) SMALL_BIN * few) few *= 2;
    nbins = few;
  }
  {
    const u32 need = SPOOL_PARTS + 64 + nbins;
    if (os_fdlimit(need))
      FATAL("need an open-file limit of at least %u", need);
    for (heavy_fanin = HEAVY_FANIN; heavy_fanin > 2
         && os_fdlimit((u64) SPOOL_PARTS + 64
                       + (u64) (heavy_fanin + 1) * threads);
         heavy_fanin /= 2) {}
    if (heavy_fanin == 2)
      (void) os_fdlimit((u64) SPOOL_PARTS + 64 + (u64) 3 * threads);
  }
  scanners = scan_slots();
  producers = xcalloc(scanners, sizeof *producers);
  Fi(scanners,
    producers[i].bins = xpages((sz) nbins * sizeof (binbuf));
    producers[i].occ = xcalloc(nbins, 8));
  bufs = xpages((sz) nbins * BIN_BUFFER);
  Fi(nbins,
    char path[4096];
    bin_path(path, i, "b");
    bin_files[i].f = os_open(path, MKC_WRITE | MKC_CREATE | MKC_TRUNCATE
                                   | MKC_SCRATCH);
    if (bin_files[i].f == MKC_NO_FILE) FATAL("%s: %s", path, os_error());
    bin_files[i].off = 0;  bin_files[i].n = 0;
    bin_files[i].buf = bufs + (sz) i * BIN_BUFFER;
    mx_init(&bin_locks[i]));
  start = os_now();
  scan_task = split_scans[K];
  parse_all(argc, argv, first);
  batch_flush();  pipe_finish();
  occ = xcalloc(nbins, 8);
  Fi(scanners,
    total += producers[i].total;  runs += producers[i].runs;
    bytes += producers[i].bytes;
    Fj(nbins,
      occ[j] += producers[i].occ[j];  flush_buffer(&producers[i], j));
    os_pages_free(producers[i].bins, (sz) nbins * sizeof (binbuf));
    xfree(producers[i].occ));
  xfree(producers);
  Fi(nbins,
    static const u8 end[10] = {0};
    if (bin_files[i].n)
      bin_write(&bin_files[i], bin_files[i].buf, bin_files[i].n);
    if (bin_files[i].off) bin_write(&bin_files[i], end, sizeof end);
    if (os_close(bin_files[i].f))
      FATAL("temporary write failed: %s", os_error());
    mx_free(&bin_locks[i]);
    if (occ[i] > largest) largest = occ[i]);
  os_pages_free(bufs, (sz) nbins * BIN_BUFFER);
  split = os_now();
  Fi(nbins, heavy += occ[i] > cap);
  fprintf(stderr, "mkc: split=%.3fs bins=%u runs=%" PRIu64
          " temporary_bytes=%" PRIu64 " largest_bin=%" PRIu64
          " chunked_bins=%u\n",
          split - start, nbins, runs, bytes, largest, heavy);
  if (fq_par)
    fprintf(stderr, "mkc: parsed in parallel: %" PRIu64 " bytes\n",
            fq_par);
  if (snprintf(dir, sizeof dir, "%s/spool", workdir) >= (int) sizeof dir)
    FATAL("temporary path too long");
  spool_begin(dir, mkc_code_bits(K));
  stats = xcalloc(threads, sizeof *stats);
  order = xmalloc(nbins * sizeof *order);
  Fi(nbins, order[i] = i);
  occ_sort = occ;
  qsort(order, nbins, sizeof *order, bin_cmp);
  Fi(nbins, used += occ[i] != 0);
  pass = used ? (sz) (total / used) : 0;
  /* Large bins already produce large writes. Reserve the staging pages
     from the two per-worker arrays when small slices are expected. */
  const sz page_bytes = SPOOL_PARTS * sizeof(spool_page);
  int batch_slices = used && pass <= SPOOL_PARTS * (SPOOL_PAGE_BYTES / 8)
                     && page_bytes <= cap * 2;
  if (batch_slices) cap -= (page_bytes + 15) / 16;
  if (pass < ((sz) 1 << 20)) pass = (sz) 1 << 20;
  if (pass > cap) pass = cap;
  job = (count_job) {order, occ, cap, pass, batch_slices, stats, 0, 0, 0};
  tp_run(pool, threads, count_worker, &job);
  xfree(occ);  xfree(order);
  spool_close();
  as = (assembly) {K, mkc_code_bits(K), threads, 0, total, 0, {0}, memory, fd,
                   out, pool};
  Fi(threads,
    as.distinct += stats[i].distinct;  as.checksum += stats[i].checksum;
    sum += stats[i].total;
    Fj(SPOOL_ABOVE, as.above[j] += stats[i].above[j]));
  xfree(stats);
  if (sum != total) FATAL("occurrence count mismatch");
  counted = os_now();
  mg_limit = may_reseed
             ? (as.distinct / 64 > 4096 ? as.distinct / 64 : 4096) : 0;
  assemble(&as, dense);
  fprintf(stderr, "mkc: count=%.3fs (table-counted bins %u) assemble=%.3fs\n",
          counted - split, job.tabled, os_now() - counted);
  *occurrences = total;
  return as.distinct;
}
