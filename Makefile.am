##  Copyright (C) 2026 Kamila Szewczyk
##
##  This program is free software; you can redistribute it and/or modify
##  it under the terms of the GNU General Public License as published by
##  the Free Software Foundation, version 3.
##
##  This program is distributed in the hope that it will be useful,
##  but WITHOUT ANY WARRANTY; without even the implied warranty of
##  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
##  GNU General Public License for more details.
##
##  You should have received a copy of the GNU General Public License
##  along with this program. If not, see <http://www.gnu.org/licenses/>.

ACLOCAL_AMFLAGS = -I m4

AM_CFLAGS = -std=c99 $(MKC_WARN_CFLAGS) $(MKC_HARDEN_CFLAGS) $(MKC_OPT_CFLAGS) \
            $(MKC_SAN_CFLAGS) $(MKC_SIZE_CFLAGS) $(MKC_THREAD_CFLAGS)
AM_LDFLAGS = $(MKC_SAN_LDFLAGS) $(MKC_WIN_LDFLAGS) $(MKC_SIZE_LDFLAGS) \
             $(MKC_THREAD_LDFLAGS)
AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src $(MKC_DEPS_CPPFLAGS) \
              $(MKC_KERNEL_CPPFLAGS)

bin_PROGRAMS = mkc
include_HEADERS = src/mkc.h

MKC_PARTS = src/assemble.c src/input.c src/seed.c \
  src/parse.c src/direct.c src/split.c src/count.c

noinst_HEADERS = \
  src/common.h src/cpu.h src/os.h src/sort.h \
  src/seed.h src/simd.h src/spool.h

noinst_LIBRARIES = libmkc.a
libmkc_a_SOURCES = src/common.c src/cpu.c
MKC_LIBS = libmkc.a

if MKC_AVX2
noinst_LIBRARIES += libmkc_avx2.a
libmkc_avx2_a_SOURCES = src/simd.c
libmkc_avx2_a_CFLAGS  = $(AM_CFLAGS) $(MKC_AVX2_CFLAGS)
MKC_LIBS += libmkc_avx2.a
endif

mkc_SOURCES = src/main.c
EXTRA_mkc_DEPENDENCIES = $(MKC_DEPS_STAMP)
mkc_LDADD = $(MKC_LIBS) $(MKC_DEPS_LIBS)

if MKC_BUNDLED
BUILT_SOURCES = $(MKC_DEPS_STAMP)
$(MKC_DEPS_STAMP): $(top_srcdir)/deps/build.sh
	$(AM_V_GEN)CC="$(CC)" AR="$(AR)" MKC_DEPS_OUT="$(abs_top_builddir)/deps/out" \
	  MKC_DEPS_WORK="$(abs_top_builddir)/deps/work" \
	  $(SHELL) $(top_srcdir)/deps/build.sh $(MKC_DEPS_TARGET)

distclean-local:
	-rm -rf deps/out deps/work
endif

check_PROGRAMS = tests/units tests/iterate tests/adversarial tests/pack \
                 tests/mkc-small tests/mkc-packed tests/mkc-unpacked \
                 tests/mkc-no-reseed tests/mkc-packed-no-reseed
EXTRA_PROGRAMS = tests/mkc-dense tests/mkc-hash tests/mkc-hash-packed \
                 tests/kernels \
                 examples/query examples/batch

tests_units_SOURCES = tests/units.c
tests_units_LDADD = $(MKC_LIBS)
tests_iterate_SOURCES = tests/iterate.c
tests_adversarial_SOURCES = tests/adversarial.c
tests_kernels_SOURCES = tests/kernels.c
tests_pack_SOURCES = tests/pack.c
tests_pack_LDADD = $(MKC_PACK_LIBS)
EXTRA_tests_pack_DEPENDENCIES = $(MKC_DEPS_STAMP)

