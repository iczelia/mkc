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

typedef struct {
  u32 k, hbits, threads;
  u64 distinct, total, checksum, above[SPOOL_ABOVE];
  u64 budget;
  mkc_file fd;  const char * path;
  mkc_pool * pool;
} assembly;

static void write_at(mkc_file fd, const void * data, sz n, u64 off) {
  if (os_pwrite_all(fd, data, n, off)) FATAL("index write: %s", os_error());
}

static void read_at(mkc_file fd, void * data, sz n, u64 off) {
  if (os_pread_all(fd, data, n, off)) FATAL("read: %s", os_error());
}

static u64 dict_slots(u64 n) {
  u64 c = 0;
  if (n) { c = 1;  while (c <= 2 * n) c *= 2; }
  return c;
}

static u32 asm_workers(const assembly * a, u64 n) {
  u64 fit = a->budget / (n + (1u << 20));
  u32 w = a->threads;
  if (w > fit) w = fit ? (u32) fit : 1;
  return w;
}

typedef struct {
  u64 order;
  spc_reader in;
  u64 records, suffix, h, count;
  u32 part, rice;
  int first, live;
} slice;

static INLINE void sl_next(slice * r, u32 low) {
  if (!r->records) { r->live = 0;  return; }
  if (spc_decode(&r->in, r->first, r->suffix, r->rice, low, &r->suffix,
                 &r->count))
    FATAL("corrupt spool slice");
  r->first = 0;  r->records--;  r->h = (u64) r->part << low | r->suffix;
}

static int sl_cmp(const void * a, const void * b) {
  u64 x = ((const slice *) a)->order, y = ((const slice *) b)->order;
  return (x > y) - (x < y);
}

typedef struct { void * p;  sz n; } slice_map;

static slice * sl_load(u32 first, u32 n, const u64 * sizes,
                       const u64 * records, u8 * buf, sz * count,
                       slice * slices, sz * cap, slice_map * maps) {
  sz ns = 0, got = 0, i;
  u32 part;
  for (part = first; part < first + n; part++) {
    char path[4096];
    mkc_file fd;
    sz want, off;
    u8 * at;
    u64 seen = 0;
    if (!sizes[part]) continue;
    spool_path(path, part);
    fd = os_open(path, MKC_READ);
    if (fd == MKC_NO_FILE) FATAL("open spool partition: %s", os_error());
    want = (sz) sizes[part];
    at = want % 4096 && 4096 - want % 4096 >= 16
         ? (u8 *) os_map_all(fd, want) : NULL;
    maps[part - first] = (slice_map) {at, at ? want : 0};
    if (!at) { at = buf + got;  read_at(fd, at, want, 0); }
    os_close(fd);
    if (os_remove(path)) FATAL("remove spool partition: %s", os_error());
    for (off = 0; off < want;) {
      u64 src, nrec, words;
      u32 rice;
      slice * r;
      if (want - off < 24) FATAL("truncated spool slice");
      src = mkc_load64(at + off);  nrec = mkc_load64(at + off + 8);
      words = mkc_load64(at + off + 16);
      rice = (u32) (src >> 32);  src &= UINT32_MAX;
      if (words > (want - off - 24) / 8 || !nrec || words > 3 * nrec + 16
          || rice >= spool.low)
        FATAL("bad spool slice");
      if (ns == *cap) {
        *cap = *cap ? 2 * *cap : 1024;
        slices = xrealloc(slices, *cap * sizeof *slices);
      }
      r = &slices[ns++];
      *r = (slice) {(u64) part << 32 | src,
                    {at + off + 24, 0, 64 * words},
                    nrec, 0, 0, 0, part, rice, 1, 1};
      sl_next(r, spool.low);
      seen += nrec;  off += 24 + 8 * (sz) words;
    }
    if (seen != records[part]) FATAL("spool record mismatch");
    got += want;
  }
  qsort(slices, ns, sizeof *slices, sl_cmp);
  for (i = 1; i < ns; i++)
    if (slices[i].order == slices[i - 1].order) FATAL("duplicate spool slice");
  *count = ns;
  return slices;
}

static void sl_unmap(slice_map * maps, u32 n) {
  u32 i;
  Fi(n, if (maps[i].p) {
    if (os_unmap(maps[i].p, maps[i].n)) FATAL("unmap spool: %s", os_error());
    maps[i].p = NULL;
  });
}

static void sl_done(const slice * s, sz n) {
  sz i;
  Fi(n, if (s[i].live || s[i].in.pos > s[i].in.limit
            || s[i].in.limit - s[i].in.pos >= 64)
          FATAL("spool slice not consumed"));
}

SI void mg_store(u8 * p, u32 bytes, u64 x) {
  u32 i;
  Fi(bytes, p[i] = (u8) (x >> (8 * i)));
}

SI void mg_put(u8 * bucket, u32 s, u32 bits, u64 r) {
  u32 b = mg_byte(s, bits), shift = s * bits - 8 * b;
  u64 x = mkc_load64(bucket + b), mask = mkc_bits(bits) << shift;
  wr64le(bucket + b, (x & ~mask) | (r << shift));
}

static u32 table_seed;
static u64 nstash;
static u64 mg_limit, mg_seen;
static volatile int mg_gave_up;
#define MG_LOAD 94u
#ifdef MKC_TEST_PACKED
#define MG_PACKED MKC_TEST_PACKED
#else
#define MG_PACKED 1
#endif

#define MG_FULL_TAG 7u
#define MP_FILTER 6u

SI u32 mp_need(u32 lb, u32 tag, u32 s, u32 n, u32 e) {
  return MP_FILTER + MP_HEADER + s + 16 + n * lb + e * (4 + tag);
}

static void mp_shape_cap(u32 lb, u32 tag, double other, u32 * slots,
                         double * mean) {
  u32 n = 15, m, e;
  double p[16] = {1}, sum = 0;
  while (n && mp_need(lb, tag, n, n, 0) > 512) n--;
  for (m = 1; m <= n; m++) {
    double fit = 0;
    for (e = m; e > 0; e--) p[e] = p[e] * (1 - other) + p[e - 1] * other;
    p[0] *= 1 - other;
    for (e = 0; e <= 3 && e <= m; e++)
      if (mp_need(lb, tag, n, m, e) <= 512) fit += p[e];
    sum += fit;
  }
  *slots = n;  *mean = sum;
}

static u64 mg_extra(u32 tag, const u64 * above) {
  u64 n;
  if (!above) return 0;
  if (tag < MG_FULL_TAG) {
    n = above[2 + tag];
    return 16 * n + 16 * dict_slots(n);
  }
  n = tag == 7 ? above[8] : tag == 8 ? above[0] : tag <= 16 ? above[1]
      : tag <= 32 ? above[2] : above[3];
  return 8 * n;
}

