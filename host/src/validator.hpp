// The validator suite V1-V13 (Arch38 §7.4). Every check maps to a MUST in
// docs/CONTRACT.md.
#pragma once
#include <cstdint>
#include <chrono>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace rp {

struct CheckResult
{
    enum Status { Pass, Warn, Fail, Skip };
    std::string id;        // "V1" ...
    std::string name;
    Status      status = Skip;
    std::vector<std::string> messages;      // failure / warning / info lines
    std::string summary;                    // one-line "what was exercised" on pass

    void fail(const std::string& m) { status = Fail; messages.push_back(m); }
    void warn(const std::string& m) { if (status != Fail) status = Warn; messages.push_back("warning: " + m); }
    void info(const std::string& m) { messages.push_back(m); }
    void skip(const std::string& why) { status = Skip; messages.push_back(why); }
};

struct ValidateOptions
{
    bool     quick = false;                 // 20 fuzz seeds, one sample rate in the grid
    int      seeds = 200;                   // fuzz seeds in V4
    std::vector<std::string> srcDirs;       // sources for V13
    std::set<std::string>    only;          // e.g. {"V4","V9"}; empty = all
    bool     allowSingleArch = false;       // V12: do not require x86_64 + arm64
    double   timingSlack = 1.0;             // multiplies the V7 budget (slow CI machines)
    uint64_t hostSeed = 0x52617450697065ull;
    std::function<void(const std::string&)> progress;   // "V4 render fuzz ..." (stderr)
};

struct ValidationReport
{
    std::string path, binary, id, name, kind;
    std::vector<CheckResult> checks;
    bool failed() const;
    bool hasWarnings() const;
};

ValidationReport validatePipe(const std::string& path, const ValidateOptions& opts);

std::string reportToText(const ValidationReport& r, bool color);
std::string reportToJson(const ValidationReport& r);

} // namespace rp
