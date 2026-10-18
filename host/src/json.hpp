// Minimal JSON value + parser + writer helpers (no dependencies).
// Enough for manifest.json: objects, arrays, strings, numbers, bools, null.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace rp {

struct Json
{
    enum Type { Null, Bool, Num, Str, Arr, Obj };
    Type t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json* get(const std::string& k) const
    {
        if (t != Obj) return nullptr;
        for (const auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    std::string str(const std::string& k, const std::string& def = "") const
    {
        const Json* j = get(k);
        return (j != nullptr && j->t == Str) ? j->s : def;
    }
    double num(const std::string& k, double def = 0) const
    {
        const Json* j = get(k);
        return (j != nullptr && j->t == Num) ? j->n : def;
    }
};

namespace detail {
struct Parser
{
    const std::string& src;
    size_t i = 0;
    std::string err;
    explicit Parser(const std::string& s) : src(s) {}

    void ws() { while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r')) ++i; }
    bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at offset " + std::to_string(i); return false; }

    bool str(std::string& out)
    {
        if (i >= src.size() || src[i] != '"') return fail("expected string");
        ++i; out.clear();
        while (i < src.size() && src[i] != '"')
        {
            char c = src[i++];
            if (c == '\\')
            {
                if (i >= src.size()) return fail("bad escape");
                char e = src[i++];
                switch (e)
                {
                    case '"': out += '"'; break;   case '\\': out += '\\'; break;  case '/': out += '/'; break;
                    case 'b': out += '\b'; break;  case 'f': out += '\f'; break;   case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;  case 't': out += '\t'; break;
                    case 'u':
                    {
                        if (i + 4 > src.size()) return fail("bad \\u escape");
                        unsigned cp = (unsigned) std::strtoul(src.substr(i, 4).c_str(), nullptr, 16);
                        i += 4;
                        if (cp < 0x80) out += (char) cp;
                        else if (cp < 0x800) { out += (char) (0xC0 | (cp >> 6)); out += (char) (0x80 | (cp & 0x3F)); }
                        else { out += (char) (0xE0 | (cp >> 12)); out += (char) (0x80 | ((cp >> 6) & 0x3F)); out += (char) (0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: return fail("bad escape");
                }
            }
            else out += c;
        }
        if (i >= src.size()) return fail("unterminated string");
        ++i;
        return true;
    }

    bool value(Json& v, int depth)
    {
        if (depth > 64) return fail("nesting too deep");
        ws();
        if (i >= src.size()) return fail("unexpected end");
        char c = src[i];
        if (c == '{')
        {
            v.t = Json::Obj; ++i; ws();
            if (i < src.size() && src[i] == '}') { ++i; return true; }
            for (;;)
            {
                ws(); std::string k;
                if (! str(k)) return false;
                ws();
                if (i >= src.size() || src[i] != ':') return fail("expected ':'");
                ++i;
                Json child;
                if (! value(child, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(child));
                ws();
                if (i < src.size() && src[i] == ',') { ++i; continue; }
                if (i < src.size() && src[i] == '}') { ++i; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[')
        {
            v.t = Json::Arr; ++i; ws();
            if (i < src.size() && src[i] == ']') { ++i; return true; }
            for (;;)
            {
                Json child;
                if (! value(child, depth + 1)) return false;
                v.a.push_back(std::move(child));
                ws();
                if (i < src.size() && src[i] == ',') { ++i; continue; }
                if (i < src.size() && src[i] == ']') { ++i; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.t = Json::Str; return str(v.s); }
        if (src.compare(i, 4, "true") == 0)  { v.t = Json::Bool; v.b = true;  i += 4; return true; }
        if (src.compare(i, 5, "false") == 0) { v.t = Json::Bool; v.b = false; i += 5; return true; }
        if (src.compare(i, 4, "null") == 0)  { v.t = Json::Null; i += 4; return true; }
        if (c == '-' || (c >= '0' && c <= '9'))
        {
            char* end = nullptr;
            v.n = std::strtod(src.c_str() + i, &end);
            if (end == src.c_str() + i) return fail("bad number");
            v.t = Json::Num;
            i = (size_t) (end - src.c_str());
            return true;
        }
        return fail("unexpected character");
    }
};
} // namespace detail

inline bool parseJson(const std::string& text, Json& out, std::string& err)
{
    detail::Parser p(text);
    out = Json{};
    if (! p.value(out, 0)) { err = p.err; return false; }
    p.ws();
    if (p.i != text.size()) { err = "trailing characters at offset " + std::to_string(p.i); return false; }
    return true;
}

inline std::string jsonEscape(const std::string& s)
{
    std::string r = "\"";
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); r += b; }
                else r += (char) c;
        }
    }
    return r + "\"";
}

inline std::string jsonNum(double v)
{
    char b[40];
    std::snprintf(b, sizeof b, "%.9g", v);
    return b;
}

} // namespace rp
