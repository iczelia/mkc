#!/bin/sh
#  Copyright (C) 2026 Kamila Szewczyk
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, version 3.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.

test -n "$PYTHON" || exit 77
"$PYTHON" "$srcdir/tests/stash.py" "$MKC" "$MKC_NO_RESEED" \
  "$MKC_ADVERSARIAL" || exit 1
"$PYTHON" "$srcdir/tests/stash.py" "$MKC_PACKED" "$MKC_PACKED_NO_RESEED" \
  "$MKC_ADVERSARIAL" || exit 1
