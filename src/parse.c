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

static void dec_scalar(u8 * p, sz n) {
  sz i;
  Fi(n, p[i] = (u8) mkc_base(p[i]));
}

static u8 * batch;
static sz blen, bdec;
static u64 bput;
static void (*decode)(u8 *, sz) = dec_scalar;
static mkc_task scan_task;

#define CHUNK_STEP ((sz) 128 * 1024)

typedef struct { const u8 * data;  sz size, chunks, next; } chunk_job;

static INLINE int chunk_next(chunk_job * jb, const u8 ** p, sz * n) {
  sz c = fetch_add(&jb->next, (sz) 1), start, end;
  if (c >= jb->chunks) return 0;
  start = c * CHUNK_STEP;  end = start + CHUNK_STEP;
  if (end > jb->size) end = jb->size;
  if (start) start -= K - 1;
  *p = jb->data + start;  *n = end - start;
  return 1;
}

static void scan_batch(const u8 * data, sz size) {
  chunk_job job = {data, size, (size + CHUNK_STEP - 1) / CHUNK_STEP, 0};
  tp_run(scan_pool, scan_pool->size, scan_task, &job);
}

static struct {
  int enabled, started, ready, stop;
  mkc_thread thread;  mkc_mutex mutex;  mkc_cond cond;
  u8 * spare, * data;  sz size;
  chunk_job job;
} pl;

static u32 scan_slots(void) {
  return scan_pool->size + (pl.enabled ? 1u : 0u);
}

static void * pipe_main(void * unused) {
  mx_lock(&pl.mutex);
  for (;;) {
    while (!pl.ready && !pl.stop)
      cv_wait(&pl.cond, &pl.mutex);
    if (pl.stop) break;
    mx_unlock(&pl.mutex);
    tp_run(scan_pool, scan_pool->size, scan_task, &pl.job);
    mx_lock(&pl.mutex);
    pl.ready = 0;  cv_bcast(&pl.cond);
  }
  mx_unlock(&pl.mutex);
  return NULL;
}

static void pipe_drain(void) {
  if (pl.ready) {
    mx_unlock(&pl.mutex);
    scan_task(&pl.job, scan_pool->size, scan_pool->size + 1);
    mx_lock(&pl.mutex);
  }
  while (pl.ready) cv_wait(&pl.cond, &pl.mutex);
}

static void pipe_submit(void) {
  sz tail;
  if (!pl.started) {
    int e;
    pl.spare = xpages(BATCH);
    e = th_start(&pl.thread, pipe_main, NULL, 0);
    if (e) FATAL("create scan thread: %s", strerror(e));
    pl.started = 1;
  }
  mx_lock(&pl.mutex);
  pipe_drain();
  tail = MIN(blen, K - 1);
  memcpy(pl.spare, batch + blen - tail, tail);
  pl.data = batch;  pl.size = blen;
  pl.job = (chunk_job) {batch, blen,
                              (blen + CHUNK_STEP - 1) / CHUNK_STEP, 0};
  batch = pl.spare;  pl.spare = pl.data;
  blen = bdec = tail;
  pl.ready = 1;  cv_bcast(&pl.cond);
  mx_unlock(&pl.mutex);
}

static void pipe_finish(void) {
  if (!pl.started) return;
  mx_lock(&pl.mutex);
  pipe_drain();
  pl.stop = 1;  cv_bcast(&pl.cond);
  mx_unlock(&pl.mutex);
  th_join(pl.thread);
  os_pages_free(pl.spare, BATCH);  pl.spare = NULL;
  pl.started = 0;  pl.stop = 0;
}

static void batch_flush(void) {
  sz tail;
  if (!blen) return;
  decode(batch + bdec, blen - bdec);
  if (pl.enabled && (pl.started || blen == BATCH)) {
    pipe_submit();  return;
  }
  scan_batch(batch, blen);
  tail = MIN(blen, K - 1);
  memmove(batch, batch + blen - tail, tail);
  blen = bdec = tail;
}

