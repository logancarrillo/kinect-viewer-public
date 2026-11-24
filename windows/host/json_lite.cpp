#include "json_lite.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace kvhost {

namespace {
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
} // namespace

std::string base64Encode(const uint8_t* data, size_t n) {
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i < n) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < n) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += (i + 1 < n) ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

bool base64Decode(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    if (s.size() % 4 != 0) return false;
    out.reserve(s.size() / 4 * 3);
    for (size_t i = 0; i < s.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i + k];
            if (c == '=') {
                if (i + 4 != s.size() || k < 2) return false;
                v[k] = 0;
                ++pad;
            } else {
                if (pad) return false;
                v[k] = b64Value(c);
                if (v[k] < 0) return false;
            }
        }
        const uint32_t w = (static_cast<uint32_t>(v[0]) << 18) | (static_cast<uint32_t>(v[1]) << 12) |
                           (static_cast<uint32_t>(v[2]) << 6) | static_cast<uint32_t>(v[3]);
        out.push_back(static_cast<uint8_t>(w >> 16));
        if (pad < 2) out.push_back(static_cast<uint8_t>(w >> 8));
        if (pad < 1) out.push_back(static_cast<uint8_t>(w));
    }
    return true;
}

namespace json {

std::string quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '/': o += "\\/"; break; // Swift JSONEncoder default (no .withoutEscapingSlashes)
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    o += '"';
    return o;
}

void ObjectWriter::key(const char* k) {
    if (!first_) body_ += ',';
    first_ = false;
    body_ += quote(k);
    body_ += ':';
}

ObjectWriter& ObjectWriter::field(const char* k, const std::string& v) {
    key(k);
    body_ += quote(v);
    return *this;
}
ObjectWriter& ObjectWriter::field(const char* k, bool v) {
    key(k);
    body_ += v ? "true" : "false";
    return *this;
}
ObjectWriter& ObjectWriter::field(const char* k, int64_t v) {
    key(k);
    body_ += std::to_string(v);
    return *this;
}
ObjectWriter& ObjectWriter::field(const char* k, uint64_t v) {
    key(k);
    body_ += std::to_string(v);
    return *this;
}
namespace {
template <typename F>
std::string shortestNumber(F v) {
    if (!std::isfinite(v)) return "0";
    if (v == 0) return "0"; // also -0, which Swift writes as "-0"; nothing here sends it
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v); // shortest round-trip
    return r.ec == std::errc() ? std::string(buf, r.ptr) : std::string("0");
}
} // namespace

ObjectWriter& ObjectWriter::floatField(const char* k, float v) {
    key(k);
    body_ += shortestNumber(v);
    return *this;
}
ObjectWriter& ObjectWriter::doubleField(const char* k, double v) {
    key(k);
    body_ += shortestNumber(v);
    return *this;
}
ObjectWriter& ObjectWriter::bytes(const char* k, const std::vector<uint8_t>& v) {
    key(k);
    body_ += quote(base64Encode(v.data(), v.size()));
    return *this;
}
ObjectWriter& ObjectWriter::bytesArray(const char* k, const std::vector<std::vector<uint8_t>>& v) {
    key(k);
    body_ += '[';
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) body_ += ',';
        body_ += quote(base64Encode(v[i].data(), v[i].size()));
    }
    body_ += ']';
    return *this;
}
ObjectWriter& ObjectWriter::raw(const char* k, const std::string& json_value) {
    key(k);
    body_ += json_value;
    return *this;
}
ObjectWriter& ObjectWriter::object(const char* k, const ObjectWriter& child) {
    key(k);
    body_ += child.finish();
    return *this;
}
ObjectWriter& ObjectWriter::array(const char* k, const ArrayWriter& child) {
    key(k);
    body_ += child.finish();
    return *this;
}
ObjectWriter& ObjectWriter::stringArray(const char* k, const std::vector<std::string>& v) {
    ArrayWriter a;
    for (const auto& s : v) a.string(s);
    return array(k, a);
}

void ArrayWriter::comma() {
    if (!first_) body_ += ',';
    first_ = false;
}
ArrayWriter& ArrayWriter::object(const ObjectWriter& child) {
    comma();
    body_ += child.finish();
    return *this;
}
ArrayWriter& ArrayWriter::string(const std::string& v) {
    comma();
    body_ += quote(v);
    return *this;
}

// ---- parser ------------------------------------------------------------------------------------