tests_mkc_small_SOURCES = src/main.c
tests_mkc_small_CPPFLAGS = $(AM_CPPFLAGS) -DMKC_READ_BLOCK=4096
tests_mkc_small_LDADD = $(mkc_LDADD)
EXTRA_tests_mkc_small_DEPENDENCIES = $(MKC_DEPS_STAMP)
tests_mkc_packed_SOURCES = src/main.c
tests_mkc_packed_CPPFLAGS = $(AM_CPPFLAGS) -DMKC_TEST_PACKED=2
tests_mkc_packed_LDADD = $(mkc_LDADD)
EXTRA_tests_mkc_packed_DEPENDENCIES = $(MKC_DEPS_STAMP)
tests_mkc_unpacked_SOURCES = src/main.c
tests_mkc_unpacked_CPPFLAGS = $(AM_CPPFLAGS) -DMKC_TEST_PACKED=0
tests_mkc_unpacked_LDADD = $(mkc_LDADD)
EXTRA_tests_mkc_unpacked_DEPENDENCIES = $(MKC_DEPS_STAMP)
tests_mkc_no_reseed_SOURCES = src/main.c
tests_mkc_no_reseed_CPPFLAGS = $(AM_CPPFLAGS) -DMKC_NO_RESEED=1
tests_mkc_no_reseed_LDADD = $(mkc_LDADD)
EXTRA_tests_mkc_no_reseed_DEPENDENCIES = $(MKC_DEPS_STAMP)
tests_mkc_packed_no_reseed_SOURCES = src/main.c
tests_mkc_packed_no_reseed_CPPFLAGS = $(AM_CPPFLAGS) -DMKC_TEST_PACKED=2 \
                                      -DMKC_NO_RESEED=1
tests_mkc_packed_no_reseed_LDADD = $(mkc_LDADD)
EXTRA_tests_mkc_packed_no_reseed_DEPENDENCIES = $(MKC_DEPS_STAMP)
tests_mkc_dense_SOURCES = src/main.c
tests_mkc_dense_CPPFLAGS = $(AM_CPPFLAGS) -DMKC_TEST_LAYOUT=1
tests_mkc_dense_LDADD = $(mkc_LDADD)
EXTRA_tests_mkc_dense_DEPENDENCIES = $(MKC_DEPS_STAMP)
tests_mkc_hash_SOURCES = src/main.c
tests_mkc_hash_CPPFLAGS = $(AM_CPPFLAGS) -DMKC_TEST_LAYOUT=2 \
                          -DMKC_TEST_DIRECT_SLOTS=1
tests_mkc_hash_LDADD = $(mkc_LDADD)
EXTRA_tests_mkc_hash_DEPENDENCIES = $(MKC_DEPS_STAMP)
tests_mkc_hash_packed_SOURCES = src/main.c
tests_mkc_hash_packed_CPPFLAGS = $(AM_CPPFLAGS) -DMKC_TEST_LAYOUT=2 \
                                 -DMKC_TEST_DIRECT_SLOTS=1 -DMKC_TEST_PACKED=2
tests_mkc_hash_packed_LDADD = $(mkc_LDADD)
EXTRA_tests_mkc_hash_packed_DEPENDENCIES = $(MKC_DEPS_STAMP)

examples_query_SOURCES = examples/query.c
examples_batch_SOURCES = examples/batch.c

TESTS = tests/units tests/exact.sh tests/stash.sh tests/fastq.sh \
        tests/fasta.sh tests/inputs.sh

test-programs: $(check_PROGRAMS)

LOG_COMPILER = $(MKC_TIMEOUT_PROG) $(MKC_TEST_TIMEOUT_SECONDS)

