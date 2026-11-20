dnl  Copyright (C) 2026 Kamila Szewczyk
dnl
dnl  This program is free software; you can redistribute it and/or modify
dnl  it under the terms of the GNU General Public License as published by
dnl  the Free Software Foundation, version 3.
dnl
dnl  This program is distributed in the hope that it will be useful,
dnl  but WITHOUT ANY WARRANTY; without even the implied warranty of
dnl  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
dnl  GNU General Public License for more details.
dnl
dnl  You should have received a copy of the GNU General Public License
dnl  along with this program. If not, see <http://www.gnu.org/licenses/>.

AC_PREREQ([2.70])
AC_INIT([mkc], m4_esyscmd_s([build-aux/version.sh]), [https://github.com/iczelia/mkc])
AC_CONFIG_HEADERS([config.h])
AC_COPYRIGHT([Copyright (C) 2026 Kamila Szewczyk])
AC_LANG([C])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([build-aux])

AC_CANONICAL_HOST
AM_INIT_AUTOMAKE([-Wall foreign color-tests subdir-objects dist-xz])

test "${CFLAGS+set}" = set || CFLAGS="-O3"
AC_PROG_CC
AC_USE_SYSTEM_EXTENSIONS
AM_PROG_AR
AC_PROG_RANLIB
AC_PATH_TOOL([PKG_CONFIG], [pkg-config])

AC_DEFINE_UNQUOTED([MKC_HOST_TRIPLE], ["$host"],
                   [Canonical host this build targets])

AC_SYS_LARGEFILE

AC_MSG_CHECKING([for a 64-bit target with 128-bit integers])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]], [[
    __extension__ typedef unsigned __int128 u128;
    u128 x = (u128) 1 << 100;
    return (int) (x >> 100) + (int) sizeof (void *) - 9;
  ]])],
  [AC_MSG_RESULT([yes])],
  [AC_MSG_RESULT([no])
   AC_MSG_ERROR([mkc needs a 64-bit GCC or Clang target])])

case "$host_os" in
  mingw*)               mkc_is_windows=yes ;;
  *)                    mkc_is_windows=no  ;;
esac
AM_CONDITIONAL([MKC_IS_WINDOWS], [test "x$mkc_is_windows" = "xyes"])
MKC_WIN_LDFLAGS=""
MKC_THREAD_CFLAGS=""
MKC_THREAD_LDFLAGS=""
if test "x$mkc_is_windows" = "xyes"; then
  MKC_WIN_LDFLAGS="-static"
  mkc_deps_target=mingw
else
  AX_APPEND_COMPILE_FLAGS([-pthread], [MKC_THREAD_CFLAGS])
  AX_APPEND_LINK_FLAGS([-pthread], [MKC_THREAD_LDFLAGS])
  AC_SEARCH_LIBS([pthread_create], [pthread], [],
    [AC_MSG_ERROR([POSIX threads are required])])
  AC_CHECK_HEADERS([sys/mman.h sys/uio.h sys/resource.h dirent.h], [],
    [AC_MSG_ERROR([a POSIX host is required])])
  mkc_deps_target=native
fi
AC_SUBST([MKC_WIN_LDFLAGS])
AC_SUBST([MKC_THREAD_CFLAGS])
AC_SUBST([MKC_THREAD_LDFLAGS])

mkc_skip_simd=no
case "$host_cpu" in
  x86_64*|amd64*) mkc_is_x86=yes ;;
  *)              mkc_is_x86=no  ;;
esac
AC_ARG_ENABLE([simd],
  [AS_HELP_STRING([--disable-simd], [build the portable scanner and lookups only])],
  [test "x$enableval" = "xno" && mkc_skip_simd=yes])

MKC_AVX2_CFLAGS=""
MKC_KERNEL_CPPFLAGS="-DMKC_NO_KERNELS"
if test "x$mkc_skip_simd" = "xno" && test "x$mkc_is_x86" = "xyes"; then
  MKC_CHECK_ISA([AVX2 and BMI2 intrinsics and runtime probing], [-mavx2 -mbmi -mbmi2], [[
      #include <immintrin.h>
      #include <cpuid.h>
    ]], [[
      __m256i a = _mm256_set1_epi32(1);
      unsigned r[4], lo, hi;
      a = _mm256_mullo_epi32(a, a);
      __get_cpuid_count(7, 0, r, r + 1, r + 2, r + 3);
      __asm__ volatile ("xgetbv" : "=a" (lo), "=d" (hi) : "c" (0));
      return _mm256_extract_epi32(a, 0) + (int) _pext_u64(r[1], lo + hi)
             + (int) _tzcnt_u64(r[2]);
    ]], [HAVE_AVX2], [Define if the AVX2 scanner is built], [MKC_AVX2_CFLAGS])
  AC_MSG_CHECKING([for target attributes on AVX2 lookup kernels])
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
      #include <immintrin.h>
      __attribute__((target("avx2,bmi,bmi2")))
      static unsigned f(unsigned long long x) {
        return (unsigned) _pext_u64(x, x >> 1)
               + (unsigned) _mm256_movemask_epi8(_mm256_set1_epi64x((long long) x));
      }
    ]], [[return (int) f(3);]])],
    [AC_MSG_RESULT([yes])
     MKC_KERNEL_CPPFLAGS=""],
    [AC_MSG_RESULT([no])])
