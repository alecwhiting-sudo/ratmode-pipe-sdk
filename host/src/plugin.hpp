// Loading a pipe (bundle or raw library) and driving instances of it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <ratpipe/ratpipe_abi.h>

namespace rp {

constexpr const char* kHostName    = "ratpipe-host";
constexpr const char* kHostVersion = "0.1.0";

struct Plugin
{
    std::string path;         // as given on the command line
    std::string binary;       // resolved library file
    std::string bundleDir;    // empty when `path` was a raw library
    void*       handle = nullptr;
    RatPipeEntryFn entry = nullptr;
    const RatPipeDescriptor* desc = nullptr;
    double      entrySeconds = 0.0;     // best of two ratpipe_entry calls (M15)
    bool        entryThrew = false;
    std::string error;                  // set when loading failed
    // Set by the loader step that failed, for V1 reporting.
    enum Stage { Ok, Resolve, Dlopen, Dlsym, EntryNull, EntryThrew } stage = Ok;
};

RatPipeHostInfo makeHostInfo(double sampleRate, int maxBlock, uint64_t seed);

// Resolves a bundle directory (Contents/MacOS/<id>.dylib|.so, using the
// manifest's id/binary as RatMode does) or accepts a raw library path.
bool resolveBinary(const std::string& path, std::string& binary, std::string& bundleDir, std::string& err);

// dlopen + dlsym + ratpipe_entry. Never dlcloses (the host never unloads, like RatMode).
bool loadPlugin(const std::string& path, double sampleRate, int maxBlock, uint64_t seed, Plugin& out);

// One live instance of a pipe. Every call into the pipe is wrapped in
// try/catch(...): an escaping exception is recorded in `threw`.
class Instance
{
public:
    Instance(const Plugin& p, double sampleRate, int maxBlock, uint64_t seed);
    ~Instance();
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    bool ok() const { return inst_ != nullptr; }
    void setParam(int index, float value);
    float getParam(int index);
    void prepare(double sampleRate, int maxBlock);
    // Plain call; the caller handles guard / timing around it.
    void render(float* l, float* r, int n, const RatPipeEvent* ev, int evCount, const RatPipeTransport* tr);

    bool threw = false;
    const RatPipeDescriptor* desc() const { return d_; }

private:
    const RatPipeDescriptor* d_;
    void* inst_ = nullptr;
};

} // namespace rp
