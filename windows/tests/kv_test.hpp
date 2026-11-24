// Minimal dependency-free test harness for the Windows host tests.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace kvtest {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

// Timing floors missed while the PC was busy (KV_FLOOR, kv_load.hpp): neither passed nor failed.
inline int& skips() {
    static int n = 0;
    return n;
}

// The runner's exit code when floors were skipped and nothing failed: ctest's SKIP_RETURN_CODE for a timing row, so
// the row shows as "Not Run" and never as passed.
constexpr int kSkipReturnCode = 77;

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline int runAll(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : nullptr;
    int ran = 0;
    for (auto& c : registry()) {
        if (filter && std::string(c.name).find(filter) == std::string::npos) continue;
        const int before = failures();
        const int skippedBefore = skips();
        std::printf("[ RUN  ] %s\n", c.name);
        std::fflush(stdout);
        c.fn();
        std::printf("[ %s ] %s\n", failures() != before ? "FAIL" : skips() != skippedBefore ? "SKIP" : " OK ", c.name);
        std::fflush(stdout);
        ++ran;
    }
    std::printf("%d test(s), %d failure(s), %d timing floor(s) skipped on a busy PC\n", ran, failures(), skips());
    if (failures() != 0 || ran == 0) return 1;
    return skips() > 0 ? kSkipReturnCode : 0;
}

} // namespace kvtest

#define KV_CONCAT2(a, b) a##b
#define KV_CONCAT(a, b) KV_CONCAT2(a, b)
#define KV_TEST(name)                                                              \
    static void name();                                                            \
    static ::kvtest::Registrar KV_CONCAT(kv_reg_, name)(#name, &name);             \
    static void name()

#define KV_CHECK(cond)                                                                       \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            ++::kvtest::failures();                                                          \
        }                                                                                    \
    } while (0)

#define KV_CHECK_MSG(cond, msg)                                                              \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::printf("  CHECK FAILED %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond,        \
                        std::string(msg).c_str());                                           \
            ++::kvtest::failures();                                                          \
        }                                                                                    \
    } while (0)

#define KV_REQUIRE(cond)                                                                     \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::printf("  REQUIRE FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            ++::kvtest::failures();                                                          \
            return;                                                                          \
        }                                                                                    \
    } while (0)

#define KV_TEST_MAIN() \
    int main(int argc, char** argv) { return ::kvtest::runAll(argc, argv); }
