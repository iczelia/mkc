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

#include <inttypes.h>
#include <stdarg.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

static void message(const char * fmt, va_list ap) MKC_PRINTF(1, 0);
static void message(const char * fmt, va_list ap) {
  static int dying;
  if (__atomic_exchange_n(&dying, 1, __ATOMIC_ACQ_REL))
#ifdef _WIN32
    for (;;) Sleep(1000);
#else
    for (;;) sleep(1);
#endif
  fputs("mkc: ", stderr);  vfprintf(stderr, fmt, ap);  fputc('\n', stderr);
}

void mkc_fatal(const char * fmt, ...) {
  va_list ap;
  va_start(ap, fmt);  message(fmt, ap);  va_end(ap);
  exit(MKC_EXIT_FAIL);
}

void mkc_fatal_unless(int cond, const char * fmt, ...) {
  va_list ap;
  if (cond) return;
  va_start(ap, fmt);  message(fmt, ap);  va_end(ap);
  exit(MKC_EXIT_FAIL);
}

static void * fail(sz n) {
  FATAL("out of memory allocating %" PRIu64 " bytes", (u64) n);
}

void * xmalloc(sz n) {
  void * p = malloc(n ? n : 1);
  return p ? p : fail(n);
}

void * xcalloc(sz n, sz size) {
  void * p = calloc(n ? n : 1, size ? size : 1);
  return p ? p : fail(n && size <= SIZE_MAX / n ? n * size : SIZE_MAX);
}

void * xrealloc(void * p, sz n) {
  void * q = realloc(p, n ? n : 1);
  return q ? q : fail(n);
}

void xfree(void * p) { free(p); }