static void seq_put(const u8 * s, sz n) {
  bput += n;
  while (n) {
    sz take = BATCH - blen;
    if (take > n) take = n;
    memcpy(batch + blen, s, take);
    blen += take;  s += take;  n -= take;
    if (blen == BATCH) batch_flush();
  }
}

static void seq_cut(void) {
  const u8 invalid = 255;
  seq_put(&invalid, 1);
}

typedef struct {
  input * in;  int held;
  u32 nrep;  const u8 * rep[2];  sz rep_len[2];
  u8 * buf, * line, * store;  sz pos, end, len, cap;
} reader;

static sz rd_fill(reader * r) {
  if (r->nrep) {
    sz n;
    r->buf = (u8 *) r->rep[0];  n = r->rep_len[0];
    r->rep[0] = r->rep[1];  r->rep_len[0] = r->rep_len[1];
    r->nrep--;
    return n;
  }
  return input_next(r->in, &r->held, &r->buf);
}

static int rd_line(reader * r) {
  r->len = 0;  r->line = r->store;
  for (;;) {
    u8 * p, * nl;
    sz n;
    if (r->pos == r->end) {
      n = rd_fill(r);
      if (!n) {
        if (r->len && r->line[r->len - 1] == '\r') r->len--;
        return r->len != 0;
      }
      r->pos = 0;  r->end = n;
    }
    p = r->buf + r->pos;  nl = memchr(p, '\n', r->end - r->pos);
    n = nl ? (sz) (nl - p) : r->end - r->pos;
    if (nl && !r->len) {
      r->line = p;  r->len = n;  r->pos += n + 1;
      if (r->len && r->line[r->len - 1] == '\r') r->len--;
      return 1;
    }
    if (n > SIZE_MAX - r->len - 1) FATAL("input line too long");
    if (r->len + n + 1 > r->cap) {
      sz need = r->len + n + 1;
      r->cap = need <= SIZE_MAX / 2 ? need * 2 : need;
      r->store = r->line = xrealloc(r->store, r->cap);
    }
    memcpy(r->line + r->len, p, n);  r->len += n;  r->pos += n + (nl != NULL);
    if (nl) {
      if (r->len && r->line[r->len - 1] == '\r') r->len--;
      return 1;
    }
  }
}

static void fq_serial(reader * r, const char * name) {
  int have;
  do {
    u64 seq = 0, qual = 0;
    if (!r->len || r->line[0] != '@')
      FATAL("%s: expected FASTQ @ header", name);
    seq_cut();
    while ((have = rd_line(r)) && !(r->len && r->line[0] == '+')) {
      if (r->len > UINT64_MAX - seq) FATAL("FASTQ record too long");
      seq += r->len;  seq_put(r->line, r->len);
    }
    if (!have) FATAL("%s: missing FASTQ + separator", name);
    do {
      u8 bad = 0;
      sz i;
      if (!rd_line(r)) FATAL("%s: truncated FASTQ quality", name);
      if (r->len > seq - qual)
        FATAL("%s: FASTQ quality longer than sequence", name);
      Fi(r->len, bad |= (u8) (r->line[i] - 33) > 93);
      if (bad) FATAL("%s: invalid FASTQ quality byte", name);
      qual += r->len;
    } while (qual < seq);
  } while (rd_line(r));
}

typedef struct { sz start, end, stop, out;  int fail; } fq_slice;

static u64 fq_par;

SI sz fq_line(const u8 * b, sz at, sz end) {
  const u8 * nl = at < end ? memchr(b + at, '\n', end - at) : NULL;
  return nl ? (sz) (nl - b) : SIZE_MAX;
}

