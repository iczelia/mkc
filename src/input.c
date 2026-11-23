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


#include <igzip_lib.h>
#include <zstd.h>
#include <lz4.h>
#include <lz4frame.h>
#ifdef MKC_XXH_LZ4
#define XXH_NAMESPACE LZ4_
#endif
#include <xxhash.h>

#ifndef MKC_READ_BLOCK
#define MKC_READ_BLOCK (8u << 20)
#endif
#define READ_BLOCK ((sz) MKC_READ_BLOCK)
#define READ_RING 16u
#define PAR_SLOTS 8u
#define RAW_ALIGN ((sz) 4096)

typedef struct {
  const u8 * src;  sz csize, cap, len;
  u32 slot;  sz at;  int kind;
} unit;

enum { UNIT_GZIP, UNIT_ZSTD, UNIT_LZ4, UNIT_STORED };

typedef struct {
  const char * name;
  mkc_file fd;  int direct, regular;  u64 offset;
  mkc_mutex mutex;  mkc_cond cond;  mkc_thread thread;
  u32 head, tail;  int done, stop;
  u8 * blocks[READ_RING];  sz sizes[READ_RING];
  u8 * raw;  sz rcap, pos, end;  int eof;
  struct inflate_state ** infl;  ZSTD_DCtx ** zdc;
  unit * units;  sz ucap;
  const char * format;  u64 decoded, parallel;
} input;

static struct { mkc_mutex lock;  mkc_pool * pool;  u32 size; } decoders;

static mkc_pool * decode_pool(void) {
  mx_lock(&decoders.lock);
  if (!decoders.pool && !(decoders.pool = tp_new(decoders.size)))
    FATAL("cannot start decoding threads");
  mx_unlock(&decoders.lock);
  return decoders.pool;
}

static u32 le32(const u8 * p) { return rd32le(p); }

static ptrdiff_t input_read(input * in, u8 * dst, sz n) {
  for (;;) {
    ptrdiff_t got = in->regular ? os_pread(in->fd, dst, n, in->offset)
                                : os_read(in->fd, dst, n);
    if (got >= 0) {
      in->offset += (u64) got;
      if (got && in->regular && !in->direct)
        os_will_need(in->fd, in->offset, (u64) READ_RING * READ_BLOCK);
      return got;
    }
    if (in->direct && !os_set_direct(&in->fd, 0)) { in->direct = 0;  continue; }
    FATAL("%s: input read error: %s", in->name, os_error());
  }
}

static void raw_more(input * in, sz want) {
  while (in->end - in->pos < want && !in->eof) {
    sz have = in->end - in->pos, at = (have + RAW_ALIGN - 1) & ~(RAW_ALIGN - 1);
    sz need = at + (want > have ? want - have : 0) + READ_BLOCK;
    ptrdiff_t got;
    if (need > in->rcap) {
      sz cap = (need + need / 2 + RAW_ALIGN - 1) & ~(RAW_ALIGN - 1);
      u8 * raw = xpages(cap);
      memcpy(raw + at - have, in->raw + in->pos, have);
      os_pages_free(in->raw, in->rcap);
      in->raw = raw;  in->rcap = cap;
    } else memmove(in->raw + at - have, in->raw + in->pos, have);
    in->pos = at - have;  in->end = at;
    got = input_read(in, in->raw + at, (in->rcap - at) & ~(RAW_ALIGN - 1));
    if (!got) in->eof = 1;
    in->end += (sz) got;
  }
}

static u32 ring_free(input * in, u32 max) {
  u32 n;
  mx_lock(&in->mutex);
  while (in->head - in->tail == READ_RING && !in->stop)
    cv_wait(&in->cond, &in->mutex);
  n = in->stop ? 0 : READ_RING - (in->head - in->tail);
  mx_unlock(&in->mutex);
  return n < max ? n : max;
}

static u8 * ring_block(input * in, u32 at) {
  return in->blocks[(in->head + at) % READ_RING];
}

static void ring_publish(input * in, u32 n) {
  if (!n) return;
  mx_lock(&in->mutex);
  in->head += n;
  cv_bcast(&in->cond);
  mx_unlock(&in->mutex);
}

typedef struct { u8 * at;  sz used; } outblock;