namespace {

struct Parser {
    const char* p;
    const char* end;
    std::string err;
    int depth = 0;
    size_t values = 0; // every value so far, containers included (kMaxValues)
    ParseLimits lim;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool fail(const char* m) {
        if (err.empty()) err = m;
        return false;
    }
    static void appendUtf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            o += static_cast<char>(0xE0 | (cp >> 12));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (cp >> 18));
            o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(uint32_t& v) {
        if (end - p < 4) return fail("short \\u escape");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool str(std::string& o) {
        if (p >= end || *p != '"') return fail("expected string");
        ++p;
        while (p < end) {
            const char c = *p++;
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control char in string");
            if (c != '\\') { o += c; continue; }
            if (p >= end) return fail("bad escape");
            const char e = *p++;
            switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return fail("lone surrogate");
                        p += 2;
                        uint32_t lo = 0;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(o, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool value(Value& v) {
        ws();
        if (p >= end) return fail("unexpected end");
        if (++depth > lim.maxDepth) return fail("too deep");
        if (++values > lim.maxValues) return fail("too many values");
        bool ok = false;
        const char c = *p;
        if (c == '{') {
            v.type = Value::Type::Object;
            ++p;
            ws();
            if (p < end && *p == '}') { ++p; ok = true; }
            else {
                while (true) {
                    if (v.members.size() >= lim.maxContainerItems) { fail("too many members"); break; }
                    ws();
                    std::string k;
                    if (!str(k)) break;
                    ws();
                    if (p >= end || *p != ':') { fail("expected ':'"); break; }
                    ++p;
                    Value child;
                    if (!value(child)) break;
                    v.members.emplace_back(std::move(k), std::move(child));
                    ws();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == '}') { ++p; ok = true; }
                    else fail("expected ',' or '}'");
                    break;
                }
            }
        } else if (c == '[') {
            v.type = Value::Type::Array;
            ++p;
            ws();
            if (p < end && *p == ']') { ++p; ok = true; }
            else {
                while (true) {
                    if (v.items.size() >= lim.maxContainerItems) { fail("too many items"); break; }
                    Value child;
                    if (!value(child)) break;
                    v.items.push_back(std::move(child));
                    ws();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == ']') { ++p; ok = true; }
                    else fail("expected ',' or ']'");
                    break;
                }
            }
        } else if (c == '"') {
            v.type = Value::Type::String;
            ok = str(v.text);
        } else if (c == 't' && end - p >= 4 && std::memcmp(p, "true", 4) == 0) {
            v.type = Value::Type::Bool; v.b = true; p += 4; ok = true;
        } else if (c == 'f' && end - p >= 5 && std::memcmp(p, "false", 5) == 0) {
            v.type = Value::Type::Bool; v.b = false; p += 5; ok = true;
        } else if (c == 'n' && end - p >= 4 && std::memcmp(p, "null", 4) == 0) {
            v.type = Value::Type::Null; p += 4; ok = true;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            v.type = Value::Type::Number;
            const char* s = p;
            if (*p == '-') ++p;
            while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) ++p;
            v.text.assign(s, p);
            ok = v.text != "-";
            if (!ok) fail("bad number");
        } else {
            fail("unexpected character");
        }
        --depth;
        return ok;
    }
};

bool isPlainInteger(const std::string& t, bool allow_minus) {
    if (t.empty()) return false;
    size_t i = 0;
    if (t[0] == '-') {
        if (!allow_minus) return false;
        i = 1;
    }
    if (i >= t.size()) return false;
    for (; i < t.size(); ++i) {
        if (t[i] < '0' || t[i] > '9') return false;
    }
    return true;
}

} // namespace

bool parse(const char* data, size_t n, Value& out, std::string* err) { return parse(data, n, out, ParseLimits{}, err); }

bool parse(const char* data, size_t n, Value& out, const ParseLimits& limits, std::string* err) {
    Parser ps{data, data + n, {}};
    ps.lim = limits;
    out = Value{};
    if (!ps.value(out)) {
        if (err) *err = ps.err;
        return false;
    }
    ps.ws();
    if (ps.p != ps.end) {
        if (err) *err = "trailing data";
        return false;
    }
    return true;
}

const Value* Value::get(const char* key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& m : members) {
        if (m.first == key) return &m.second;
    }
    return nullptr;
}

bool Value::asBool(bool& out) const {
    if (type != Type::Bool) return false;
    out = b;
    return true;
}

bool Value::asUInt64(uint64_t& out) const {
    if (type != Type::Number || !isPlainInteger(text, false)) return false;
    errno = 0;
    char* e = nullptr;
    const unsigned long long v = std::strtoull(text.c_str(), &e, 10);
    if (errno == ERANGE || !e || *e) return false;
    out = v;
    return true;
}

bool Value::asInt64(int64_t& out) const {
    if (type != Type::Number || !isPlainInteger(text, true)) return false;
    errno = 0;
    char* e = nullptr;
    const long long v = std::strtoll(text.c_str(), &e, 10);
    if (errno == ERANGE || !e || *e) return false;
    out = v;
    return true;
}

bool Value::asString(std::string& out) const {
    if (type != Type::String) return false;
    out = text;
    return true;
}

bool Value::asBytes(std::vector<uint8_t>& out) const {
    return type == Type::String && base64Decode(text, out);
}

