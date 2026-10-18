#include "manifest.hpp"
#include "util.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>

namespace rp {

// ── RatMode's lint rules ─────────────────────────────────────────────────────

bool isValidExtensionId(const std::string& id)
{
    // ^[a-z0-9]+(\.[a-z0-9_]+)+$ , at least one dot, lowercase only (first segment: [a-z0-9] only).
    if (id.empty() || id.size() > 128) return false;
    int segments = 0, segLen = 0;
    for (char c : id)
    {
        if (c == '.')
        {
            if (segLen == 0) return false;
            ++segments; segLen = 0; continue;
        }
        const bool lower = (c >= 'a' && c <= 'z');
        const bool digit = (c >= '0' && c <= '9');
        const bool under = (c == '_' && segments > 0);
        if (! (lower || digit || under)) return false;
        ++segLen;
    }
    return segments >= 1 && segLen > 0;
}

bool isReservedMacroPrefix(const std::string& p)
{
    static const char* const kReserved[] = {
        "fx", "sculpt", "synth", "gain", "sample", "dry", "ra", "rb",
        "an", "sv", "sn", "sub0", "sub1", "sub2", "sub3", "au_param", "__"
    };
    for (const char* r : kReserved) if (p == r) return true;
    return p.compare(0, 2, "__") == 0 || p.compare(0, 8, "au_param") == 0;
}

bool isValidMacroPrefix(const std::string& p)
{
    if (p.size() < 2 || p.size() > 6) return false;
    for (char c : p) if (! (c >= 'a' && c <= 'z')) return false;
    return ! isReservedMacroPrefix(p);
}

static bool isLowerDotted(const std::string& s)
{
    for (char c : s)
        if (! ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
    return ! s.empty() && s.back() != '.' && s.find("..") == std::string::npos;
}

const char* partKindName(int k)
{
    switch (k) { case RATPIPE_DRUM: return "drum"; case RATPIPE_MONO: return "mono"; case RATPIPE_POLY: return "poly"; }
    return "?";
}

static bool looksSemver(const std::string& v)
{
    int dots = 0; size_t i = 0;
    if (v.empty()) return false;
    for (; i < v.size() && (std::isdigit((unsigned char) v[i]) || v[i] == '.'); ++i) if (v[i] == '.') ++dots;
    return dots == 2 && i >= 5 && v.front() != '.' && v.find("..") == std::string::npos
        && (i == v.size() || v[i] == '-' || v[i] == '+');
}

// ── V1-V3 ────────────────────────────────────────────────────────────────────

bool lintDescriptor(const RatPipeDescriptor* d, std::vector<Finding>& out)
{
    bool safe = true;
    auto add = [&](const char* check, const std::string& m, bool warn = false) { out.push_back({ check, m, warn }); };
    auto unsafe = [&](const char* check, const std::string& m) { add(check, m); safe = false; };

    if (d == nullptr) { unsafe("V1", "descriptor is NULL"); return false; }
    if (d->struct_size < sizeof(RatPipeDescriptor))
    {
        unsafe("V1", fmt("descriptor struct_size %u < %zu the host was built with (built against an older ABI?)",
                         d->struct_size, sizeof(RatPipeDescriptor)));
        return false;   // nothing past struct_size may be read
    }
    if (d->abi_major != RATPIPE_ABI_MAJOR)
    {
        unsafe("V1", fmt("abi_major %u != host %u", d->abi_major, RATPIPE_ABI_MAJOR));
        return false;
    }
    if (d->create == nullptr || d->destroy == nullptr || d->prepare == nullptr
        || d->render == nullptr || d->set_param == nullptr || d->get_param == nullptr)
        unsafe("V1", "a required function pointer (create/destroy/prepare/render/set_param/get_param) is NULL");
    if (d->theme != nullptr) add("V1", "theme must be NULL in ABI 1.0");
    for (void* r : d->reserved) if (r != nullptr) { add("V1", "reserved descriptor slots must be NULL"); break; }
    if (d->param_count > 256) { unsafe("V1", fmt("param_count %u > 256", d->param_count)); return false; }
    if (d->param_count > 0 && d->params == nullptr) { unsafe("V1", "params is NULL with param_count > 0"); return false; }

    // Array-element struct: every element's struct_size must equal sizeof exactly,
    // checked with plain integer reads BEFORE any string pointer is followed.
    bool strideOk = true;
    for (uint32_t i = 0; i < d->param_count; ++i)
        if (d->params[i].struct_size != (uint32_t) sizeof(RatPipeParamDesc))
        {
            add("V1", fmt("param %u: struct_size %u != host sizeof(RatPipeParamDesc) %zu (array-element structs must match exactly)",
                          i, d->params[i].struct_size, sizeof(RatPipeParamDesc)));
            strideOk = false; safe = false;
            break;
        }
    if (! strideOk) return false;

    // ── V2 identity ──
    const std::string id = d->id ? d->id : "";
    if (d->id == nullptr || ! isValidExtensionId(id))
        add("V2", "id '" + id + "' is not lowercase reverse-DNS (^[a-z0-9]+(\\.[a-z0-9_]+)+$)");
    if (d->display_name == nullptr || *d->display_name == 0) add("V2", "display_name is empty");
    else if (std::strlen(d->display_name) > 24) add("V2", fmt("display_name is %zu characters (max 24)", std::strlen(d->display_name)));
    if (d->author == nullptr || d->version == nullptr || d->sdk_version == nullptr)
        add("V2", "author / version / sdk_version must be non-NULL strings");
    else
    {
        if (*d->author == 0) add("V2", "author is empty", true);
        if (! looksSemver(d->version)) add("V2", std::string("version '") + d->version + "' is not semver (x.y.z)", true);
    }
    if (d->part_kind != RATPIPE_DRUM && d->part_kind != RATPIPE_MONO && d->part_kind != RATPIPE_POLY)
        add("V2", fmt("part_kind %d out of range", d->part_kind));
    const std::string prefix = d->macro_prefix ? d->macro_prefix : "";
    if (! isValidMacroPrefix(prefix))
        add("V2", "macro_prefix '" + prefix + "' must be 2-6 lowercase letters and not reserved (fx sculpt synth gain sample dry ra rb an sv sn sub0-3 au_param __)");

    std::set<std::string> seen;
    for (uint32_t i = 0; i < d->param_count; ++i)
    {
        const RatPipeParamDesc& p = d->params[i];
        const std::string w = fmt("param %u: ", i);
        for (uint32_t r : p.reserved) if (r != 0) { add("V1", w + "reserved[] must be zero in ABI 1.0"); break; }
        if (p.id == nullptr || p.label == nullptr || p.group == nullptr)
        {
            add("V2", w + "id / label / group must be non-NULL");
            safe = false;
            continue;
        }
        const std::string pid = p.id;
        if (pid.compare(0, prefix.size() + 1, prefix + ".") != 0 || pid.size() <= prefix.size() + 1)
            add("V2", w + "id '" + pid + "' must be '" + prefix + ".<path>'");
        else if (! isLowerDotted(pid))
            add("V2", w + "id '" + pid + "' must use lowercase letters, digits, '_' and '.' only");
        if (! seen.insert(pid).second) add("V2", w + "duplicate id '" + pid + "'");
        const size_t ll = std::strlen(p.label);
        if (ll > 8) add("V2", w + fmt("label '%s' is %zu characters (max 8)", p.label, ll));
        if (ll == 0) add("V2", w + "label is empty", true);

        // ── V3 bounds ──
        const bool gain = (p.flags & RATPIPE_PARAM_GAIN_STYLE) != 0;
        const float hi = gain ? 1.5f : 1.0f;
        if (! std::isfinite(p.min_value) || ! std::isfinite(p.max_value) || ! std::isfinite(p.default_value))
        { add("V3", w + "'" + pid + "' has non-finite bounds/default"); continue; }
        if (! (p.min_value < p.max_value)) add("V3", w + "'" + pid + "' min must be < max");
        if (p.min_value < -1e-6f || p.max_value > hi + 1e-6f)
            add("V3", w + "'" + pid + "' " + fmt("range [%g, %g] must lie within 0..%.1f", p.min_value, p.max_value, hi));
        if (p.default_value < p.min_value || p.default_value > p.max_value)
            add("V3", w + "'" + pid + "' " + fmt("default %g outside [%g, %g]", p.default_value, p.min_value, p.max_value));
        if (p.mutate_lo != p.mutate_hi)
        {
            if (! std::isfinite(p.mutate_lo) || ! std::isfinite(p.mutate_hi) || p.mutate_lo > p.mutate_hi
                || p.mutate_lo < p.min_value || p.mutate_hi > p.max_value)
                add("V3", w + "'" + pid + "' " + fmt("mutate band [%g, %g] not inside [%g, %g]", p.mutate_lo, p.mutate_hi, p.min_value, p.max_value));
        }
        if ((p.flags & RATPIPE_PARAM_DISCRETE) != 0 && (p.steps < 2 || p.steps > 64))
            add("V3", w + "'" + pid + "' " + fmt("DISCRETE needs 2 <= steps <= 64 (got %d)", p.steps));
        if ((p.flags & RATPIPE_PARAM_DISCRETE) == 0 && p.steps != 0)
            add("V3", w + "'" + pid + "' steps must be 0 unless DISCRETE");
        if (p.choices != nullptr && (p.flags & RATPIPE_PARAM_DISCRETE) == 0)
            add("V3", w + "'" + pid + "' choices only allowed on DISCRETE params");
        if (p.choices != nullptr && (p.flags & RATPIPE_PARAM_DISCRETE) != 0 && p.steps >= 2 && p.steps <= 64)
            for (int k = 0; k < p.steps; ++k)
                if (p.choices[k] == nullptr) { add("V3", w + "'" + pid + "' " + fmt("choices[%d] is NULL", k)); break; }
        if (p.readout_kind < RATPIPE_READOUT_NONE || p.readout_kind > RATPIPE_READOUT_ST)
            add("V3", w + "'" + pid + "' readout_kind out of range");
    }
    for (int k = 0; k < RATPIPE_PERFORM_KNOBS; ++k)
    {
        const int32_t idx = d->perform_knobs[k];
        if (idx != RATPIPE_PERFORM_KNOB_NONE && (idx < 0 || (uint32_t) idx >= d->param_count))
            add("V3", fmt("perform_knobs[%d] = %d is not a valid parameter index (0..%u) or -1", k, idx, d->param_count ? d->param_count - 1 : 0));
    }
    return safe;
}

// ── generation ───────────────────────────────────────────────────────────────

static std::string flagsJson(uint32_t f)
{
    std::string s = "[";
    bool first = true;
    auto add = [&](const char* n) { if (! first) s += ", "; s += std::string("\"") + n + "\""; first = false; };
    if (f & RATPIPE_PARAM_DISCRETE)    add("discrete");
    if (f & RATPIPE_PARAM_BIPOLAR)     add("bipolar");
    if (f & RATPIPE_PARAM_GAIN_STYLE)  add("gain_style");
    if (f & RATPIPE_PARAM_NOT_MUTABLE) add("not_mutable");
    return s + "]";
}

static const char* readoutName(int k)
{
    switch (k) { case RATPIPE_READOUT_HZ: return "hz"; case RATPIPE_READOUT_MS: return "ms"; case RATPIPE_READOUT_PCT: return "pct";
                 case RATPIPE_READOUT_ST: return "st"; default: return "none"; }
}

std::string buildManifest(const RatPipeDescriptor* d, const std::string& minVer)
{
    std::string m = "{\n";
    auto kv = [&](const char* k, const std::string& v, bool comma = true) { m += std::string("  \"") + k + "\": " + v + (comma ? ",\n" : "\n"); };
    kv("id", jsonEscape(d->id));
    kv("name", jsonEscape(d->display_name));
    kv("author", jsonEscape(d->author));
    kv("version", jsonEscape(d->version));
    kv("sdk_version", jsonEscape(d->sdk_version));
    kv("part_kind", jsonEscape(partKindName(d->part_kind)));
    kv("macro_prefix", jsonEscape(d->macro_prefix));
    kv("abi_major", std::to_string(d->abi_major));
    kv("abi_minor", std::to_string(d->abi_minor));
    kv("min_ratmode_version", jsonEscape(minVer));
    std::string fl = "[";
    bool first = true;
    auto addf = [&](const char* n) { if (! first) fl += ", "; fl += std::string("\"") + n + "\""; first = false; };
    if (d->flags & RATPIPE_FLAG_WANTS_TRANSPORT)         addf("wants_transport");
    if (d->flags & RATPIPE_FLAG_HAS_TAILS)               addf("has_tails");
    if (d->flags & RATPIPE_FLAG_DETERMINISTIC_WITH_SEED) addf("deterministic_with_seed");
    kv("flags", fl + "]");
    std::string pk = "[";
    for (int i = 0; i < RATPIPE_PERFORM_KNOBS; ++i) pk += (i ? ", " : "") + std::to_string(d->perform_knobs[i]);
    kv("perform_knobs", pk + "]");

    m += "  \"params\": [\n";
    for (uint32_t i = 0; i < d->param_count; ++i)
    {
        const RatPipeParamDesc& p = d->params[i];
        m += "    {\"id\": " + jsonEscape(p.id) + ", \"label\": " + jsonEscape(p.label) + ", \"group\": " + jsonEscape(p.group)
           + ", \"default\": " + jsonNum(p.default_value) + ", \"min\": " + jsonNum(p.min_value) + ", \"max\": " + jsonNum(p.max_value)
           + ", \"mutate_lo\": " + jsonNum(p.mutate_lo) + ", \"mutate_hi\": " + jsonNum(p.mutate_hi)
           + ", \"flags\": " + flagsJson(p.flags) + ", \"steps\": " + std::to_string(p.steps)
           + ", \"readout\": " + jsonEscape(readoutName(p.readout_kind))
           + ", \"readout_min\": " + jsonNum(p.readout_min) + ", \"readout_max\": " + jsonNum(p.readout_max);
        if (p.choices != nullptr && (p.flags & RATPIPE_PARAM_DISCRETE) && p.steps >= 2 && p.steps <= 64)
        {
            m += ", \"choices\": [";
            for (int k = 0; k < p.steps; ++k) m += (k ? ", " : "") + jsonEscape(p.choices[k] ? p.choices[k] : "");
            m += "]";
        }
        m += "}";
        m += (i + 1 < d->param_count) ? ",\n" : "\n";
    }
    m += "  ]\n}\n";
    return m;
}

static std::string xmlEscape(const std::string& s)
{
    std::string r;
    for (char c : s)
        switch (c) { case '&': r += "&amp;"; break; case '<': r += "&lt;"; break; case '>': r += "&gt;"; break; default: r += c; }
    return r;
}

std::string buildPlist(const RatPipeDescriptor* d, const std::string& exe)
{
    const std::string id = xmlEscape(d->id);
    const std::string ver = xmlEscape(d->version);
    std::string p;
    p += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    p += "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    p += "<plist version=\"1.0\">\n<dict>\n";
    auto kv = [&](const char* k, const std::string& v) { p += std::string("\t<key>") + k + "</key>\n\t<string>" + v + "</string>\n"; };
    kv("CFBundleDevelopmentRegion", "en");
    kv("CFBundleExecutable", xmlEscape(exe));
    kv("CFBundleIdentifier", id);
    kv("CFBundleInfoDictionaryVersion", "6.0");
    kv("CFBundleName", xmlEscape(d->display_name));
    kv("CFBundlePackageType", "BNDL");
    kv("CFBundleShortVersionString", ver);
    kv("CFBundleVersion", ver);
    kv("LSMinimumSystemVersion", "11.0");
    p += "</dict>\n</plist>\n";
    return p;
}

// ── manifest rules (what RatMode's parseManifest enforces) ──────────────────

std::vector<std::string> lintManifest(const Json& m)
{
    std::vector<std::string> v;
    if (m.t != Json::Obj) { v.push_back("manifest.json is not a JSON object"); return v; }
    const std::string id = m.str("id");
    if (! isValidExtensionId(id)) v.push_back("manifest id '" + id + "' is not lowercase reverse-DNS");
    const std::string name = m.str("name");
    if (name.empty() || name.size() > 24) v.push_back("manifest name missing or longer than 24 characters");
    if (! isValidMacroPrefix(m.str("macro_prefix"))) v.push_back("manifest macro_prefix '" + m.str("macro_prefix") + "' invalid or reserved");
    std::string kind = m.str("part_kind");
    std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    if (kind != "drum" && kind != "mono" && kind != "poly" && kind != "0" && kind != "1" && kind != "2")
        v.push_back("manifest part_kind must be drum, mono or poly");
    if ((int) m.num("abi_major", 0) != (int) RATPIPE_ABI_MAJOR)
        v.push_back(fmt("manifest abi_major %d != %u", (int) m.num("abi_major", 0), RATPIPE_ABI_MAJOR));
    if (const Json* pk = m.get("perform_knobs"))
        if (pk->t != Json::Arr) v.push_back("manifest perform_knobs must be an array");
    return v;
}

std::vector<std::string> diffManifest(const Json& m, const RatPipeDescriptor* d)
{
    std::vector<std::string> v;
    auto cmp = [&](const char* key, const std::string& want)
    {
        if (m.str(key) != want) v.push_back(std::string("manifest ") + key + " '" + m.str(key) + "' != descriptor '" + want + "'");
    };
    cmp("id", d->id);
    cmp("name", d->display_name);
    cmp("author", d->author);
    cmp("version", d->version);
    cmp("macro_prefix", d->macro_prefix);
    std::string kind = m.str("part_kind");
    if (kind != partKindName(d->part_kind) && kind != std::to_string(d->part_kind))
        v.push_back("manifest part_kind '" + kind + "' != descriptor '" + partKindName(d->part_kind) + "'");
    if ((int) m.num("abi_major", -1) != (int) d->abi_major) v.push_back("manifest abi_major differs from descriptor");
    if (const Json* pk = m.get("perform_knobs"))
    {
        for (int i = 0; i < RATPIPE_PERFORM_KNOBS; ++i)
        {
            const int want = d->perform_knobs[i];
            const int got = (pk->t == Json::Arr && i < (int) pk->a.size() && pk->a[(size_t) i].t == Json::Num) ? (int) pk->a[(size_t) i].n : -1;
            if (got != want) { v.push_back(fmt("manifest perform_knobs[%d] = %d != descriptor %d", i, got, want)); break; }
        }
    }
    if (const Json* ps = m.get("params"))
    {
        if (ps->t != Json::Arr || ps->a.size() != d->param_count)
            v.push_back(fmt("manifest lists %zu params, descriptor has %u", ps->t == Json::Arr ? ps->a.size() : 0, d->param_count));
        else
            for (uint32_t i = 0; i < d->param_count; ++i)
            {
                const Json& pj = ps->a[i];
                if (pj.str("id") != d->params[i].id) { v.push_back(fmt("manifest params[%u].id '%s' != descriptor '%s'", i, pj.str("id").c_str(), d->params[i].id)); break; }
                if (std::fabs(pj.num("default") - d->params[i].default_value) > 1e-6) { v.push_back(fmt("manifest params[%u] (%s) default differs", i, d->params[i].id)); break; }
            }
    }
    return v;
}

} // namespace rp
