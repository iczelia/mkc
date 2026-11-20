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

#ifndef MKC_OS_H
#define MKC_OS_H

#include "common.h"

#include <errno.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#include <fcntl.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/resource.h>
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define cpu_relax() _mm_pause()
#elif defined(__aarch64__)
#define cpu_relax() __asm__ __volatile__("yield")
#else
#define cpu_relax() ((void) 0)
#endif

#define ld_acq(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define st_rel(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define fetch_add(p, v) __atomic_fetch_add((p), (v), __ATOMIC_ACQ_REL)
#define cas(p, expected, desired) \
  __atomic_compare_exchange_n((p), (expected), (desired), 0, \
                              __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)

#ifdef _WIN32
typedef SRWLOCK mkc_mutex;
typedef CONDITION_VARIABLE mkc_cond;
typedef struct {
  HANDLE handle;
  void * (*fn)(void *);
  void * arg;
} * mkc_thread;

SI void mx_init(mkc_mutex * m) { InitializeSRWLock(m); }
SI void mx_free(mkc_mutex * m) { }
SI void mx_lock(mkc_mutex * m) { AcquireSRWLockExclusive(m); }
SI void mx_unlock(mkc_mutex * m) { ReleaseSRWLockExclusive(m); }
SI void cv_init(mkc_cond * c) {
  InitializeConditionVariable(c);
}
SI void cv_free(mkc_cond * c) { }
SI void cv_wait(mkc_cond * c, mkc_mutex * m) {
  SleepConditionVariableSRW(c, m, INFINITE, 0);
}
SI void cv_signal(mkc_cond * c) { WakeConditionVariable(c); }
SI void cv_bcast(mkc_cond * c) { WakeAllConditionVariable(c); }

SI DWORD WINAPI thread_entry(LPVOID p) {
  mkc_thread t = (mkc_thread) p;
  t->fn(t->arg);
  return 0;
}

SI int th_start(mkc_thread * t, void * (*fn)(void *),
                void * arg, u32 ord) {
  mkc_thread p = (mkc_thread) malloc(sizeof *p);
  WORD ng;
  if (!p) return ENOMEM;
  p->fn = fn;  p->arg = arg;
  p->handle = CreateThread(NULL, 0, thread_entry, p, CREATE_SUSPENDED,
                           NULL);
  if (!p->handle) { free(p);  return EAGAIN; }
  ng = GetActiveProcessorGroupCount();
  if (ng > 1) {
    GROUP_AFFINITY a;
    DWORD n;
    memset(&a, 0, sizeof a);
    a.Group = (WORD) (ord % ng);
    n = GetActiveProcessorCount(a.Group);
    a.Mask = n >= 64 ? ~(KAFFINITY) 0 : (((KAFFINITY) 1 << n) - 1);
    SetThreadGroupAffinity(p->handle, &a, NULL);
  }
  ResumeThread(p->handle);
  *t = p;
  return 0;
}

SI void th_join(mkc_thread t) {
  WaitForSingleObject(t->handle, INFINITE);
  CloseHandle(t->handle);  free(t);
}
#else
typedef pthread_mutex_t mkc_mutex;
typedef pthread_cond_t mkc_cond;
typedef pthread_t mkc_thread;

SI void mx_init(mkc_mutex * m) {
  pthread_mutex_init(m, NULL);
}
SI void mx_free(mkc_mutex * m) {
  pthread_mutex_destroy(m);
}
SI void mx_lock(mkc_mutex * m) { pthread_mutex_lock(m); }
SI void mx_unlock(mkc_mutex * m) { pthread_mutex_unlock(m); }
SI void cv_init(mkc_cond * c) { pthread_cond_init(c, NULL); }
SI void cv_free(mkc_cond * c) { pthread_cond_destroy(c); }
SI void cv_wait(mkc_cond * c, mkc_mutex * m) {
  pthread_cond_wait(c, m);
}
SI void cv_signal(mkc_cond * c) { pthread_cond_signal(c); }
SI void cv_bcast(mkc_cond * c) { pthread_cond_broadcast(c); }

SI int th_start(mkc_thread * t, void * (*fn)(void *),
                void * arg, u32 ord) {
  return pthread_create(t, NULL, fn, arg);
}

SI void th_join(mkc_thread t) { pthread_join(t, NULL); }
#endif