static int out_open(input * in, outblock * o) {
  if (o->at) return 1;
  if (!ring_free(in, 1)) return 0;
  o->at = ring_block(in, 0);  o->used = 0;
  return 1;
}

static void out_flush(input * in, outblock * o) {
  if (o->at && o->used) {
    in->sizes[in->head % READ_RING] = o->used;  in->decoded += o->used;
    ring_publish(in, 1);
  }
  o->at = NULL;  o->used = 0;
}

typedef struct {
  input * in;  sz n, next;  u32 failed;  sz bad;  const char * why;
} ubatch;

static int unit_decode(input * in, u32 id, unit * u, const char ** why) {
  u8 * dst = ring_block(in, u->slot) + u->at;
  switch (u->kind) {
  case UNIT_GZIP: {
    struct inflate_state * st = in->infl[id];
    int ret;
    isal_inflate_init(st);  st->crc_flag = ISAL_GZIP;
    st->next_in = (u8 *) u->src;  st->avail_in = (u32) u->csize;
    st->next_out = dst;  st->avail_out = (u32) u->cap;
    ret = isal_inflate_stateless(st);
    if (ret != ISAL_DECOMP_OK || st->avail_in || st->total_out != u->cap) {
      *why = "corrupt BGZF block";  return -1;
    }
    u->len = u->cap;
    return 0;
  }
  case UNIT_ZSTD: {
    sz got = ZSTD_decompressDCtx(in->zdc[id], dst, u->cap, u->src, u->csize);
    if (ZSTD_isError(got)) { *why = ZSTD_getErrorName(got);  return -1; }
    if (got != u->cap) {
      *why = "zstd frame shorter than its content size";  return -1;
    }
    u->len = got;
    return 0;
  }
  case UNIT_LZ4: {
    int got = LZ4_decompress_safe((const char *) u->src, (char *) dst,
                                  (int) u->csize, (int) u->cap);
    if (got < 0) { *why = "corrupt lz4 block";  return -1; }
    u->len = (sz) got;
    return 0;
  }
  default:
    memcpy(dst, u->src, u->csize);  u->len = u->csize;
    return 0;
  }
}

static void batch_task(void * arg, u32 id, u32 m) {
  ubatch * b = (ubatch *) arg;
  sz x;
  while ((x = fetch_add(&b->next, (sz) 1)) < b->n) {
    const char * why = NULL;
    if (unit_decode(b->in, id, &b->in->units[x], &why)
        && !fetch_add(&b->failed, 1u)) { b->bad = x;  b->why = why; }
  }
}

static unit * unit_add(input * in, sz n) {
  if (n == in->ucap) {
    in->ucap = in->ucap ? 2 * in->ucap : 1024;
    in->units = xrealloc(in->units, in->ucap * sizeof (unit));
  }
  return &in->units[n];
}

static void batch_run(input * in, sz n, u32 slots, XXH32_state_t * sum) {
  mkc_pool * pool;
  u32 i, j, size;
  ubatch b = {in, n, 0, 0, 0, NULL};
  sz x = 0;
  if (!n) return;
  pool = decode_pool();
  size = decoders.size;
  if (!in->infl) {
    in->infl = xcalloc(size, sizeof *in->infl);
    in->zdc = xcalloc(size, sizeof *in->zdc);
  }
  Fj(size,
     if (in->units[0].kind == UNIT_GZIP && !in->infl[j])
       in->infl[j] = xmalloc(sizeof (struct inflate_state));
     if (in->units[0].kind == UNIT_ZSTD && !in->zdc[j]
         && !(in->zdc[j] = ZSTD_createDCtx())) FATAL("out of memory"));
  tp_run(pool, size, batch_task, &b);
  if (b.failed) FATAL("%s: %s", in->name, b.why);
  Fi(slots,
     sz used = 0;
     for (; x < n && in->units[x].slot == i; x++) {
       unit * u = &in->units[x];
       if (u->at != used)
         memmove(ring_block(in, i) + used, ring_block(in, i) + u->at, u->len);
       used += u->len;
     }
     if (!used) FATAL("%s: internal error: empty input block", in->name);
     if (sum) XXH32_update(sum, ring_block(in, i), used);
     in->sizes[(in->head + i) % READ_RING] = used;  in->decoded += used;
     in->parallel += used);
  ring_publish(in, slots);
}

