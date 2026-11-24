#include "outside_test_router.hpp"

#include <utility>

namespace kvwin {

namespace {

using kvhost::outside::RouterMapping;

// The address another machine on the home network holds a taken port for (TestRouterScript::takenPort).
constexpr const char* kOtherMachine = "192.168.1.250";

class FakeSession final : public kvhost::outside::RouterSession {
public:
    explicit FakeSession(std::shared_ptr<OutsideTestRouter::State> st) : st_(std::move(st)) {}

    std::optional<RouterMapping> find(uint16_t port) override {
        std::lock_guard<std::mutex> lock(st_->mu);
        if (port != 0 && port == st_->script.takenPort) return RouterMapping{kOtherMachine, port, true, st_->script.wan};
        const auto it = st_->mappings.find(port);
        if (it == st_->mappings.end()) return std::nullopt;
        RouterMapping m = it->second;
        m.externalAddress = st_->script.wan; // read from the mapping, as get_ExternalIPAddress is (§8.3)
        return m;
    }
    bool add(uint16_t port, const std::string& internalClient, const std::string& description) override {
        std::lock_guard<std::mutex> lock(st_->mu);
        ++st_->counts.adds;
        if (port == st_->script.takenPort || description != kvhost::outside::kMappingDescription) return false;
        // Kept with its description, as a router does (get_Description): only a mapping with ours is ours.
        st_->mappings[port] = RouterMapping{internalClient, port, true, {}, description};
        st_->counts.mappings = st_->mappings.size();
        return true;
    }
    bool remove(uint16_t port) override {
        std::lock_guard<std::mutex> lock(st_->mu);
        ++st_->counts.removes;
        const bool removed = st_->mappings.erase(port) == 1;
        st_->counts.mappings = st_->mappings.size();
        return removed;
    }

private:
    std::shared_ptr<OutsideTestRouter::State> st_;
};

} // namespace

OutsideTestRouter::OutsideTestRouter() : st_(std::make_shared<State>()) {}

void OutsideTestRouter::setScript(const TestRouterScript& script) {
    std::lock_guard<std::mutex> lock(st_->mu);
    st_->script = script;
}

TestRouterCounts OutsideTestRouter::counts() const {
    std::lock_guard<std::mutex> lock(st_->mu);
    return st_->counts;
}

std::function<std::unique_ptr<kvhost::outside::RouterSession>()> OutsideTestRouter::factory() {
    std::shared_ptr<State> st = st_;
    return [st]() -> std::unique_ptr<kvhost::outside::RouterSession> {
        std::lock_guard<std::mutex> lock(st->mu);
        ++st->counts.opens;
        if (!st->script.present) return nullptr;
        return std::make_unique<FakeSession>(st);
    };
}

} // namespace kvwin
