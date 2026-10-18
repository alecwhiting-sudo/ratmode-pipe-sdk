// ┌─────────────────────────────────────────────────────────────────────────────┐
// │ VENDORED FROM RatMode — DO NOT EDIT HERE.                                   │
// │                                                                             │
// │ Source : RatMode repository, Source/Ext/ExtMachOCheck.cpp                   │
// │ Commit : ce6206a08aeb73fde09f2537052f2892ac48790c (claude/ext-allowlist-memset,  │
// │          RatMode PR #65: allowlist ___bzero / _memset_pattern*)              │
// │ Licence: RatMode and this SDK share an author; this copy is distributed     │
// │          under the SDK's MIT licence (see LICENSE).                         │
// │                                                                             │
// │ This is the load-time Mach-O permissions check RatMode applies to every    │
// │ .ratpipe bundle. The SDK validator's V12 runs the very same code on any    │
// │ Mach-O binary (on every platform — it is pure C++17), so a bundle the      │
// │ validator passes is one RatMode will load. Everything below the marker     │
// │ line is byte-identical to RatMode's file; scripts/check_macho_sync.sh       │
// │ (run in CI) strips this banner and compares the sha256 with the one        │
// │ recorded in ExtMachOCheck.sha256 — and with a RatMode checkout when given  │
// │ one. To update: re-copy both files from RatMode, refresh the commit hashes │
// │ here and the hashes in ExtMachOCheck.sha256, in one commit that says why.  │
// └─────────────────────────────────────────────────────────────────────────────┘
// --- end of SDK provenance banner; everything below is byte-identical to RatMode ---
/// ExtMachOCheck.cpp — see ExtMachOCheck.h. Pure C++17, no JUCE, no IO.
/// Every multi-byte read goes through Reader::u32/u64 which refuse to read
/// outside the slice; every offset arithmetic is done in uint64_t and
/// range-checked before use, so a hostile header can only produce a
/// "rejected" result, never an out-of-bounds read.
#include "ExtMachOCheck.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

namespace openmusic {
namespace machocheck {

namespace {

// ── Mach-O constants (mach-o/loader.h, mach-o/fat.h, mach-o/nlist.h) ─────────
constexpr uint32_t kFatMagic      = 0xCAFEBABEu;   // big-endian on disk
constexpr uint32_t kFatMagic64    = 0xCAFEBABFu;
constexpr uint32_t kMhMagic64     = 0xFEEDFACFu;   // little-endian (host) on disk
constexpr uint32_t kMhMagic32     = 0xFEEDFACEu;
constexpr uint32_t kMhCigam64     = 0xCFFAEDFEu;
constexpr uint32_t kMhCigam32     = 0xCEFAEDFEu;

constexpr uint32_t kCpuArm64      = 0x0100000Cu;
constexpr uint32_t kCpuX86_64     = 0x01000007u;

constexpr uint32_t kMhDylib       = 6;

constexpr uint32_t kLcReqDyld             = 0x80000000u;
constexpr uint32_t LC_SEGMENT             = 0x1;
constexpr uint32_t LC_SYMTAB              = 0x2;
constexpr uint32_t LC_UNIXTHREAD          = 0x5;
constexpr uint32_t LC_DYSYMTAB            = 0xB;
constexpr uint32_t LC_LOAD_DYLIB          = 0xC;
constexpr uint32_t LC_ID_DYLIB            = 0xD;
constexpr uint32_t LC_LOAD_DYLINKER       = 0xE;
constexpr uint32_t LC_SEGMENT_64          = 0x19;
constexpr uint32_t LC_UUID                = 0x1B;
constexpr uint32_t LC_CODE_SIGNATURE      = 0x1D;
constexpr uint32_t LC_SEGMENT_SPLIT_INFO  = 0x1E;
constexpr uint32_t LC_LAZY_LOAD_DYLIB     = 0x20;
constexpr uint32_t LC_DYLD_INFO           = 0x22;
constexpr uint32_t LC_VERSION_MIN_MACOSX  = 0x24;
constexpr uint32_t LC_FUNCTION_STARTS     = 0x26;
constexpr uint32_t LC_DATA_IN_CODE        = 0x29;
constexpr uint32_t LC_SOURCE_VERSION      = 0x2A;
constexpr uint32_t LC_DYLIB_CODE_SIGN_DRS = 0x2B;
constexpr uint32_t LC_ENCRYPTION_INFO_64  = 0x2C;
constexpr uint32_t LC_LINKER_OPTION       = 0x2D;
constexpr uint32_t LC_LINKER_OPT_HINT     = 0x2E;
constexpr uint32_t LC_BUILD_VERSION       = 0x32;
constexpr uint32_t LC_LOAD_WEAK_DYLIB     = 0x18 | kLcReqDyld;
constexpr uint32_t LC_RPATH               = 0x1C | kLcReqDyld;
constexpr uint32_t LC_REEXPORT_DYLIB      = 0x1F | kLcReqDyld;
constexpr uint32_t LC_DYLD_INFO_ONLY      = 0x22 | kLcReqDyld;
constexpr uint32_t LC_LOAD_UPWARD_DYLIB   = 0x23 | kLcReqDyld;
constexpr uint32_t LC_MAIN                = 0x28 | kLcReqDyld;
constexpr uint32_t LC_DYLD_EXPORTS_TRIE   = 0x33 | kLcReqDyld;
constexpr uint32_t LC_DYLD_CHAINED_FIXUPS = 0x34 | kLcReqDyld;

constexpr uint32_t kVmProtWrite   = 0x2;
constexpr uint32_t kVmProtExecute = 0x4;

constexpr uint32_t kSectionTypeMask        = 0x000000FFu;
constexpr uint32_t S_ZEROFILL              = 0x1;
constexpr uint32_t S_GB_ZEROFILL           = 0xC;
constexpr uint32_t S_THREAD_LOCAL_ZEROFILL = 0x12;
constexpr uint32_t S_INTERPOSING           = 0xD;
constexpr uint32_t S_ATTR_PURE_INSTRUCTIONS = 0x80000000u;
constexpr uint32_t S_ATTR_SOME_INSTRUCTIONS = 0x00000400u;

constexpr uint8_t N_STAB = 0xE0, N_TYPE = 0x0E, N_EXT = 0x01, N_UNDF = 0x0, N_PBUD = 0xC;

// Hard caps so a hostile header cannot make us loop for seconds.
constexpr uint32_t kMaxNcmds   = 4096;
constexpr uint32_t kMaxSlices  = 16;
constexpr uint64_t kMaxSymbols = 2000000;
constexpr uint64_t kMaxImports = 200000;
constexpr size_t   kMaxSymbolNameLen = 4096;

// ── Bounds-checked reader over one slice ─────────────────────────────────────
struct Reader
{
    const uint8_t* d;
    size_t         n;