typedef void (*mkc_task)(void * ctx, u32 id, u32 m);

typedef struct {
  u32 size, m, sleepers, stop;
  u32 generation, remaining;
  mkc_task task;
  void * ctx;
  mkc_mutex lock, caller;
  mkc_cond wake, done;
  mkc_thread * threads;
} mkc_pool;

typedef struct {
  mkc_pool * pool;
  u32 id;
} pool_seat;

#ifndef MKC_SPIN
#define MKC_SPIN 512u
#endif

SI void * pool_worker(void * arg) {
  pool_seat * s = (pool_seat *) arg;
  mkc_pool * p = s->pool;
  const u32 id = s->id;
  u32 seen = 0;
  for (;;) {
    u32 spin, g = ld_acq(&p->generation);
    for (spin = 0; g == seen && spin < MKC_SPIN; spin++) {
      cpu_relax();  g = ld_acq(&p->generation);
    }
    if (g == seen) {
      mx_lock(&p->lock);
      p->sleepers++;
      while ((g = ld_acq(&p->generation)) == seen)
        cv_wait(&p->wake, &p->lock);
      p->sleepers--;
      mx_unlock(&p->lock);
    }
    seen = g;
    if (p->stop) break;
    if (id < p->m) p->task(p->ctx, id, p->m);
    if (fetch_add(&p->remaining, (u32) -1) == 1) {
      mx_lock(&p->lock);  cv_bcast(&p->done);  mx_unlock(&p->lock);
    }
  }
  free(s);
  return NULL;
}

SI mkc_pool * tp_new(u32 size) {
  mkc_pool * p = (mkc_pool *) calloc(1, sizeof *p);
  u32 i;
  if (!p) return NULL;
  p->size = size ? size : 1;
  mx_init(&p->lock);  mx_init(&p->caller);
  cv_init(&p->wake);  cv_init(&p->done);
  p->threads = (mkc_thread *) calloc(p->size, sizeof *p->threads);
  if (!p->threads) { free(p);  return NULL; }
  for (i = 1; i < p->size; i++) {
    pool_seat * s = (pool_seat *) malloc(sizeof *s);
    if (s) { s->pool = p;  s->id = i; }
    if (!s || th_start(&p->threads[i], pool_worker, s, i)) {
      free(s);  p->size = i;  break;
    }
  }
  return p;
}

SI void pool_publish(mkc_pool * p) {
  mx_lock(&p->lock);
  st_rel(&p->generation, p->generation + 1);
  if (p->sleepers) cv_bcast(&p->wake);
  mx_unlock(&p->lock);
}

SI void tp_run(mkc_pool * p, u32 m, mkc_task task,
               void * ctx) {
  u32 left, spin;
  if (m > p->size) m = p->size;
  if (m <= 1) { task(ctx, 0, 1);  return; }
  mx_lock(&p->caller);
  p->task = task;  p->ctx = ctx;  p->m = m;
  st_rel(&p->remaining, p->size - 1);
  pool_publish(p);
  task(ctx, 0, p->m);
  left = ld_acq(&p->remaining);
  for (spin = 0; left && spin < MKC_SPIN; spin++) {
    cpu_relax();  left = ld_acq(&p->remaining);
  }
  if (left) {
    mx_lock(&p->lock);
    while (ld_acq(&p->remaining)) cv_wait(&p->done, &p->lock);
    mx_unlock(&p->lock);
  }
  mx_unlock(&p->caller);
}

SI void tp_free(mkc_pool * p) {
  u32 i;
  if (!p) return;
  p->stop = 1;
  pool_publish(p);
  for (i = 1; i < p->size; i++) th_join(p->threads[i]);
  mx_free(&p->lock);  mx_free(&p->caller);
  cv_free(&p->wake);  cv_free(&p->done);
  free(p->threads);  free(p);
}

