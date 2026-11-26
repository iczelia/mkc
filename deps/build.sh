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

set -eu
target=${1:-native}
here=`dirname "$0"`
here=`cd "$here" && pwd`
cache=${MKC_DEPS_CACHE:-$here/cache}
outs=${MKC_DEPS_OUT:-$here/out}
out=$outs/$target
work=${MKC_DEPS_WORK:-$here/work}/$target
jobs=`getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4`

ISAL=isa-l-2.32.1
ISAL_URL=https://github.com/intel/isa-l/archive/refs/tags/v2.32.1.tar.gz
ISAL_SHA=d9f7179ab0e14a3db9b610fac22793854a1435e8423ec9ce07f4cbedc5f92f5e
ZSTD=zstd-1.5.7
ZSTD_URL=https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
ZSTD_SHA=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
LZ4=lz4-1.10.0
LZ4_URL=https://github.com/lz4/lz4/releases/download/v1.10.0/lz4-1.10.0.tar.gz
LZ4_SHA=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
NASM=nasm-2.16.03
NASM_URL=https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/nasm-2.16.03.tar.xz
NASM_SHA=1412a1c760bbd05db026b6c0d1657affd6631cd0a63cddb6f73cc6d4aa616148

sha256() {
  if command -v sha256sum >/dev/null; then sha256sum "$1"
  else shasum -a 256 "$1"; fi | cut -d' ' -f1
}

fetch() {
  file=$cache/`basename "$2"`
  case $1 in isa-l-*) file=$cache/$1.tar.gz ;; esac
  mkdir -p "$cache" "$work"
  if test ! -f "$file"; then
    echo "deps: downloading $2" >&2
    curl -sSfL -o "$file.part" "$2" || wget -q -O "$file.part" "$2"
    mv "$file.part" "$file"
  fi
  if test "`sha256 "$file"`" != "$3"; then
    echo "deps: checksum mismatch for $file" >&2;  exit 1
  fi
  rm -rf "$work/$1"
  case $file in
    *.xz) xz -dc "$file" | tar xf - -C "$work" ;;
    *)    gzip -dc "$file" | tar xf - -C "$work" ;;
  esac
}

case $target in
  native)
    CC=${CC:-cc};  AR=${AR:-ar};  isal_arch=;  host=${2:-`uname -m`} ;;
  mingw)
    if test -z "${CC:-}"; then
      CC=x86_64-w64-mingw32-gcc
      command -v x86_64-w64-mingw32-clang >/dev/null && CC=x86_64-w64-mingw32-clang
    fi
    AR=${AR:-x86_64-w64-mingw32-ar}
    command -v "$AR" >/dev/null || AR=llvm-ar
    isal_arch=mingw;  host=x86_64 ;;
  *) echo "usage: $0 native|mingw" >&2;  exit 2 ;;
esac
case $host in
  amd64|x86_64) host=x86_64 ;;
  arm64|aarch64) host=aarch64 ;;
esac
mkdir -p "$out/include" "$out/lib"
SECT="-ffunction-sections -fdata-sections"
OPT="-O3 -fPIC $SECT"
test "$target" = mingw && OPT="-O3 $SECT"

nasm_ok() {
  v=`nasm -v 2>/dev/null | sed -n 's/^NASM version \([0-9]*\)\.\([0-9]*\).*/\1 \2/p'`
  test -n "$v" || return 1
  major=`echo $v | cut -d' ' -f1`;  minor=`echo $v | cut -d' ' -f2`
  test "$major" -gt 2 || { test "$major" -eq 2 && test "$minor" -ge 14; }
}

fetch $ISAL $ISAL_URL $ISAL_SHA
asm=nasm
if test "$host" = x86_64; then
  if ! nasm_ok; then
    tools=$outs/host
    if test ! -x "$tools/bin/nasm"; then
      fetch $NASM $NASM_URL $NASM_SHA
      (cd "$work/$NASM" && ./configure --prefix="$tools" >/dev/null \
         && make -j"$jobs" nasm >/dev/null && mkdir -p "$tools/bin" \
         && cp nasm "$tools/bin/")
    fi
    asm=$tools/bin/nasm
  fi
  isal_cpu=x86_64
  if test "$target" = native && test "`uname -s`" = Darwin; then isal_arch=Darwin; fi
elif test "$host" = aarch64; then
  isal_cpu=aarch64;  isal_arch=aarch64;  asm="$CC -D__ASSEMBLY__"
else
  isal_cpu=base_aliases;  isal_arch=noarch
fi
(cd "$work/$ISAL" && make -f Makefile.unx -j"$jobs" lib CC="$CC" AR="$AR" \
   AS="$asm" DEBUG="$SECT" arch="$isal_arch" host_cpu="$isal_cpu" >/dev/null)
cp "$work/$ISAL/bin/isa-l.a" "$out/lib/libisal.a"
cp "$work/$ISAL/include/igzip_lib.h" "$out/include/"

fetch $ZSTD $ZSTD_URL $ZSTD_SHA
zstd_make() {
  (cd "$work/$ZSTD/lib" && make -j"$jobs" libzstd.a-nomt CC="$CC" AR="$AR" \
     MOREFLAGS="$OPT" ZSTD_LIB_DICTBUILDER=0 ZSTD_LIB_DEPRECATED=0 \
     ZSTD_LEGACY_SUPPORT=0 "$@" >/dev/null)
}
zstd_make ZSTD_LIB_COMPRESSION=0
cp "$work/$ZSTD/lib/libzstd.a" "$out/lib/libzstd-dec.a"
(cd "$work/$ZSTD/lib" && make clean >/dev/null)
zstd_make
cp "$work/$ZSTD/lib/libzstd.a" "$out/lib/"
cp "$work/$ZSTD/lib/zstd.h" "$work/$ZSTD/lib/zstd_errors.h" "$out/include/"

fetch $LZ4 $LZ4_URL $LZ4_SHA
(cd "$work/$LZ4/lib" && make -j"$jobs" liblz4.a CC="$CC" AR="$AR" \
   CFLAGS="$OPT" >/dev/null)
cp "$work/$LZ4/lib/liblz4.a" "$out/lib/"
cp "$work/$LZ4/lib/lz4.h" "$work/$LZ4/lib/lz4frame.h" \
   "$work/$LZ4/lib/xxhash.h" "$out/include/"

rm -rf "$work"
touch "$out/.built"
echo "deps: $target libraries in $out" >&2
