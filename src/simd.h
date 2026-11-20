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

#ifndef MKC_SIMD_H
#define MKC_SIMD_H

#include "common.h"

#ifdef HAVE_AVX2
void dec_avx2(u8 * p, sz n);
void scan_ranks_avx2(const u8 * p, sz count, u32 m, u32 * out);
void scan_minima_avx2(const u32 * r, sz len, u32 w, u32 * a, u32 * out);
#endif

#endif
