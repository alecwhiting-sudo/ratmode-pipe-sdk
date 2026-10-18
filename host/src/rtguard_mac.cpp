// macOS interposers for V5 (allocation) and V6 (IO / locks / sleeps / dispatch).
//
// Built as libratpipe_rtguard.dylib. ratpipe-host re-executes itself once with
// DYLD_INSERT_LIBRARIES pointing at this library, so dyld applies the
// __DATA,__interpose tuples below to every image — including the pipe that is
// dlopen'ed later. Inside this library the original functions are called
// directly (dyld never interposes the interposing image itself).
//
// Counting is gated on a per-thread flag that the host sets only around the
// pipe's `render` call (see rtguard_core.h), so host activity is invisible.
//
// Limits: inlined syscalls, custom allocators using mmap/vm_allocate
// directly, lock-free spinning on atomics and os_unfair_lock-free
// primitives are not seen; Hardened-runtime / SIP-protected hosts ignore
// DYLD_INSERT_LIBRARIES (the SDK's own unsigned ratpipe-host is fine).
// NOT exercised on Linux CI — first verified on a real Mac.
#include "rtguard_core.h"

#if defined(__APPLE__)

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <os/lock.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

using rtcore::note;

#define DYLD_INTERPOSE(_replacement, _replacee)                                              \
    __attribute__((used)) static struct { const void* replacement; const void* replacee; }   \
    _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) =                  \
        { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };

extern "C" {

static void* rp_malloc(size_t n)                    { note(rtcore::kAlloc, "malloc");  return malloc(n); }
static void* rp_calloc(size_t a, size_t b)          { note(rtcore::kAlloc, "calloc");  return calloc(a, b); }
static void* rp_realloc(void* p, size_t n)          { note(rtcore::kAlloc, "realloc"); return realloc(p, n); }
static void* rp_valloc(size_t n)                    { note(rtcore::kAlloc, "valloc");  return valloc(n); }
static void  rp_free(void* p)                       { if (p != nullptr) note(rtcore::kFree, "free"); free(p); }
static int   rp_posix_memalign(void** o, size_t a, size_t n) { note(rtcore::kAlloc, "posix_memalign"); return posix_memalign(o, a, n); }
DYLD_INTERPOSE(rp_malloc, malloc)
DYLD_INTERPOSE(rp_calloc, calloc)
DYLD_INTERPOSE(rp_realloc, realloc)
DYLD_INTERPOSE(rp_valloc, valloc)
DYLD_INTERPOSE(rp_free, free)
DYLD_INTERPOSE(rp_posix_memalign, posix_memalign)

static int rp_open(const char* p, int flags, ...)
{
    note(rtcore::kIo, "open");
    mode_t m = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); m = (mode_t) va_arg(ap, int); va_end(ap); }
    return open(p, flags, m);
}
static FILE*   rp_fopen(const char* p, const char* m)           { note(rtcore::kIo, "fopen");  return fopen(p, m); }
static ssize_t rp_read(int fd, void* b, size_t n)               { note(rtcore::kIo, "read");   return read(fd, b, n); }
static ssize_t rp_write(int fd, const void* b, size_t n)        { note(rtcore::kIo, "write");  return write(fd, b, n); }
static size_t  rp_fwrite(const void* b, size_t s, size_t n, FILE* f) { note(rtcore::kIo, "fwrite"); return fwrite(b, s, n, f); }
static int     rp_fputs(const char* s, FILE* f)                 { note(rtcore::kIo, "fputs");  return fputs(s, f); }
static int     rp_puts(const char* s)                           { note(rtcore::kIo, "puts");   return puts(s); }
static int     rp_vfprintf(FILE* f, const char* fmt, va_list ap){ note(rtcore::kIo, "fprintf"); return vfprintf(f, fmt, ap); }
static int     rp_vprintf(const char* fmt, va_list ap)          { note(rtcore::kIo, "printf"); return vprintf(fmt, ap); }
DYLD_INTERPOSE(rp_open, open)
DYLD_INTERPOSE(rp_fopen, fopen)
DYLD_INTERPOSE(rp_read, read)
DYLD_INTERPOSE(rp_write, write)
DYLD_INTERPOSE(rp_fwrite, fwrite)
DYLD_INTERPOSE(rp_fputs, fputs)
DYLD_INTERPOSE(rp_puts, puts)
DYLD_INTERPOSE(rp_vfprintf, vfprintf)
DYLD_INTERPOSE(rp_vprintf, vprintf)

