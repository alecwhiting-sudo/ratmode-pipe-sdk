// Descriptor lint (V1-V3), manifest.json / Info.plist generation, and the
// manifest rules RatMode's ExtensionRegistry::parseManifest applies.
#pragma once
#include "json.hpp"
#include <string>
#include <vector>
#include <ratpipe/ratpipe_abi.h>

namespace rp {

struct Finding
{
    std::string check;     // "V1", "V2", "V3"
    std::string message;
    bool        warning = false;
};

// Same rules as RatMode (Source/Ext/ExtensionRegistry.cpp).
bool isValidExtensionId(const std::string& id);
bool isValidMacroPrefix(const std::string& prefix);
bool isReservedMacroPrefix(const std::string& prefix);

// Runs V1-V3 on the descriptor. Returns false when the descriptor is not safe
// to use at all (wrong struct sizes, missing function pointers ...): the caller
// must not run the pipe in that case.
bool lintDescriptor(const RatPipeDescriptor* d, std::vector<Finding>& out);

const char* partKindName(int kind);

// manifest.json generated from the descriptor (what RatMode's parser reads,
// plus the full parameter table for tools).
std::string buildManifest(const RatPipeDescriptor* d, const std::string& minRatModeVersion);
std::string buildPlist(const RatPipeDescriptor* d, const std::string& executableFileName);

// RatMode's manifest rules on a parsed manifest (V12). Returns problems.
std::vector<std::string> lintManifest(const Json& m);

// Structural comparison manifest <-> descriptor (V8). Returns differences.
std::vector<std::string> diffManifest(const Json& m, const RatPipeDescriptor* d);

} // namespace rp
