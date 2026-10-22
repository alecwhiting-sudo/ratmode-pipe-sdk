// ┌─────────────────────────────────────────────────────────────────────────────┐
// │ VENDORED FROM RatMode — DO NOT EDIT HERE.                                   │
// │                                                                             │
// │ Source : RatMode repository, Source/Ext/ExtMachOCheck.h                     │
// │ Commit : 6ec82c51dfb8c00b177fecd5915456361f4345b6 (origin/main)             │
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
#pragma once
/// ExtMachOCheck — load-time "binary permissions" check for .ratpipe bundles
/// (mirrors the catalogue CI check in "Distribution & trust
/// model" step 2e). PURE C++17, NO JUCE, NO IO: it inspects a byte image the
/// caller already read, so the same bytes that were checked are the bytes
/// that get hashed (A12) and whose path is then dlopened (A11 TOCTOU rule),
/// and so the Linux harness (scratchpad/a38/harness/macho_harness.cpp) can
/// drive it with crafted blobs under ASan/UBSan.
///
/// What it enforces on EVERY slice of a fat or thin image:
///   - 64-bit Mach-O only (MH_MAGIC_64, little-endian), cputype arm64 or
///     x86_64, filetype MH_DYLIB only (the SDK's add_ratpipe emits a .dylib;
///     MH_BUNDLE is refused). Fat (32- or 64-bit fat headers) → every slice
///     is checked; an unknown-arch slice fails.
///   - Load commands walked with bounds-checked reads: cmdsize >= 8, 8-byte
///     aligned, inside sizeofcmds, sizeofcmds inside the slice. Any command
///     not on the explicit allowlist fails (fail closed), notably LC_RPATH,
///     LC_REEXPORT_DYLIB, LC_LAZY_LOAD_DYLIB, LC_LOAD_UPWARD_DYLIB,
///     LC_LOAD_DYLINKER, LC_MAIN/LC_UNIXTHREAD, LC_ENCRYPTION_INFO*,
///     LC_SEGMENT (32-bit), LC_SUB_*, LC_LINKER_OPTION.
///   - LC_LOAD_DYLIB / LC_LOAD_WEAK_DYLIB names must be exactly one of
///     /usr/lib/libSystem.B.dylib, /usr/lib/libc++.1.dylib,
///     /usr/lib/libc++abi.dylib (so @rpath / @loader_path / @executable_path
///     and every other library are refused).
///   - Segments: no W+X initprot/maxprot; file ranges inside the slice;
///     non-zerofill sections inside the slice; no dyld interposing
///     (S_INTERPOSING type or a section named __interpose).
///   - Imported symbols from ALL three tables dyld can bind from — LC_SYMTAB
///     undefined externals, LC_DYLD_INFO bind/weak/lazy opcode streams, and
///     LC_DYLD_CHAINED_FIXUPS imports — against an allowlist (maths, memory,
///     string, C++ runtime; see isImportAllowed). Compressed chained-fixups
///     symbol tables are refused. An image with none of the three tables is
///     refused ("no import table").
///   - Raw syscall instructions anywhere in the FILE-BACKED RANGE OF EVERY
///     EXECUTABLE SEGMENT (initprot or maxprot has VM_PROT_EXECUTE) — dyld
///     maps whole segments, so __TEXT,__const, __cstring and bytes no section
///     covers are just as jumpable as __text — plus any instruction-flagged
///     section elsewhere: arm64 `svc #imm` (any imm, 4-byte-aligned words),
///     x86-64 `0F 05` (syscall) / `0F 34` (sysenter). The x86 scan is a
///     byte-pattern scan over data too, so it can false-positive (e.g. a
///     float constant 0x050F in __TEXT,__const); the catalogue CI runs the
///     same scan so a legitimate pipe that trips it is caught before
///     publication and can move the constant to __DATA_CONST.
///
/// Known gaps (documented in the RatMode tracker): the exports trie is not
/// parsed (V1 "only ratpipe_entry exported" is the SDK validator's job, and
/// RTLD_LOCAL makes extra exports harmless); x86-64 `int 0x80` is not
/// scanned (32-bit ABI, unusable from a 64-bit dylib).
#include <cstddef>
#include <cstdint>
#include <string>

namespace openmusic {
namespace machocheck {

struct Result
{
    bool        ok = false;
    std::string reason;          // human-readable, first failure only
    int         slices = 0;      // slices checked (1 for thin)
    int         imports = 0;     // distinct import names seen across slices
};

/// Check a complete Mach-O image (fat or thin). `data` may be any bytes;
/// never reads outside [data, data + size).
Result checkImage(const uint8_t* data, size_t size);

/// The import allowlist, exposed for tests and the SDK validator parity
/// check. `name` is the Mach-O symbol (with its leading underscore, e.g.
/// "_tanhf", "__ZNSt3__1...", "dyld_stub_binder").
bool isImportAllowed(const std::string& name);

/// The dylib allowlist (exact match).
bool isDylibAllowed(const std::string& installName);

} // namespace machocheck
} // namespace openmusic
