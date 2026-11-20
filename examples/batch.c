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
#include <inttypes.h>

int main(int argc, char ** argv) {
  mkc_index db;
  size_t n, j;
  uint64_t * keys, * counts, key;
  const char * s;
  unsigned i;
  int b;
  if (argc < 3) {
    fprintf(stderr, "usage: %s index KMER...\n", argv[0]);
    return 2;
  }
  if (mkc_open(&db, argv[1])) { perror("mkc_open");  return 1; }
  n = (size_t) (argc - 2);
  keys = malloc(n * sizeof *keys);  counts = malloc(n * sizeof *counts);
  if (!keys || !counts) { perror("malloc");  return 1; }
  for (j = 0; j < n; j++) {
    s = argv[2 + j];  key = 0;
    if (strlen(s) != db.k) {
      fprintf(stderr, "%s: expected %u bases\n", s, db.k);
      return 2;
    }
    for (i = 0; i < db.k; i++) {
      b = mkc_base((unsigned char) s[i]);
      if (b < 0) { fprintf(stderr, "%s: invalid base\n", s);  return 2; }
      key = key << 2 | (unsigned) b;
    }
    keys[j] = key;
  }
  mkc_count_batch(&db, keys, counts, n);
  for (j = 0; j < n; j++) printf("%s\t%" PRIu64 "\n", argv[2 + j], counts[j]);
  free(keys);  free(counts);
  mkc_close(&db);
  return fflush(stdout) ? 1 : 0;
}