static sz fq_records(const u8 * b, sz at, sz until, sz end, u8 * out,
                     sz * n, int * fail) {
  sz o = 0;
  while (at < until) {
    sz h, se, pe, qe, s, sl, q, ql, i;
    u8 bad = 0;
    if (b[at] != '@') { *fail = 1;  break; }
    h = fq_line(b, at, end);
    if (h == SIZE_MAX || (se = fq_line(b, h + 1, end)) == SIZE_MAX
        || (pe = fq_line(b, se + 1, end)) == SIZE_MAX
        || (qe = fq_line(b, pe + 1, end)) == SIZE_MAX) break;
    s = h + 1;  sl = se - s;  q = pe + 1;  ql = qe - q;
    if (sl && b[s + sl - 1] == '\r') sl--;
    if (ql && b[q + ql - 1] == '\r') ql--;
    if ((sl && b[s] == '+') || pe == se + 1 || b[se + 1] != '+' || ql != sl) {
      *fail = 1;  break;
    }
    Fi(ql, bad |= (u8) (b[q + i] - 33) > 93);
    if (bad) { *fail = 1;  break; }
    out[o++] = 255;  memcpy(out + o, b + s, sl);  o += sl;
    at = qe + 1;
  }
  *n = o;
  return at;
}

static sz fa_records(const u8 * b, sz at, sz until, sz end, u8 * out,
                     sz * n) {
  sz o = 0;
  while (at < until) {
    sz e = fq_line(b, at, end), len;
    if (e == SIZE_MAX) break;
    len = e - at;
    if (len && b[at + len - 1] == '\r') len--;
    if (len && b[at] == '>') out[o++] = 255;
    else { memcpy(out + o, b + at, len);  o += len; }
    at = e + 1;
  }
  *n = o;
  return at;
}

static sz fa_resync(const u8 * b, sz x, sz end) {
  sz e;
  if (!x || b[x - 1] == '\n') return x;
  e = fq_line(b, x, end);
  return e == SIZE_MAX ? end : e + 1;
}

static sz fq_resync(const u8 * b, sz x, sz end) {
  if (x && b[x - 1] != '\n') {
    sz e = fq_line(b, x, end);
    if (e == SIZE_MAX) return end;
    x = e + 1;
  }
  while (x < end) {
    sz e0 = fq_line(b, x, end), e1;
    if (e0 == SIZE_MAX || (e1 = fq_line(b, e0 + 1, end)) == SIZE_MAX
        || e1 + 1 >= end) return end;
    if (b[x] == '@' && b[e1 + 1] == '+') return x;
    x = e0 + 1;
  }
  return end;
}

static void bdec_sync(void) {
  decode(batch + bdec, blen - bdec);
  bdec = blen;
}

typedef struct {
  const u8 * b, * src;  u8 * out, * dst;  fq_slice * sl;
  sz at, end, span, done, take;  int fasta;
} fq_job;

static void fq_starts(void * arg, u32 id, u32 n) {
  fq_job * q = (fq_job *) arg;
  sz x = q->at + q->span / n * id;
  q->sl[id].start = !id ? q->at
                  : q->fasta ? fa_resync(q->b, x, q->end)
                  : fq_resync(q->b, x, q->end);
  q->sl[id].fail = 0;
}

static void fq_parse(void * arg, u32 id, u32 n) {
  fq_job * q = (fq_job *) arg;
  fq_slice * s = &q->sl[id];
  sz o = 0;
  s->end = id + 1 < n ? q->sl[id + 1].start : q->end;
  s->stop = q->fasta ? fa_records(q->b, s->start, s->end, q->end,
                                  q->out + (s->start - q->at), &o)
                     : fq_records(q->b, s->start, s->end, q->end,
                                  q->out + (s->start - q->at), &o, &s->fail);
  s->out = o;
  decode(q->out + (s->start - q->at), o);
}

static void fq_copy(void * arg, u32 id, u32 n) {
  fq_job * q = (fq_job *) arg;
  sz before = 0, lo, hi;
  u32 i;
  Fi(id, before += q->sl[i].out);
  lo = MAX(before, q->done);
  hi = MIN(before + q->sl[id].out, q->done + q->take);
  if (lo < hi)
    memcpy(q->dst + (lo - q->done), q->src + q->sl[id].start + (lo - before),
           hi - lo);
}

static void fq_commit(const u8 * out, fq_slice * sl, u32 n) {
  sz total = 0;
  u32 j;
  fq_job job = {NULL, out, NULL, NULL, sl, 0, 0, 0, 0, 0, 0};
  Fj(n, total += sl[j].out);
  bdec_sync();
  bput += total;
  while (job.done < total) {
    job.take = MIN(BATCH - blen, total - job.done);
    job.dst = batch + blen;
    tp_run(pool, n, fq_copy, &job);
    blen += job.take;  bdec = blen;
    job.done += job.take;
    if (blen == BATCH) batch_flush();
  }
}