static int unit_place(u32 * slot, sz * fill, u32 slots, sz cap) {
  if (*fill + cap > READ_BLOCK) { (*slot)++;  *fill = 0; }
  return *slot < slots;
}

static const char * isal_error(int ret) {
  switch (ret) {
  case ISAL_INVALID_BLOCK: return "invalid deflate block";
  case ISAL_INVALID_SYMBOL: return "invalid deflate symbol";
  case ISAL_INVALID_LOOKBACK: return "invalid deflate distance";
  case ISAL_INVALID_WRAPPER: return "invalid gzip header";
  case ISAL_UNSUPPORTED_METHOD: return "unsupported gzip compression method";
  case ISAL_INCORRECT_CHECKSUM: return "gzip checksum mismatch";
  case ISAL_NEED_DICT: return "deflate dictionary required";
  default: return "gzip decoding error";
  }
}

static int bgzf_member(const u8 * p, sz have, sz * total) {
  sz xlen, x;
  if (have < 18 || p[0] != 0x1F || p[1] != 0x8B || p[2] != 8 || !(p[3] & 4))
    return 0;
  xlen = (sz) p[10] | (sz) p[11] << 8;
  if (have < 12 + xlen) return 0;
  for (x = 12; x + 4 <= 12 + xlen;) {
    sz len = (sz) p[x + 2] | (sz) p[x + 3] << 8;
    if (p[x] == 'B' && p[x + 1] == 'C' && len == 2 && x + 6 <= 12 + xlen) {
      *total = ((sz) p[x + 4] | (sz) p[x + 5] << 8) + 1;
      return *total >= 12 + xlen + 8;
    }
    x += 4 + len;
  }
  return 0;
}

static int bgzf_run(input * in) {
  for (;;) {
    u32 slots = ring_free(in, PAR_SLOTS), slot = 0, used = 0;
    sz n = 0, fill = 0, total, i;
    int other = 0;
    if (!slots) return 0;
    raw_more(in, in->rcap / 2);
    for (;;) {
      sz have;
      const u8 * p;
      u32 isize;
      if (n == 65536) break;
      if (in->end - in->pos < 18) { if (n) break;  raw_more(in, 18); }
      have = in->end - in->pos;
      p = in->raw + in->pos;
      if (!have) break;
      if (have < 2 || p[0] != 0x1F || p[1] != 0x8B)
        FATAL("%s: unexpected data after gzip member", in->name);
      if (!bgzf_member(p, have, &total)) {
        if (have < 18 && !in->eof) break;
        other = 1;  break;
      }
      if (have < total) {
        if (n) break;
        raw_more(in, total);
        if (in->end - in->pos < total)
          FATAL("%s: truncated gzip input", in->name);
        continue;
      }
      isize = le32(p + total - 4);
      if (isize > 65536) { other = 1;  break; }
      if (!unit_place(&slot, &fill, slots, isize)) break;
      *unit_add(in, n++) = (unit) {p, total, isize, 0, slot, fill, UNIT_GZIP};
      fill += isize;  in->pos += total;
    }
    Fi(n, if (in->units[i].cap) used = in->units[i].slot + 1);
    batch_run(in, n, used, NULL);
    if (other) return 1;
    if (!n && in->end == in->pos && in->eof) return 0;
  }
}

