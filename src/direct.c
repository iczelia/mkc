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

#define DIRECT_LOCAL_BITS 18u
#define DIRECT_STAGE      4096u

static struct {
  u32 hbits, parts, pbits;
  u32 * counts;
  u64 ** local;
  mkc_mutex * locks;
  u32 ** stage;
  u32 ** fill;
  mkc_mutex cmu;
  u64 * ckey, * cval;
  sz carries, ccap;
  u64 * occ;
} direct;

static void direct_carry(u64 code) {
  sz i = 0;
  mx_lock(&direct.cmu);
  while (i < direct.carries && direct.ckey[i] != code) i++;
  if (i == direct.carries) {
    if (direct.carries == direct.ccap) {
      direct.ccap = direct.ccap ? 2 * direct.ccap : 64;
      direct.ckey = xrealloc(direct.ckey, direct.ccap * 8);
      direct.cval = xrealloc(direct.cval, direct.ccap * 8);
    }
    direct.ckey[i] = code;  direct.cval[i] = 0;  direct.carries++;
  }
  direct.cval[i] += UINT64_C(1) << 32;
  mx_unlock(&direct.cmu);
}

static void direct_apply(u32 part, const u32 * codes, u32 n) {
  const u32 bits = direct.pbits;
  u32 * c = direct.counts + ((sz) part << bits);
  u32 i;
  mx_lock(&direct.locks[part]);
  Fi(n,
    u32 low = codes[i] & ((1u << bits) - 1);
    if (!++c[low]) direct_carry((u64) part << bits | low));
  mx_unlock(&direct.locks[part]);
}

static INLINE void direct_add(u32 tid, u64 code) {
  u32 part, * fill;
  u32 * stage;
  if (direct.local) { direct.local[tid][code]++;  return; }
  part = (u32) (code >> direct.pbits);
  fill = &direct.fill[tid][part];
  stage = direct.stage[tid] + (sz) part * DIRECT_STAGE;
  stage[(*fill)++] = (u32) code;
  if (*fill == DIRECT_STAGE) {
    direct_apply(part, stage, DIRECT_STAGE);  *fill = 0;
  }
}

static INLINE void direct_scan_k(chunk_job * job, u32 tid,
                                 const u32 kl) {
  const u8 * p;
  sz n, i;
  while (chunk_next(job, &p, &n)) {
    u64 key = 0, rc = 0, seen = 0;
    u32 valid = 0;
    const u64 mask = mkc_mask(kl);
    Fi(n,
      u32 c = p[i];
      if (c > 3) { valid = 0;  continue; }
      key = ((key << 2) | c) & mask;
      rc = (rc >> 2) | ((u64) (3 - c) << (2 * kl - 2));
      if (++valid < kl) continue;
      valid = kl;
      direct_add(tid, mkc_code(key, rc, kl));  seen++);
    direct.occ[tid] += seen;
  }
}

#define DIRECT_INSTANCE(kl)                                                   \
  static void direct_scan_##kl(void * q, u32 id, u32 m) {                     \
    direct_scan_k((chunk_job *) q, id, kl);                                   \
  }
EACH_K(DIRECT_INSTANCE)
#define DIRECT_ENTRY(kl) direct_scan_##kl,
static const mkc_task direct_scans[33] = {NULL, EACH_K(DIRECT_ENTRY)};