static int mg_shape(mkc_index * d, u32 kl, u64 distinct, const u64 * above) {
  u32 hbits = mkc_code_bits(kl), slots, mintag, gb, tag, i;
  u64 best = UINT64_MAX, flat;
  mkc_index c;
  double other;
  if (distinct > UINT64_MAX / 100) return -1;
  for (slots = 14; slots >= 8; slots--)
    for (mintag = MG_FULL_TAG;
         mintag >= (above ? 2u : MG_FULL_TAG); mintag--) {
      u32 bits = 512 / slots, qmax = bits - 1 - mintag;
      if (slots > 12 && mintag == MG_FULL_TAG) continue;
      for (gb = 0; gb <= 30 && gb < hbits; gb++) {
        u32 sb = hbits - gb, qbits, tagbits;
        u64 groups, denom, cap, lo, table;
        if (sb > 62) continue;
        groups = UINT64_C(1) << gb;
        denom = groups * slots * MG_LOAD;
        cap = (distinct * 100) / denom + !!((distinct * 100) % denom);
        lo = sb > qmax ? UINT64_C(1) << (sb - qmax) : 1;
        if (cap < lo) cap = lo;
        if (cap > 4096) continue;
        qbits = mkc_bitlen(mkc_bits(sb) / cap);  tagbits = bits - qbits - 1;
        if (qbits > qmax) continue;
        table = groups * cap * 64
                + mg_extra(tagbits > 24 ? 24 : tagbits, above);
        if (table >= best) continue;
        memset(&c, 0, sizeof c);
        c.layout = MKC_HASH;  c.k = kl;  c.hbits = hbits;
        c.tagbits = tagbits > 24 ? 24 : tagbits;  c.gbits = gb;  c.sbits = sb;
        c.groups = groups;  c.cap = cap;  c.qbits = qbits;  c.bits = bits;
        c.slots = slots;  c.distinct = distinct;
        c.recip = cap > 1 ? UINT64_MAX / cap + 1 : 0;
        c.flags = slots * bits < 512 ? 2 : 0;
        c.fspan = c.flags == 2 ? 512 - slots * bits : 1;
        c.fbase = 64 - c.fspan;
        *d = c;  best = table;
      }
    }
  other = distinct && above ? (double) above[9] / (double) distinct : 1;
  flat = best;
  for (tag = 12;
       MG_PACKED && above && tag >= 2u; tag--)
    for (gb = 0; gb <= 30 && gb < hbits; gb++) {
      u32 sb = hbits - gb, qbits = 0;
      u64 groups, cap = 1, table;
      double mean = 15;
      slots = 15;
      if (sb > 62) continue;
      groups = UINT64_C(1) << gb;
      Fi(4,
        double want = (double) distinct * 100
                      / ((double) groups * mean * MG_LOAD);
        cap = want < 1 ? 1 : (u64) want + ((double) (u64) want < want);
        if (cap > 4096) break;
        qbits = mkc_bitlen(mkc_bits(sb) / cap);
        if (qbits < 8) break;
        mp_shape_cap(qbits + 1 - 4, tag, other, &slots, &mean));
      if (cap > 2048 || qbits < 8 || qbits + 1 - 4 > 57 || qbits + 1 + tag > 64
          || !slots || mean < 1)
        continue;
      if (MG_PACKED == 1 && slots * other > 1) continue;
      table = groups * cap * 64 + mg_extra(tag, above);
      if (table >= best || (MG_PACKED == 1 && table > flat / 100 * 97))
        continue;
      memset(&c, 0, sizeof c);
      c.layout = MKC_HASH;  c.k = kl;  c.hbits = hbits;  c.tagbits = tag;
      c.gbits = gb;  c.sbits = sb;  c.groups = groups;  c.cap = cap;
      c.qbits = qbits;  c.slots = slots;  c.distinct = distinct;
      c.packed = 1;  c.lbits = qbits + 1 - 4;  c.bits = 0;
      c.recip = cap > 1 ? UINT64_MAX / cap + 1 : 0;  c.flags = 2;
      *d = c;  best = table;
    }
  return best == UINT64_MAX ? -1 : 0;
}

SI u64 mg_record(const mkc_index * d, u64 s, u32 choice, u32 tag,
                 u64 * bucket) {
  u64 v = mg_alt(s, d->sbits, choice), q = mg_div(d, v);
  *bucket = v - q * d->cap;
  return q | (u64) choice << d->qbits | (u64) tag << (d->qbits + 1);
}

SI u64 mg_suffix(const mkc_index * d, u64 r, u64 bucket) {
  u64 v = (r & mkc_bits(d->qbits)) * d->cap + bucket;
  return (r >> d->qbits) & 1 ? mkc_unhash(v, d->sbits) : v;
}

static int mg_insert(const mkc_index * d, u8 * table, u8 * used, u64 s,
                     u32 tag, u32 maxwalk, u64 * rng, u64 * res, u32 * rtag) {
  u32 walk, i;
  for (walk = 0;; walk++) {
    u64 pos[2], rec[2], old;
    u32 c, slot;
    Fi(2,
      rec[i] = mg_record(d, s, i, tag, &pos[i]);
      if (used[pos[i]] < d->slots) {
        mg_put(table + 64 * pos[i], used[pos[i]]++, d->bits, rec[i]);
        return 0;
      });
    if (walk == maxwalk) { *res = s;  *rtag = tag;  return 1; }
    *rng ^= *rng << 13;  *rng ^= *rng >> 7;  *rng ^= *rng << 17;
    c = (u32) (*rng & 1);  slot = (u32) ((*rng >> 8) % d->slots);
    old = mg_get(table + 64 * pos[c], slot, d->bits);
    mg_put(table + 64 * pos[c], slot, d->bits, rec[c]);
    s = mg_suffix(d, old, pos[c]);  tag = (u32) (old >> (d->qbits + 1));
  }
}

static int mg_evict(const mkc_index * d, u8 * table, u8 * used, u64 bucket,
                    u32 depth) {
  u8 * base = table + 64 * bucket;
  u32 pass, j;
  for (pass = 1; pass <= depth; pass++)
    for (j = d->slots; j-- > 0;) {
      u64 r = mg_get(base, j, d->bits), to, rec;
      int slot;
      if ((r >> d->qbits) & 1) continue;
      rec = mg_record(d, (r & mkc_bits(d->qbits)) * d->cap + bucket, 1,
                      (u32) (r >> (d->qbits + 1)), &to);
      if (to == bucket) continue;
      if (used[to] < d->slots) slot = used[to]++;
      else if (pass <= 1
               || (slot = mg_evict(d, table, used, to, pass - 1)) < 0)
        continue;
      mg_put(table + 64 * to, (u32) slot, d->bits, rec);
      return (int) j;
    }
  return -1;
}

static int mg_place(const mkc_index * d, u8 * table, u8 * used, u64 s,
                    u32 tag) {
  u64 pos[2], want[2];
  u32 i;
  Fi(2, want[i] = mg_record(d, s, i, tag, &pos[i]));
  if (used[pos[1]] < d->slots) {
    mg_put(table + 64 * pos[1], used[pos[1]]++, d->bits, want[1]);
    return 1;
  }
  Fi(2,
    int slot = mg_evict(d, table, used, pos[i], 1);
    if (slot >= 0) {
      mg_put(table + 64 * pos[i], (u32) slot, d->bits, want[i]);
      return 1;
    });
  return 0;
}