    bool inRange(uint64_t off, uint64_t len) const
    {
        return off <= n && len <= n && off + len <= n;   // no overflow: both <= n <= SIZE_MAX
    }
    bool u8(uint64_t off, uint8_t& v) const
    {
        if (! inRange(off, 1)) return false;
        v = d[off]; return true;
    }
    bool u16(uint64_t off, uint16_t& v) const
    {
        if (! inRange(off, 2)) return false;
        v = (uint16_t) (d[off] | (d[off + 1] << 8)); return true;
    }
    bool u32(uint64_t off, uint32_t& v) const
    {
        if (! inRange(off, 4)) return false;
        v = (uint32_t) d[off] | ((uint32_t) d[off + 1] << 8) | ((uint32_t) d[off + 2] << 16) | ((uint32_t) d[off + 3] << 24);
        return true;
    }
    bool u32be(uint64_t off, uint32_t& v) const
    {
        if (! inRange(off, 4)) return false;
        v = ((uint32_t) d[off] << 24) | ((uint32_t) d[off + 1] << 16) | ((uint32_t) d[off + 2] << 8) | (uint32_t) d[off + 3];
        return true;
    }
    bool u64(uint64_t off, uint64_t& v) const
    {
        uint32_t lo, hi;
        if (! u32(off, lo) || ! u32(off + 4, hi)) return false;
        v = ((uint64_t) hi << 32) | lo; return true;
    }
    bool u64be(uint64_t off, uint64_t& v) const
    {
        uint32_t hi, lo;
        if (! u32be(off, hi) || ! u32be(off + 4, lo)) return false;
        v = ((uint64_t) hi << 32) | lo; return true;
    }
    /// NUL-terminated string starting at `off`, terminator required before
    /// `limit` (exclusive, clamped to the slice) and before kMaxSymbolNameLen.
    bool cstr(uint64_t off, uint64_t limit, std::string& out) const
    {
        if (limit > n) limit = n;
        if (off >= limit) return false;
        const uint64_t maxLen = limit - off;
        for (uint64_t i = 0; i < maxLen && i < kMaxSymbolNameLen; ++i)
        {
            if (d[off + i] == 0)
            {
                out.assign(reinterpret_cast<const char*>(d + off), (size_t) i);
                return true;
            }
        }
        return false;
    }
    bool uleb(uint64_t& off, uint64_t limit, uint64_t& v) const
    {
        v = 0;
        unsigned shift = 0;
        for (;;)
        {
            uint8_t b;
            if (off >= limit || ! u8(off, b)) return false;
            ++off;
            if (shift >= 64) return false;
            v |= (uint64_t) (b & 0x7F) << shift;
            if ((b & 0x80) == 0) return true;
            shift += 7;
        }
    }
    bool sleb(uint64_t& off, uint64_t limit) const   // value discarded
    {
        unsigned shift = 0;
        for (;;)
        {
            uint8_t b;
            if (off >= limit || ! u8(off, b)) return false;
            ++off;
            shift += 7;
            if ((b & 0x80) == 0) return true;
            if (shift > 70) return false;
        }
    }
};

struct SliceResult
{
    bool        ok = true;
    std::string reason;
    void fail(const std::string& r) { if (ok) { ok = false; reason = r; } }
};

std::string hex(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", v);
    return buf;
}

std::string segName16(const uint8_t* p)
{
    size_t len = 0;
    while (len < 16 && p[len] != 0) ++len;
    return std::string(reinterpret_cast<const char*>(p), len);
}

// ── Import allowlist ─────────────────────────────────────────────────────────

const std::set<std::string>& allowedCImports()
{
    static const std::set<std::string> s = []
    {
        std::set<std::string> a;
        // libm — every C99 function in f / (none) / l flavours.
        static const char* const math[] = {
            "sin","cos","tan","asin","acos","atan","atan2","sinh","cosh","tanh","asinh","acosh","atanh",
            "exp","exp2","expm1","log","log2","log10","log1p","logb","ilogb","pow","sqrt","cbrt","hypot",
            "fabs","fmod","remainder","remquo","floor","ceil","round","lround","llround","trunc","rint",
            "lrint","llrint","nearbyint","fmin","fmax","fdim","fma","copysign","frexp","ldexp","modf",
            "scalbn","scalbln","erf","erfc","tgamma","lgamma","nan","nextafter","nexttoward","fpclassify",
            "isnan","isinf","isfinite","signbit"
        };
        for (const char* m : math)
        {
            a.insert(std::string("_") + m);
            a.insert(std::string("_") + m + "f");
            a.insert(std::string("_") + m + "l");
        }
        static const char* const exact[] = {
            // libm extras Apple's clang emits
            "___sincos_stret","___sincosf_stret","___sincospi_stret","___sincospif_stret",
            "___exp10","___exp10f","___cospi","___cospif","___sinpi","___sinpif","___tanpi","___tanpif",
            "___fpclassifyf","___fpclassifyd","___fpclassify","___isnanf","___isnand","___isinff","___isinfd",
            "___isfinitef","___isfinited","___signbitf","___signbitd","___math_errhandling",
            "_fesetenv","_fegetenv","_feclearexcept","_fetestexcept","_fesetround","_fegetround","_feraiseexcept",
            "_feholdexcept","_feupdateenv","_fegetexceptflag","_fesetexceptflag","__FE_DFL_ENV","__FE_DFL_DISABLE_SSE_DENORMS_ENV",
            // memory
            "_malloc","_calloc","_realloc","_free","_posix_memalign","_aligned_alloc","_malloc_size",
            "_memcpy","_memmove","_memset","_memcmp","_memchr","_bzero","_bcopy","_bcmp",
            "___memcpy_chk","___memmove_chk","___memset_chk","___bzero_chk",
            // compiler-lowered fills: x86_64 clang emits ___bzero for runtime-length
            // zeroing; constant non-zero fill loops become memset_pattern*. Pure memory ops.
            "___bzero","_memset_pattern4","_memset_pattern8","_memset_pattern16",
            // strings / ctype / conversions
            "_strlen","_strnlen","_strcmp","_strncmp","_strcpy","_strncpy","_strlcpy","_strlcat","_strcat","_strncat",
            "_strchr","_strrchr","_strstr","_strdup","_strndup","_strcasecmp","_strncasecmp","_strspn","_strcspn","_strpbrk",
            "_snprintf","_vsnprintf","___snprintf_chk","___vsnprintf_chk","___strcpy_chk","___strncpy_chk","___strcat_chk","___strncat_chk","___strlcpy_chk","___strlcat_chk",
            "_strtol","_strtoul","_strtoll","_strtoull","_strtod","_strtof","_strtold","_atoi","_atol","_atof",
            "_toupper","_tolower","_isdigit","_isalpha","_isalnum","_isspace","_isupper","_islower","_isxdigit","_ispunct","_isprint",
            "___tolower","___toupper","___maskrune","___istype","___isctype",
            // misc libc that is RT-irrelevant and IO-free
            "_qsort","_bsearch","_abs","_labs","_llabs","_div","_ldiv","_lldiv","_rand","_srand","_rand_r","_random","_srandom",
            "_arc4random","_arc4random_uniform","_arc4random_buf",
            "_abort","___assert_rtn","___stack_chk_fail","___stack_chk_guard","___error",
            "_dyld_stub_binder","dyld_stub_binder",
            // compiler-rt builtins exported by libSystem (libcompiler_rt)
            "___divti3","___modti3","___udivti3","___umodti3","___multi3","___muloti4","___mulodi4",
            "___floattidf","___floattisf","___floatuntidf","___floatuntisf","___fixdfti","___fixsfti","___fixunsdfti","___fixunssfti",
            "___divdc3","___muldc3","___mulsc3","___divsc3","___mulxc3","___divxc3",
            "___popcountdi2","___popcountsi2","___popcountti2","___bswapdi2","___bswapsi2","___clzti2","___ctzti2","___ffsti2",
            "___ashlti3","___ashrti3","___lshrti3","___negti2","___cmpti2","___ucmpti2","___absvti2","___paritysi2","___paritydi2",
            "___chkstk_darwin",
            // C++ ABI (libc++abi) — exceptions, RTTI, guards, pure virtual
            "___cxa_begin_catch","___cxa_end_catch","___cxa_rethrow","___cxa_throw","___cxa_allocate_exception",
            "___cxa_free_exception","___cxa_guard_acquire","___cxa_guard_release","___cxa_guard_abort",
            "___cxa_pure_virtual","___cxa_deleted_virtual","___cxa_atexit","___cxa_bad_cast","___cxa_bad_typeid",
            "___cxa_call_unexpected","___cxa_get_exception_ptr","___cxa_current_exception_type","___cxa_uncaught_exceptions",
            "___cxa_increment_exception_refcount","___cxa_decrement_exception_refcount","___cxa_rethrow_primary_exception",
            "___cxa_demangle","___cxa_thread_atexit","___cxa_vec_new","___cxa_vec_delete",
            "___gxx_personality_v0","__Unwind_Resume","__Unwind_RaiseException","__Unwind_DeleteException",
            "__Unwind_GetIP","__Unwind_SetIP","__Unwind_GetLanguageSpecificData","__Unwind_GetRegionStart",
            "__Unwind_SetGR","__Unwind_GetGR","__Unwind_GetDataRelBase","__Unwind_GetTextRelBase","__Unwind_GetCFA",
            "__Unwind_Backtrace","__Unwind_Resume_or_Rethrow","__Unwind_GetIPInfo",
            "___dynamic_cast","__ZSt9terminatev","__ZSt13get_terminatev","__ZSt13set_terminatePFvvE",
            "__ZSt14get_unexpectedv","__ZSt14set_unexpectedPFvvE","__ZSt10unexpectedv","__ZSt17current_exceptionv",
            "__ZSt17rethrow_exceptionNSt3__113exception_ptrE","__ZSt18uncaught_exceptionv","__ZSt19uncaught_exceptionsv",
            // operator new / delete (all shapes)
            "__Znwm","__Znam","__ZdlPv","__ZdaPv","__ZdlPvm","__ZdaPvm","__ZnwmSt11align_val_t","__ZnamSt11align_val_t",
            "__ZdlPvSt11align_val_t","__ZdaPvSt11align_val_t","__ZdlPvmSt11align_val_t","__ZdaPvmSt11align_val_t",
            "__ZnwmRKSt9nothrow_t","__ZnamRKSt9nothrow_t","__ZdlPvRKSt9nothrow_t","__ZdaPvRKSt9nothrow_t",
            "__ZSt15get_new_handlerv","__ZSt15set_new_handlerPFvvE","__ZSt7nothrow",
        };
        for (const char* e : exact) a.insert(e);
        return a;
    }();
    return s;
}

// Any mangled libc++ import containing one of these is refused: these are the
// std facilities that open files, talk to the kernel, spawn threads or block.
const char* const kCxxDenyTokens[] = {
    "filesystem", "__fs", "fstream", "filebuf", "basic_ostream", "basic_istream", "basic_iostream",
    "basic_streambuf", "ios_base", "basic_ios", "4coutE", "4cerrE", "4clogE", "3cinE", "5wcoutE", "5wcerrE",
    "thread", "mutex", "condition_variable", "call_once", "once_flag", "future", "promise", "packaged_task",
    "random_device", "system_clock", "steady_clock", "high_resolution_clock", "locale", "codecvt", "ctype",
    "system_error", "error_code", "error_category", "9exception_ptr", "__libcpp_",   // __libcpp_ wrappers around pthread/time
    "iostream", "basic_syncbuf", "stop_token", "latch", "barrier", "semaphore", "jthread",
    "6atomic", "__cxx_atomic",   // libc++'s out-of-line atomic wait/notify (futex) paths
};

} // anonymous namespace

bool isDylibAllowed(const std::string& installName)
{
    return installName == "/usr/lib/libSystem.B.dylib"
        || installName == "/usr/lib/libc++.1.dylib"
        || installName == "/usr/lib/libc++abi.dylib";
}

bool isImportAllowed(const std::string& name)
{
    if (name.empty() || name.size() > 1024) return false;
    if (allowedCImports().count(name) != 0) return true;

    // Everything else must be a C++ mangled name from libc++ (the only other
    // dylib that can be linked), minus the denied facilities.
    if (name.compare(0, 3, "__Z") != 0) return false;
    for (const char* tok : kCxxDenyTokens)
        if (name.find(tok) != std::string::npos) return false;
    return true;
}

namespace {

// ── Per-slice state ──────────────────────────────────────────────────────────
struct Slice
{
    Reader r;
    uint32_t cputype = 0;
    SliceResult res;
    std::set<std::string> imports;
    bool sawSymtab = false, sawDyldInfo = false, sawChained = false, sawSegment = false, sawId = false;
    uint32_t segCount = 0;