fi
AC_SUBST([MKC_AVX2_CFLAGS])
AM_CONDITIONAL([MKC_AVX2], [test "x$MKC_AVX2_CFLAGS" != "x"])
AC_SUBST([MKC_KERNEL_CPPFLAGS])
test -z "$MKC_KERNEL_CPPFLAGS" && mkc_lookups=yes || mkc_lookups=no

AC_ARG_WITH([bundled-deps],
  [AS_HELP_STRING([--with-bundled-deps=@<:@auto|yes|no@:>@],
    [build ISA-L, zstd and lz4 from the pinned sources in deps/ and link them
     statically (default: yes; auto: only when the system lacks one of them;
     no: always use the system libraries)])],
  [mkc_bundled=$withval], [mkc_bundled=yes])
case "$mkc_bundled" in
  auto|yes|no) ;;
  *) AC_MSG_ERROR([--with-bundled-deps must be auto, yes or no]) ;;
esac

mkc_system_deps=no
if test "x$mkc_bundled" != "xyes"; then
  mkc_system_deps=yes
  mkc_save_LIBS=$LIBS
  mkc_save_CPPFLAGS=$CPPFLAGS
  mkc_cflags=""
  mkc_libs=""
  for mkc_pkg in libzstd:-lzstd liblz4:-llz4 libisal:-lisal; do
    mkc_name=`echo "$mkc_pkg" | sed 's/:.*//'`
    mkc_lib=`echo "$mkc_pkg" | sed 's/.*://'`
    if test -n "$PKG_CONFIG" && $PKG_CONFIG --exists "$mkc_name"; then
      mkc_cflags="$mkc_cflags `$PKG_CONFIG --cflags $mkc_name`"
      mkc_lib=`$PKG_CONFIG --libs $mkc_name`
    fi
    mkc_libs="$mkc_libs $mkc_lib"
  done
  CPPFLAGS="$CPPFLAGS $mkc_cflags"
  LIBS="$mkc_libs $LIBS"
  AC_CHECK_HEADERS([zstd.h lz4.h lz4frame.h xxhash.h igzip_lib.h], [],
                   [mkc_system_deps=no])
  if test "x$mkc_system_deps" = "xyes"; then
    AC_MSG_CHECKING([for the zstd, lz4 and ISA-L decoders])
    AC_LINK_IFELSE([AC_LANG_PROGRAM([[
        #include <zstd.h>
        #include <lz4.h>
        #include <lz4frame.h>
        #include <igzip_lib.h>
      ]], [[
        struct inflate_state s;
        isal_inflate_init(&s);
        return (int) ZSTD_freeDCtx(ZSTD_createDCtx())
               + (int) LZ4F_isError(0) + LZ4_decompress_safe(0, 0, 0, 0);
      ]])],
      [AC_MSG_RESULT([yes])],
      [AC_MSG_RESULT([no]);  mkc_system_deps=no])
  fi
  mkc_xxh=
  if test "x$mkc_system_deps" = "xyes"; then
    AC_MSG_CHECKING([for xxHash inside liblz4])
    AC_LINK_IFELSE([AC_LANG_PROGRAM([[
        #define XXH_NAMESPACE LZ4_
        #include <xxhash.h>
      ]], [[return (int) XXH32("", 0, 0);]])],
      [AC_MSG_RESULT([yes]);  mkc_xxh=lz4],
      [AC_MSG_RESULT([no])])
    if test -z "$mkc_xxh"; then
      AC_SEARCH_LIBS([XXH32], [xxhash],
        [test "x$ac_cv_search_XXH32" = "xnone required" || \
           mkc_libs="$mkc_libs $ac_cv_search_XXH32"],
        [mkc_system_deps=no])
    fi
  fi
  LIBS=$mkc_save_LIBS
  CPPFLAGS=$mkc_save_CPPFLAGS
  if test "x$mkc_system_deps" = "xno" && test "x$mkc_bundled" = "xno"; then
    AC_MSG_ERROR([ISA-L, zstd, lz4 or xxHash are missing; use --with-bundled-deps])
  fi
  MKC_DEPS_CPPFLAGS="$mkc_cflags"
  MKC_DEPS_LIBS="$mkc_libs"
  MKC_PACK_LIBS="$mkc_libs"
  test "x$mkc_xxh" = "xlz4" && \
    AC_DEFINE([MKC_XXH_LZ4], [1], [Define when xxHash comes from liblz4])