SI void mg_flag(const mkc_index * d, u8 * table, u64 s) {
  u64 q = mg_div(d, s);
  u8 * w = table + 64 * (s - q * d->cap) + 56;
  u32 bit = mg_filter_bit(d, q);
  w[bit >> 3] |= (u8) (1u << (bit & 7));
}

static void mg_flags(const mkc_index * d, u8 * table) {
  u64 pick[8] = {0}, i, m;
  u32 j, s, bit;
  Fj(d->slots,
    bit = j * d->bits + d->qbits;
    pick[bit >> 6] |= UINT64_C(1) << (bit & 63));
  Fi(d->cap,
    const u8 * b = table + 64 * i;
    Fj(8, for (m = mkc_load64(b + 8 * j) & pick[j]; m; m &= m - 1) {
      s = (64 * j + (u32) __builtin_ctzll(m) - d->qbits) / d->bits;
      mg_flag(d, table, mg_suffix(d, mg_get(b, s, d->bits), i));
    }));
}

typedef struct { u64 * rec;  u8 most[4]; } mp_work;

SI u32 mp_rtag(const mkc_index * d, u64 r) {
  return (u32) (r >> (d->qbits + 1));
}

SI u32 mp_n(const mp_work * w, u64 x) {
  return (u32) (w->rec[16 * x] & 255);
}

SI u32 mp_e(const mp_work * w, u64 x) {
  return (u32) (w->rec[16 * x] >> 8);
}

SI u64 * mp_row(const mp_work * w, u64 x) {
  return w->rec + 16 * x + 1;
}

SI void mp_limits(const mkc_index * d, mp_work * w) {
  u32 i;
  Fi(4,
    u32 n = d->slots;
    while (n && mp_need(d->lbits, d->tagbits, d->slots, n, i) > 512) n--;
    w->most[i] = (u8) n);
}

SI int mp_fits(const mp_work * w, u32 n, u32 e) {
  return e < 4 && n <= w->most[e];
}

SI int mp_room(const mkc_index * d, const mp_work * w, u64 x, u32 tag) {
  return mp_fits(w, mp_n(w, x) + 1u, mp_e(w, x) + (tag != 1));
}

SI int mp_swap_ok(const mkc_index * d, const mp_work * w, u64 x, u32 old,
                  u32 nt) {
  return mp_fits(w, mp_n(w, x), mp_e(w, x) - (old != 1) + (nt != 1));
}

SI void mp_add(const mkc_index * d, mp_work * w, u64 x, u64 rec) {
  mp_row(w, x)[mp_n(w, x)] = rec;
  w->rec[16 * x] += 1 + ((u64) (mp_rtag(d, rec) != 1) << 8);
}

SI void mp_set(const mkc_index * d, mp_work * w, u64 x, u32 s, u64 rec) {
  w->rec[16 * x] += ((u64) (mp_rtag(d, rec) != 1) << 8)
                    - ((u64) (mp_rtag(d, mp_row(w, x)[s]) != 1) << 8);
  mp_row(w, x)[s] = rec;
}

static int mp_insert(const mkc_index * d, mp_work * w, u64 s,
                     u32 tag, u32 maxwalk, u64 * rng, u64 * res, u32 * rtag) {
  u32 walk, i, j;
  for (walk = 0;; walk++) {
    u64 pos[2], rec[2], x, old;
    u32 c, slot = 0, n;
    int found = 0;
    Fi(2,
      rec[i] = mg_record(d, s, i, tag, &pos[i]);
      if (mp_room(d, w, pos[i], tag)) {
        mp_add(d, w, pos[i], rec[i]);
        return 0;
      });
    if (walk == maxwalk) { *res = s;  *rtag = tag;  return 1; }
    *rng ^= *rng << 13;  *rng ^= *rng >> 7;  *rng ^= *rng << 17;
    c = (u32) (*rng & 1);
    Fi(2,
      x = pos[c];  n = mp_n(w, x);
      slot = (u32) ((*rng >> 8) % n);
      for (j = 0; j < n && !found; j++)
        if (mp_swap_ok(d, w, x, mp_rtag(d, mp_row(w, x)[slot]), tag))
          found = 1;
        else slot = (slot + 1) % n;
      if (found) break;
      c ^= 1);
    if (!found) { *res = s;  *rtag = tag;  return 1; }
    x = pos[c];  old = mp_row(w, x)[slot];
    mp_set(d, w, x, slot, rec[c]);
    s = mg_suffix(d, old, x);  tag = mp_rtag(d, old);
  }
}

static int mp_evict(const mkc_index * d, mp_work * w, u64 x, u32 nt,
                    u32 depth) {
  u32 pass, j;
  for (pass = 1; pass <= depth; pass++)
    for (j = mp_n(w, x); j-- > 0;) {
      u64 r = mp_row(w, x)[j], to, rec;
      u32 tag = mp_rtag(d, r);
      int slot;
      if ((r >> d->qbits) & 1 || !mp_swap_ok(d, w, x, tag, nt)) continue;
      rec = mg_record(d, (r & mkc_bits(d->qbits)) * d->cap + x, 1, tag, &to);
      if (to == x) continue;
      if (mp_room(d, w, to, tag)) mp_add(d, w, to, rec);
      else if (pass > 1
               && (slot = mp_evict(d, w, to, tag, pass - 1)) >= 0)
        mp_set(d, w, to, (u32) slot, rec);
      else continue;
      return (int) j;
    }
  return -1;
}

static int mp_place(const mkc_index * d, mp_work * w, u64 s, u32 tag) {
  u64 pos[2], want[2];
  u32 i;
  Fi(2, want[i] = mg_record(d, s, i, tag, &pos[i]));
  if (mp_room(d, w, pos[1], tag)) { mp_add(d, w, pos[1], want[1]);  return 1; }
  Fi(2,
    int slot = mp_evict(d, w, pos[i], tag, 1);
    if (slot >= 0) { mp_set(d, w, pos[i], (u32) slot, want[i]);  return 1; });
  return 0;
}

SI void mp_flag(const mkc_index * d, u8 * table, u64 s) {
  u64 q = mg_div(d, s);
  u8 * p = table + 64 * (s - q * d->cap);
  u64 w = mkc_load64(p);
  u32 lo, hi, bit;
  mp_filter(d, (u32) (w & 15), (u32) (w >> 4) & 3, &lo, &hi);
  if (lo >= hi) return;
  bit = mp_filter_bit(q, lo, hi);
  p[bit >> 3] |= (u8) (1u << (bit & 7));
}

SI void mp_poke(u64 * line, u32 at, u32 w, u64 v) {
  u32 x = at >> 6, s = at & 63;
  v &= mkc_bits(w);
  line[x] |= v << s;
  if (s + w > 64) line[x + 1] |= v >> (64 - s);
}

#define MP_CE(a, b) { const u64 p = x[a], q = x[b]; \
  x[a] = p < q ? p : q;  x[b] = p < q ? q : p; }

