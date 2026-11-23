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
#include "cpu.h"
#include "os.h"
#include "mkc.h"
#include <inttypes.h>
#include <stdarg.h>

#define DIRECT_MAX_K 13u
#define MIN_BINS 1024u
#define FEW_BINS 64u
#define SMALL_BIN (256u * 1024)
#define MAX_BINS 16384u
#define BATCH (32u << 20)

static char * workdir, * out_tmp;
static u32 K = 31, M;
static int no_sync, may_reseed;

static void cleanup(void) {
  if (out_tmp) os_remove(out_tmp);
  if (workdir) {
    sz n = strlen(workdir) + 8;
    char * gone = malloc(n);
    if (gone && snprintf(gone, n, "%s.gone", workdir) > 0
        && !os_replace(workdir, gone))
      os_rmtree(gone);
    else os_rmtree(workdir);
    xfree(gone);  xfree(workdir);  workdir = NULL;
  }
}

static u32 number(const char * s, u32 max) {
  char * end;
  unsigned long v;
  errno = 0;
  v = strtoul(s, &end, 10);
  if (errno || !*s || *end || !v || v > max)
    FATAL("invalid numeric option: %s", s);
  return (u32) v;
}

#include "sort.h"
#include "spool.h"
#include "assemble.c"
#include "input.c"

#define EACH_K(F) F(1) F(2) F(3) F(4) F(5) F(6) F(7) F(8) F(9) F(10) \
  F(11) F(12) F(13) F(14) F(15) F(16) F(17) F(18) F(19) F(20) F(21) F(22) \
  F(23) F(24) F(25) F(26) F(27) F(28) F(29) F(30) F(31) F(32)

static u32 threads = 1;
static mkc_pool * pool, * scan_pool;

#include "seed.h"
#include "simd.h"
#include "seed.c"
#include "parse.c"
#include "direct.c"
#include "split.c"
#include "count.c"

static void banner(FILE * to) {
  fprintf(to,
    "mkc " PACKAGE_VERSION " -- exact DNA k-mer counter\n"
    "Written by Kamila Szewczyk <k@iczelia.net>.\n"
    "License GNU GPL version 3.\n");
}

static void usage(FILE * to) {
  banner(to);
  fprintf(to,
    "\n"
    "Usage: mkc build [-k 1..32] [-m minimizer-length] [-t threads] "
    "[-M MiB]\n"
    "                 [-T temp-directory] [--no-sync] [--seed 0..255] "
    "-o index input...\n"
    "       mkc query index KMER...   "
    "(or '-' to read k-mers, one per line)\n"
    "       mkc dump index\n"
    "       mkc info index\n"
    "       mkc -h | --help | -v | --version\n"
    "Inputs: FASTA/FASTQ, plain or compressed (gzip, BGZF, zstd, lz4); "
    "'-' reads\n"
    "stdin. Exact canonical counts.\n"
    "Defaults: k=31, m=min(12,max(9,k-8),k), threads=online CPUs, "
    "workspace 6400 MiB.\n");
}

static void version(void) {
  banner(stdout);
  printf("Build host %s.\n", MKC_HOST_TRIPLE);
  printf("Copyright (C) 2026 Kamila Szewczyk.\n"
         "This is free software: you are free to change and redistribute it.\n"
         "There is NO WARRANTY, to the extent permitted by law.\n");
  printf("SIMD: %s built, %s dispatched\n", mkc_simd_built(),
         mkc_simd_dispatched());
#ifdef MKC_X86
  printf("Lookups: %s\n", mkc_cpu_avx2(1) ? "avx2 kernels" : "portable");
#else
  printf("Lookups: portable\n");
#endif
}

static u64 peak(const mkc_index * db) {
  u64 m = 0, i, c, nd;
  if (db->layout == MKC_HASH) {
    Fi(db->over + db->stash,
      c = mkc_load64(db->map + db->table_end + 16 * i + 8);
      if (c > m) m = c);
    nd = db->direct_base ? mkc_load64(db->direct_base + 8 * db->groups) : 0;
    Fi(nd, c = mkc_load64(db->direct_counts + 8 * i);  if (c > m) m = c);
    if (db->over || nd) return m;
    if (db->packed) {
      mkc_iterator it = { 0 };
      u64 key;
      while (mkc_next(db, &it, &key, &c)) if (c > m) m = c;
      return m;
    }
    Fi(db->groups * db->cap * db->slots,
      c = mg_get(db->map + 128 + 64 * (i / db->slots),
                 (u32) (i % db->slots), db->bits) >> (db->qbits + 1);
      if (c > m) m = c);
    return m;
  }
  Fi(db->hcap, c = mkc_load64(db->map + db->hash_off + 16 * i + 8);
               if (c > m) m = c);
  if (db->hcap) return m;
  Fi(UINT64_C(1) << db->hbits, c = md_count(db, i);  if (c > m) m = c);
  return m;
}