    void noteImport(const std::string& name)
    {
        if (imports.size() > kMaxImports) { res.fail("too many imports"); return; }
        if (imports.insert(name).second && ! isImportAllowed(name))
            res.fail("imports disallowed symbol '" + name + "'");
    }

    // ── instruction scan ─────────────────────────────────────────────────
    void scanCode(uint64_t off, uint64_t size)
    {
        if (! r.inRange(off, size)) { res.fail("instruction section outside file"); return; }
        const uint8_t* p = r.d + off;
        if (cputype == kCpuArm64)
        {
            // svc #imm16 : 1101 0100 000 imm16 000 01 → mask 0xFFE0001F == 0xD4000001
            for (uint64_t i = 0; i + 4 <= size; i += 4)
            {
                const uint32_t w = (uint32_t) p[i] | ((uint32_t) p[i + 1] << 8) | ((uint32_t) p[i + 2] << 16) | ((uint32_t) p[i + 3] << 24);
                if ((w & 0xFFE0001Fu) == 0xD4000001u)
                {
                    res.fail("raw syscall instruction (arm64 svc) in code at +" + hex((uint32_t) i));
                    return;
                }
            }
        }
        else if (cputype == kCpuX86_64)
        {
            for (uint64_t i = 0; i + 2 <= size; ++i)
            {
                if (p[i] == 0x0F && (p[i + 1] == 0x05 || p[i + 1] == 0x34))
                {
                    res.fail(std::string("raw syscall instruction (x86-64 ") + (p[i + 1] == 0x05 ? "syscall" : "sysenter")
                             + ") in code at +" + hex((uint32_t) i));
                    return;
                }
            }
        }
    }

