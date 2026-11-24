// json_lite on its own: the nested writers and the optional and array getters that protocol v3 adds
// (docs/protocol-v3-pet-camera.md §1 "JSON conventions"). Synthetic strings only.
#include "kv_test.hpp"

#include "json_lite.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace kvhost;

KV_TEST(nested_object_and_arrays_follow_the_swift_rules) {
    json::ObjectWriter inner;
    inner.field("host", "a/b").field("port", static_cast<uint16_t>(443));
    json::ArrayWriter list;
    list.object(json::ObjectWriter().field("k", 1)).object(json::ObjectWriter()).string("x/y");
    const std::string got = json::ObjectWriter()
                                .field("first", true)
                                .object("inner", inner)
                                .array("list", list)
                                .stringArray("names", {"auto", "a\"b", "c/d"})
                                .stringArray("none", {})
                                .array("empty", json::ArrayWriter())
                                .object("blank", json::ObjectWriter())
                                .finish();
    // Compact, declaration order, '/' escaped as "\/" at every depth.
    KV_CHECK_MSG(got == "{\"first\":true,\"inner\":{\"host\":\"a\\/b\",\"port\":443},\"list\":[{\"k\":1},{},\"x\\/y\"],"
                        "\"names\":[\"auto\",\"a\\\"b\",\"c\\/d\"],\"none\":[],\"empty\":[],\"blank\":{}}",
                 got);
    // The writer's output is read back by the parser.
    json::Value v;
    KV_REQUIRE(json::parse(got, v));
    const json::Value* in = nullptr;
    KV_REQUIRE(json::getObject(v, "inner", in));
    std::string host;
    uint16_t port = 0;
    KV_CHECK(json::getString(*in, "host", host) && host == "a/b" && json::getU16(*in, "port", port) && port == 443);
}

KV_TEST(optional_number_getters_follow_decode_if_present) {
    json::Value v;
    KV_REQUIRE(json::parse(std::string("{\"big\":18446744073709551615,\"u32\":4294967295,\"u16\":65535,\"d\":12.5,"
                                       "\"n\":null,\"s\":\"7\",\"neg\":-1,\"over32\":4294967296,\"over16\":65536,"
                                       "\"frac\":1.5,\"i\":3}"),
                           v));
    std::optional<uint64_t> u64;
    std::optional<uint32_t> u32;
    std::optional<uint16_t> u16;
    std::optional<double> d;

    KV_CHECK(json::getOptU64(v, "big", u64) && u64 == 18446744073709551615ULL);
    KV_CHECK(json::getOptU32(v, "u32", u32) && u32 == 4294967295u);
    KV_CHECK(json::getOptU16(v, "u16", u16) && u16 == 65535);
    KV_CHECK(json::getOptDouble(v, "d", d) && d == 12.5);
    KV_CHECK(json::getOptDouble(v, "i", d) && d == 3.0); // an integer is a valid Double

    // Missing or null: nullopt and true, and an earlier value is cleared.
    for (const char* k : {"missing", "n"}) {
        u64 = 1;
        u32 = 1;
        u16 = 1;
        d = 1.0;
        KV_CHECK(json::getOptU64(v, k, u64) && !u64);
        KV_CHECK(json::getOptU32(v, k, u32) && !u32);
        KV_CHECK(json::getOptU16(v, k, u16) && !u16);
        KV_CHECK(json::getOptDouble(v, k, d) && !d);
    }

    // Present but of the wrong type or out of range: false.
    KV_CHECK(!json::getOptU64(v, "s", u64));
    KV_CHECK(!json::getOptU64(v, "neg", u64));
    KV_CHECK(!json::getOptU64(v, "frac", u64));
    KV_CHECK(!json::getOptU32(v, "over32", u32));
    KV_CHECK(!json::getOptU16(v, "over16", u16));
    KV_CHECK(!json::getOptDouble(v, "s", d));
}

