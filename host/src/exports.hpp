// Which symbols does a pipe library export? (V1: exactly `ratpipe_entry`.)
#pragma once
#include <string>
#include <vector>

namespace rp {

// Lists defined, externally visible symbols. ELF is parsed natively; Mach-O
// (macOS) goes through `nm -gU`. Returns false (with `err`) when it cannot tell.
bool listExportedSymbols(const std::string& binary, std::vector<std::string>& names, std::string& err);

// True when `name` is toolchain noise rather than a real export.
bool isToolchainSymbol(const std::string& name);

} // namespace rp