SI void * os_pages(sz n) {
#ifdef _WIN32
  return VirtualAlloc(NULL, n ? n : 1, MEM_RESERVE | MEM_COMMIT,
                      PAGE_READWRITE);
#else
  void * p = mmap(NULL, n ? n : 1, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? NULL : p;
#endif
}

SI void * xpages(sz n) {
  void * p = os_pages(n);
  if (!p) FATAL("out of memory allocating %llu bytes", (unsigned long long) n);
  return p;
}

SI void os_pages_free(void * p, sz n) {
  if (!p) return;
#ifdef _WIN32
  VirtualFree(p, 0, MEM_RELEASE);
#else
  munmap(p, n ? n : 1);
#endif
}

SI u64 os_mem(void) {
#ifdef _WIN32
  MEMORYSTATUSEX s;
  s.dwLength = sizeof s;
  return GlobalMemoryStatusEx(&s) ? (u64) s.ullTotalPhys : 0;
#else
  long n = sysconf(_SC_PHYS_PAGES), ps = sysconf(_SC_PAGESIZE);
  return n > 0 && ps > 0 ? (u64) n * (u64) ps : 0;
#endif
}

SI u32 os_cpus(void) {
#ifdef _WIN32
  DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  return n ? (u32) n : 1;
#else
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (u32) n : 1;
#endif
}

SI void os_sleep_ms(u32 ms) {
#ifdef _WIN32
  Sleep(ms);
#else
  struct timespec t = {(time_t) (ms / 1000), (long) (ms % 1000) * 1000000L};
  nanosleep(&t, NULL);
#endif
}

SI double os_now(void) {
#ifdef _WIN32
  LARGE_INTEGER t, f;
  QueryPerformanceCounter(&t);  QueryPerformanceFrequency(&f);
  return (double) t.QuadPart / (double) f.QuadPart;
#else
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double) t.tv_sec + t.tv_nsec * 1e-9;
#endif
}

#ifdef _WIN32
SI const char * os_error(void) {
  static MKC_TLS char msg[256];
  DWORD n, e = GetLastError();
  if (!e) return strerror(errno);
  n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                     NULL, e, 0, msg, sizeof msg, NULL);
  while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r'
               || msg[n - 1] == ' ' || msg[n - 1] == '.'))
    msg[--n] = 0;
  if (!n) snprintf(msg, sizeof msg, "error %lu", (unsigned long) e);
  return msg;
}
#else
SI const char * os_error(void) { return strerror(errno); }
#endif

typedef intptr_t mkc_file;
#define MKC_NO_FILE ((mkc_file) -1)
enum {
  MKC_READ = 1, MKC_WRITE = 2,
  MKC_CREATE = 4,
  MKC_TRUNCATE = 8,
  MKC_EXCLUSIVE = 16,
  MKC_DIRECT = 32,
  MKC_SEQUENTIAL = 64,
  MKC_TEMPORARY = 128,
  MKC_SCRATCH = 256
};

#ifdef _WIN32
SI wchar_t * os_wide(const char * s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t * w = n > 0 ? (wchar_t *) malloc((sz) n * sizeof (wchar_t)) : NULL;
  if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
    free(w);  w = NULL;
  }
  if (!w) SetLastError(ERROR_INVALID_NAME);
  return w;
}

SI char * os_narrow(const wchar_t * w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  char * s = n > 0 ? (char *) malloc((sz) n) : NULL;
  if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
    free(s);  s = NULL;
  }
  return s;
}
#endif

SI mkc_file os_open(const char * path, int flags) {
#ifdef _WIN32
  wchar_t * w = os_wide(path);
  DWORD acc = (flags & MKC_READ ? GENERIC_READ : 0)
              | (flags & MKC_WRITE ? GENERIC_WRITE : 0);
  DWORD how = !(flags & MKC_CREATE) ? OPEN_EXISTING
              : flags & MKC_EXCLUSIVE ? CREATE_NEW
              : flags & MKC_TRUNCATE ? CREATE_ALWAYS : OPEN_ALWAYS;
  DWORD attr = flags & MKC_TEMPORARY
               ? FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE
               : flags & MKC_SCRATCH ? FILE_ATTRIBUTE_TEMPORARY
               : FILE_ATTRIBUTE_NORMAL;
  HANDLE h;
  if (!w) return MKC_NO_FILE;
  if (flags & MKC_DIRECT) attr |= FILE_FLAG_NO_BUFFERING;
  if (flags & MKC_SEQUENTIAL) attr |= FILE_FLAG_SEQUENTIAL_SCAN;
  h = CreateFileW(w, acc,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  how, attr, NULL);
  free(w);
  return h == INVALID_HANDLE_VALUE ? MKC_NO_FILE : (mkc_file) h;
#else
  int fd, o = (flags & MKC_READ) && (flags & MKC_WRITE) ? O_RDWR
              : flags & MKC_WRITE ? O_WRONLY : O_RDONLY;
  if (flags & MKC_CREATE)
    o |= O_CREAT | (flags & MKC_TRUNCATE ? O_TRUNC : 0)
         | (flags & MKC_EXCLUSIVE ? O_EXCL : 0);
#ifdef O_CLOEXEC
  o |= O_CLOEXEC;
#endif
#ifdef O_DIRECT
  if (flags & MKC_DIRECT) o |= O_DIRECT;
#endif
  fd = open(path, o, 0600);
  if (fd < 0) return MKC_NO_FILE;
#ifdef POSIX_FADV_SEQUENTIAL
  if (flags & MKC_SEQUENTIAL)
    (void) posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  if (flags & MKC_TEMPORARY) unlink(path);
  return fd;
#endif
}