KV_TEST(array_and_object_getters) {
    json::Value v;
    KV_REQUIRE(json::parse(std::string("{\"objs\":[{\"a\":1},{\"a\":2}],\"strs\":[\"x\",\"y\\/z\"],\"empty\":[],"
                                       "\"mixed\":[{\"a\":1},\"b\"],\"badstrs\":[\"x\",1],\"obj\":{\"a\":3},"
                                       "\"notarr\":{\"a\":1},\"n\":null,\"num\":4}"),
                           v));
    std::vector<const json::Value*> objs;
    KV_REQUIRE(json::getObjectArray(v, "objs", objs));
    KV_REQUIRE(objs.size() == 2);
    int a0 = 0, a1 = 0;
    KV_CHECK(json::getInt(*objs[0], "a", a0) && a0 == 1 && json::getInt(*objs[1], "a", a1) && a1 == 2);

    std::vector<std::string> strs;
    KV_CHECK(json::getStringArray(v, "strs", strs) && strs == (std::vector<std::string>{"x", "y/z"}));
    KV_CHECK(json::getStringArray(v, "empty", strs) && strs.empty());
    KV_CHECK(json::getObjectArray(v, "empty", objs) && objs.empty());

    // One element of the wrong type fails the whole array, as a Swift [T] decode does.
    KV_CHECK(!json::getObjectArray(v, "mixed", objs));
    KV_CHECK(!json::getStringArray(v, "badstrs", strs));
    KV_CHECK(!json::getStringArray(v, "objs", strs));
    // Not an array, missing, or null: a required getter fails.
    KV_CHECK(!json::getObjectArray(v, "notarr", objs));
    KV_CHECK(!json::getStringArray(v, "missing", strs));
    KV_CHECK(!json::getStringArray(v, "n", strs));

    const json::Value* o = nullptr;
    int a = 0;
    KV_CHECK(json::getObject(v, "obj", o) && o && json::getInt(*o, "a", a) && a == 3);
    KV_CHECK(!json::getObject(v, "objs", o));
    KV_CHECK(!json::getObject(v, "n", o));
    KV_CHECK(!json::getObject(v, "missing", o));

    // The decodeIfPresent forms.
    o = &v;
    KV_CHECK(json::getOptObject(v, "n", o) && o == nullptr);
    o = &v;
    KV_CHECK(json::getOptObject(v, "missing", o) && o == nullptr);
    KV_CHECK(json::getOptObject(v, "obj", o) && o != nullptr);
    KV_CHECK(!json::getOptObject(v, "num", o));

    std::optional<std::vector<const json::Value*>> oobjs = std::vector<const json::Value*>{};
    KV_CHECK(json::getOptObjectArray(v, "missing", oobjs) && !oobjs);
    KV_CHECK(json::getOptObjectArray(v, "n", oobjs) && !oobjs);
    KV_CHECK(json::getOptObjectArray(v, "objs", oobjs) && oobjs && oobjs->size() == 2);
    KV_CHECK(!json::getOptObjectArray(v, "mixed", oobjs));
    KV_CHECK(!json::getOptObjectArray(v, "num", oobjs));

    std::optional<std::vector<std::string>> ostrs = std::vector<std::string>{"old"};
    KV_CHECK(json::getOptStringArray(v, "n", ostrs) && !ostrs);
    KV_CHECK(json::getOptStringArray(v, "strs", ostrs) && ostrs && ostrs->size() == 2);
    KV_CHECK(json::getOptStringArray(v, "empty", ostrs) && ostrs && ostrs->empty());
    KV_CHECK(!json::getOptStringArray(v, "badstrs", ostrs));
}

KV_TEST(nesting_stays_within_the_parser_depth_limit) {
    // v3 nests at most 3 levels (0x0B -> devices -> device); the parser takes 32 and refuses deeper.
    std::string ok(32, '[');
    ok += std::string(32, ']');
    json::Value v;
    KV_CHECK(json::parse(ok, v));
    std::string deep(33, '[');
    deep += std::string(33, ']');
    KV_CHECK(!json::parse(deep, v));
}

// A message from the network is parsed before anything else looks at it, so the parser's fan-out is
// capped: at most 1024 items in one array or object, and 65536 values in all. A 4 MiB "[0,0,0,...]"
// would otherwise cost hundreds of MB of Values. Nothing the host reads or writes comes near either.
KV_TEST(fan_out_is_capped_per_container_and_in_total) {
    auto zeros = [](size_t n) {
        std::string s = "[";
        for (size_t i = 0; i < n; ++i) s += i ? ",0" : "0";
        return s + "]";
    };
    auto members = [](size_t n) {
        std::string s = "{";
        for (size_t i = 0; i < n; ++i) s += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":0";
        return s + "}";
    };
    json::Value v;
    std::string err;
    KV_CHECK(json::parse(zeros(1024), v) && v.items.size() == 1024);
    KV_CHECK_MSG(!json::parse(zeros(1025), v, &err), "a 1025-item array was accepted");
    KV_CHECK(!err.empty());
    KV_CHECK(json::parse(members(1024), v) && v.members.size() == 1024);
    KV_CHECK_MSG(!json::parse(members(1025), v), "a 1025-member object was accepted");
    // The 4 MiB attack itself: refused, and quickly.
    std::string big = "[";
    while (big.size() < 4u * 1024u * 1024u - 2) big += "0,";
    big += "0]";
    KV_CHECK(!json::parse(big, v));
    // In total: the outer array, 64 inner ones and 65471 zeros are 65536 values and fit; one more
    // zero does not, though no container passes 1024 items.
    auto nested = [&](size_t last) {
        std::string s = "[";
        for (int i = 0; i < 63; ++i) s += zeros(1023) + ",";
        return s + zeros(last) + "]";
    };
    KV_CHECK_MSG(json::parse(nested(1022), v) && v.items.size() == 64, "65536 values in all should parse");
    KV_CHECK_MSG(!json::parse(nested(1023), v), "65537 values were accepted");
}

KV_TEST_MAIN()
