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
#include <inttypes.h>
#include <stdio.h>

typedef struct { uint64_t n, sum, xr; } print;

static void add(print * p, uint64_t key, uint64_t count) {
  uint64_t v = mkc_mix(key ^ mkc_mix(count));
  p->n++;  p->sum += v;  p->xr ^= v;
}

int main(int argc, char ** argv) {
  const uint64_t chunks[] = { 1, 7, 1000, UINT64_C(1) << 20 };
  mkc_index d;
  print all = { 0 };
  mkc_iterator it = { 0 };
  uint64_t key, count, end, start;
  unsigned c;
  int bad;
  if (argc != 2 || mkc_open_mapped(&d, argv[1])) { perror("open");  return 2; }
  while (mkc_next(&d, &it, &key, &count)) add(&all, key, count);
  bad = all.n != d.distinct || it.at > mkc_iter_end(&d);
  end = mkc_iter_end(&d);
  for (c = 0; c < sizeof chunks / sizeof *chunks; c++) {
    print part = { 0 };
    for (start = 0; start < end; start += chunks[c]) {
      mkc_iterator r = { start };
      while (r.at < start + chunks[c] && mkc_next(&d, &r, &key, &count)
             && r.at - 1 < start + chunks[c])
        add(&part, key, count);
    }
    if (part.n != all.n || part.sum != all.sum || part.xr != all.xr) {
      printf("chunk %" PRIu64 ": %" PRIu64 " keys against %" PRIu64 "\n",
             chunks[c], part.n, all.n);
      bad = 1;
    }
  }
  if (bad)
    printf("iterate %s: distinct %" PRIu64 ", sequential %" PRIu64 "\n",
           argv[1], d.distinct, all.n);
  mkc_close(&d);
  return bad;
}
