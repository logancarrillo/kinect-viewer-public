// The --test-outside hook's fake router (v3 §10: no test reaches the real router, not even to read). The host
// gets it through HostController::Options::router_factory, so its mapper runs every §8.2/§8.3 step against this
// object. HOST's test utilities have no scripted fake the viewer can link, so this one is the viewer's own. The
// script and the counts are guarded: the host's mapper calls it from its own worker threads.
#pragma once

#include "router_mapper.hpp"
#include "viewer_app.hpp" // TestRouterScript, TestRouterCounts

#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace kvwin {

class OutsideTestRouter {
public:
    OutsideTestRouter();
    void setScript(const TestRouterScript& script);
    TestRouterCounts counts() const;
    // What Options::router_factory takes: each call is one look at the router (a fresh session, as NATUPnP's
    // collection is a snapshot), or nullptr while the script has no router.
    std::function<std::unique_ptr<kvhost::outside::RouterSession>()> factory();

    struct State {
        mutable std::mutex mu;
        TestRouterScript script;
        std::map<uint16_t, kvhost::outside::RouterMapping> mappings;
        TestRouterCounts counts;
    };

private:
    std::shared_ptr<State> st_;
};

} // namespace kvwin
