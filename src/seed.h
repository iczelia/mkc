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

#ifndef MKC_SEED_H
#define MKC_SEED_H

#include "common.h"

#define DEC_BIAS 8192
#define DEC_EPS  2

extern u32 dec_pk[4][256];
extern u32 dec_chunks;

SI u32 seed_rank(u32 x) {
  x ^= x >> 16;  x *= UINT32_C(0x7FEB352D);
  x ^= x >> 15;  x *= UINT32_C(0x846CA68B);
  return x ^ (x >> 16);
}

SI u32 seed_tier(i32 a, i32 b) {
  if (a > DEC_EPS && b <= DEC_EPS) return 0;
  if (a < -DEC_EPS && b >= -DEC_EPS) return 1;
  return 2;
}

SI u32 seed_order(u32 seed) {
  u32 i, sum = 0;
  i32 bias = (i32) dec_chunks * DEC_BIAS;
  Fi(dec_chunks, sum += dec_pk[i][(seed >> (8 * i)) & 255]);
  return (seed_rank(seed) >> 2)
         | seed_tier((i32) (sum & 0xFFFF) - bias, (i32) (sum >> 16) - bias)
           << 30;
}

#endif