    // ── LC_SEGMENT_64 ────────────────────────────────────────────────────
    void segment64(uint64_t cmdOff, uint32_t cmdsize)
    {
        if (cmdsize < 72) { res.fail("LC_SEGMENT_64 too small"); return; }
        sawSegment = true;
        if (++segCount > 64) { res.fail("too many segments"); return; }
        const std::string seg = segName16(r.d + cmdOff + 8);
        uint64_t vmsize, fileoff, filesize;
        uint32_t maxprot, initprot, nsects;
        if (! r.u64(cmdOff + 32, vmsize) || ! r.u64(cmdOff + 40, fileoff) || ! r.u64(cmdOff + 48, filesize)
            || ! r.u32(cmdOff + 56, maxprot) || ! r.u32(cmdOff + 60, initprot) || ! r.u32(cmdOff + 64, nsects))
        { res.fail("LC_SEGMENT_64 truncated"); return; }

        if ((initprot & (kVmProtWrite | kVmProtExecute)) == (kVmProtWrite | kVmProtExecute)
            || (maxprot & (kVmProtWrite | kVmProtExecute)) == (kVmProtWrite | kVmProtExecute))
        { res.fail("segment " + seg + " is writable and executable"); return; }
        if (filesize > 0 && ! r.inRange(fileoff, filesize))
        { res.fail("segment " + seg + " file range outside slice"); return; }
        if (filesize > vmsize) { res.fail("segment " + seg + " filesize > vmsize"); return; }

        // B1: dyld maps the WHOLE segment with its protection, so any byte in
        // the file-backed range of an executable segment can be jumped to —
        // including non-instruction sections (__TEXT,__const, __cstring…)
        // and bytes no section covers. Scan the entire range, not just
        // sections that declare themselves code.
        const bool executable = ((initprot | maxprot) & kVmProtExecute) != 0;
        if (executable && filesize > 0)
        {
            scanCode(fileoff, filesize);
            if (! res.ok) return;
        }

        const uint64_t need = 72 + (uint64_t) nsects * 80;
        if (need > cmdsize) { res.fail("segment " + seg + " nsects overflows cmdsize"); return; }

        for (uint32_t i = 0; i < nsects; ++i)
        {
            const uint64_t s = cmdOff + 72 + (uint64_t) i * 80;
            const std::string sectname = segName16(r.d + s);
            uint64_t size, addr; uint32_t offset, flags;
            if (! r.u64(s + 32, addr) || ! r.u64(s + 40, size) || ! r.u32(s + 48, offset) || ! r.u32(s + 64, flags))
            { res.fail("section truncated"); return; }
            const uint32_t type = flags & kSectionTypeMask;
            // S2: dyld interposing (__DATA,__interpose / S_INTERPOSING) lets a
            // dylib replace functions in OTHER images — never acceptable here.
            if (type == S_INTERPOSING || sectname == "__interpose")
            { res.fail("section " + seg + "," + sectname + " requests dyld interposing"); return; }
            const bool zerofill = type == S_ZEROFILL || type == S_GB_ZEROFILL || type == S_THREAD_LOCAL_ZEROFILL;
            if (! zerofill && size > 0 && ! r.inRange(offset, size))
            { res.fail("section " + seg + "," + sectname + " outside slice"); return; }
            const bool code = (flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) != 0
                           || (seg == "__TEXT" && sectname == "__text");
            if (code && ! zerofill && size > 0)
            {
                if (! executable) { res.fail("code section in non-executable segment " + seg); return; }
                // Instruction sections of executable segments were covered by
                // the whole-segment scan above; a code section that lies
                // outside its own segment's file range is scanned here too.
                if (offset < fileoff || offset + size > fileoff + filesize)
                {
                    scanCode(offset, size);
                    if (! res.ok) return;
                }
            }
            (void) addr;
        }
    }