AM_TESTS_ENVIRONMENT = \
  srcdir='$(srcdir)';                                  export srcdir; \
  PYTHON='$(PYTHON)';                                  export PYTHON; \
  MKC='$(abs_top_builddir)/mkc$(EXEEXT)';              export MKC; \
  MKC_SMALL='$(abs_top_builddir)/tests/mkc-small$(EXEEXT)'; export MKC_SMALL; \
  MKC_PACKED='$(abs_top_builddir)/tests/mkc-packed$(EXEEXT)'; \
    export MKC_PACKED; \
  MKC_UNPACKED='$(abs_top_builddir)/tests/mkc-unpacked$(EXEEXT)'; \
    export MKC_UNPACKED; \
  MKC_NO_RESEED='$(abs_top_builddir)/tests/mkc-no-reseed$(EXEEXT)'; \
    export MKC_NO_RESEED; \
  MKC_PACKED_NO_RESEED='$(abs_top_builddir)/tests/mkc-packed-no-reseed$(EXEEXT)'; \
    export MKC_PACKED_NO_RESEED; \
  MKC_HASH_PACKED='$(abs_top_builddir)/tests/mkc-hash-packed$(EXEEXT)'; \
    export MKC_HASH_PACKED; \
  MKC_DENSE='$(abs_top_builddir)/tests/mkc-dense$(EXEEXT)'; export MKC_DENSE; \
  MKC_HASH='$(abs_top_builddir)/tests/mkc-hash$(EXEEXT)'; export MKC_HASH; \
  MKC_PACK='$(abs_top_builddir)/tests/pack$(EXEEXT)';  export MKC_PACK; \
  MKC_ITERATE='$(abs_top_builddir)/tests/iterate$(EXEEXT)'; \
                                                       export MKC_ITERATE; \
  MKC_ADVERSARIAL='$(abs_top_builddir)/tests/adversarial$(EXEEXT)'; \
                                                       export MKC_ADVERSARIAL; \
  MKC_TEST_TMP="$${MKC_TEST_TMP-}";                    export MKC_TEST_TMP;

EXTRA_DIST = \
  bootstrap build-aux/version.sh README COPYING $(MKC_PARTS) \
  deps/build.sh tests/exact.sh tests/stash.sh tests/fastq.sh tests/fasta.sh \
  tests/inputs.sh tests/layouts.sh tests/exact.py tests/stash.py \
  tests/fastq.py tests/fasta.py tests/inputs.py tests/python.py \
  python/mkc/__init__.py python/mkc/_mkc.c setup.py pyproject.toml MANIFEST.in

CLEANFILES = $(EXTRA_PROGRAMS)

.PHONY: check-layouts check-simd check-full python examples print-distfiles \
        test-programs

check-layouts: tests/mkc-dense$(EXEEXT) tests/mkc-hash$(EXEEXT) \
               tests/mkc-hash-packed$(EXEEXT) \
               tests/iterate$(EXEEXT)
	@$(AM_TESTS_ENVIRONMENT) $(SHELL) $(srcdir)/tests/layouts.sh

check-simd: mkc$(EXEEXT)
	@tiers=scalar; \
	if MKC_SIMD=avx2 ./mkc$(EXEEXT) --version >/dev/null 2>&1; then \
	  tiers="$$tiers avx2"; \
	fi; \
	for t in $$tiers; do \
	  echo "== scanner kernel $$t"; \
	  MKC_SIMD=$$t $(MAKE) check || exit 1; \
	done

check-full: check check-layouts check-simd

python: mkc$(EXEEXT)
	cd $(srcdir) && MKC_PREBUILT='$(abs_top_builddir)/mkc$(EXEEXT)' \
	  $(PYTHON) setup.py build_ext --inplace
	cd $(srcdir) && PYTHONPATH=python $(PYTHON) tests/python.py

examples: examples/query$(EXEEXT) examples/batch$(EXEEXT)

print-distfiles:
	@for f in $(DISTFILES) $(DIST_COMMON) $(am__DIST_COMMON) \
	          $(ACLOCAL_M4); do \
	  case $$f in \
	    $(srcdir)/*)     echo "$${f#$(srcdir)/}" ;; \
	    $(top_srcdir)/*) echo "$${f#$(top_srcdir)/}" ;; \
	    *)               echo "$$f" ;; \
	  esac; \
	done

dist-hook:
	@echo '$(VERSION)' > "$(distdir)/.tarball-version"