static int rp_mutex_lock(pthread_mutex_t* m)                    { note(rtcore::kLock, "pthread_mutex_lock"); return pthread_mutex_lock(m); }
static int rp_rwlock_rdlock(pthread_rwlock_t* l)                { note(rtcore::kLock, "pthread_rwlock_rdlock"); return pthread_rwlock_rdlock(l); }
static int rp_rwlock_wrlock(pthread_rwlock_t* l)                { note(rtcore::kLock, "pthread_rwlock_wrlock"); return pthread_rwlock_wrlock(l); }
static int rp_cond_wait(pthread_cond_t* c, pthread_mutex_t* m)  { note(rtcore::kLock, "pthread_cond_wait"); return pthread_cond_wait(c, m); }
static int rp_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* t) { note(rtcore::kLock, "pthread_cond_timedwait"); return pthread_cond_timedwait(c, m, t); }
static void rp_unfair_lock(os_unfair_lock_t l)                  { note(rtcore::kLock, "os_unfair_lock_lock"); os_unfair_lock_lock(l); }
DYLD_INTERPOSE(rp_mutex_lock, pthread_mutex_lock)
DYLD_INTERPOSE(rp_rwlock_rdlock, pthread_rwlock_rdlock)
DYLD_INTERPOSE(rp_rwlock_wrlock, pthread_rwlock_wrlock)
DYLD_INTERPOSE(rp_cond_wait, pthread_cond_wait)
DYLD_INTERPOSE(rp_cond_timedwait, pthread_cond_timedwait)
DYLD_INTERPOSE(rp_unfair_lock, os_unfair_lock_lock)

static int      rp_usleep(useconds_t u)                         { note(rtcore::kSleep, "usleep");    return usleep(u); }
static int      rp_nanosleep(const struct timespec* a, struct timespec* b) { note(rtcore::kSleep, "nanosleep"); return nanosleep(a, b); }
static unsigned rp_sleep(unsigned s)                            { note(rtcore::kSleep, "sleep");     return sleep(s); }
static int      rp_sched_yield(void)                            { note(rtcore::kSleep, "sched_yield"); return sched_yield(); }
DYLD_INTERPOSE(rp_usleep, usleep)
DYLD_INTERPOSE(rp_nanosleep, nanosleep)
DYLD_INTERPOSE(rp_sleep, sleep)
DYLD_INTERPOSE(rp_sched_yield, sched_yield)

static void rp_dispatch_async(dispatch_queue_t q, dispatch_block_t b)  { note(rtcore::kLock, "dispatch_async"); dispatch_async(q, b); }
static void rp_dispatch_sync(dispatch_queue_t q, dispatch_block_t b)   { note(rtcore::kLock, "dispatch_sync");  dispatch_sync(q, b); }
static intptr_t rp_sem_wait(dispatch_semaphore_t s, dispatch_time_t t) { note(rtcore::kLock, "dispatch_semaphore_wait"); return dispatch_semaphore_wait(s, t); }
static intptr_t rp_group_wait(dispatch_group_t g, dispatch_time_t t)   { note(rtcore::kLock, "dispatch_group_wait");     return dispatch_group_wait(g, t); }
DYLD_INTERPOSE(rp_dispatch_async, dispatch_async)
DYLD_INTERPOSE(rp_dispatch_sync, dispatch_sync)
DYLD_INTERPOSE(rp_sem_wait, dispatch_semaphore_wait)
DYLD_INTERPOSE(rp_group_wait, dispatch_group_wait)

} // extern "C"

#endif // __APPLE__
