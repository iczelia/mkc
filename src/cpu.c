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

#include "cpu.h"

#if defined(HAVE_AVX2) && defined(__x86_64__)
#include <cpuid.h>

static int probe_avx2(void) {
  u32 a, b, c, d, lo, hi;
  const u32 want = 1u << 3 | 1u << 5 | 1u << 8;
  if (!__get_cpuid(1, &a, &b, &c, &d) || (c & (3u << 27)) != (3u << 27))
    return 0;
  __asm__ volatile ("xgetbv" : "=a" (lo), "=d" (hi) : "c" (0));
  if ((lo & 6) != 6) return 0;
  if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return 0;
  return (b & want) == want;
}
#define MKC_AVX2_KERNEL 1
#else
static int probe_avx2(void) { return 0; }
#define MKC_AVX2_KERNEL 0
#endif

static int decided, use_avx2;

static void decide(void) {
  const char * e = getenv("MKC_SIMD");
  if (decided) return;
  decided = 1;
  use_avx2 = probe_avx2();
  if (!e || !*e) return;
  if (!strcmp(e, "scalar")) use_avx2 = 0;
  else if (!strcmp(e, "avx2")) {
    if (!MKC_AVX2_KERNEL) FATAL("MKC_SIMD=avx2 but no AVX2 kernel was built");
    if (!use_avx2)
      FATAL("MKC_SIMD=avx2 but this CPU or OS has no AVX2 and BMI2 support");
  } else FATAL("invalid MKC_SIMD value '%s'", e);
}

int mkc_simd_avx2(void) { decide();  return use_avx2; }

const char * mkc_simd_built(void) { return MKC_AVX2_KERNEL ? "avx2" : "none"; }

const char * mkc_simd_dispatched(void) {
  return mkc_simd_avx2() ? "avx2" : "scalar";
}