    // ── dylib commands ───────────────────────────────────────────────────
    void dylibCmd(uint64_t cmdOff, uint32_t cmdsize, const char* what)
    {
        if (cmdsize < 24) { res.fail(std::string(what) + " too small"); return; }
        uint32_t nameOff;
        if (! r.u32(cmdOff + 8, nameOff)) { res.fail("dylib command truncated"); return; }
        if (nameOff < 24 || nameOff >= cmdsize) { res.fail(std::string(what) + " name offset outside command"); return; }
        std::string name;
        if (! r.cstr(cmdOff + nameOff, cmdOff + cmdsize, name)) { res.fail(std::string(what) + " name not terminated"); return; }
        if (! isDylibAllowed(name)) res.fail(std::string(what) + " links disallowed library '" + name + "'");
    }

    // ── LC_SYMTAB undefined externals ────────────────────────────────────
    void symtab(uint64_t cmdOff, uint32_t cmdsize)
    {
        if (cmdsize < 24) { res.fail("LC_SYMTAB too small"); return; }
        if (sawSymtab) { res.fail("duplicate LC_SYMTAB"); return; }
        sawSymtab = true;
        uint32_t symoff, nsyms, stroff, strsize;
        if (! r.u32(cmdOff + 8, symoff) || ! r.u32(cmdOff + 12, nsyms) || ! r.u32(cmdOff + 16, stroff) || ! r.u32(cmdOff + 20, strsize))
        { res.fail("LC_SYMTAB truncated"); return; }
        if (nsyms == 0) return;
        if (nsyms > kMaxSymbols) { res.fail("too many symbols"); return; }
        if (! r.inRange(symoff, (uint64_t) nsyms * 16)) { res.fail("symbol table outside slice"); return; }
        if (! r.inRange(stroff, strsize)) { res.fail("string table outside slice"); return; }
        const uint64_t strEnd = (uint64_t) stroff + strsize;

        for (uint32_t i = 0; i < nsyms && res.ok; ++i)
        {
            const uint64_t e = (uint64_t) symoff + (uint64_t) i * 16;
            uint32_t strx; uint8_t type;
            if (! r.u32(e, strx) || ! r.u8(e + 4, type)) { res.fail("nlist truncated"); return; }
            if ((type & N_STAB) != 0) continue;
            const uint8_t t = type & N_TYPE;
            if ((t != N_UNDF && t != N_PBUD) || (type & N_EXT) == 0) continue;
            if (strx == 0) continue;   // empty name
            std::string name;
            if (! r.cstr((uint64_t) stroff + strx, strEnd, name)) { res.fail("symbol name outside string table"); return; }
            noteImport(name);
        }
    }