SI void mp_sort(const u64 * in, u32 n, u32 u, u64 * r) {
  u64 x[16];
  u32 i;
  Fi(16, x[i] = i < n ? in[i] << (64 - u) | in[i] >> u : UINT64_MAX);
  MP_CE(0, 1) MP_CE(2, 3) MP_CE(0, 2) MP_CE(1, 3) MP_CE(1, 2) MP_CE(4, 5)
  MP_CE(6, 7) MP_CE(4, 6) MP_CE(5, 7) MP_CE(5, 6) MP_CE(0, 4) MP_CE(2, 6)
  MP_CE(2, 4) MP_CE(1, 5) MP_CE(3, 7) MP_CE(3, 5) MP_CE(1, 2) MP_CE(3, 4)
  MP_CE(5, 6) MP_CE(8, 9) MP_CE(10, 11) MP_CE(8, 10) MP_CE(9, 11)
  MP_CE(9, 10) MP_CE(12, 13) MP_CE(14, 15) MP_CE(12, 14) MP_CE(13, 15)
  MP_CE(13, 14) MP_CE(8, 12) MP_CE(10, 14) MP_CE(10, 12) MP_CE(9, 13)
  MP_CE(11, 15) MP_CE(11, 13) MP_CE(9, 10) MP_CE(11, 12) MP_CE(13, 14)
  MP_CE(0, 8) MP_CE(4, 12) MP_CE(4, 8) MP_CE(2, 10) MP_CE(6, 14) MP_CE(6, 10)
  MP_CE(2, 4) MP_CE(6, 8) MP_CE(10, 12) MP_CE(1, 9) MP_CE(5, 13) MP_CE(5, 9)
  MP_CE(3, 11) MP_CE(7, 15) MP_CE(7, 11) MP_CE(3, 5) MP_CE(7, 9)
  MP_CE(11, 13) MP_CE(1, 2) MP_CE(3, 4) MP_CE(5, 6) MP_CE(7, 8) MP_CE(9, 10)
  MP_CE(11, 12) MP_CE(13, 14)
  Fi(n, r[i] = x[i] >> (64 - u) | x[i] << u);
}

#undef MP_CE

typedef struct { u64 * line, acc;  u32 at; } mp_writer;

SI void mp_write(mp_writer * o, u64 v, u32 w) {
  const u32 s = o->at & 63;
  v &= mkc_bits(w);
  o->acc |= v << s;
  if (s + w >= 64) {
    o->line[o->at >> 6] = o->acc;  o->acc = s ? v >> (64 - s) : 0;
  }
  o->at += w;
}

static void mp_encode(const mkc_index * d, u8 * table, const mp_work * w,
                      const u64 * st, sz nst, u64 ** sec, sz * cap2) {
  const u32 u = d->qbits + 1, lb = d->lbits, t = d->tagbits,
            hdr = MP_HEADER, base = mp_base(d);
  const u64 vm = mkc_bits(u);
  sz n2 = 0, k;
  u64 i;
  u32 j;
  Fi(d->cap,
    const u32 n = mp_n(w, i);
    u64 r[16], line[8] = {0}, upper = 0;
    u32 lidx[3], ltag[3], e = 0;
    mp_writer o;
    mp_sort(mp_row(w, i), n, u, r);
    if (*cap2 < n2 + 16) {
      *cap2 = 2 * (n2 + 16);
      *sec = xrealloc(*sec, *cap2 * 8);
    }
    Fj(n,
      const u64 v = r[j] & vm;
      const u32 tag = (u32) (r[j] >> u);
      upper |= UINT64_C(1) << ((v >> lb) + j);
      if (tag != 1) { lidx[e] = j;  ltag[e++] = tag; }
      if (v >> d->qbits) (*sec)[n2++] = mg_suffix(d, r[j], i));
    o.line = line;  o.acc = n | (u64) e << 4 | upper << hdr;  o.at = base;
    Fj(n, mp_write(&o, r[j], lb));
    if (o.at & 63 || !o.at) line[o.at >> 6] = o.acc;
    Fj(e,
      mp_poke(line, 508 - 4 * j, 4, lidx[j]);
      mp_poke(line, 512 - 4 * e - (j + 1) * t, t, ltag[j]));
    Fj(8, wr64le(table + 64 * i + 8 * j, line[j])));
  Fk(n2, mp_flag(d, table, (*sec)[k]));
  Fk(nst, mp_flag(d, table, st[k]));
}

typedef struct { u64 key, count; } pair;
typedef struct { u64 s;  u32 tag, tier; } placed;
typedef struct { mkc_file f;  u64 n; } exc_file;
typedef struct {
  exc_file * file;
  u32 used;
  u8 buf[16 * 1024];
} exc_batch;

static mkc_mutex exc_lock;

static exc_file exc_open(const char * beside) {
  sz n = strlen(beside) + 8;
  char * p = xmalloc(n), * path;
  exc_file e = {MKC_NO_FILE, 0};
  snprintf(p, n, "%s.exc.", beside);
  path = os_mkunique(p, 0, MKC_TEMPORARY, &e.f);
  if (!path) FATAL("exception temporary: %s", os_error());
  xfree(path);  xfree(p);
  return e;
}

static void exc_flush(exc_batch * b) {
  if (!b->used) return;
  mx_lock(&exc_lock);
  write_at(b->file->f, b->buf, 16 * (sz) b->used, 16 * b->file->n);
  b->file->n += b->used;
  mx_unlock(&exc_lock);
  b->used = 0;
}

static void exc_add(exc_batch * b, u64 key, u64 count) {
  wr64le(b->buf + 16 * b->used, key);
  wr64le(b->buf + 16 * b->used + 8, count);
  if (++b->used == 1024) exc_flush(b);
}

typedef struct {
  const exc_file * e;
  u64 next;
  sz pos, end;
  u8 buf[65536];
} row_rd;

static const u8 * row_next(row_rd * r) {
  if (r->pos == r->end) {
    u64 left = 16 * (r->e->n - r->next);
    sz take = left < sizeof r->buf ? (sz) left : sizeof r->buf;
    if (!take) FATAL("exception read failed");
    read_at(r->e->f, r->buf, take, 16 * r->next);
    r->pos = 0;  r->end = take;  r->next += take / 16;
  }
  r->pos += 16;
  return r->buf + r->pos - 16;
}

static row_rd * row_open(const exc_file * e) {
  row_rd * r = xmalloc(sizeof *r);
  r->e = e;  r->next = 0;  r->pos = r->end = 0;
  return r;
}

static void exc_sort(exc_file * e, u64 budget) {
  sz n, i;
  pair * a, * t, * x;
  u8 * rows;
  u32 shift, j;
  if (e->n < 2 || 32 * e->n > budget || 32 * e->n > SIZE_MAX) return;
  n = (sz) e->n;
  a = xmalloc(n * sizeof (pair));  t = xmalloc(n * sizeof (pair));
  rows = xmalloc(16 * n);
  read_at(e->f, rows, 16 * n, 0);
  Fi(n,
    a[i].key = mkc_load64(rows + 16 * i);
    a[i].count = mkc_load64(rows + 16 * i + 8));
  for (shift = 0; shift < 64; shift += 8) {
    sz count[257] = {0};
    Fi(n, count[((a[i].key >> shift) & 255) + 1]++);
    if (count[((a[0].key >> shift) & 255) + 1] == n) continue;
    Fj(256, count[j + 1] += count[j]);
    Fi(n, t[count[(a[i].key >> shift) & 255]++] = a[i]);
    x = a;  a = t;  t = x;
  }
  Fi(n,
    wr64le(rows + 16 * i, a[i].key);
    wr64le(rows + 16 * i + 8, a[i].count));
  write_at(e->f, rows, 16 * n, 0);
  xfree(a);  xfree(t);  xfree(rows);
}

