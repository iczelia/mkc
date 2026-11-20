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
  uint64_t count;
  int status;
  if (argc != 3) {
    fprintf(stderr, "usage: %s index KMER\n", argv[0]);
    return 2;
  }
  if (mkc_open(&db, argv[1])) { perror("mkc_open");  return 1; }
  status = mkc_count(&db, argv[2], strlen(argv[2]), &count);
  if (!status) printf("%" PRIu64 "\n", count);
  else fprintf(stderr, "expected %u A/C/G/T bases\n", db.k);
  mkc_close(&db);
  return status != 0;
}