static void gzip_run(input * in) {
  struct inflate_state * st = xmalloc(sizeof *st);
  struct isal_gzip_header head;
  outblock o = {NULL, 0};
  int parallel = decoders.size > 1 && READ_BLOCK >= 65536;
  for (;;) {
    sz have, total;
    raw_more(in, 18);
    have = in->end - in->pos;
    if (!have) break;
    if (have < 2 || in->raw[in->pos] != 0x1F || in->raw[in->pos + 1] != 0x8B)
      FATAL("%s: unexpected data after gzip member", in->name);
    if (parallel && bgzf_member(in->raw + in->pos, have, &total)) {
      out_flush(in, &o);
      in->format = "gzip (BGZF, parallel)";
      if (bgzf_run(in)) continue;
      break;
    }
    isal_inflate_init(st);  st->crc_flag = ISAL_GZIP;
    isal_gzip_header_init(&head);
    for (;;) {
      sz avail;
      int ret;
      if (in->pos == in->end) {
        raw_more(in, 1);
        if (in->pos == in->end) FATAL("%s: truncated gzip input", in->name);
      }
      avail = MIN(in->end - in->pos, 1u << 30);
      st->next_in = in->raw + in->pos;  st->avail_in = (u32) avail;
      ret = isal_read_gzip_header(st, &head);
      in->pos += avail - st->avail_in;
      if (ret == ISAL_DECOMP_OK) break;
      if (ret != ISAL_END_INPUT) FATAL("%s: %s", in->name, isal_error(ret));
    }
    for (;;) {
      sz avail, before;
      int ret;
      if (!out_open(in, &o)) goto stop;
      if (in->pos == in->end) {
        raw_more(in, 1);
        if (in->pos == in->end) FATAL("%s: truncated gzip input", in->name);
      }
      avail = MIN(in->end - in->pos, 1u << 30);  before = o.used;
      st->next_in = in->raw + in->pos;  st->avail_in = (u32) avail;
      st->next_out = o.at + o.used;
      st->avail_out = (u32) (READ_BLOCK - o.used);
      ret = isal_inflate(st);
      in->pos += avail - st->avail_in;  o.used = (sz) (st->next_out - o.at);
      if (ret != ISAL_DECOMP_OK) FATAL("%s: %s", in->name, isal_error(ret));
      if (st->block_state == ISAL_BLOCK_FINISH) break;
      if (o.used == READ_BLOCK) out_flush(in, &o);
      else if (o.used == before && st->avail_in == avail)
        FATAL("%s: gzip decoder made no progress", in->name);
    }
  }
  out_flush(in, &o);
stop:
  xfree(st);
}

#define ZSTD_MAGIC 0xFD2FB528u
#define ZSTD_HMAX 18u

static int skippable(u32 magic) {
  return (magic & 0xFFFFFFF0u) == 0x184D2A50u;
}

static void skip_frame(input * in) {
  u64 left;
  raw_more(in, 8);
  if (in->end - in->pos < 8) FATAL("%s: truncated skippable frame", in->name);
  left = 8 + (u64) le32(in->raw + in->pos + 4);
  while (left) {
    sz take;
    if (in->pos == in->end) {
      raw_more(in, 1);
      if (in->pos == in->end)
        FATAL("%s: truncated skippable frame", in->name);
    }
    take = in->end - in->pos < left ? in->end - in->pos : (sz) left;
    in->pos += take;  left -= take;
  }
}

#define IN_HEAD 64u

static u32 input_magic(const u8 * p, sz n) {
  sz at = 0;
  while (n - at >= 8 && skippable(le32(p + at))) {
    u64 next = (u64) at + 8 + le32(p + at + 4);
    if (next + 4 > n) return ZSTD_MAGIC;
    at = (sz) next;
  }
  return n - at >= 4 ? le32(p + at) : 0;
}

static int zstd_small(const u8 * p, sz have, unsigned long long * size) {
  *size = ZSTD_getFrameContentSize(p, have);
  return *size != ZSTD_CONTENTSIZE_UNKNOWN && *size != ZSTD_CONTENTSIZE_ERROR
         && *size <= READ_BLOCK && *size > 0;
}

static void zstd_frames(input * in) {
  for (;;) {
    u32 slots = ring_free(in, PAR_SLOTS), slot = 0;
    sz n = 0, fill = 0;
    if (!slots) return;
    raw_more(in, in->rcap / 2);
    for (;;) {
      sz have, csize;
      const u8 * p;
      unsigned long long size;
      if (in->end - in->pos < ZSTD_HMAX) {
        if (n) break;
        raw_more(in, ZSTD_HMAX);
      }
      have = in->end - in->pos;
      p = in->raw + in->pos;
      if (have < 4 || le32(p) != ZSTD_MAGIC || !zstd_small(p, have, &size))
        break;
      csize = ZSTD_findFrameCompressedSize(p, have);
      if (ZSTD_isError(csize)) {
        sz before;
        if (n) break;
        before = have;
        raw_more(in, have + READ_BLOCK);
        if (in->end - in->pos == before)
          FATAL("%s: truncated or corrupt zstd frame", in->name);
        continue;
      }
      if (!unit_place(&slot, &fill, slots, (sz) size)) break;
      *unit_add(in, n++) = (unit) {p, csize, (sz) size, 0, slot, fill,
                                   UNIT_ZSTD};
      fill += (sz) size;  in->pos += csize;
    }
    if (!n) return;
    batch_run(in, n, in->units[n - 1].slot + 1, NULL);
  }
}