static void exc_copy(mkc_file fd, exc_file * e, u64 off) {
  u8 * buf = xmalloc(1u << 20);
  u64 at, left;
  for (at = 0, left = 16 * e->n; left;) {
    sz take = left > (1u << 20) ? (1u << 20) : (sz) left;
    read_at(e->f, buf, take, at);
    write_at(fd, buf, take, off + at);  left -= take;  at += take;
  }
  xfree(buf);
}

static void exc_dict(mkc_file fd, exc_file ** lists, u32 nlists,
                     u64 hcap, u64 off, u64 budget) {
  u64 bytes, j;
  u8 * dict;
  u32 i;
  if (!hcap) return;
  bytes = 16 * hcap;
  dict = bytes <= budget && bytes <= SIZE_MAX ? calloc(1, (sz) bytes) : NULL;
  Fi(nlists,
    row_rd * rows = row_open(lists[i]);
    Fj(lists[i]->n,
      u8 old[16];
      const u8 * row = row_next(rows);
      u64 id = mkc_load64(row), pos = mkc_mix(id) & (hcap - 1);
      for (;;) {
        u8 * slot = dict ? dict + 16 * pos : old;
        if (!dict) read_at(fd, old, 16, off + 16 * pos);
        if (!mkc_load64(slot + 8)) {
          if (dict) memcpy(slot, row, 16);
          else write_at(fd, row, 16, off + 16 * pos);
          break;
        }
        if (mkc_load64(slot) == id) {
          if (memcmp(slot, row, 16)) FATAL("conflicting exception");
          break;
        }
        pos = (pos + 1) & (hcap - 1);
      });
    xfree(rows));
  if (dict) { write_at(fd, dict, (sz) bytes, off);  xfree(dict); }
}

typedef struct {
  u8 * image;
  u64 at, length;
  sz skip;
  int ready;
} out_job;

static struct {
  mkc_mutex lock;  mkc_cond freed, queued;
  u8 ** idle;  sz nfree, bytes;
  out_job * jobs;  sz head, tail, cap;
  int ordered, stop;  mkc_file fd;
} out_pool;

#ifdef _WIN32
#define OUT_WRITERS 1u
#define OUT_ORDERED 1
#else
#define OUT_WRITERS 2u
#define OUT_ORDERED 0
#endif

static u8 * out_take(void) {
  u8 * p;
  mx_lock(&out_pool.lock);
  while (!out_pool.nfree) cv_wait(&out_pool.freed, &out_pool.lock);
  p = out_pool.idle[--out_pool.nfree];
  mx_unlock(&out_pool.lock);
  return p;
}

static void out_give(u8 * p) {
  mx_lock(&out_pool.lock);
  out_pool.idle[out_pool.nfree++] = p;
  cv_signal(&out_pool.freed);  mx_unlock(&out_pool.lock);
}

static void out_submit(u32 unit, out_job job) {
  job.ready = 1;
  mx_lock(&out_pool.lock);
  if (out_pool.ordered) out_pool.jobs[unit] = job;
  else out_pool.jobs[out_pool.tail++ % out_pool.cap] = job;
  cv_bcast(&out_pool.queued);  mx_unlock(&out_pool.lock);
}

static void * out_writer(void * unused) {
  for (;;) {
    out_job job;
    mx_lock(&out_pool.lock);
    for (;;) {
      if (out_pool.ordered ? out_pool.head < out_pool.tail
                             && out_pool.jobs[out_pool.head].ready
                           : out_pool.head != out_pool.tail)
        break;
      if (out_pool.stop) { mx_unlock(&out_pool.lock);  return NULL; }
      cv_wait(&out_pool.queued, &out_pool.lock);
    }
    job = out_pool.jobs[out_pool.ordered ? out_pool.head
                                         : out_pool.head % out_pool.cap];
    out_pool.head++;
    mx_unlock(&out_pool.lock);
    if (job.length)
      write_at(out_pool.fd, job.image + job.skip, (sz) job.length, job.at);
    out_give(job.image);
  }
}

typedef struct {
  const assembly * a;  mkc_index * d;  const u64 * sizes, * records;
  u32 step, units, low;  sz maxb;
  int direct;  u8 * boundary;
  exc_file * over, * stash, * directs;
  u32 sent, maxwalk, imax, dslots;
  u32 next;
  struct { u64 seen, check;  u128 sum;  char pad[64]; } * totals;
} hash_job;