bool Value::asDouble(double& out) const {
    if (type != Type::Number) return false;
    double v = 0.0;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
    if (r.ec != std::errc() || r.ptr != text.data() + text.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool getBool(const Value& o, const char* k, bool& out) {
    const Value* v = o.get(k);
    return v && v->asBool(out);
}
bool getInt(const Value& o, const char* k, int& out) {
    const Value* v = o.get(k);
    int64_t x = 0;
    if (!v || !v->asInt64(x) || x < INT_MIN || x > INT_MAX) return false;
    out = static_cast<int>(x);
    return true;
}
bool getU64(const Value& o, const char* k, uint64_t& out) {
    const Value* v = o.get(k);
    return v && v->asUInt64(out);
}
bool getU32(const Value& o, const char* k, uint32_t& out) {
    uint64_t x = 0;
    if (!getU64(o, k, x) || x > 0xFFFFFFFFULL) return false;
    out = static_cast<uint32_t>(x);
    return true;
}
bool getU16(const Value& o, const char* k, uint16_t& out) {
    uint64_t x = 0;
    if (!getU64(o, k, x) || x > 0xFFFFULL) return false;
    out = static_cast<uint16_t>(x);
    return true;
}
bool getU8(const Value& o, const char* k, uint8_t& out) {
    uint64_t x = 0;
    if (!getU64(o, k, x) || x > 0xFFULL) return false;
    out = static_cast<uint8_t>(x);
    return true;
}
bool getString(const Value& o, const char* k, std::string& out) {
    const Value* v = o.get(k);
    return v && v->asString(out);
}
bool getBytes(const Value& o, const char* k, std::vector<uint8_t>& out) {
    const Value* v = o.get(k);
    return v && v->asBytes(out);
}
bool getDouble(const Value& o, const char* k, double& out) {
    const Value* v = o.get(k);
    return v && v->asDouble(out);
}
bool getFloat(const Value& o, const char* k, float& out) {
    double d = 0.0;
    if (!getDouble(o, k, d) || std::fabs(d) > 3.4028234663852886e38) return false;
    out = static_cast<float>(d);
    return true;
}

namespace {
bool absent(const Value& o, const char* k) {
    const Value* v = o.get(k);
    return !v || v->type == Value::Type::Null;
}
} // namespace

bool getOptBool(const Value& o, const char* k, std::optional<bool>& out) {
    out.reset();
    if (absent(o, k)) return true;
    bool b = false;
    if (!getBool(o, k, b)) return false;
    out = b;
    return true;
}
bool getOptInt(const Value& o, const char* k, std::optional<int>& out) {
    out.reset();
    if (absent(o, k)) return true;
    int i = 0;
    if (!getInt(o, k, i)) return false;
    out = i;
    return true;
}
bool getOptString(const Value& o, const char* k, std::optional<std::string>& out) {
    out.reset();
    if (absent(o, k)) return true;
    std::string s;
    if (!getString(o, k, s)) return false;
    out = std::move(s);
    return true;
}

namespace {
// decodeIfPresent over any of the required getters above.
template <typename T, typename Get>
bool getOpt(const Value& o, const char* k, std::optional<T>& out, Get get) {
    out.reset();
    if (absent(o, k)) return true;
    T x{};
    if (!get(o, k, x)) return false;
    out = std::move(x);
    return true;
}
} // namespace

bool getOptU64(const Value& o, const char* k, std::optional<uint64_t>& out) {
    return getOpt<uint64_t>(o, k, out, getU64);
}
bool getOptU32(const Value& o, const char* k, std::optional<uint32_t>& out) {
    return getOpt<uint32_t>(o, k, out, getU32);
}
bool getOptU16(const Value& o, const char* k, std::optional<uint16_t>& out) {
    return getOpt<uint16_t>(o, k, out, getU16);
}
bool getOptDouble(const Value& o, const char* k, std::optional<double>& out) {
    return getOpt<double>(o, k, out, getDouble);
}

bool getObject(const Value& o, const char* k, const Value*& out) {
    const Value* v = o.get(k);
    if (!v || v->type != Value::Type::Object) return false;
    out = v;
    return true;
}
bool getObjectArray(const Value& o, const char* k, std::vector<const Value*>& out) {
    const Value* v = o.get(k);
    if (!v || v->type != Value::Type::Array) return false;
    std::vector<const Value*> items;
    items.reserve(v->items.size());
    for (const auto& item : v->items) {
        if (item.type != Value::Type::Object) return false;
        items.push_back(&item);
    }
    out = std::move(items);
    return true;
}
bool getStringArray(const Value& o, const char* k, std::vector<std::string>& out) {
    const Value* v = o.get(k);
    if (!v || v->type != Value::Type::Array) return false;
    std::vector<std::string> items;
    items.reserve(v->items.size());
    for (const auto& item : v->items) {
        std::string s;
        if (!item.asString(s)) return false;
        items.push_back(std::move(s));
    }
    out = std::move(items);
    return true;
}
bool getOptObject(const Value& o, const char* k, const Value*& out) {
    out = nullptr;
    if (absent(o, k)) return true;
    return getObject(o, k, out);
}
bool getOptObjectArray(const Value& o, const char* k, std::optional<std::vector<const Value*>>& out) {
    return getOpt<std::vector<const Value*>>(o, k, out, getObjectArray);
}
bool getOptStringArray(const Value& o, const char* k, std::optional<std::vector<std::string>>& out) {
    return getOpt<std::vector<std::string>>(o, k, out, getStringArray);
}

} // namespace json
} // namespace kvhost