fi
if test "x$mkc_system_deps" = "xno"; then
  if test "x$mkc_is_x86" = "xyes" && test "x$mkc_is_windows" = "xno"; then
    AC_CHECK_PROGS([NASM], [nasm], [])
  fi
  AC_CHECK_PROGS([MKC_FETCH], [curl wget], [])
  mkc_deps="deps/out/$mkc_deps_target"
  MKC_DEPS_CPPFLAGS="-isystem \$(top_builddir)/$mkc_deps/include"
  MKC_DEPS_LIBS="\$(top_builddir)/$mkc_deps/lib/libisal.a"
  MKC_DEPS_LIBS="$MKC_DEPS_LIBS \$(top_builddir)/$mkc_deps/lib/libzstd-dec.a"
  MKC_DEPS_LIBS="$MKC_DEPS_LIBS \$(top_builddir)/$mkc_deps/lib/liblz4.a"
  MKC_PACK_LIBS="\$(top_builddir)/$mkc_deps/lib/libzstd.a"
  MKC_PACK_LIBS="$MKC_PACK_LIBS \$(top_builddir)/$mkc_deps/lib/liblz4.a"
  MKC_DEPS_STAMP="$mkc_deps/.built"
  AC_DEFINE([MKC_XXH_LZ4], [1], [Define when xxHash comes from liblz4])
  mkc_deps_summary="bundled ($mkc_deps)"
else
  MKC_DEPS_STAMP=""
  mkc_deps_target=
  mkc_deps_summary="system"
fi
AC_SUBST([MKC_DEPS_CPPFLAGS])
AC_SUBST([MKC_DEPS_LIBS])
AC_SUBST([MKC_PACK_LIBS])
AC_SUBST([MKC_DEPS_STAMP])
AC_SUBST([MKC_DEPS_TARGET], [$mkc_deps_target])
AM_CONDITIONAL([MKC_BUNDLED], [test "x$mkc_system_deps" = "xno"])

AX_APPEND_COMPILE_FLAGS([-Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align \
  -Wwrite-strings \
  -Wmissing-prototypes -Wstrict-prototypes -Wredundant-decls \
  -Wno-unused-parameter -Wvla -Wformat=2 -Wundef], [MKC_WARN_CFLAGS])
AC_SUBST([MKC_WARN_CFLAGS])

AX_APPEND_COMPILE_FLAGS([-fno-delete-null-pointer-checks], [MKC_HARDEN_CFLAGS])
AC_SUBST([MKC_HARDEN_CFLAGS])

AX_APPEND_COMPILE_FLAGS([-falign-functions=64], [MKC_OPT_CFLAGS])
AC_SUBST([MKC_OPT_CFLAGS])

AC_ARG_ENABLE([sanitizers],
  [AS_HELP_STRING([--enable-sanitizers=LIST],
    [build with address,undefined (or yes) or undefined sanitizers])],
  [enable_sanitizers=$enableval], [enable_sanitizers=no])
case "$enable_sanitizers" in
  no) ;;
  yes|address|address,undefined|undefined,address)
    enable_sanitizers=address,undefined
    AX_APPEND_COMPILE_FLAGS([-fsanitize=address -fsanitize=undefined \
                             -fno-omit-frame-pointer], [MKC_SAN_CFLAGS])
    AX_APPEND_LINK_FLAGS([-fsanitize=address -fsanitize=undefined],
                         [MKC_SAN_LDFLAGS])
    ;;
  undefined)
    AX_APPEND_COMPILE_FLAGS([-fsanitize=undefined -fno-omit-frame-pointer],
                            [MKC_SAN_CFLAGS])
    AX_APPEND_LINK_FLAGS([-fsanitize=undefined], [MKC_SAN_LDFLAGS])
    ;;
  *) AC_MSG_ERROR([unknown sanitizer selection: $enable_sanitizers]) ;;
esac
AC_SUBST([MKC_SAN_CFLAGS])
AC_SUBST([MKC_SAN_LDFLAGS])

MKC_SIZE_CFLAGS=""
MKC_SIZE_LDFLAGS=""
case "$host_os" in
  linux*|mingw*)
    AX_APPEND_COMPILE_FLAGS([-ffunction-sections -fdata-sections], [MKC_SIZE_CFLAGS])
    AX_APPEND_LINK_FLAGS([-Wl,--gc-sections], [MKC_SIZE_LDFLAGS])
    ;;
esac
AC_SUBST([MKC_SIZE_CFLAGS])
AC_SUBST([MKC_SIZE_LDFLAGS])

AC_CHECK_PROGS([PYTHON], [python3 python], [])
AC_ARG_VAR([MKC_TEST_TIMEOUT_SECONDS],
           [seconds the test suite may run (default 3600)])
test -n "$MKC_TEST_TIMEOUT_SECONDS" || MKC_TEST_TIMEOUT_SECONDS=3600
AC_CHECK_PROGS([MKC_TIMEOUT_PROG], [timeout gtimeout], [])
if test -z "$MKC_TIMEOUT_PROG"; then
  MKC_TEST_TIMEOUT_SECONDS=
fi
AC_SUBST([MKC_TIMEOUT_PROG])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

AC_MSG_NOTICE([

  mkc $PACKAGE_VERSION configuration
    host          : $host
    AVX2 scanner  : ${MKC_AVX2_CFLAGS:-no}
    AVX2 lookups  : $mkc_lookups
    libraries     : $mkc_deps_summary
    sanitizers    : $enable_sanitizers
    Python tests  : ${PYTHON:-no}
    CFLAGS        : $CFLAGS
])