SI mkc_file os_stdin(void) {
#ifdef _WIN32
  HANDLE h;
  if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_INPUT_HANDLE),
                       GetCurrentProcess(), &h, 0, FALSE,
                       DUPLICATE_SAME_ACCESS))
    return MKC_NO_FILE;
  return (mkc_file) h;
#else
  int fd = dup(0);
  return fd < 0 ? MKC_NO_FILE : fd;
#endif
}

SI int os_close(mkc_file f) {
#ifdef _WIN32
  return CloseHandle((HANDLE) f) ? 0 : -1;
#else
  return close((int) f);
#endif
}

SI ptrdiff_t os_read(mkc_file f, void * buf, sz n) {
#ifdef _WIN32
  DWORD got = 0;
  if (n > (1u << 30)) n = 1u << 30;
  if (!ReadFile((HANDLE) f, buf, (DWORD) n, &got, NULL)) {
    DWORD e = GetLastError();
    if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) return 0;
    return -1;
  }
  return (ptrdiff_t) got;
#else
  for (;;) {
    ssize_t got = read((int) f, buf, n);
    if (got >= 0 || errno != EINTR) return got;
  }
#endif
}

SI ptrdiff_t os_pread(mkc_file f, void * buf, sz n, u64 off) {
#ifdef _WIN32
  OVERLAPPED o;
  DWORD got = 0;
  memset(&o, 0, sizeof o);
  o.Offset = (DWORD) off;  o.OffsetHigh = (DWORD) (off >> 32);
  if (n > (1u << 30)) n = 1u << 30;
  if (!ReadFile((HANDLE) f, buf, (DWORD) n, &got, &o))
    return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
  return (ptrdiff_t) got;
#else
  for (;;) {
    ssize_t got = pread((int) f, buf, n, (off_t) off);
    if (got >= 0 || errno != EINTR) return got;
  }
#endif
}

SI ptrdiff_t os_pwrite(mkc_file f, const void * buf, sz n,
                       u64 off) {
#ifdef _WIN32
  OVERLAPPED o;
  DWORD done = 0;
  memset(&o, 0, sizeof o);
  o.Offset = (DWORD) off;  o.OffsetHigh = (DWORD) (off >> 32);
  if (n > (1u << 30)) n = 1u << 30;
  if (!WriteFile((HANDLE) f, buf, (DWORD) n, &done, &o)) return -1;
  return (ptrdiff_t) done;
#else
  for (;;) {
    ssize_t done = pwrite((int) f, buf, n, (off_t) off);
    if (done >= 0 || errno != EINTR) return done;
  }
#endif
}

SI int os_pread_all(mkc_file f, void * buf, sz n, u64 off) {
  u8 * p = (u8 *) buf;
  while (n) {
    ptrdiff_t got = os_pread(f, p, n, off);
    if (got <= 0) { if (!got) errno = EIO;  return -1; }
    p += got;  n -= (sz) got;  off += (u64) got;
  }
  return 0;
}

SI int os_pwrite_all(mkc_file f, const void * buf, sz n,
                     u64 off) {
  const u8 * p = (const u8 *) buf;
  while (n) {
    ptrdiff_t done = os_pwrite(f, p, n, off);
    if (done <= 0) { if (!done) errno = EIO;  return -1; }
    p += done;  n -= (sz) done;  off += (u64) done;
  }
  return 0;
}

