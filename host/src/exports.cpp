#include "exports.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace rp {

bool isToolchainSymbol(const std::string& n)
{
    static const char* const kNoise[] = { "_init", "_fini", "_edata", "_end", "__bss_start", "__mh_dylib_header", "__mh_execute_header" };
    for (const char* k : kNoise) if (n == k) return true;
    return false;
}

namespace {

struct Elf64Ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
                   uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Elf64Shdr { uint32_t name, type; uint64_t flags, addr, offset, size; uint32_t link, info; uint64_t addralign, entsize; };
struct Elf64Sym  { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; };

bool listElf(const std::vector<char>& f, std::vector<std::string>& names, std::string& err)
{
    if (f.size() < sizeof(Elf64Ehdr)) { err = "file too small"; return false; }
    Elf64Ehdr eh; std::memcpy(&eh, f.data(), sizeof eh);
    if (eh.ident[4] != 2 || eh.ident[5] != 1) { err = "only 64-bit little-endian ELF is supported"; return false; }
    if (eh.shentsize != sizeof(Elf64Shdr) || eh.shoff == 0 || eh.shoff + (uint64_t) eh.shnum * sizeof(Elf64Shdr) > f.size())
    { err = "section headers unavailable"; return false; }
    for (unsigned i = 0; i < eh.shnum; ++i)
    {
        Elf64Shdr sh; std::memcpy(&sh, f.data() + eh.shoff + (uint64_t) i * sizeof sh, sizeof sh);
        if (sh.type != 11 /*SHT_DYNSYM*/) continue;
        if (sh.link >= eh.shnum) { err = "bad dynsym link"; return false; }
        Elf64Shdr str; std::memcpy(&str, f.data() + eh.shoff + (uint64_t) sh.link * sizeof str, sizeof str);
        if (sh.offset + sh.size > f.size() || str.offset + str.size > f.size()) { err = "dynsym out of range"; return false; }
        const size_t count = (size_t) (sh.size / sizeof(Elf64Sym));
        for (size_t k = 1; k < count; ++k)
        {
            Elf64Sym s; std::memcpy(&s, f.data() + sh.offset + k * sizeof s, sizeof s);
            if (s.shndx == 0) continue;                      // undefined (an import)
            const unsigned bind = s.info >> 4;               // 1 GLOBAL, 2 WEAK
            if (bind != 1 && bind != 2) continue;
            if ((s.other & 3) != 0) continue;                // hidden / internal / protected-less
            if (s.name >= str.size) continue;
            const char* nm = f.data() + str.offset + s.name;
            const size_t maxLen = (size_t) (str.size - s.name);
            names.emplace_back(nm, strnlen(nm, maxLen));
        }
        return true;
    }
    err = "no dynamic symbol table";
    return false;
}

} // namespace

bool listExportedSymbols(const std::string& binary, std::vector<std::string>& names, std::string& err)
{
    names.clear();
    std::ifstream in(binary, std::ios::binary);
    if (! in) { err = "cannot read " + binary; return false; }
    std::vector<char> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (f.size() >= 4 && std::memcmp(f.data(), "\x7f" "ELF", 4) == 0)
        return listElf(f, names, err);

   #if defined(__APPLE__)
    // Mach-O / fat: ask the toolchain (Command Line Tools always ship nm).
    std::string cmd = "nm -gU '" + binary + "' 2>/dev/null";
    std::FILE* p = popen(cmd.c_str(), "r");
    if (p == nullptr) { err = "cannot run nm"; return false; }
    char line[1024];
    bool any = false;
    while (std::fgets(line, sizeof line, p))
    {
        // "<hex> T _name"   (fat files also print "<path> (for architecture x):" headers)
        char type = 0; unsigned long long addr = 0; char nm[900];
        if (std::sscanf(line, "%llx %c %899s", &addr, &type, nm) == 3) { any = true; names.emplace_back(nm); }
    }
    const int rc = pclose(p);
    if (rc != 0 && ! any) { err = "nm failed"; return false; }
    return true;
   #else
    err = "unrecognised binary format (not ELF)";
    return false;
   #endif
}

} // namespace rp