    // ── LC_DYLD_INFO bind opcode streams ─────────────────────────────────
    void bindStream(uint32_t off, uint32_t size, const char* what)
    {
        if (size == 0) return;
        if (! r.inRange(off, size)) { res.fail(std::string(what) + " outside slice"); return; }
        uint64_t p = off;
        const uint64_t end = (uint64_t) off + size;
        uint64_t guard = 0;
        while (p < end && res.ok)
        {
            if (++guard > 10000000) { res.fail("bind stream too long"); return; }
            uint8_t b;
            if (! r.u8(p, b)) { res.fail("bind stream truncated"); return; }
            ++p;
            const uint8_t op = b & 0xF0, imm = b & 0x0F;
            uint64_t v;
            switch (op)
            {
                case 0x00: /* DONE: lazy streams chain several; keep going */ break;
                case 0x10: break;                                          // SET_DYLIB_ORDINAL_IMM
                case 0x20: if (! r.uleb(p, end, v)) { res.fail("bad uleb"); return; } break;   // SET_DYLIB_ORDINAL_ULEB
                case 0x30: break;                                          // SET_DYLIB_SPECIAL_IMM
                case 0x40:                                                 // SET_SYMBOL_TRAILING_FLAGS_IMM
                {
                    std::string name;
                    if (! r.cstr(p, end, name)) { res.fail("bind symbol name not terminated"); return; }
                    p += name.size() + 1;
                    noteImport(name);
                    break;
                }
                case 0x50: break;                                          // SET_TYPE_IMM
                case 0x60: if (! r.sleb(p, end)) { res.fail("bad sleb"); return; } break;      // SET_ADDEND_SLEB
                case 0x70: if (! r.uleb(p, end, v)) { res.fail("bad uleb"); return; } break;   // SET_SEGMENT_AND_OFFSET_ULEB
                case 0x80: if (! r.uleb(p, end, v)) { res.fail("bad uleb"); return; } break;   // ADD_ADDR_ULEB
                case 0x90: break;                                          // DO_BIND
                case 0xA0: if (! r.uleb(p, end, v)) { res.fail("bad uleb"); return; } break;   // DO_BIND_ADD_ADDR_ULEB
                case 0xB0: break;                                          // DO_BIND_ADD_ADDR_IMM_SCALED
                case 0xC0:                                                 // DO_BIND_ULEB_TIMES_SKIPPING_ULEB
                    if (! r.uleb(p, end, v) || ! r.uleb(p, end, v)) { res.fail("bad uleb"); return; }
                    break;
                case 0xD0:                                                 // THREADED
                    if (imm == 0x00) { if (! r.uleb(p, end, v)) { res.fail("bad uleb"); return; } }
                    else if (imm != 0x01) { res.fail("unknown threaded bind subopcode"); return; }
                    break;
                default:
                    res.fail("unknown bind opcode " + hex(op));
                    return;
            }
        }
    }

    void dyldInfo(uint64_t cmdOff, uint32_t cmdsize)
    {
        if (cmdsize < 48) { res.fail("LC_DYLD_INFO too small"); return; }
        if (sawDyldInfo) { res.fail("duplicate LC_DYLD_INFO"); return; }
        sawDyldInfo = true;
        uint32_t bo, bs, wo, ws, lo, ls;
        if (! r.u32(cmdOff + 16, bo) || ! r.u32(cmdOff + 20, bs) || ! r.u32(cmdOff + 24, wo) || ! r.u32(cmdOff + 28, ws)
            || ! r.u32(cmdOff + 32, lo) || ! r.u32(cmdOff + 36, ls))
        { res.fail("LC_DYLD_INFO truncated"); return; }
        bindStream(bo, bs, "bind info");
        if (res.ok) bindStream(wo, ws, "weak bind info");
        if (res.ok) bindStream(lo, ls, "lazy bind info");
    }

