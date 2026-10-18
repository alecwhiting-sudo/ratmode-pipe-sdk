#include "plugin.hpp"
#include "json.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace rp {

namespace {
bool isDir(const std::string& p)  { struct stat st; return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
bool isFile(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
bool endsWith(const std::string& s, const std::string& suf)
{
    return s.size() > suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}
bool slurp(const std::string& p, std::string& out)
{
    std::ifstream f(p, std::ios::binary);
    if (! f) return false;
    std::ostringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}
} // namespace

RatPipeHostInfo makeHostInfo(double sr, int maxBlock, uint64_t seed)
{
    RatPipeHostInfo h;
    std::memset(&h, 0, sizeof h);
    h.struct_size  = (uint32_t) sizeof(RatPipeHostInfo);
    h.abi_major    = RATPIPE_ABI_MAJOR;
    h.abi_minor    = RATPIPE_ABI_MINOR;
    h.host_name    = kHostName;
    h.host_version = kHostVersion;
    h.sample_rate  = sr;
    h.max_block    = maxBlock;
    h.seed         = seed;
    return h;
}

bool resolveBinary(const std::string& pathIn, std::string& binary, std::string& bundleDir, std::string& err)
{
    std::string path = pathIn;
    while (path.size() > 1 && path.back() == '/') path.pop_back();

    if (! isDir(path))
    {
        if (! isFile(path)) { err = "no such file or bundle: " + pathIn; return false; }
        binary = path; bundleDir.clear();
        return true;
    }

    bundleDir = path;
    const std::string macos = path + "/Contents/MacOS";
    if (! isDir(macos)) { err = "not a .ratpipe bundle (no Contents/MacOS): " + pathIn; return false; }

    // Same rule as RatMode's ExtensionRegistry::parseManifest: explicit
    // "binary" (plain file name) wins, else <id>.dylib, else <id>.so.
    std::string id, explicitBinary, text, perr;
    if (slurp(path + "/Contents/Resources/manifest.json", text))
    {
        Json j;
        if (parseJson(text, j, perr)) { id = j.str("id"); explicitBinary = j.str("binary"); }
    }
    if (! explicitBinary.empty() && explicitBinary.find_first_of("/\\:") == std::string::npos && explicitBinary[0] != '.')
    {
        binary = macos + "/" + explicitBinary;
        if (isFile(binary)) return true;
    }
    if (! id.empty())
    {
        for (const char* ext : { ".dylib", ".so" })
            if (isFile(macos + "/" + id + ext)) { binary = macos + "/" + id + ext; return true; }
    }
    // No (usable) manifest: accept the single library in Contents/MacOS.
    std::vector<std::string> libs;
    if (DIR* d = ::opendir(macos.c_str()))
    {
        while (dirent* e = ::readdir(d))
        {
            const std::string n = e->d_name;
            if (endsWith(n, ".dylib") || endsWith(n, ".so")) libs.push_back(n);
        }
        ::closedir(d);
    }
    if (libs.size() == 1) { binary = macos + "/" + libs[0]; return true; }
    err = libs.empty() ? "no .dylib/.so in " + macos : "several libraries in " + macos + " and no manifest id to pick one";
    return false;
}

bool loadPlugin(const std::string& path, double sr, int maxBlock, uint64_t seed, Plugin& out)
{
    out = Plugin{};
    out.path = path;
    if (! resolveBinary(path, out.binary, out.bundleDir, out.error)) { out.stage = Plugin::Resolve; return false; }
    // dlopen() only treats names containing '/' as paths; make it absolute.
    if (char* rp = ::realpath(out.binary.c_str(), nullptr)) { out.binary = rp; std::free(rp); }

    out.handle = ::dlopen(out.binary.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (out.handle == nullptr)
    {
        const char* e = ::dlerror();
        out.error = std::string("dlopen failed: ") + (e ? e : "unknown error");
        out.stage = Plugin::Dlopen;
        return false;
    }
    out.entry = (RatPipeEntryFn) ::dlsym(out.handle, RATPIPE_ENTRY_SYMBOL);
    if (out.entry == nullptr)
    {
        out.error = std::string("symbol '") + RATPIPE_ENTRY_SYMBOL + "' not exported";
        out.stage = Plugin::Dlsym;
        return false;
    }

    const RatPipeHostInfo host = makeHostInfo(sr, maxBlock, seed);
    double best = 1e9;
    for (int i = 0; i < 2; ++i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        try { out.desc = out.entry(&host); }
        catch (...) { out.entryThrew = true; out.error = "ratpipe_entry threw a C++ exception"; out.stage = Plugin::EntryThrew; return false; }
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (dt < best) best = dt;
    }
    out.entrySeconds = best;
    if (out.desc == nullptr) { out.error = "ratpipe_entry returned NULL (bundle refused this host)"; out.stage = Plugin::EntryNull; return false; }
    return true;
}

// ── Instance ────────────────────────────────────────────────────────────────

Instance::Instance(const Plugin& p, double sr, int maxBlock, uint64_t seed) : d_(p.desc)
{
    const RatPipeHostInfo host = makeHostInfo(sr, maxBlock, seed);
    try { inst_ = d_->create(sr, maxBlock, &host); }
    catch (...) { threw = true; inst_ = nullptr; }
}

Instance::~Instance()
{
    if (inst_ == nullptr) return;
    try { d_->destroy(inst_); } catch (...) { threw = true; }
    inst_ = nullptr;
}

void Instance::setParam(int i, float v)
{
    try { d_->set_param(inst_, i, v); } catch (...) { threw = true; }
}

float Instance::getParam(int i)
{
    try { return d_->get_param(inst_, i); } catch (...) { threw = true; return 0.0f; }
}

void Instance::prepare(double sr, int maxBlock)
{
    try { d_->prepare(inst_, sr, maxBlock); } catch (...) { threw = true; }
}

void Instance::render(float* l, float* r, int n, const RatPipeEvent* ev, int evCount, const RatPipeTransport* tr)
{
    try { d_->render(inst_, l, r, n, ev, evCount, tr); } catch (...) { threw = true; }
}

} // namespace rp
