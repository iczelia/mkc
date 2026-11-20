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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zstd.h"
#include "lz4frame.h"

static void fail(const char * what) {
  fprintf(stderr, "pack: %s\n", what);  exit(1);
}

static unsigned char * slurp(size_t * n) {
  size_t cap = 1 << 20, got;
  unsigned char * p = malloc(cap);
  *n = 0;
  while (p && (got = fread(p + *n, 1, cap - *n, stdin)) > 0) {
    *n += got;
    if (*n == cap && !(p = realloc(p, cap *= 2))) break;
  }
  if (!p) fail("out of memory");
  return p;
}

static void emit(const void * p, size_t n) {
  if (fwrite(p, 1, n, stdout) != n) fail("write");
}

static void skippable(void) {
  static const unsigned char frame[12] = {
    0x5A, 0x2A, 0x4D, 0x18, 4, 0, 0, 0, 'm', 'k', 'c', '!'
  };
  emit(frame, sizeof frame);
}

int main(int argc, char ** argv) {
  size_t n;
  unsigned char * in;
  if (argc < 2) fail("usage: see tests/pack.c");
  in = slurp(&n);
  if (!strcmp(argv[1], "zstd-frame") || !strcmp(argv[1], "zstd-stream")) {
    ZSTD_CCtx * c = ZSTD_createCCtx();
    size_t cap, got;
    unsigned char * out;
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel,
                           argc > 2 ? atoi(argv[2]) : 3);
    ZSTD_CCtx_setParameter(c, ZSTD_c_contentSizeFlag,
                           !strcmp(argv[1], "zstd-frame"));
    ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 1);
    cap = ZSTD_compressBound(n);  out = malloc(cap);
    if (!strcmp(argv[1], "zstd-stream")) {
      ZSTD_inBuffer ib = { in, n, 0 };
      ZSTD_outBuffer ob = { out, cap, 0 };
      size_t r;
      do r = ZSTD_compressStream2(c, &ob, &ib, ZSTD_e_end);
      while (r && !ZSTD_isError(r));
      if (ZSTD_isError(r)) fail(ZSTD_getErrorName(r));
      emit(out, ob.pos);
    } else {
      got = ZSTD_compress2(c, out, cap, in, n);
      if (ZSTD_isError(got)) fail(ZSTD_getErrorName(got));
      emit(out, got);
    }
    free(out);  ZSTD_freeCCtx(c);
  } else if (!strcmp(argv[1], "zstd-frames") && argc > 2) {
    size_t chunk = (size_t) atol(argv[2]), at, i, take, got;
    unsigned char * out = malloc(ZSTD_compressBound(chunk));
    ZSTD_CCtx * c = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 1);
    for (at = 0, i = 0; at < n; at += chunk, i++) {
      take = n - at < chunk ? n - at : chunk;
      ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, 1 + (int) (i % 5));
      got = ZSTD_compress2(c, out, ZSTD_compressBound(chunk), in + at, take);
      if (ZSTD_isError(got)) fail(ZSTD_getErrorName(got));
      emit(out, got);
      if (!i) skippable();
    }
    free(out);  ZSTD_freeCCtx(c);
  } else if (!strcmp(argv[1], "lz4") && argc > 7) {
    LZ4F_preferences_t prefs;
    size_t chunk, at, i, take, cap, got;
    unsigned char * out;
    int sized;
    memset(&prefs, 0, sizeof prefs);
    prefs.frameInfo.blockSizeID = (LZ4F_blockSizeID_t) atoi(argv[2]);
    prefs.frameInfo.blockMode = atoi(argv[3]) ? LZ4F_blockIndependent
                                              : LZ4F_blockLinked;
    prefs.frameInfo.blockChecksumFlag = atoi(argv[4])
                                        ? LZ4F_blockChecksumEnabled
                                        : LZ4F_noBlockChecksum;
    prefs.frameInfo.contentChecksumFlag = atoi(argv[5])
                                          ? LZ4F_contentChecksumEnabled
                                          : LZ4F_noContentChecksum;
    sized = atoi(argv[6]);  chunk = (size_t) atol(argv[7]);
    if (!chunk) chunk = n ? n : 1;
    for (at = 0, i = 0; at < n || (!n && !i); at += chunk, i++) {
      take = n - at < chunk ? n - at : chunk;
      prefs.frameInfo.contentSize = sized ? take : 0;
      cap = LZ4F_compressFrameBound(take, &prefs);  out = malloc(cap);
      got = LZ4F_compressFrame(out, cap, in + at, take, &prefs);
      if (LZ4F_isError(got)) fail(LZ4F_getErrorName(got));
      if (i) skippable();
      emit(out, got);  free(out);
      if (!n) break;
    }
  } else fail("unknown mode");
  free(in);
  return fflush(stdout) ? 1 : 0;
}
