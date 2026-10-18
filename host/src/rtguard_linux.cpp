// Linux (glibc) interposers for V5 (allocation) and V6 (IO / locks / sleeps).
//
// These symbols are defined in the HOST EXECUTABLE (linked with -rdynamic), so
// ELF symbol interposition makes every dlopen'ed pipe — and the C / C++
// runtime it calls — bind to them. Each wrapper counts the call ONLY while
// the calling thread is inside a pipe `render` (rtcore::inRender()) and then
// forwards to the real implementation, so the host itself is unaffected.
//
// Limits (documented in docs/): inline syscalls, custom allocators that mmap
// directly, spin-wait loops on atomics and futex-based locks that bypass
// pthread are not seen. A pipe that statically links its own malloc is not
// seen either. glibc only (uses __libc_malloc & friends); other libcs build
// without the guard and V5/V6 report SKIP.
#include "rtguard_core.h"

#if defined(__linux__) && defined(__GLIBC__) \
    && ! defined(__SANITIZE_ADDRESS__) && ! defined(__SANITIZE_THREAD__) \
    && ! defined(RATPIPE_NO_RTGUARD)

#include <cstddef>
#include <cstdarg>
#include <dlfcn.h>
#include <new>

#define RP_RT_GUARD_ACTIVE 1

using rtcore::note;

extern "C" {
void* __libc_malloc(size_t);
void  __libc_free(void*);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
void* __libc_memalign(size_t, size_t);
}

static inline void* nextSym(const char* n) { return dlsym(RTLD_NEXT, n); }

// ── Allocation ──────────────────────────────────────────────────────────────
extern "C" {

void* malloc(size_t n)                 { note(rtcore::kAlloc, "malloc");  return __libc_malloc(n); }
void* calloc(size_t a, size_t b)       { note(rtcore::kAlloc, "calloc");  return __libc_calloc(a, b); }
void* realloc(void* p, size_t n)       { note(rtcore::kAlloc, "realloc"); return __libc_realloc(p, n); }
void  free(void* p)                    { if (p != nullptr) note(rtcore::kFree, "free"); __libc_free(p); }
void* memalign(size_t a, size_t n)     { note(rtcore::kAlloc, "memalign");      return __libc_memalign(a, n); }
void* aligned_alloc(size_t a, size_t n){ note(rtcore::kAlloc, "aligned_alloc"); return __libc_memalign(a, n); }
void* valloc(size_t n)                 { note(rtcore::kAlloc, "valloc");        return __libc_memalign(4096, n); }
int posix_memalign(void** out, size_t a, size_t n)
{
    note(rtcore::kAlloc, "posix_memalign");
    if (a < sizeof(void*) || (a & (a - 1)) != 0) return 22;   // EINVAL
    void* p = __libc_memalign(a, n);
    if (p == nullptr) return 12;                              // ENOMEM
    *out = p;
    return 0;
}

} // extern "C"