    // ── LC_DYLD_CHAINED_FIXUPS imports ───────────────────────────────────
    void chainedFixups(uint64_t cmdOff, uint32_t cmdsize)
    {
        if (cmdsize < 16) { res.fail("LC_DYLD_CHAINED_FIXUPS too small"); return; }
        if (sawChained) { res.fail("duplicate LC_DYLD_CHAINED_FIXUPS"); return; }
        sawChained = true;
        uint32_t dataoff, datasize;
        if (! r.u32(cmdOff + 8, dataoff) || ! r.u32(cmdOff + 12, datasize)) { res.fail("chained fixups truncated"); return; }
        if (datasize < 28 || ! r.inRange(dataoff, datasize)) { res.fail("chained fixups data outside slice"); return; }
        const uint64_t base = dataoff, end = (uint64_t) dataoff + datasize;
        uint32_t version, importsOff, symbolsOff, importsCount, importsFormat, symbolsFormat;
        if (! r.u32(base, version) || ! r.u32(base + 8, importsOff) || ! r.u32(base + 12, symbolsOff)
            || ! r.u32(base + 16, importsCount) || ! r.u32(base + 20, importsFormat) || ! r.u32(base + 24, symbolsFormat))
        { res.fail("chained fixups header truncated"); return; }
        if (version != 0) { res.fail("unknown chained fixups version"); return; }
        if (symbolsFormat != 0) { res.fail("compressed chained-fixups symbol table"); return; }
        if (importsCount == 0) return;
        if (importsCount > kMaxImports) { res.fail("too many chained imports"); return; }
        uint32_t stride;
        switch (importsFormat) { case 1: stride = 4; break; case 2: stride = 8; break; case 3: stride = 16; break;
                                 default: res.fail("unknown chained import format"); return; }
        const uint64_t impStart = base + importsOff;
        if (impStart < base || impStart > end || (uint64_t) importsCount * stride > end - impStart)
        { res.fail("chained imports table outside fixups data"); return; }
        const uint64_t symStart = base + symbolsOff;
        if (symStart < base || symStart > end) { res.fail("chained symbol table outside fixups data"); return; }

        for (uint32_t i = 0; i < importsCount && res.ok; ++i)
        {
            const uint64_t e = impStart + (uint64_t) i * stride;
            uint64_t nameOff;
            if (importsFormat == 3)
            {
                uint32_t hi; if (! r.u32(e + 4, hi)) { res.fail("chained import truncated"); return; }
                nameOff = hi;
            }
            else
            {
                uint32_t w; if (! r.u32(e, w)) { res.fail("chained import truncated"); return; }
                nameOff = w >> 9;   // lib_ordinal:8, weak_import:1, name_offset:23
            }
            std::string name;
            if (! r.cstr(symStart + nameOff, end, name)) { res.fail("chained import name outside fixups data"); return; }
            noteImport(name);
        }
    }