static int do_info(const char * path) {
  mkc_index db;
  if (mkc_open_mapped(&db, path))
    FATAL("open index %s: %s", path, strerror(errno));
  printf("{\"layout\":\"%s\",\"k\":%u,\"distinct\":%" PRIu64
         ",\"occurrences\":%" PRIu64 ",\"bytes\":%" PRIu64,
         db.layout == MKC_DENSE ? "dense" : "hash", db.k, db.distinct,
         db.total, (u64) db.size);
  if (db.layout == MKC_HASH) {
    u64 slots = db.groups * db.cap * db.slots;
    printf(",\"slots\":%" PRIu64 ",\"groups\":%" PRIu64
           ",\"buckets_per_group\":%" PRIu64 ",\"record_bits\":%u"
           ",\"overflow\":%" PRIu64 ",\"stash\":%" PRIu64 ",\"fill\":%.9f",
           slots, db.groups, db.cap, db.bits, db.over, db.stash,
           slots ? (double) (db.distinct - db.stash) / slots : 0);
    if (db.packed)
      printf(",\"packed\":%u,\"low_bits\":%u,\"tag_bits\":%u"
             ",\"bits_per_kmer\":%.3f", db.packed, db.lbits, db.tagbits,
             8.0 * (double) db.size / (double) db.distinct);
  } else printf(",\"count_bytes\":%u,\"exceptions\":%" PRIu64, db.width,
                mkc_load64(db.map + 48));
  printf(",\"max_count\":%" PRIu64 "}\n", peak(&db));
  mkc_close(&db);
  return 0;
}

static void query_one(const mkc_index * db, const char * s, sz n) {
  u64 count;
  if (mkc_count(db, s, n, &count))
    FATAL("query must contain exactly %u A/C/G/T bases: %.*s", db->k,
          (int) n, s);
  printf("%.*s\t%" PRIu64 "\n", (int) n, s, count);
}

static int do_query(int argc, char ** argv) {
  mkc_index db;
  int i, many = argc == 4 && !strcmp(argv[3], "-");
  if ((many ? mkc_open : mkc_open_mapped)(&db, argv[2]))
    FATAL("open index %s: %s", argv[2], strerror(errno));
  if (many) {
    sz cap = 256, n = 0;
    char * line = xmalloc(cap);
    for (;;) {
      int more = fgets(line + n, (int) (cap - n), stdin) != NULL;
      if (more) n += strlen(line + n);
      if (more && (!n || line[n - 1] != '\n')) {
        if (n + 1 == cap) { cap *= 2;  line = xrealloc(line, cap); }
        continue;
      }
      while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
      if (n) query_one(&db, line, n);
      n = 0;
      if (!more) break;
    }
    if (ferror(stdin)) FATAL("stdin read failed");
    xfree(line);
  } else for (i = 3; i < argc; i++) query_one(&db, argv[i], strlen(argv[i]));
  mkc_close(&db);
  if (fflush(stdout)) FATAL("stdout write failed");
  return 0;
}

static int do_dump(const char * path) {
  mkc_index db;
  mkc_iterator it = { 0 };
  u64 key, count;
  char dna[33];
  u32 i;
  if (mkc_open_mapped(&db, path))
    FATAL("open index %s: %s", path, strerror(errno));
  while (mkc_next(&db, &it, &key, &count)) {
    dna[db.k] = 0;
    for (i = db.k; i; i--) { dna[i - 1] = "ACGT"[key & 3];  key >>= 2; }
    printf("%s\t%" PRIu64 "\n", dna, count);
  }
  mkc_close(&db);
  if (fflush(stdout)) FATAL("stdout write failed");
  return 0;
}

#define MAX_RESEED 3
#ifndef MKC_NO_RESEED
#define MKC_NO_RESEED 0
#endif

static int rereadable(int argc, char ** argv, int first) {
  int i;
  for (i = first; i < argc; i++)
    if (!strcmp(argv[i], "-") || !os_is_regular(argv[i])) return 0;
  return 1;
}

static void restart(int argc, char ** argv, u32 seed) {
  char v[16], ** a;
  int n = 0, i;
  snprintf(v, sizeof v, "%u", seed);
  a = xmalloc(((sz) argc + 4) * sizeof *a);
  a[n++] = argv[0];  a[n++] = argv[1];
  a[n++] = (char *) "--seed";  a[n++] = v;
  for (i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--seed") && i + 1 < argc) { i++;  continue; }
    a[n++] = argv[i];
  }
  a[n] = NULL;
  fflush(NULL);
  exit(os_run_again(a));
}