SI int os_write_all(mkc_file f, const void * buf, sz n) {
  const u8 * p = (const u8 *) buf;
  while (n) {
#ifdef _WIN32
    DWORD done = 0, take = n > (1u << 30) ? 1u << 30 : (DWORD) n;
    if (!WriteFile((HANDLE) f, p, take, &done, NULL) || !done) return -1;
#else
    ssize_t done = write((int) f, p, n);
    if (done < 0 && errno == EINTR) continue;
    if (done <= 0) { if (!done) errno = EIO;  return -1; }
#endif
    p += done;  n -= (sz) done;
  }
  return 0;
}

SI int os_pwrite2(mkc_file f, const void * a, sz an, const void * b,
                  sz bn, u64 off) {
#ifdef _WIN32
  if (os_pwrite_all(f, a, an, off)) return -1;
  return os_pwrite_all(f, b, bn, off + an);
#else
  struct iovec v[2] = {{(void *) a, an}, {(void *) b, bn}};
  u32 at = 0;
  sz done = 0, len = an + bn;
  while (done < len) {
    ssize_t n = pwritev((int) f, v + at, 2 - (int) at, (off_t) (off + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { if (!n) errno = EIO;  return -1; }
    done += (sz) n;
    while (at < 2 && (sz) n >= v[at].iov_len) {
      n -= (ssize_t) v[at].iov_len;  at++;
    }
    if (at < 2) {
      v[at].iov_base = (u8 *) v[at].iov_base + n;  v[at].iov_len -= (sz) n;
    }
  }
  return 0;
#endif
}

SI i64 os_file_size(mkc_file f) {
#ifdef _WIN32
  LARGE_INTEGER s;
  if (GetFileType((HANDLE) f) != FILE_TYPE_DISK
      || !GetFileSizeEx((HANDLE) f, &s))
    return -1;
  return (i64) s.QuadPart;
#else
  struct stat st;
  if (fstat((int) f, &st) || !S_ISREG(st.st_mode)) return -1;
  return (i64) st.st_size;
#endif
}

SI i64 os_path_size(const char * path) {
  mkc_file f = os_open(path, MKC_READ);
  i64 s;
  if (f == MKC_NO_FILE) return -1;
  s = os_file_size(f);
  os_close(f);
  return s;
}

SI int os_set_size(mkc_file f, u64 size) {
#ifdef _WIN32
  FILE_END_OF_FILE_INFO e;
  e.EndOfFile.QuadPart = (LONGLONG) size;
  return SetFileInformationByHandle((HANDLE) f, FileEndOfFileInfo, &e,
                                    sizeof e) ? 0 : -1;
#else
  return ftruncate((int) f, (off_t) size);
#endif
}

SI int os_reserve(mkc_file f, u64 size) {
#ifdef _WIN32
  FILE_ALLOCATION_INFO a;
  a.AllocationSize.QuadPart = (LONGLONG) size;
  return SetFileInformationByHandle((HANDLE) f, FileAllocationInfo, &a,
                                    sizeof a) ? 0 : -1;
#elif defined(__linux__)
  return fallocate((int) f, 0, 0, (off_t) size);
#else
  errno = EOPNOTSUPP;  return -1;
#endif
}

SI int os_sync(mkc_file f) {
#ifdef _WIN32
  return FlushFileBuffers((HANDLE) f) ? 0 : -1;
#else
  return fsync((int) f);
#endif
}

SI int os_set_direct(mkc_file * f, int on) {
#ifdef _WIN32
  HANDLE h = ReOpenFile((HANDLE) *f, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        on ? FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN
                           : FILE_FLAG_SEQUENTIAL_SCAN);
  if (h == INVALID_HANDLE_VALUE) return -1;
  CloseHandle((HANDLE) *f);  *f = (mkc_file) h;
  return 0;
#elif defined(O_DIRECT)
  int fl = fcntl((int) *f, F_GETFL);
  return fl < 0 ? -1
         : fcntl((int) *f, F_SETFL, on ? fl | O_DIRECT : fl & ~O_DIRECT);
#else
  return on ? -1 : 0;
#endif
}

SI void os_writeback(mkc_file f, u64 off, u64 n) {
#if defined(__linux__) && defined(SYNC_FILE_RANGE_WRITE)
  sync_file_range((int) f, (off_t) off, (off_t) n, SYNC_FILE_RANGE_WRITE);
#endif
}

SI void os_will_need(mkc_file f, u64 off, u64 n) {
#ifdef POSIX_FADV_WILLNEED
  (void) posix_fadvise((int) f, (off_t) off, (off_t) n, POSIX_FADV_WILLNEED);
#endif
}

SI int os_remove(const char * path) {
#ifdef _WIN32
  wchar_t * w = os_wide(path);
  struct { DWORD Flags; } info = {0x1 | 0x2};
  int ok = 0;
  HANDLE h;
  if (!w) return -1;
  h = CreateFileW(w, DELETE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
  if (h != INVALID_HANDLE_VALUE) {
    ok = SetFileInformationByHandle(h, (FILE_INFO_BY_HANDLE_CLASS) 21, &info,
                                    sizeof info);
    CloseHandle(h);
  }
  if (!ok) ok = DeleteFileW(w);
  free(w);
  return ok ? 0 : -1;
#else
  return unlink(path);
#endif
}

SI int os_make_dir(const char * path) {
#ifdef _WIN32
  wchar_t * w = os_wide(path);
  int ok = w && CreateDirectoryW(w, NULL);
  free(w);
  return ok ? 0 : -1;
#else
  return mkdir(path, 0700);
#endif
}

SI int os_replace(const char * from, const char * to) {
#ifdef _WIN32
  wchar_t * a = os_wide(from), * b = os_wide(to);
  int ok = a && b
           && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING
                                | MOVEFILE_WRITE_THROUGH);
  free(a);  free(b);
  return ok ? 0 : -1;
#else
  return rename(from, to);
#endif
}

SI void os_rmtree(const char * dir) {
  sz n = strlen(dir);
  char * path = (char *) malloc(n + 512);
#ifdef _WIN32
  wchar_t * w, * d;
  WIN32_FIND_DATAW e;
  HANDLE h;
#else
  DIR * d;
  struct dirent * e;
#endif
  if (!path) return;
#ifdef _WIN32
  memcpy(path, dir, n);  memcpy(path + n, "\\*", 3);
  w = os_wide(path);
  h = w ? FindFirstFileW(w, &e) : INVALID_HANDLE_VALUE;
  free(w);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      char * name;
      if (e.cFileName[0] == L'.') continue;
      name = os_narrow(e.cFileName);
      if (name && strlen(name) < 500) {
        snprintf(path, n + 512, "%s\\%s", dir, name);
        if (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) os_rmtree(path);
        else os_remove(path);
      }
      free(name);
    } while (FindNextFileW(h, &e));
    FindClose(h);
  }
  d = os_wide(dir);
  if (d) RemoveDirectoryW(d);
  free(d);
#else
  d = opendir(dir);
  if (d) {
    while ((e = readdir(d))) {
      struct stat st;
      if (e->d_name[0] == '.' || strlen(e->d_name) >= 500) continue;
      snprintf(path, n + 512, "%s/%s", dir, e->d_name);
      if (!lstat(path, &st) && S_ISDIR(st.st_mode)) os_rmtree(path);
      else unlink(path);
    }
    closedir(d);
  }
  rmdir(dir);
#endif
  free(path);
}