static void hash_worker(void * arg, u32 id, u32 m) {
  hash_job * jb = (hash_job *) arg;
  const assembly * a = jb->a;
  const mkc_index * d = jb->d;
  const u32 low = jb->low, step = jb->step, sent = jb->sent,
            maxwalk = jb->maxwalk, imax = jb->imax, dslots = jb->dslots;
  const int direct = jb->direct;
  u64 seen = 0, check = 0;
  u128 sum = 0;
  u8 * buf = xmalloc(jb->maxb + 16), * used = xmalloc((sz) d->cap);
  const int packed = d->packed != 0;
  mp_work work = {NULL};
  u64 * st = NULL, * sec = NULL, * dcnt = NULL, * ones = NULL;
  sz nst = 0, stcap = 0, cap2 = 0, slcap = 0, lgcap = 0, ovcap = 0,
     dcap = 0, ocap = 0, onecap = 0, i, j;
  slice * slices = NULL;
  pair * large = NULL, * ovf = NULL;
  placed * order = NULL, * arr = NULL;
  const u32 sbits = d->sbits;
  const u64 smask = mkc_bits(sbits);
  exc_batch * ob, * sb, * db;
  u64 align, k;
  slice_map * maps;
  u32 unit;
  if (packed) { work.rec = xpages((sz) d->cap * 128);  mp_limits(d, &work); }
  ob = xmalloc(sizeof *ob);  sb = xmalloc(sizeof *sb);
  db = xmalloc(sizeof *db);
  ob->file = jb->over;  ob->used = 0;  sb->file = jb->stash;  sb->used = 0;
  db->file = jb->directs;  db->used = 0;
  align = os_map_align();
  maps = xcalloc(step, sizeof *maps);
  while (!mg_gave_up && (unit = fetch_add(&jb->next, 1u)) < jb->units) {
    u32 b = unit * step;
    u64 rng = mkc_mix(UINT64_C(714924) + b);
    u64 g0, g1, from, to, vb, g;
    sz ns, vlen;
    u8 * view;
    slices = sl_load(b, step, jb->sizes, jb->records, buf, &ns, slices,
                     &slcap, maps);
    g0 = ((u64) b << low) >> d->sbits;
    g1 = ((((u64) (b + step)) << low) - 1) >> d->sbits;
    from = 128 + 64 * d->cap * g0;
    to = 128 + 64 * d->cap * (g1 + 1);
    vb = from & ~(direct ? (u64) 4095 : align - 1);
    vlen = (sz) (to - vb);
    view = direct ? out_take() : os_map(a->fd, vb, vlen, 1);
    if (!view) FATAL("map index: %s", os_error());
    for (g = g0; g <= g1 && !mg_gave_up; g++) {
      sz nlg = 0, nov = 0, ndc = 0, narr = 0, n1 = 0, start[65] = {0};
      u8 * table = view + (128 + 64 * d->cap * g - vb);
      const u32 slots = d->slots, bits = d->bits;
      u32 t;
      if (direct) memset(table, 0, (sz) d->cap * 64);
      memset(used, 0, (sz) d->cap);
      if (packed) Fk(d->cap, work.rec[16 * k] = 0);
      nst = 0;
      Fi(ns,
        slice local, * r;
        if (!slices[i].live || (slices[i].h >> sbits) != g) continue;
        local = slices[i];  r = &local;
        while (r->live && (r->h >> sbits) == g) {
          u64 h = r->h, c = r->count;
          u32 tag, tier;
          sl_next(r, low);
          sum += c;  seen++;
          check += mkc_mix(h ^ (c == 1 ? mkc_mix(1) : mkc_mix(c)));
          tag = (u32) c;
          if (c > imax && ndc < dslots) {
            if (ndc == dcap) {
              dcap = dcap ? 2 * dcap : 256;
              dcnt = xrealloc(dcnt, dcap * 8);
            }
            dcnt[ndc] = c;
            tag = imax + 1 + (u32) ndc++;
          } else if (c > imax) {
            if (nlg == lgcap) {
              lgcap = lgcap ? 2 * lgcap : 256;
              large = xrealloc(large, lgcap * sizeof *large);
            }
            large[nlg++] = (pair) {h, c};
            exc_add(ob, h, c);  tag = sent;
          }
          if (c == 1) {
            if (UNLIKELY(n1 == onecap)) {
              onecap = onecap ? 2 * onecap : 65536;
              ones = xrealloc(ones, onecap * sizeof *ones);
            }
            ones[n1++] = h & smask;
            continue;
          }
          if (narr == ocap) {
            ocap = ocap ? 2 * ocap : 65536;
            arr = xrealloc(arr, ocap * sizeof *arr);
            order = xrealloc(order, ocap * sizeof *order);
          }
          tier = (u32) (63 - __builtin_clzll(c));
          arr[narr++] = (placed) {h & smask, tag, tier};
        }
        slices[i] = local);
      Fi(narr, start[64 - arr[i].tier]++);
      for (t = 1; t <= 64; t++) start[t] += start[t - 1];
      Fi(narr, order[start[63 - arr[i].tier]++] = arr[i]);
      Fi(2,
        const sz count = i ? n1 : narr;
        Fj(count,
          u64 s = i ? ones[j] : order[j].s, pos;
          u32 tag = i ? 1u : order[j].tag;
          u64 rec = mg_record(d, s, 0, tag, &pos);
          if (packed ? mp_room(d, &work, pos, tag) : used[pos] < slots) {
            if (packed) mp_add(d, &work, pos, rec);
            else mg_put(table + 64 * pos, used[pos]++, bits, rec);
          } else {
            if (nov == ovcap) {
              ovcap = ovcap ? 2 * ovcap : 1024;
              ovf = xrealloc(ovf, ovcap * sizeof *ovf);
            }
            ovf[nov++] = (pair) {s, tag};
          }));
      for (i = 0; i < nov && !mg_gave_up; i++) {
        u64 s = ovf[i].key, res, h, count;
        u32 tag = (u32) ovf[i].count, rtag;
        if (packed ? mp_place(d, &work, s, tag)
                   : mg_place(d, table, used, s, tag))
          continue;
        if (packed ? !mp_insert(d, &work, s, tag, maxwalk, &rng, &res, &rtag)
                   : !mg_insert(d, table, used, s, tag, maxwalk, &rng, &res,
                                &rtag))
          continue;
        h = g << d->sbits | res;  count = rtag;
        if (rtag == sent) {
          count = 0;
          Fj(nlg, if (large[j].key == h) { count = large[j].count;  break; });
          if (!count) FATAL("lost displaced key");
        } else if (rtag > imax) {
          count = dcnt[rtag - imax - 1];
        }
        exc_add(sb, h, count);
        if (mg_limit) {
          mx_lock(&exc_lock);
          if (++mg_seen > mg_limit) mg_gave_up = 1;
          mx_unlock(&exc_lock);
        }
        if (packed) {
          if (nst == stcap) {
            stcap = stcap ? 2 * stcap : 64;
            st = xrealloc(st, stcap * 8);
          }
          st[nst++] = res;
        } else if (d->flags) {
          mg_flag(d, table, res);
        }
      }
      if (packed)
        mp_encode(d, table, &work, st, nst, &sec, &cap2);
      else if (d->flags) mg_flags(d, table);
      Fi(ndc, exc_add(db, g << 24 | i, dcnt[i]));
    }
    if (direct) {
      u64 head = (from + 4095) & ~(u64) 4095,
          tail = to & ~(u64) 4095;
      memcpy(jb->boundary + 4096 * (sz) unit + (from & 4095),
             view + (from - vb), (sz) (head - from));
      memcpy(jb->boundary + 4096 * ((sz) unit + 1), view + (tail - vb),
             (sz) (to - tail));
      out_submit(unit, (out_job) {view, head, tail > head ? tail - head : 0,
                                  (sz) (head - vb), 0});
    } else if (os_unmap(view, vlen)) {
      FATAL("unmap index: %s", os_error());
    }
    sl_done(slices, ns);
    sl_unmap(maps, step);
  }
  exc_flush(ob);  exc_flush(sb);  exc_flush(db);
  if (work.rec) os_pages_free(work.rec, (sz) d->cap * 128);
  xfree(st);  xfree(sec);
  xfree(ob);  xfree(sb);  xfree(db);  xfree(buf);  xfree(maps);  xfree(used);
  xfree(slices);  xfree(large);  xfree(ovf);  xfree(dcnt);
  xfree(order);  xfree(arr);  xfree(ones);
  jb->totals[id].seen = seen;  jb->totals[id].check = check;
  jb->totals[id].sum = sum;
}