static u64 direct_build(int argc, char ** argv, int first,
                        const char * out, mkc_file fd, u64 budget,
                        u64 * occurrences) {
  u32 hbits = mkc_code_bits(K), workers = scan_slots(), width;
  u64 entries = UINT64_C(1) << hbits, total = 0, distinct = 0,
      above[4] = {0}, sum = 0, at = 128, size, lim, k;
  sz i, j, used = 0, nc = 0;
  exc_file over;
  exc_batch * eb;
  u8 * buf;
  direct.hbits = hbits;
  mx_init(&direct.cmu);
  direct.occ = xcalloc(workers, sizeof (u64));
  if (hbits <= DIRECT_LOCAL_BITS) {
    direct.local = xmalloc(workers * sizeof *direct.local);
    Fi(workers, direct.local[i] = xcalloc(entries, 8));
  } else {
    direct.pbits = hbits - 8 < 14 ? hbits - 8 : 14;
    direct.parts = (u32) (entries >> direct.pbits);
    direct.counts = xpages((sz) entries * 4);
    direct.locks = xmalloc(direct.parts * sizeof *direct.locks);
    Fi(direct.parts, mx_init(&direct.locks[i]));
    direct.stage = xmalloc(workers * sizeof *direct.stage);
    direct.fill = xmalloc(workers * sizeof *direct.fill);
    Fi(workers,
      direct.stage[i] = xpages((sz) direct.parts * DIRECT_STAGE * 4);
      direct.fill[i] = xcalloc(direct.parts, sizeof (u32)));
  }
  scan_task = direct_scans[K];
  parse_all(argc, argv, first);
  batch_flush();  pipe_finish();
  Fi(workers, total += direct.occ[i]);
  if (direct.local) {
    for (i = 1; i < workers; i++) {
      Fk(entries, direct.local[0][k] += direct.local[i][k]);
      xfree(direct.local[i]);
    }
  } else {
    Fi(workers,
      Fj(direct.parts,
        direct_apply((u32) j, direct.stage[i] + j * DIRECT_STAGE,
                     direct.fill[i][j]));
      os_pages_free(direct.stage[i], (sz) direct.parts * DIRECT_STAGE * 4);
      xfree(direct.fill[i]));
  }
#define DIRECT_COUNT(code)                                                    \
  (direct.local ? direct.local[0][code] : direct.counts[code])
  Fk(entries,
    u64 v = DIRECT_COUNT(k);
    distinct += v != 0;  sum += v;
    Fi(4, above[i] += v > inl_max[i]));
  Fi(direct.carries,
    u64 code = direct.ckey[i], low = direct.counts[code],
        full = low + direct.cval[i];
    sum += direct.cval[i];
    Fj(4, above[j] += (full > inl_max[j]) - (low > inl_max[j])));
  if (sum != total) FATAL("direct count mismatch");
  width = dense_width(hbits, above, &size);
  lim = mkc_bits(8 * width);
  over = exc_open(out);
  eb = xmalloc(sizeof *eb);  eb->file = &over;  eb->used = 0;
  buf = xmalloc(1u << 20);
  for (i = 1; i < direct.carries; i++)
    for (j = i; j && direct.ckey[j - 1] > direct.ckey[j]; j--) {
      u64 x = direct.ckey[j];
      direct.ckey[j] = direct.ckey[j - 1];
      direct.ckey[j - 1] = x;
      x = direct.cval[j];
      direct.cval[j] = direct.cval[j - 1];
      direct.cval[j - 1] = x;
    }
  Fk(entries,
    u64 v = DIRECT_COUNT(k);
    if (nc < direct.carries && direct.ckey[nc] == k)
      v += direct.cval[nc++];
    if (v > lim) exc_add(eb, k, v);
    mg_store(buf + used, width, v < lim ? v : lim);
    used += width;
    if (used == (1u << 20)) {
      write_at(fd, buf, used, at);  at += used;  used = 0;
    });
#undef DIRECT_COUNT
  write_at(fd, buf, used, at);
  exc_flush(eb);  xfree(eb);  xfree(buf);
  if (direct.local) { xfree(direct.local[0]);  xfree(direct.local); }
  else {
    os_pages_free(direct.counts, (sz) entries * 4);
    xfree(direct.stage);  xfree(direct.fill);
    Fi(direct.parts, mx_free(&direct.locks[i]));
    xfree(direct.locks);
  }
  xfree(direct.ckey);  xfree(direct.cval);
  xfree(direct.occ);
  dense_finish(fd, K, width, distinct, total, &over, budget);
  os_close(over.f);
  *occurrences = total;
  return distinct;
}