void* operator new(size_t n)                          { note(rtcore::kAlloc, "operator new");   void* p = __libc_malloc(n ? n : 1); if (! p) throw std::bad_alloc(); return p; }
void* operator new[](size_t n)                        { note(rtcore::kAlloc, "operator new[]"); void* p = __libc_malloc(n ? n : 1); if (! p) throw std::bad_alloc(); return p; }
void* operator new(size_t n, const std::nothrow_t&) noexcept   { note(rtcore::kAlloc, "operator new");   return __libc_malloc(n ? n : 1); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { note(rtcore::kAlloc, "operator new[]"); return __libc_malloc(n ? n : 1); }
void* operator new(size_t n, std::align_val_t a)      { note(rtcore::kAlloc, "operator new"); void* p = __libc_memalign((size_t) a, n ? n : 1); if (! p) throw std::bad_alloc(); return p; }
void* operator new[](size_t n, std::align_val_t a)    { note(rtcore::kAlloc, "operator new[]"); void* p = __libc_memalign((size_t) a, n ? n : 1); if (! p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept                       { if (p) note(rtcore::kFree, "operator delete"); __libc_free(p); }
void operator delete[](void* p) noexcept                     { if (p) note(rtcore::kFree, "operator delete[]"); __libc_free(p); }
void operator delete(void* p, size_t) noexcept               { if (p) note(rtcore::kFree, "operator delete"); __libc_free(p); }
void operator delete[](void* p, size_t) noexcept             { if (p) note(rtcore::kFree, "operator delete[]"); __libc_free(p); }
void operator delete(void* p, std::align_val_t) noexcept     { if (p) note(rtcore::kFree, "operator delete"); __libc_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept   { if (p) note(rtcore::kFree, "operator delete[]"); __libc_free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept   { if (p) note(rtcore::kFree, "operator delete"); __libc_free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { if (p) note(rtcore::kFree, "operator delete[]"); __libc_free(p); }

// ── IO, locks, sleeps ───────────────────────────────────────────────────────
// Parameter types are spelled with void*/long so no libc header (and its
// exception specifications) has to be included; the C symbol is what matters.
static inline void* rp_resolve1(void** slot, const char* name)
{
    if (*slot == nullptr) *slot = nextSym(name);
    return *slot;
}
#define RP_FWD(var, name)  ((decltype(var)) rp_resolve1((void**) &(var), name))

extern "C" {

static int  (*r_open)(const char*, int, unsigned)           = nullptr;
static int  (*r_open64)(const char*, int, unsigned)         = nullptr;
static int  (*r_openat)(int, const char*, int, unsigned)    = nullptr;
static int  (*r_creat)(const char*, unsigned)               = nullptr;
static void*(*r_fopen)(const char*, const char*)            = nullptr;
static void*(*r_fopen64)(const char*, const char*)          = nullptr;
static long (*r_read)(int, void*, size_t)                   = nullptr;
static long (*r_write)(int, const void*, size_t)            = nullptr;
static size_t(*r_fwrite)(const void*, size_t, size_t, void*)= nullptr;
static int  (*r_fputs)(const char*, void*)                  = nullptr;
static int  (*r_puts)(const char*)                          = nullptr;
static int  (*r_vfprintf)(void*, const char*, va_list)      = nullptr;
static int  (*r_vprintf)(const char*, va_list)              = nullptr;
static long (*r_syscall)(long, long, long, long, long, long, long) = nullptr;
static int  (*r_mutex_lock)(void*)                          = nullptr;
static int  (*r_mutex_timedlock)(void*, const void*)        = nullptr;
static int  (*r_rdlock)(void*)                              = nullptr;
static int  (*r_wrlock)(void*)                              = nullptr;
static int  (*r_cond_wait)(void*, void*)                    = nullptr;
static int  (*r_cond_timedwait)(void*, void*, const void*)  = nullptr;
static int  (*r_spin_lock)(void*)                           = nullptr;
static int  (*r_sem_wait)(void*)                            = nullptr;
static int  (*r_usleep)(unsigned)                           = nullptr;
static int  (*r_nanosleep)(const void*, void*)              = nullptr;
static int  (*r_clock_nanosleep)(int, int, const void*, void*) = nullptr;
static unsigned (*r_sleep)(unsigned)                        = nullptr;
static int  (*r_sched_yield)()                              = nullptr;

// Resolve everything up front so the render path never calls dlsym.
__attribute__((constructor(101))) static void rp_resolve_real()
{
    (void) (RP_FWD(r_open, "open")); RP_FWD(r_open64, "open64"); RP_FWD(r_openat, "openat"); RP_FWD(r_creat, "creat");
    RP_FWD(r_fopen, "fopen"); RP_FWD(r_fopen64, "fopen64");
    RP_FWD(r_read, "read"); RP_FWD(r_write, "write"); RP_FWD(r_fwrite, "fwrite"); RP_FWD(r_fputs, "fputs");
    RP_FWD(r_puts, "puts"); RP_FWD(r_vfprintf, "vfprintf"); RP_FWD(r_vprintf, "vprintf"); RP_FWD(r_syscall, "syscall");
    RP_FWD(r_mutex_lock, "pthread_mutex_lock"); RP_FWD(r_mutex_timedlock, "pthread_mutex_timedlock");
    RP_FWD(r_rdlock, "pthread_rwlock_rdlock"); RP_FWD(r_wrlock, "pthread_rwlock_wrlock");
    RP_FWD(r_cond_wait, "pthread_cond_wait"); RP_FWD(r_cond_timedwait, "pthread_cond_timedwait");
    RP_FWD(r_spin_lock, "pthread_spin_lock"); RP_FWD(r_sem_wait, "sem_wait");
    RP_FWD(r_usleep, "usleep"); RP_FWD(r_nanosleep, "nanosleep"); RP_FWD(r_clock_nanosleep, "clock_nanosleep");
    RP_FWD(r_sleep, "sleep"); RP_FWD(r_sched_yield, "sched_yield");
}

#define RP_O_CREAT 0100
#define RP_O_TMPFILE 020200000

static unsigned rp_mode(int flags, va_list ap)
{
    return ((flags & RP_O_CREAT) != 0 || (flags & RP_O_TMPFILE) == RP_O_TMPFILE) ? va_arg(ap, unsigned) : 0u;
}

int open(const char* p, int flags, ...)
{
    note(rtcore::kIo, "open");
    va_list ap; va_start(ap, flags); const unsigned m = rp_mode(flags, ap); va_end(ap);
    return RP_FWD(r_open, "open")(p, flags, m);
}
int open64(const char* p, int flags, ...)
{
    note(rtcore::kIo, "open");
    va_list ap; va_start(ap, flags); const unsigned m = rp_mode(flags, ap); va_end(ap);
    return RP_FWD(r_open64, "open64")(p, flags, m);
}
int openat(int d, const char* p, int flags, ...)
{
    note(rtcore::kIo, "openat");
    va_list ap; va_start(ap, flags); const unsigned m = rp_mode(flags, ap); va_end(ap);
    return RP_FWD(r_openat, "openat")(d, p, flags, m);
}
int creat(const char* p, unsigned m)                  { note(rtcore::kIo, "creat");  return RP_FWD(r_creat, "creat")(p, m); }
void* fopen(const char* p, const char* m)             { note(rtcore::kIo, "fopen");  return RP_FWD(r_fopen, "fopen")(p, m); }
void* fopen64(const char* p, const char* m)           { note(rtcore::kIo, "fopen");  return RP_FWD(r_fopen64, "fopen64")(p, m); }
long read(int fd, void* b, size_t n)                  { note(rtcore::kIo, "read");   return RP_FWD(r_read, "read")(fd, b, n); }
long write(int fd, const void* b, size_t n)           { note(rtcore::kIo, "write");  return RP_FWD(r_write, "write")(fd, b, n); }
size_t fwrite(const void* b, size_t s, size_t n, void* f) { note(rtcore::kIo, "fwrite"); return RP_FWD(r_fwrite, "fwrite")(b, s, n, f); }
int fputs(const char* s, void* f)                     { note(rtcore::kIo, "fputs");  return RP_FWD(r_fputs, "fputs")(s, f); }
int puts(const char* s)                               { note(rtcore::kIo, "puts");   return RP_FWD(r_puts, "puts")(s); }
int vfprintf(void* f, const char* fmt, va_list ap)    { note(rtcore::kIo, "fprintf"); return RP_FWD(r_vfprintf, "vfprintf")(f, fmt, ap); }
int vprintf(const char* fmt, va_list ap)              { note(rtcore::kIo, "printf"); return RP_FWD(r_vprintf, "vprintf")(fmt, ap); }
int printf(const char* fmt, ...)
{
    note(rtcore::kIo, "printf");
    va_list ap; va_start(ap, fmt); const int r = RP_FWD(r_vprintf, "vprintf")(fmt, ap); va_end(ap);
    return r;
}
int fprintf(void* f, const char* fmt, ...)
{
    note(rtcore::kIo, "fprintf");
    va_list ap; va_start(ap, fmt); const int r = RP_FWD(r_vfprintf, "vfprintf")(f, fmt, ap); va_end(ap);
    return r;
}
long syscall(long no, ...)
{
    note(rtcore::kIo, "syscall");
    va_list ap; va_start(ap, no);
    long a[6];
    for (int i = 0; i < 6; ++i) a[i] = va_arg(ap, long);
    va_end(ap);
    return RP_FWD(r_syscall, "syscall")(no, a[0], a[1], a[2], a[3], a[4], a[5]);
}

int pthread_mutex_lock(void* m)                        { note(rtcore::kLock, "pthread_mutex_lock");      return RP_FWD(r_mutex_lock, "pthread_mutex_lock")(m); }
int pthread_mutex_timedlock(void* m, const void* t)    { note(rtcore::kLock, "pthread_mutex_timedlock"); return RP_FWD(r_mutex_timedlock, "pthread_mutex_timedlock")(m, t); }
int pthread_rwlock_rdlock(void* m)                     { note(rtcore::kLock, "pthread_rwlock_rdlock");   return RP_FWD(r_rdlock, "pthread_rwlock_rdlock")(m); }
int pthread_rwlock_wrlock(void* m)                     { note(rtcore::kLock, "pthread_rwlock_wrlock");   return RP_FWD(r_wrlock, "pthread_rwlock_wrlock")(m); }
int pthread_cond_wait(void* c, void* m)                { note(rtcore::kLock, "pthread_cond_wait");       return RP_FWD(r_cond_wait, "pthread_cond_wait")(c, m); }
int pthread_cond_timedwait(void* c, void* m, const void* t) { note(rtcore::kLock, "pthread_cond_timedwait"); return RP_FWD(r_cond_timedwait, "pthread_cond_timedwait")(c, m, t); }
int pthread_spin_lock(void* m)                         { note(rtcore::kLock, "pthread_spin_lock");       return RP_FWD(r_spin_lock, "pthread_spin_lock")(m); }
int sem_wait(void* s)                                  { note(rtcore::kLock, "sem_wait");                return RP_FWD(r_sem_wait, "sem_wait")(s); }
int usleep(unsigned us)                                { note(rtcore::kSleep, "usleep");     return RP_FWD(r_usleep, "usleep")(us); }
int nanosleep(const void* a, void* b)                  { note(rtcore::kSleep, "nanosleep");  return RP_FWD(r_nanosleep, "nanosleep")(a, b); }
int clock_nanosleep(int c, int f, const void* a, void* b) { note(rtcore::kSleep, "clock_nanosleep"); return RP_FWD(r_clock_nanosleep, "clock_nanosleep")(c, f, a, b); }
unsigned sleep(unsigned s)                             { note(rtcore::kSleep, "sleep");      return RP_FWD(r_sleep, "sleep")(s); }
int sched_yield()                                      { note(rtcore::kSleep, "sched_yield"); return RP_FWD(r_sched_yield, "sched_yield")(); }

} // extern "C"

extern "C" int ratpipe_rt_linux_active() { return 1; }

#else   // not glibc, or a sanitizer build

extern "C" int ratpipe_rt_linux_active() { return 0; }

#endif