int main(int argc, char ** argv) {
  u32 cpus, mib = 6400;
  const char * out = NULL;
  char * tmp, * pre;
  int i = 2, dense = 1;
  u64 mem, total, distinct;
  sz n;
  mkc_file fd;
  double t0;
  os_utf8_args(&argc, &argv);
  if (argc < 2) { usage(stderr);  return 1; }
  if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))
    { usage(stdout);  return 0; }
  if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))
    { version();  return 0; }
  if (!strcmp(argv[1], "info")) {
    if (argc != 3) FATAL("info requires an index");
    return do_info(argv[2]);
  }
  if (!strcmp(argv[1], "dump")) {
    if (argc != 3) FATAL("dump requires an index");
    return do_dump(argv[2]);
  }
  if (!strcmp(argv[1], "query")) {
    if (argc < 4) { usage(stderr);  return 1; }
    return do_query(argc, argv);
  }
  if (strcmp(argv[1], "build")) { usage(stderr);  return 1; }

  cpus = os_cpus();
  threads = cpus > 64 ? 64 : cpus;
  tmp = os_temp_dir();
  if (!tmp) FATAL("out of memory");
  for (; i < argc; i++) {
    const char * opt = argv[i], * val;
    if (!strcmp(opt, "--")) { i++;  break; }
    if (opt[0] != '-' || !opt[1]) break;
    if (!strcmp(opt, "-h") || !strcmp(opt, "--help"))
      { usage(stdout);  return 0; }
    if (!strcmp(opt, "--no-sync")) { no_sync = 1;  continue; }
    if (!strcmp(opt, "--seed") && i + 1 < argc) {
      i++;
      table_seed = strcmp(argv[i], "0") ? number(argv[i], 255) : 0;
      continue;
    }
    if (strlen(opt) != 2 || !strchr("kmtMTo", opt[1]) || i + 1 == argc)
      FATAL("unknown/incomplete option: %s", opt);
    val = argv[++i];
    switch (opt[1]) {
    case 'k': K = number(val, 32);  break;
    case 'm': M = number(val, 32);  break;
    case 't': threads = number(val, 256);  break;
    case 'M': mib = number(val, 1u << 24);  break;
    case 'T':
      xfree(tmp);  tmp = xmalloc(strlen(val) + 1);  strcpy(tmp, val);
      break;
    case 'o': out = val;  break;
    }
  }
  if (!out || i == argc) { usage(stderr);  return 1; }
  if (!M) { M = K > 20 ? 12 : K > 17 ? K - 8 : 9;  if (M > K) M = K; }
  if (M > K) FATAL("minimizer length must not exceed k");
  mem = (u64) mib << 20;
#ifdef HAVE_AVX2
  if (mkc_simd_avx2()) decode = dec_avx2;
#endif
  pl.enabled = threads > 1;
  mx_init(&pl.mutex);  cv_init(&pl.cond);
  mx_init(&exc_lock);
  if (!(pool = tp_new(threads))
      || !(scan_pool = tp_new(threads > 1 ? threads - 1 : 1)))
    FATAL("cannot start threads");
  if (pool->size < threads) threads = pool->size;
  mx_init(&decoders.lock);  decoders.size = threads;
  batch = xpages(BATCH);
  n = strlen(tmp) + 8;
  pre = xmalloc(n);
  snprintf(pre, n, "%s/mkc.", tmp);
  if (!(workdir = os_mkunique(pre, 1, 0, NULL)))
    FATAL("create temporary directory: %s", os_error());
  xfree(pre);  xfree(tmp);
  atexit(cleanup);
  n = strlen(out) + 8;
  pre = xmalloc(n);
  snprintf(pre, n, "%s.tmp.", out);
  if (!(out_tmp = os_mkunique(pre, 0, 0, &fd)))
    FATAL("create output %s: %s", out, os_error());
  xfree(pre);
  t0 = os_now();
  may_reseed = OS_CAN_RUN_AGAIN && !MKC_NO_RESEED
               && table_seed < MAX_RESEED && rereadable(argc, argv, i);
  if (K <= DIRECT_MAX_K)
    distinct = direct_build(argc, argv, i, out_tmp, fd, mem, &total);
  else distinct = spool_build(argc, argv, i, mem, fd, out_tmp, &total,
                              &dense);
  os_pages_free(batch, BATCH);
  if (may_reseed && !dense && nstash > 4096
      && nstash > distinct / 64) {
    if (mg_gave_up)
      fprintf(stderr, "mkc: the k-mers do not fit their groups with hash "
              "seed %u (%" PRIu64 " of %" PRIu64 " displaced so far); "
              "building again with seed %u\n",
              table_seed, nstash, distinct, table_seed + 1);
    else fprintf(stderr, "mkc: %" PRIu64 " of %" PRIu64 " k-mers did not "
                 "fit their groups with hash seed %u; building again with "
                 "seed %u\n", nstash, distinct, table_seed,
                 table_seed + 1);
    os_close(fd);  cleanup();  out_tmp = NULL;
    restart(argc, argv, table_seed + 1);
  }
  if (mg_gave_up) FATAL("internal error: build abandoned without a restart");
  if (!no_sync && os_sync(fd)) FATAL("sync index: %s", os_error());
  if (os_close(fd)) FATAL("close index: %s", os_error());
  if (os_replace(out_tmp, out)) FATAL("publish index: %s", os_error());
  xfree(out_tmp);  out_tmp = NULL;
  fprintf(stderr, "mkc: k=%u m=%u threads=%u layout=%s occurrences=%" PRIu64
          " distinct=%" PRIu64 " total=%.3fs\n", K, M, threads,
          dense ? "dense" : "hash", total, distinct, os_now() - t0);
  tp_free(pool);  tp_free(scan_pool);
  tp_free(decoders.pool);
  return 0;
}