static u64 asm_hash(const assembly * a, mkc_index * d, const u64 * sizes,
                    const u64 * records) {
  const u32 low = spool.low, cb = SPOOL_BITS, parts = SPOOL_PARTS;
  const u32 sent = (1u << d->tagbits) - 1,
            imax = d->tagbits < MG_FULL_TAG
                   ? sent - 1 : (1u << (d->tagbits - 1)) - 1;
#ifdef MKC_TEST_DIRECT_SLOTS
  const u32 dslots = d->tagbits < MG_FULL_TAG ? 0 : MKC_TEST_DIRECT_SLOTS;
#else
  const u32 dslots = sent - 1 - imax;
#endif
  u32 step = cb > d->gbits ? 1u << (cb - d->gbits) : 1,
      units = parts / step, workers, u, i, j;
  u64 maxb = 0, per, seen = 0, check = 0, size;
  u64 boff = 0, coff = 0;
  u128 sum = 0;
  int direct;
  mkc_file dfd;
  mkc_thread writers[OUT_WRITERS];
  u8 * boundary = NULL;
  sz bbytes = 4096 * ((sz) units + 1), images = 0;
  exc_file over, stash, directs, * lists[2] = {&over, &stash};
  hash_job job;
  u8 h[128] = {0};
  Fi(units,
    u64 need = 0;
    Fj(step, need += sizes[i * step + j]);
    if (need > maxb) maxb = need);
  workers = asm_workers(a, maxb);
  if (workers > units) workers = units;
  d->table_end = 128 + 64 * d->groups * d->cap;
  if (os_set_size(a->fd, d->table_end)) FATAL("reserve index: %s", os_error());
  per = d->groups / units ? d->groups / units : 1;
  direct = d->groups >= units && 64 * d->cap * per >= 8192;
  dfd = direct ? os_open(a->path, MKC_WRITE | MKC_DIRECT) : MKC_NO_FILE;
  if (dfd == MKC_NO_FILE
      || os_reserve(a->fd, (d->table_end + 4095) & ~(u64) 4095)) {
    if (dfd != MKC_NO_FILE) os_close(dfd);
    dfd = MKC_NO_FILE;  direct = 0;
  }
  if (direct) {
    sz k;
    boundary = xpages(bbytes);
    images = (sz) workers + OUT_WRITERS + 2;
    mx_init(&out_pool.lock);  cv_init(&out_pool.freed);
    cv_init(&out_pool.queued);
    out_pool.bytes = (sz) (64 * d->cap * per) + 8192;
    out_pool.ordered = OUT_ORDERED;  out_pool.stop = 0;  out_pool.head = 0;
    out_pool.cap = out_pool.ordered ? units : images + 1;
    out_pool.tail = out_pool.ordered ? units : 0;
    out_pool.idle = xmalloc(images * sizeof *out_pool.idle);
    out_pool.jobs = xcalloc(out_pool.cap, sizeof *out_pool.jobs);
    out_pool.fd = dfd;
    Fk(images, out_pool.idle[k] = xpages(out_pool.bytes));
    out_pool.nfree = images;
    Fi(OUT_WRITERS, if (th_start(&writers[i], out_writer, NULL, i))
                      FATAL("start writer thread"));
  }
  over = exc_open(a->path);  stash = exc_open(a->path);
  directs = exc_open(a->path);
  job = (hash_job) {a, d, sizes, records, step, units, low, (sz) maxb,
                    direct, boundary, &over, &stash, &directs, sent, 256,
                    imax, dslots, 0, NULL};
  job.totals = xcalloc(workers, sizeof *job.totals);
  tp_run(a->pool, workers, hash_worker, &job);
  Fi(workers,
    seen += job.totals[i].seen;  check += job.totals[i].check;
    sum += job.totals[i].sum);
  xfree(job.totals);
  if (direct) {
    sz k;
    mx_lock(&out_pool.lock);  out_pool.stop = 1;
    cv_bcast(&out_pool.queued);  mx_unlock(&out_pool.lock);
    Fi(OUT_WRITERS, th_join(writers[i]));
    if (out_pool.nfree != images) FATAL("output images lost");
    Fk(out_pool.nfree, os_pages_free(out_pool.idle[k], out_pool.bytes));
    xfree(out_pool.idle);  xfree(out_pool.jobs);
    mx_free(&out_pool.lock);  cv_free(&out_pool.freed);
    cv_free(&out_pool.queued);
    for (u = 0; u <= units; u++) {
      u64 start = 128 + 64 * d->cap * ((u64) u * per);
      u64 at = u < units ? start & ~(u64) 4095
                         : d->table_end & ~(u64) 4095;
      if (u == units && at == d->table_end) break;
      if (u > 0 && u < units && !(start & 4095)) continue;
      write_at(dfd, boundary + 4096 * (sz) u, 4096, at);
    }
    os_pages_free(boundary, bbytes);
    if (os_close(dfd)) FATAL("index write: %s", os_error());
  }
  if (mg_gave_up) { nstash = mg_seen;  return 0; }
  if (seen != a->distinct || sum != a->total || check != a->checksum)
    FATAL("spool integrity mismatch");
  d->over = over.n;  d->stash = stash.n;  nstash = stash.n;
  d->stash_off = d->table_end + 16 * d->over;
  d->hash_off = d->stash_off + 16 * d->stash;
  d->hcap = dict_slots(d->over + d->stash);
  size = d->hash_off + 16 * d->hcap;
  if (size > INT64_MAX || os_set_size(a->fd, size))
    FATAL("reserve exceptions: %s", os_error());
  exc_sort(&over, a->budget);  exc_sort(&stash, a->budget);
  exc_copy(a->fd, &over, d->table_end);
  exc_copy(a->fd, &stash, d->stash_off);
  exc_dict(a->fd, lists, 2, d->hcap, d->hash_off, a->budget);
  os_close(over.f);  os_close(stash.f);
  if (directs.n) {
    u64 * base, k, g;
    row_rd * rows;
    u8 * counts;
    boff = size;  coff = boff + 8 * (d->groups + 1);
    size = coff + 8 * directs.n;
    if (size > INT64_MAX || os_set_size(a->fd, size))
      FATAL("reserve direct counts: %s", os_error());
    base = xcalloc(d->groups + 1, 8);
    rows = row_open(&directs);
    Fk(directs.n, base[(mkc_load64(row_next(rows)) >> 24) + 1]++);
    Fk(d->groups, base[k + 1] += base[k]);
    counts = 8 * directs.n <= a->budget ? malloc((sz) (8 * directs.n)) : NULL;
    rows->next = 0;  rows->pos = rows->end = 0;
    Fk(directs.n,
      const u8 * row = row_next(rows);
      u64 key = mkc_load64(row), at = base[key >> 24] + (key & 0xFFFFFF);
      if (counts) memcpy(counts + 8 * at, row + 8, 8);
      else write_at(a->fd, row + 8, 8, coff + 8 * at));
    xfree(rows);
    if (counts) {
      write_at(a->fd, counts, (sz) (8 * directs.n), coff);
      xfree(counts);
    }
    for (g = 0; g <= d->groups; g++) wr64le((u8 *) &base[g], base[g]);
    write_at(a->fd, base, (sz) (8 * (d->groups + 1)), boff);
    xfree(base);
  }
  os_close(directs.f);
  memcpy(h, d->packed ? "MKCGLOB3" : d->bits % 8 ? "MKCGLOB2" : "MKCGLOB1", 8);
  h[16] = (u8) d->k;  h[17] = (u8) d->gbits;  h[18] = (u8) d->tagbits;
  h[19] = (u8) d->qbits;
  h[20] = (u8) (d->packed ? 0 : d->bits % 8 ? d->bits : d->bits / 8);
  h[21] = (u8) d->slots;  h[22] = (u8) d->flags;  h[23] = (u8) table_seed;
  wr64le(h + 24, a->distinct);  wr64le(h + 32, a->total);
  wr64le(h + 40, d->cap);  wr64le(h + 48, d->over);
  wr64le(h + 56, d->stash);  wr64le(h + 64, d->hcap);
  wr64le(h + 72, d->table_end);  wr64le(h + 80, d->stash_off);
  wr64le(h + 88, d->hash_off);  wr64le(h + 96, size);
  if (boff) {
    wr64le(h + 104, boff);  wr64le(h + 112, coff);
    wr64le(h + 120, imax);
  }
  write_at(a->fd, h, 128, 0);
  return size;
}