static void zstd_run(input * in) {
  ZSTD_DCtx * dc = ZSTD_createDCtx();
  outblock o = {NULL, 0};
  int parallel = decoders.size > 1;
  if (!dc) FATAL("out of memory");
  ZSTD_DCtx_setParameter(dc, ZSTD_d_windowLogMax, 31);
  for (;;) {
    sz have;
    u32 magic;
    unsigned long long size;
    raw_more(in, ZSTD_HMAX);
    have = in->end - in->pos;
    if (!have) break;
    magic = have >= 4 ? le32(in->raw + in->pos) : 0;
    if (skippable(magic)) { skip_frame(in);  continue; }
    if (magic != ZSTD_MAGIC)
      FATAL("%s: unexpected data after zstd frame", in->name);
    if (parallel && zstd_small(in->raw + in->pos, have, &size)) {
      out_flush(in, &o);
      in->format = "zstd (parallel frames)";
      zstd_frames(in);
      continue;
    }
    ZSTD_DCtx_reset(dc, ZSTD_reset_session_only);
    for (;;) {
      ZSTD_inBuffer ib;
      ZSTD_outBuffer ob;
      sz r;
      if (!out_open(in, &o)) goto stop;
      if (in->pos == in->end) {
        raw_more(in, 1);
        if (in->pos == in->end) FATAL("%s: truncated zstd input", in->name);
      }
      ib.src = in->raw + in->pos;  ib.size = in->end - in->pos;  ib.pos = 0;
      ob.dst = o.at;  ob.size = READ_BLOCK;  ob.pos = o.used;
      r = ZSTD_decompressStream(dc, &ob, &ib);
      if (ZSTD_isError(r)) FATAL("%s: %s", in->name, ZSTD_getErrorName(r));
      in->pos += ib.pos;  o.used = ob.pos;
      if (!r) break;
      if (o.used == READ_BLOCK) out_flush(in, &o);
    }
  }
  out_flush(in, &o);
stop:
  ZSTD_freeDCtx(dc);
}

#define LZ4_MAGIC 0x184D2204u

typedef struct {
  sz header, bmax;
  int indep, bsum, csum, sized, dict;
  u64 size;
} lz4_frame;

static int lz4_header(const u8 * p, sz have, lz4_frame * f) {
  u32 flg, bd, id, i;
  if (have < 7 || le32(p) != LZ4_MAGIC) return 0;
  flg = p[4];  bd = p[5];  id = (bd >> 4) & 7;
  if (flg >> 6 != 1 || (flg & 2) || (bd & 0x8F) || id < 4) return 0;
  f->indep = (flg >> 5) & 1;  f->bsum = (flg >> 4) & 1;
  f->sized = (flg >> 3) & 1;
  f->csum = (flg >> 2) & 1;  f->dict = flg & 1;
  f->bmax = (sz) 1 << (8 + 2 * id);
  f->header = 7 + (f->sized ? 8 : 0) + (f->dict ? 4 : 0);
  if (have < f->header) return 0;
  if (((XXH32(p + 4, f->header - 5, 0) >> 8) & 0xFF) != p[f->header - 1])
    return 0;
  f->size = 0;
  if (f->sized) Fi(8, f->size |= (u64) p[6 + i] << (8 * i));
  return 1;
}

