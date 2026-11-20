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
#include <stdlib.h>

int main(int argc, char ** argv) {
  enum { B = 1000 };
  mkc_index d;
  mkc_iterator it = { 0 };
  uint64_t stride, n = 0, bad = 0, s = 0x9E3779B97F4A7C15ull;
  uint64_t keys[3 * B], want[3 * B], got[3 * B], key, count, rc, other,
           expect;
  uint64_t (*generic)(const mkc_index *, uint64_t);
  size_t held = 0, i;
  int more;
  if (argc < 2 || mkc_open_mapped(&d, argv[1])) { perror("open");  return 2; }
  stride = argc > 2 ? strtoull(argv[2], NULL, 10) : 1;
  generic = d.layout == MKC_DENSE ? mkc_generic_dense : mkc_generic_hash;
  if (d.count == generic) {
    printf("no kernel for this index (k=%u bits=%u); generic only\n", d.k,
           d.bits);
    return 0;
  }
  for (more = 1; more;) {
    more = mkc_next(&d, &it, &key, &count);
    if (more && n++ % stride == 0) {
      rc = mkc_revcomp(key, d.k);
      s ^= s << 13;  s ^= s >> 7;  s ^= s << 17;
      other = s & mkc_mask(d.k);  expect = generic(&d, other);
      bad += d.count(&d, key) != count || d.count(&d, rc) != count
             || d.count(&d, other) != expect;
      keys[held] = key;  want[held++] = count;
      keys[held] = rc;  want[held++] = count;
      keys[held] = other;  want[held++] = expect;
    }
    if (held == 3 * B || (!more && held)) {
      mkc_count_batch(&d, keys, got, held);
      for (i = 0; i < held; i++) bad += got[i] != want[i];
      held = 0;
    }
  }
  printf("kernel k=%u layout=%s bits=%u keys=%" PRIu64 " bad=%" PRIu64 "\n",
         d.k, d.layout == MKC_DENSE ? "dense" : "hash", d.bits, n, bad);
  return bad != 0;
}
