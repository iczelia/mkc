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


#include "mkc.h"
#include <stdio.h>
#include <stdlib.h>

static uint64_t s = 0x9E3779B97F4A7C15ull;

static uint64_t rnd(void) {
  s ^= s << 13;  s ^= s >> 7;  s ^= s << 17;  return s;
}

static void dna(uint64_t key, unsigned k, char * out) {
  unsigned j;
  out[k] = 0;
  for (j = k; j; j--) { out[j - 1] = "ACGT"[key & 3];  key >>= 2; }
}

int main(int argc, char ** argv) {
  unsigned k, p, bits, times, t;
  uint64_t n, r, j, h, key;
  FILE * want, * fixture;
  char text[33];
  if (argc != 6 && argc != 7) return 2;
  k = (unsigned) atoi(argv[1]);  p = (unsigned) atoi(argv[3]);
  bits = mkc_code_bits(k);
  n = strtoull(argv[2], NULL, 10);  r = strtoull(argv[4], NULL, 10);
  if (!(k & 1) || k > 31 || p >= bits
      || (bits - p < 63 && n > (UINT64_C(1) << (bits - p))))
    return 2;
  want = fopen(argv[5], "w");
  fixture = argc == 7 ? fopen(argv[6], "wb") : NULL;
  if (!want || (argc == 7 && !fixture)) return 3;
  for (j = 0; j < n + r; j++) {
    if (j < n) h = (j * UINT64_C(0x9E3779B97F4A7C15)) & mkc_bits(bits - p);
    else do h = rnd() & mkc_bits(bits);  while (!(h >> (bits - p)));
    key = mkc_uncode(mkc_unhash(h, bits), k);
    times = j < n && j % 9973 == 0 ? 70000 : j < n && j % 997 == 0 ? 3 : 1;
    dna(key, k, text);
    for (t = 0; t < times; t++)
      printf(">%llu\n%s\n", (unsigned long long) j, text);
    fprintf(want, "%s\t%u\n", text, times);
    if (fixture && j < 1000000) {
      uint64_t rec[2];
      rec[0] = key;  rec[1] = times;
      fwrite(rec, 8, 2, fixture);
    }
  }
  if (fixture && fclose(fixture)) return 3;
  return fclose(want) || fflush(stdout) ? 3 : 0;
}