static void lz4_blocks(input * in, const lz4_frame * f) {
  XXH32_state_t * sum = f->csum ? XXH32_createState() : NULL;
  u64 total = 0;
  int end = 0;
  if (sum) XXH32_reset(sum, 0);
  in->pos += f->header;
  while (!end) {
    u32 slots = ring_free(in, PAR_SLOTS), slot = 0;
    sz n = 0, fill = 0, i;
    if (!slots) break;
    raw_more(in, in->rcap / 2);
    for (;;) {
      const u8 * p;
      u32 word;
      sz len, need, cap;
      if (in->end - in->pos < 4) {
        if (n) break;
        raw_more(in, 4);
        if (in->end - in->pos < 4) FATAL("%s: truncated lz4 input", in->name);
      }
      p = in->raw + in->pos;
      word = le32(p);
      if (!word) { end = 1;  in->pos += 4;  break; }
      len = word & 0x7FFFFFFFu;  need = 4 + len + (f->bsum ? 4 : 0);
      cap = word >> 31 ? len : f->bmax;
      if (len > f->bmax) FATAL("%s: corrupt lz4 block size", in->name);
      if (in->end - in->pos < need) {
        if (n) break;
        raw_more(in, need);
        if (in->end - in->pos < need)
          FATAL("%s: truncated lz4 input", in->name);
        continue;
      }
      if (f->bsum && XXH32(p + 4, len, 0) != le32(p + 4 + len))
        FATAL("%s: lz4 block checksum mismatch", in->name);
      if (!len) { in->pos += need;  continue; }
      if (!unit_place(&slot, &fill, slots, cap)) break;
      *unit_add(in, n++) = (unit) {p + 4, len, cap, 0, slot, fill,
                                   word >> 31 ? UNIT_STORED : UNIT_LZ4};
      fill += cap;  in->pos += need;
    }
    if (n) {
      batch_run(in, n, in->units[n - 1].slot + 1, sum);
      Fi(n, total += in->units[i].len);
    }
  }
  if (f->sized && total != f->size)
    FATAL("%s: lz4 content size mismatch", in->name);
  if (sum) {
    raw_more(in, 4);
    if (in->end - in->pos < 4) FATAL("%s: truncated lz4 input", in->name);
    if (XXH32_digest(sum) != le32(in->raw + in->pos))
      FATAL("%s: lz4 content checksum mismatch", in->name);
    in->pos += 4;
    XXH32_freeState(sum);
  }
}

static void lz4_run(input * in) {
  LZ4F_dctx * dc;
  outblock o = {NULL, 0};
  int parallel = decoders.size > 1;
  if (LZ4F_isError(LZ4F_createDecompressionContext(&dc, LZ4F_VERSION)))
    FATAL("out of memory");
  for (;;) {
    sz have;
    u32 magic;
    lz4_frame f;
    raw_more(in, 19);
    have = in->end - in->pos;
    if (!have) break;
    magic = have >= 4 ? le32(in->raw + in->pos) : 0;
    if (skippable(magic)) { skip_frame(in);  continue; }
    if (!lz4_header(in->raw + in->pos, have, &f))
      FATAL("%s: %s", in->name,
            magic == LZ4_MAGIC ? "invalid lz4 frame header"
                               : "unexpected data after lz4 frame");
    if (parallel && f.indep && !f.dict && f.bmax <= READ_BLOCK) {
      out_flush(in, &o);
      in->format = "lz4 (parallel blocks)";
      lz4_blocks(in, &f);
      continue;
    }
    LZ4F_resetDecompressionContext(dc);
    for (;;) {
      sz src, dst, r;
      if (!out_open(in, &o)) goto stop;
      if (in->pos == in->end) {
        raw_more(in, 1);
        if (in->pos == in->end) FATAL("%s: truncated lz4 input", in->name);
      }
      src = in->end - in->pos;  dst = READ_BLOCK - o.used;
      r = LZ4F_decompress(dc, o.at + o.used, &dst, in->raw + in->pos, &src,
                          NULL);
      if (LZ4F_isError(r))
        FATAL("%s: lz4: %s", in->name, LZ4F_getErrorName(r));
      in->pos += src;  o.used += dst;
      if (!r) break;
      if (o.used == READ_BLOCK) out_flush(in, &o);
    }
  }
  out_flush(in, &o);
stop:
  LZ4F_freeDecompressionContext(dc);
}

static void plain_run(input * in) {
  while (!in->eof) {
    u8 * b;
    sz got = 0;
    if (!ring_free(in, 1)) return;
    b = ring_block(in, 0);
    while (got < READ_BLOCK) {
      ptrdiff_t n = input_read(in, b + got, READ_BLOCK - got);
      if (!n) { in->eof = 1;  break; }
      got += (sz) n;
    }
    if (!got) break;
    in->sizes[in->head % READ_RING] = got;  in->decoded += got;
    ring_publish(in, 1);
  }
}