    // ── Header + load-command walk ───────────────────────────────────────
    void run()
    {
        uint32_t magic, filetype, ncmds, sizeofcmds;
        if (! r.u32(0, magic)) { res.fail("slice too small for a Mach-O header"); return; }
        if (magic == kMhMagic32 || magic == kMhCigam32) { res.fail("32-bit Mach-O is not supported"); return; }
        if (magic == kMhCigam64) { res.fail("big-endian Mach-O is not supported"); return; }
        if (magic != kMhMagic64) { res.fail("not a Mach-O image (magic " + hex(magic) + ")"); return; }
        if (! r.u32(4, cputype) || ! r.u32(12, filetype) || ! r.u32(16, ncmds) || ! r.u32(20, sizeofcmds))
        { res.fail("Mach-O header truncated"); return; }
        if (cputype != kCpuArm64 && cputype != kCpuX86_64) { res.fail("unsupported cputype " + hex(cputype)); return; }
        if (filetype != kMhDylib) { res.fail("not a dylib (filetype " + hex(filetype) + ")"); return; }
        if (ncmds > kMaxNcmds) { res.fail("too many load commands"); return; }
        if (! r.inRange(32, sizeofcmds)) { res.fail("load commands extend past the slice"); return; }

        uint64_t off = 32;
        const uint64_t cmdsEnd = 32 + (uint64_t) sizeofcmds;
        for (uint32_t i = 0; i < ncmds && res.ok; ++i)
        {
            uint32_t cmd, cmdsize;
            if (off + 8 > cmdsEnd || ! r.u32(off, cmd) || ! r.u32(off + 4, cmdsize))
            { res.fail("load command " + std::to_string(i) + " truncated"); return; }
            if (cmdsize < 8 || (cmdsize & 7) != 0) { res.fail("load command " + std::to_string(i) + " has bad cmdsize"); return; }
            if (cmdsize > cmdsEnd - off) { res.fail("load command " + std::to_string(i) + " overflows sizeofcmds"); return; }

            switch (cmd)
            {
                case LC_SEGMENT_64:          segment64(off, cmdsize); break;
                case LC_ID_DYLIB:
                    if (sawId) { res.fail("duplicate LC_ID_DYLIB"); return; }
                    sawId = true;
                    if (cmdsize < 24) { res.fail("LC_ID_DYLIB too small"); return; }
                    break;
                case LC_LOAD_DYLIB:          dylibCmd(off, cmdsize, "LC_LOAD_DYLIB"); break;
                case LC_LOAD_WEAK_DYLIB:     dylibCmd(off, cmdsize, "LC_LOAD_WEAK_DYLIB"); break;
                case LC_SYMTAB:              symtab(off, cmdsize); break;
                case LC_DYLD_INFO:
                case LC_DYLD_INFO_ONLY:      dyldInfo(off, cmdsize); break;
                case LC_DYLD_CHAINED_FIXUPS: chainedFixups(off, cmdsize); break;
                // Accepted, nothing to inspect:
                case LC_DYSYMTAB: case LC_DYLD_EXPORTS_TRIE: case LC_UUID: case LC_BUILD_VERSION:
                case LC_VERSION_MIN_MACOSX: case LC_SOURCE_VERSION: case LC_FUNCTION_STARTS:
                case LC_DATA_IN_CODE: case LC_CODE_SIGNATURE: case LC_SEGMENT_SPLIT_INFO:
                case LC_DYLIB_CODE_SIGN_DRS: case LC_LINKER_OPT_HINT:
                    break;
                // Explicitly refused (named for the message):
                case LC_RPATH:              res.fail("LC_RPATH is not allowed"); return;
                case LC_REEXPORT_DYLIB:     res.fail("LC_REEXPORT_DYLIB is not allowed"); return;
                case LC_LAZY_LOAD_DYLIB:    res.fail("LC_LAZY_LOAD_DYLIB is not allowed"); return;
                case LC_LOAD_UPWARD_DYLIB:  res.fail("LC_LOAD_UPWARD_DYLIB is not allowed"); return;
                case LC_LOAD_DYLINKER:      res.fail("LC_LOAD_DYLINKER is not allowed in a dylib"); return;
                case LC_MAIN: case LC_UNIXTHREAD: res.fail("executable entry point in a dylib"); return;
                case LC_ENCRYPTION_INFO_64: res.fail("encrypted segments are not allowed"); return;
                case LC_SEGMENT:            res.fail("32-bit segment in a 64-bit image"); return;
                case LC_LINKER_OPTION:      res.fail("LC_LINKER_OPTION is not allowed"); return;
                default:                    res.fail("unknown load command " + hex(cmd)); return;
            }
            off += cmdsize;
        }
        if (! res.ok) return;
        if (! sawSegment) { res.fail("no LC_SEGMENT_64"); return; }
        if (! sawId)      { res.fail("no LC_ID_DYLIB"); return; }
        if (! sawSymtab && ! sawDyldInfo && ! sawChained) { res.fail("no import table (LC_SYMTAB / LC_DYLD_INFO / chained fixups)"); return; }
    }
};

} // anonymous namespace

Result checkImage(const uint8_t* data, size_t size)
{
    Result out;
    if (data == nullptr || size < 32) { out.reason = "file too small to be a Mach-O image"; return out; }

    std::vector<std::pair<uint64_t, uint64_t>> slices;   // offset, size
    Reader whole { data, size };

    uint32_t magicBE = 0;
    whole.u32be(0, magicBE);
    if (magicBE == kFatMagic || magicBE == kFatMagic64)
    {
        const bool fat64 = magicBE == kFatMagic64;
        uint32_t nfat;
        if (! whole.u32be(4, nfat)) { out.reason = "fat header truncated"; return out; }
        if (nfat == 0 || nfat > kMaxSlices) { out.reason = "fat header has " + std::to_string(nfat) + " slices"; return out; }
        const uint64_t archSize = fat64 ? 32 : 20;
        for (uint32_t i = 0; i < nfat; ++i)
        {
            const uint64_t a = 8 + (uint64_t) i * archSize;
            uint32_t cputype; uint64_t off, len;
            if (! whole.u32be(a, cputype)) { out.reason = "fat_arch truncated"; return out; }
            if (fat64) { if (! whole.u64be(a + 8, off) || ! whole.u64be(a + 16, len)) { out.reason = "fat_arch_64 truncated"; return out; } }
            else       { uint32_t o32, l32; if (! whole.u32be(a + 8, o32) || ! whole.u32be(a + 12, l32)) { out.reason = "fat_arch truncated"; return out; } off = o32; len = l32; }
            if (cputype != kCpuArm64 && cputype != kCpuX86_64) { out.reason = "fat slice " + std::to_string(i) + " has unsupported cputype " + hex(cputype); return out; }
            if (len < 32 || ! whole.inRange(off, len)) { out.reason = "fat slice " + std::to_string(i) + " outside file"; return out; }
            for (const auto& s : slices)
                if (off < s.first + s.second && s.first < off + len) { out.reason = "fat slices overlap"; return out; }
            slices.emplace_back(off, len);
        }
    }
    else
    {
        slices.emplace_back(0, size);
    }

    std::set<std::string> allImports;
    for (size_t i = 0; i < slices.size(); ++i)
    {
        Slice s;
        s.r = Reader { data + slices[i].first, (size_t) slices[i].second };
        s.run();
        ++out.slices;
        if (! s.res.ok)
        {
            out.reason = (slices.size() > 1 ? "slice " + std::to_string(i) + ": " : std::string()) + s.res.reason;
            return out;
        }
        allImports.insert(s.imports.begin(), s.imports.end());
    }
    out.imports = (int) allImports.size();
    out.ok = true;
    return out;
}

} // namespace machocheck
} // namespace openmusic