SI char * os_mkunique(const char * prefix, int dir,
                      int flags, mkc_file * f) {
  static unsigned long long ctr;
  sz n = strlen(prefix);
  char * path = (char *) malloc(n + 24);
  u64 seed;
  u32 i, t;
  if (!path) return NULL;
  seed = (u64) (os_now() * 1e9) ^ ((u64) (uintptr_t) path << 16);
#ifdef _WIN32
  seed ^= (u64) GetCurrentProcessId() << 40;
#else
  seed ^= (u64) getpid() << 40;
#endif
  for (t = 0; t < 1000; t++) {
    u64 x = seed + 0x9E3779B97F4A7C15ull * (++ctr + t);
    x ^= x >> 31;  x *= 0xBF58476D1CE4E5B9ull;  x ^= x >> 29;
    memcpy(path, prefix, n);
    Fi(12, path[n + i] = "abcdefghijklmnopqrstuvwxyz0123456789"[x % 36];
           x /= 36);
    path[n + 12] = 0;
    if (dir) {
      if (!os_make_dir(path)) return path;
    } else {
      *f = os_open(path, MKC_READ | MKC_WRITE | MKC_CREATE | MKC_EXCLUSIVE
                         | flags);
      if (*f != MKC_NO_FILE) return path;
    }
#ifdef _WIN32
    { DWORD e = GetLastError();
      if (e != ERROR_FILE_EXISTS && e != ERROR_ALREADY_EXISTS) break; }
#else
    if (errno != EEXIST) break;
#endif
  }
  free(path);
  return NULL;
}

