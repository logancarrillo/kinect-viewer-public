// Checks DnsServiceRegister works on this PC: registers a throwaway "_kv2test._tcp" instance (never the
// real _kv2s/_kv2p types), browses for it with DnsServiceBrowse, then deregisters.
#include "kv_test.hpp"

#include "mdns.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windns.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace {

struct BrowseState {
    std::mutex m;
    std::condition_variable cv;
    std::wstring want;
    bool found = false;
};

VOID WINAPI onBrowse(DWORD status, PVOID ctx, PDNS_RECORD records) {
    auto* st = static_cast<BrowseState*>(ctx);
    if (status == ERROR_SUCCESS) {
        for (PDNS_RECORD r = records; r; r = r->pNext) {
            if (r->wType == DNS_TYPE_PTR && r->Data.PTR.pNameHost) {
                std::wstring name = r->Data.PTR.pNameHost;
                std::lock_guard<std::mutex> lk(st->m);
                if (_wcsicmp(name.c_str(), st->want.c_str()) == 0) {
                    st->found = true;
                    st->cv.notify_all();
                }
            }
        }
    }
    if (records) DnsRecordListFree(records, DnsFreeRecordList);
}

} // namespace

KV_TEST(register_browse_and_deregister_a_test_service) {
    const std::string instance = "kvtest-" + std::to_string(GetCurrentProcessId());
    const std::string ip = kvhost::primaryLanIPv4();
    std::printf("  primary LAN IPv4: %s\n", ip == "127.0.0.1" ? "none (loopback)" : "found");
    kvhost::MdnsAdvertisement ad;
    std::string err;
    KV_REQUIRE(ad.start(instance, "_kv2test._tcp", 5555, ip, &err));
    KV_CHECK(ad.active());

    BrowseState st;
    std::wstring w(instance.begin(), instance.end());
    st.want = w + L"._kv2test._tcp.local";
    DNS_SERVICE_BROWSE_REQUEST req{};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex = 0;
    req.QueryName = L"_kv2test._tcp.local";
    req.pBrowseCallback = onBrowse;
    req.pQueryContext = &st;
    DNS_SERVICE_CANCEL cancel{};
    const DWORD rc = DnsServiceBrowse(&req, &cancel);
    KV_CHECK(rc == DNS_REQUEST_PENDING);
    {
        std::unique_lock<std::mutex> lk(st.m);
        st.cv.wait_for(lk, std::chrono::seconds(5), [&] { return st.found; });
    }
    DnsServiceBrowseCancel(&cancel);
    std::printf("  browse found our instance: %s\n", st.found ? "yes" : "no");
    KV_CHECK(st.found);
    ad.stop();
    KV_CHECK(!ad.active());
}

KV_TEST_MAIN()