#define INPUT_WINDOW 4u

enum { STREAM_PARALLEL, STREAM_FALLBACK, STREAM_SERIAL, STREAM_DONE };

typedef struct {
  const char * name;  reader r;  int mode, fasta;
  u8 * carry;  sz clen, ccap, at, end;
  const u8 * b;
  u8 tail[32];  u32 tlen;  u64 mark;
} stream;

static void stream_resume(stream * s) {
  u8 again[32] = {255};
  fq_slice one = {0, 0, 0, 1 + (sz) s->tlen, 0};
  if (bput == s->mark) return;
  memcpy(again + 1, s->tail, s->tlen);
  fq_commit(again, &one, 1);
  s->mark = bput;
}

static void stream_pause(stream * s) {
  const sz keep = (sz) MIN(K - 1, s->tlen + (bput - s->mark));
  bdec_sync();
  memcpy(s->tail, batch + blen - keep, keep);
  s->tlen = (u32) keep;  s->mark = bput;
}

static void stream_open(stream * s, const char * name) {
  mkc_file fd;
  memset(s, 0, sizeof *s);
  s->name = name;
  seq_cut();  s->mark = bput;
  fd = !strcmp(name, "-") ? os_stdin()
                          : os_open(name, MKC_READ | MKC_SEQUENTIAL);
  if (fd == MKC_NO_FILE) FATAL("cannot open input %s: %s", name, os_error());
  s->r.in = input_start(name, fd);
  s->mode = STREAM_SERIAL;
  if (pool->size > 1) {
    s->r.end = s->end = rd_fill(&s->r);
    s->b = s->r.buf;
    if (!s->end) s->mode = STREAM_DONE;
    else if (s->b[0] == '@') s->mode = STREAM_PARALLEL;
    else if (s->b[0] == '>') { s->mode = STREAM_PARALLEL;  s->fasta = 1; }
  }
}

static void stream_step(stream * s, u8 * out) {
  const u32 n = MIN(pool->size, 256);
  fq_slice sl[256];
  reader * r = &s->r;
  if (s->clen) {
    u32 lines = 0, need = s->fasta ? 1 : 4;
    sz p = 0, i;
    Fi(s->clen, lines += s->carry[i] == '\n');
    while (lines < need) {
      sz e = fq_line(s->b, p, s->end);
      if (e == SIZE_MAX) { p = s->end;  break; }
      p = e + 1;  lines++;
    }
    if (s->clen + p > s->ccap) {
      s->ccap = 2 * (s->clen + p);
      s->carry = xrealloc(s->carry, s->ccap);
    }
    memcpy(s->carry + s->clen, s->b, p);  s->clen += p;  s->at = p;
    if (lines == need) {
      sz o, stop;
      int fail = 0;
      u8 * co = xmalloc(s->clen);
      fq_slice one = {0, 0, 0, 0, 0};
      stop = s->fasta
             ? fa_records(s->carry, 0, s->clen, s->clen, co, &o)
             : fq_records(s->carry, 0, s->clen, s->clen, co, &o,
                          &fail);
      if (fail || stop != s->clen) {
        xfree(co);  s->mode = STREAM_FALLBACK;
        goto fallback;
      }
      decode(co, o);
      one.out = o;
      fq_commit(co, &one, 1);  xfree(co);
      s->clen = 0;
    }
  }
  if (!s->clen && s->at < s->end) {
    fq_job job = {s->b, NULL, out, NULL, sl, s->at, s->end, s->end - s->at,
                  0, 0, s->fasta};
    int fail = 0;
    u32 j;
    sz stop;
    tp_run(pool, n, fq_starts, &job);
    tp_run(pool, n, fq_parse, &job);
    for (j = 0; j < n && !fail; j++)
      fail = sl[j].fail || (sl[j].end < s->end ? sl[j].stop != sl[j].end
                                               : sl[j].stop > s->end);
    if (fail) { s->mode = STREAM_FALLBACK;  goto fallback; }
    Fj(n, sl[j].start -= s->at);
    fq_par += s->end - s->at;
    fq_commit(out, sl, n);
    stop = sl[n - 1].stop;
    for (j = n; j-- > 0;) if (sl[j].end == s->end) stop = sl[j].stop;
    if (stop < s->end) {
      if (s->end - stop > s->ccap) {
        s->ccap = 2 * (s->end - stop);
        s->carry = xrealloc(s->carry, s->ccap);
      }
      memcpy(s->carry, s->b + stop, s->end - stop);
      s->clen = s->end - stop;
    }
  }
  r->pos = r->end = 0;
  if ((s->end = rd_fill(r))) { s->b = r->buf;  s->at = 0;  return; }
  s->at = 0;
  s->mode = s->clen ? STREAM_FALLBACK : STREAM_DONE;
fallback:
  r->nrep = 0;
  if (s->clen) {
    r->rep[r->nrep] = s->carry;
    r->rep_len[r->nrep++] = s->clen;
  }
  if (s->mode == STREAM_FALLBACK && s->at < s->end) {
    r->rep[r->nrep] = s->b + s->at;
    r->rep_len[r->nrep++] = s->end - s->at;
  }
  r->pos = r->end = 0;
}

