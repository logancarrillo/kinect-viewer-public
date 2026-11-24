// Minimal JSON writer/reader matching Swift's JSONEncoder/JSONDecoder for the wire messages:
// compact output, keys in declaration order, Data as padded base64, '/' escaped as "\/".
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kvhost {

std::string base64Encode(const uint8_t* data, size_t n);
bool base64Decode(const std::string& s, std::vector<uint8_t>& out); // standard alphabet, padding required

namespace json {

class ArrayWriter;

class ObjectWriter {
public:
    ObjectWriter& field(const char* key, const std::string& v);
    ObjectWriter& field(const char* key, const char* v) { return field(key, std::string(v)); }
    ObjectWriter& field(const char* key, bool v);
    ObjectWriter& field(const char* key, int64_t v);
    ObjectWriter& field(const char* key, uint64_t v);
    ObjectWriter& field(const char* key, int v) { return field(key, static_cast<int64_t>(v)); }
    ObjectWriter& field(const char* key, uint32_t v) { return field(key, static_cast<uint64_t>(v)); }
    ObjectWriter& field(const char* key, uint16_t v) { return field(key, static_cast<uint64_t>(v)); }
    ObjectWriter& field(const char* key, uint8_t v) { return field(key, static_cast<uint64_t>(v)); }
    // Swift's JSONEncoder writes a Float or Double as its shortest round-trip text, without a
    // trailing ".0" (500.0 -> 500, 0.25 -> 0.25). Non-finite values are refused there; here they
    // are written as 0 so a bug can't produce invalid JSON.
    ObjectWriter& floatField(const char* key, float v);
    ObjectWriter& doubleField(const char* key, double v);
    ObjectWriter& bytes(const char* key, const std::vector<uint8_t>& v);
    ObjectWriter& bytesArray(const char* key, const std::vector<std::vector<uint8_t>>& v);
    ObjectWriter& raw(const char* key, const std::string& json_value);
    // Nested values (protocol v3), written with the same rules as the top level, so callers never
    // hand-build JSON text for an object or an array.
    ObjectWriter& object(const char* key, const ObjectWriter& child);
    ObjectWriter& array(const char* key, const ArrayWriter& child);
    ObjectWriter& stringArray(const char* key, const std::vector<std::string>& v);
    std::string finish() const { return body_ + "}"; }

private:
    std::string body_ = "{";
    bool first_ = true;
    void key(const char* k);
};

// A JSON array, built element by element, for arrays of objects (or mixed arrays of strings).
class ArrayWriter {
public:
    ArrayWriter& object(const ObjectWriter& child);
    ArrayWriter& string(const std::string& v);
    std::string finish() const { return body_ + "]"; }

private:
    std::string body_ = "[";
    bool first_ = true;
    void comma();
};

std::string quote(const std::string& s); // JSON string literal, Swift-style escaping

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    std::string text; // number literal or decoded string
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    const Value* get(const char* key) const;
    bool asBool(bool& out) const;
    bool asInt64(int64_t& out) const;
    bool asUInt64(uint64_t& out) const;
    bool asString(std::string& out) const;
    bool asBytes(std::vector<uint8_t>& out) const; // base64 string
    bool asDouble(double& out) const;              // any finite JSON number
};

// The parser's limits. A message from the network is parsed before anything else looks at it, so its
// fan-out is capped as well as its depth: a 4 MiB "[0,0,...]" would otherwise cost hundreds of MB of
// Values. Nothing the host or the viewer writes comes near any of them.
constexpr int kMaxDepth = 32;              // nested arrays and objects
constexpr size_t kMaxContainerItems = 1024; // items in one array, or members in one object
constexpr size_t kMaxValues = 65536;       // values in the whole document, containers included

// False (with err) on bad JSON, trailing data, or a document past one of the limits above.
bool parse(const char* data, size_t n, Value& out, std::string* err = nullptr);
// The same limits, raised only for a file the host wrote itself and reads back (the roll's per-day index.json,
// which holds one entry per clip: a busy day passes 1024). Never for anything a device sends.
struct ParseLimits {
    int maxDepth = kMaxDepth;
    size_t maxContainerItems = kMaxContainerItems;
    size_t maxValues = kMaxValues;
};
constexpr ParseLimits kLocalIndexLimits{kMaxDepth, 1u << 20, 1u << 24};
bool parse(const char* data, size_t n, Value& out, const ParseLimits& limits, std::string* err = nullptr);
inline bool parse(const std::string& s, Value& out, std::string* err = nullptr) {
    return parse(s.data(), s.size(), out, err);
}

// Typed field helpers: false if missing or of the wrong type/range (Swift's non-optional decode).
bool getBool(const Value& obj, const char* key, bool& out);
bool getInt(const Value& obj, const char* key, int& out);
bool getU64(const Value& obj, const char* key, uint64_t& out);
bool getU32(const Value& obj, const char* key, uint32_t& out);
bool getU16(const Value& obj, const char* key, uint16_t& out);
bool getU8(const Value& obj, const char* key, uint8_t& out);
bool getString(const Value& obj, const char* key, std::string& out);
bool getBytes(const Value& obj, const char* key, std::vector<uint8_t>& out);
bool getDouble(const Value& obj, const char* key, double& out);
bool getFloat(const Value& obj, const char* key, float& out);

// Swift's decodeIfPresent: a missing key or null gives nullopt and true; a present value of the
// wrong type gives false (the whole message fails to decode, as in Swift).
bool getOptBool(const Value& obj, const char* key, std::optional<bool>& out);
bool getOptInt(const Value& obj, const char* key, std::optional<int>& out);
bool getOptString(const Value& obj, const char* key, std::optional<std::string>& out);
bool getOptU64(const Value& obj, const char* key, std::optional<uint64_t>& out);
bool getOptU32(const Value& obj, const char* key, std::optional<uint32_t>& out);
bool getOptU16(const Value& obj, const char* key, std::optional<uint16_t>& out);
bool getOptDouble(const Value& obj, const char* key, std::optional<double>& out);

// Nested values. The pointers point into obj, so they live as long as obj does. A required getter
// is false when the key is missing, null, or not of that shape; an array getter is also false when
// any element is of the wrong type (Swift fails a [String] or [T] decode on one bad element).
bool getObject(const Value& obj, const char* key, const Value*& out);
bool getObjectArray(const Value& obj, const char* key, std::vector<const Value*>& out);
bool getStringArray(const Value& obj, const char* key, std::vector<std::string>& out);
// The decodeIfPresent forms: missing or null gives nullptr / nullopt and true.
bool getOptObject(const Value& obj, const char* key, const Value*& out);
bool getOptObjectArray(const Value& obj, const char* key, std::optional<std::vector<const Value*>>& out);
bool getOptStringArray(const Value& obj, const char* key, std::optional<std::vector<std::string>>& out);

} // namespace json
} // namespace kvhost