SI char * os_temp_dir(void) {
#ifdef _WIN32
  wchar_t w[MAX_PATH + 2];
  DWORD n = GetTempPathW(MAX_PATH + 1, w);
  const char * e;
  char * s;
  if (n && n <= MAX_PATH) {
    if (n > 1 && (w[n - 1] == L'\\' || w[n - 1] == L'/')) w[n - 1] = 0;
    s = os_narrow(w);
    if (s) return s;
  }
  e = getenv("TEMP");
  s = (char *) malloc(strlen(e ? e : ".") + 1);
  if (s) strcpy(s, e ? e : ".");
  return s;
#else
  const char * e = getenv("TMPDIR");
  char * s;
  if (!e || !*e) e = "/tmp";
  s = (char *) malloc(strlen(e) + 1);
  if (s) strcpy(s, e);
  return s;
#endif
}

SI int os_fdlimit(u64 n) {
#ifdef _WIN32
  return 0;
#else
  struct rlimit r;
  if (getrlimit(RLIMIT_NOFILE, &r)) return -1;
  if (r.rlim_cur >= (rlim_t) n) return 0;
  if (r.rlim_max < (rlim_t) n) { errno = EMFILE;  return -1; }
  r.rlim_cur = (rlim_t) n;
  return setrlimit(RLIMIT_NOFILE, &r);
#endif
}

SI u64 os_map_align(void) {
#ifdef _WIN32
  SYSTEM_INFO s;
  GetSystemInfo(&s);
  return s.dwAllocationGranularity ? s.dwAllocationGranularity : 65536;
#else
  return 4096;
#endif
}

SI void * os_map(mkc_file f, u64 off, sz n, int wr) {
#ifdef _WIN32
  u64 end = off + n;
  HANDLE m = CreateFileMappingW((HANDLE) f, NULL,
                                wr ? PAGE_READWRITE : PAGE_READONLY,
                                (DWORD) (end >> 32), (DWORD) end, NULL);
  void * p;
  if (!m) return NULL;
  p = MapViewOfFile(m, wr ? FILE_MAP_WRITE : FILE_MAP_READ,
                    (DWORD) (off >> 32), (DWORD) off, n);
  CloseHandle(m);
  return p;
#else
  void * p = mmap(NULL, n, wr ? PROT_READ | PROT_WRITE : PROT_READ,
                  MAP_SHARED, (int) f, (off_t) off);
  return p == MAP_FAILED ? NULL : p;
#endif
}

SI void * os_map_all(mkc_file f, sz n) {
#if defined(__linux__)
  void * p = mmap(NULL, n, PROT_READ, MAP_PRIVATE | MAP_POPULATE, (int) f, 0);
  return p == MAP_FAILED ? NULL : p;
#else
  return NULL;
#endif
}

SI int os_unmap(void * p, sz n) {
#ifdef _WIN32
  return UnmapViewOfFile(p) ? 0 : -1;
#else
  return munmap(p, n);
#endif
}

#ifdef _WIN32
#define OS_CAN_RUN_AGAIN 0
SI int os_run_again(char ** argv) { return 127; }
#else
#define OS_CAN_RUN_AGAIN 1
SI int os_run_again(char ** argv) {
#ifdef __linux__
  execv("/proc/self/exe", argv);
#endif
  execvp(argv[0], argv);
  return 127;
}
#endif

SI void os_utf8_args(int * argc, char *** argv) {
#ifdef _WIN32
  int i, n;
  wchar_t ** w = CommandLineToArgvW(GetCommandLineW(), &n);
  char ** a;
  if (!w) return;
  a = (char **) calloc((sz) n + 1, sizeof (char *));
  if (!a) return;
  Fi(n, if (!(a[i] = os_narrow(w[i]))) return);
  LocalFree(w);
  *argc = n;  *argv = a;
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stdin), _O_BINARY);
#endif
}

#endif