static sz read_block(input * in) {
  u8 * b = ring_block(in, 0);
  sz got = 0;
  while (got < READ_BLOCK) {
    ptrdiff_t n = input_read(in, b + got, READ_BLOCK - got);
    if (!n) { in->eof = 1;  break; }
    got += (sz) n;
  }
  return got;
}

static void * input_main(void * arg) {
  input * in = (input *) arg;
  sz got = ring_free(in, 1) ? read_block(in) : 0;
  const u8 * p = ring_block(in, 0);
  u32 magic = input_magic(p, got);
  int gz = got >= 2 && p[0] == 0x1F && p[1] == 0x8B,
      zs = !gz && magic == ZSTD_MAGIC, l4 = !gz && magic == LZ4_MAGIC;
  if (gz || zs || l4) {
    in->rcap = (4 * READ_BLOCK + RAW_ALIGN - 1) & ~(RAW_ALIGN - 1);
    in->raw = xpages(in->rcap);
    memcpy(in->raw, p, got);  in->pos = 0;  in->end = got;
    if (gz) { in->format = "gzip";  gzip_run(in); }
    else if (zs) { in->format = "zstd";  zstd_run(in); }
    else { in->format = "lz4";  lz4_run(in); }
  } else {
    in->format = "plain";
    if (got) {
      in->sizes[in->head % READ_RING] = got;  in->decoded += got;
      ring_publish(in, 1);
    }
    plain_run(in);
  }
  mx_lock(&in->mutex);
  in->done = 1;  cv_bcast(&in->cond);
  mx_unlock(&in->mutex);
  return NULL;
}

static input * input_start(const char * name, mkc_file fd) {
  input * in = xcalloc(1, sizeof *in);
  i64 size;
  u32 i;
  int e;
  in->name = name;  in->fd = fd;  in->format = "plain";
  size = os_file_size(fd);
  in->regular = size >= 0;
  if (in->regular && !strcmp(name, "-")) {
    const i64 at = os_take(fd);
    if (at < 0) in->regular = 0;
    else { in->offset = (u64) at;  size = at < size ? size - at : 0; }
  }
  if (in->regular && READ_BLOCK % RAW_ALIGN == 0
      && in->offset % RAW_ALIGN == 0 && (u64) size > os_mem() / 4
      && !os_set_direct(&in->fd, 1)) in->direct = 1;
  Fi(READ_RING, in->blocks[i] = xpages(READ_BLOCK));
  mx_init(&in->mutex);  cv_init(&in->cond);
  e = th_start(&in->thread, input_main, in, 0);
  if (e) FATAL("create input thread: %s", strerror(e));
  return in;
}

static void input_stop(input * in) {
  u32 i;
  mx_lock(&in->mutex);
  in->stop = 1;  cv_bcast(&in->cond);
  mx_unlock(&in->mutex);
  th_join(in->thread);
  Fi(READ_RING, os_pages_free(in->blocks[i], READ_BLOCK));
  os_pages_free(in->raw, in->rcap);
  if (in->infl)
    Fi(decoders.size, xfree(in->infl[i]);  ZSTD_freeDCtx(in->zdc[i]));
  xfree(in->infl);  xfree(in->zdc);  xfree(in->units);
  mx_free(&in->mutex);  cv_free(&in->cond);
  if (os_close(in->fd))
    FATAL("%s: input close error: %s", in->name, os_error());
  xfree(in);
}

static sz input_next(input * in, int * held, u8 ** block) {
  sz n = 0;
  mx_lock(&in->mutex);
  if (*held) { in->tail++;  *held = 0;  cv_bcast(&in->cond); }
  while (in->tail == in->head && !in->done) cv_wait(&in->cond, &in->mutex);
  if (in->tail != in->head) {
    u32 slot = in->tail % READ_RING;
    n = in->sizes[slot];  *block = in->blocks[slot];  *held = 1;
  }
  mx_unlock(&in->mutex);
  return n;
}

static u32 in_ratio(const u8 * head, sz n) {
  u32 magic;
  if (n >= 2 && head[0] == 0x1F && head[1] == 0x8B) return 4;
  magic = input_magic(head, n);
  if (magic == ZSTD_MAGIC) return 6;
  if (magic == LZ4_MAGIC) return 3;
  return 1;
}