static void stream_serial(stream * s) {
  reader * r = &s->r;
  int have;
  if (s->mode == STREAM_FALLBACK) {
    if (s->fasta) {
      while (rd_line(r)) {
        if (r->len && r->line[0] == '>') seq_cut();
        else seq_put(r->line, r->len);
      }
    } else if (rd_line(r)) fq_serial(r, s->name);
    return;
  }
  do have = rd_line(r); while (have && !r->len);
  if (!have) return;
  if (r->line[0] == '>') {
    while (rd_line(r)) {
      if (r->len && r->line[0] == '>') seq_cut();
      else seq_put(r->line, r->len);
    }
  } else if (r->line[0] == '@') fq_serial(r, s->name);
  else FATAL("%s: expected FASTA > or FASTQ @ header", s->name);
}

static void stream_close(stream * s) {
  input * in = s->r.in;
  seq_cut();  xfree(s->r.store);  xfree(s->carry);
  if (strcmp(in->format, "plain"))
    fprintf(stderr, "mkc: %s: %s, %" PRIu64 " bytes decoded (%" PRIu64
            " in parallel)\n", s->name, in->format, in->decoded, in->parallel);
  input_stop(in);
}

static int is_packed(const char * name) {
  mkc_file f;
  u8 head[IN_HEAD] = {0};
  ptrdiff_t got;
  if (!strcmp(name, "-")) return 0;
  f = os_open(name, MKC_READ);
  if (f == MKC_NO_FILE) return 0;
  got = os_file_size(f) > 0 ? os_pread(f, head, IN_HEAD, 0) : 0;
  os_close(f);
  return got > 0 && in_ratio(head, (sz) got) > 1;
}

static void parse_all(int argc, char ** argv, int first) {
  stream st[INPUT_WINDOW];
  u8 * out = pool->size > 1 ? xpages(READ_BLOCK) : NULL;
  int i;
  for (i = first; i < argc;) {
    u32 n = 1, j;
    int active;
    if (pool->size > 1 && is_packed(argv[i]))
      while (n < INPUT_WINDOW && i + (int) n < argc
             && is_packed(argv[i + n])) n++;
    Fj(n, stream_open(&st[j], argv[i + j]));
    for (active = 1; active;) {
      active = 0;
      Fj(n, if (st[j].mode != STREAM_PARALLEL) continue;
            stream_resume(&st[j]);  stream_step(&st[j], out);
            stream_pause(&st[j]);  active = 1);
    }
    Fj(n, if (st[j].mode != STREAM_DONE) {
            stream_resume(&st[j]);  stream_serial(&st[j]);
          }
          stream_close(&st[j]));
    i += (int) n;
  }
  if (out) os_pages_free(out, READ_BLOCK);
}