static u64 dense_size(u32 width, u32 hbits, u64 n) {
  return mkc_dense_end(width, hbits) + 16 * dict_slots(n);
}

static u32 dense_width(u32 hbits, const u64 above[4], u64 * size) {
  static const u32 widths[4] = {1, 2, 4, 8};
  u32 best = 8, i;
  *size = dense_size(8, hbits, 0);
  Fi(3,
    u64 s = dense_size(widths[i], hbits, above[i + 1]);
    if (s < *size) { *size = s;  best = widths[i]; });
  return best;
}

static u64 dense_finish(mkc_file fd, u32 kl, u32 width,
                        u64 distinct, u64 total, exc_file * e,
                        u64 budget) {
  u32 hbits = mkc_code_bits(kl);
  u64 data = mkc_dense_end(width, hbits),
      n = e ? e->n : 0, hcap = dict_slots(n);
  u64 size = data + 16 * hcap;
  u8 h[128] = {0};
  if (os_set_size(fd, size)) FATAL("reserve index: %s", os_error());
  if (n) {
    exc_sort(e, budget);
    exc_dict(fd, &e, 1, hcap, data, budget);
  }
  memcpy(h, "MKCDENS1", 8);  h[16] = (u8) kl;  h[17] = (u8) width;
  wr64le(h + 24, distinct);  wr64le(h + 32, total);
  wr64le(h + 40, UINT64_C(1) << hbits);  wr64le(h + 48, n);
  wr64le(h + 56, hcap);  wr64le(h + 64, 128);  wr64le(h + 72, data);
  wr64le(h + 80, size);
  write_at(fd, h, 128, 0);
  return size;
}

typedef struct {
  const assembly * a;  u32 width;  const u64 * sizes, * records;
  sz maxb;
  u8 * counts;  exc_file * over;  u32 next;
  struct { u64 seen, check;  u128 sum;  char pad[64]; } * totals;
} dense_job;

static void dense_worker(void * arg, u32 id, u32 m) {
  dense_job * jb = (dense_job *) arg;
  const u32 low = spool.low, hbits = jb->a->hbits, width = jb->width;
  const u64 imax = mkc_bits(8 * width);
  u64 seen = 0, check = 0;
  u128 sum = 0;
  u8 * buf = xmalloc(jb->maxb + 16);
  slice * slices = NULL;
  sz cap = 0, n, i;
  exc_batch * ob = xmalloc(sizeof *ob);
  u32 part;
  ob->file = jb->over;  ob->used = 0;
  while ((part = fetch_add(&jb->next, 1u)) < SPOOL_PARTS) {
    slice_map map = {NULL, 0};
    slice * r;
    slices = sl_load(part, 1, jb->sizes, jb->records, buf, &n, slices, &cap,
                     &map);
    Fi(n, for (r = &slices[i]; r->live; sl_next(r, low)) {
      u64 code = mkc_unhash(r->h, hbits)
                 ^ mkc_seed_word(table_seed, hbits), c = r->count;
      sum += c;  seen++;  check += mkc_mix(r->h ^ mkc_mix(c));
      if (c > imax) exc_add(ob, code, c);
      mg_store(jb->counts + 128 + width * code, width,
               c < imax ? c : imax);
    });
    sl_done(slices, n);
    sl_unmap(&map, 1);
  }
  exc_flush(ob);  xfree(ob);  xfree(buf);  xfree(slices);
  jb->totals[id].seen = seen;  jb->totals[id].check = check;
  jb->totals[id].sum = sum;
}

static u64 asm_dense(const assembly * a, u32 width, const u64 * sizes,
                     const u64 * records) {
  const u32 hbits = a->hbits;
  u64 data = mkc_dense_end(width, hbits), maxb = 0, seen = 0,
      check = 0, size;
  u128 sum = 0;
  u32 i, workers;
  u8 * counts;
  exc_file over;
  dense_job job;
  Fi(SPOOL_PARTS, if (sizes[i] > maxb) maxb = sizes[i]);
  workers = asm_workers(a, maxb);
  if (os_set_size(a->fd, data)) FATAL("reserve index: %s", os_error());
  counts = os_map(a->fd, 0, (sz) data, 1);
  if (!counts) FATAL("map index: %s", os_error());
  over = exc_open(a->path);
  job = (dense_job) {a, width, sizes, records, (sz) maxb, counts, &over,
                     0, NULL};
  job.totals = xcalloc(workers, sizeof *job.totals);
  tp_run(a->pool, workers, dense_worker, &job);
  Fi(workers,
    seen += job.totals[i].seen;  check += job.totals[i].check;
    sum += job.totals[i].sum);
  xfree(job.totals);
  if (seen != a->distinct || sum != a->total || check != a->checksum)
    FATAL("spool integrity mismatch");
  if (os_unmap(counts, (sz) data)) FATAL("unmap index: %s", os_error());
  size = dense_finish(a->fd, a->k, width, a->distinct, a->total, &over,
                      a->budget);
  os_close(over.f);
  return size;
}

static u64 assemble(const assembly * a, int * dense) {
  u64 sizes[SPOOL_PARTS], records[SPOOL_PARTS], n = 0;
  u64 hest, dbytes = UINT64_MAX;
  u32 i, width = 0;
  mkc_index d;
  int have;
  Fi(SPOOL_PARTS,
    sizes[i] = spool.sizes[i];  records[i] = spool.records[i];
    n += records[i]);
  if (n != a->distinct) FATAL("spool record mismatch");
  have = !mg_shape(&d, a->k, a->distinct, a->above);
  hest = have ? 128 + 64 * d.groups * d.cap + 16 * a->above[0]
                + 16 * dict_slots(a->above[0])
              : UINT64_MAX;
  if (a->hbits <= 40) width = dense_width(a->hbits, a->above, &dbytes);
  *dense = width && dbytes <= hest;
#ifdef MKC_TEST_LAYOUT
  if (MKC_TEST_LAYOUT == 1 && width) *dense = 1;
  if (MKC_TEST_LAYOUT == 2 && have) *dense = 0;
#endif
  if (*dense) return asm_dense(a, width, sizes, records);
  if (!have)
    FATAL("no index geometry fits %" PRIu64 " k-mers", a->distinct);
  return asm_hash(a, &d, sizes, records);
}
