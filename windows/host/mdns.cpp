#include "mdns.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <windns.h>

#include <condition_variable>
#include <mutex>
#include <vector>

namespace kvhost {

namespace {
std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
} // namespace

struct MdnsAdvertisement::Impl {
    std::mutex m;
    std::condition_variable cv;
    bool registered = false;
    bool done = false;
    DWORD status = 0;
    PDNS_SERVICE_INSTANCE instance = nullptr;
    DNS_SERVICE_REGISTER_REQUEST req{};
    DNS_SERVICE_CANCEL cancel{};
    std::string full_name;
    std::wstring wname, whost;

    static VOID WINAPI onRegistered(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE inst) {
        auto* self = static_cast<Impl*>(ctx);
        if (inst) DnsServiceFreeInstance(inst);
        std::lock_guard<std::mutex> lk(self->m);
        self->status = status;
        self->done = true;
        self->cv.notify_all();
    }
};

MdnsAdvertisement::MdnsAdvertisement() = default;
MdnsAdvertisement::~MdnsAdvertisement() { stop(); }

bool MdnsAdvertisement::active() const { return impl_ && impl_->registered; }
std::string MdnsAdvertisement::fullName() const { return impl_ ? impl_->full_name : std::string(); }

bool MdnsAdvertisement::start(const std::string& instance, const std::string& type, uint16_t port, const std::string& ipv4,
                              std::string* err, int timeout_ms) {
    stop();
    auto impl = std::make_unique<Impl>();
    impl->full_name = instance + "." + type + ".local";
    impl->wname = widen(impl->full_name);

    wchar_t host[256];
    DWORD hlen = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, host, &hlen)) {
        if (err) *err = "GetComputerNameExW failed";
        return false;
    }
    impl->whost = std::wstring(host, hlen) + L".local";

    IP4_ADDRESS ip = 0;
    IN_ADDR a{};
    const bool have_ip = !ipv4.empty() && inet_pton(AF_INET, ipv4.c_str(), &a) == 1;
    if (have_ip) ip = a.S_un.S_addr;

    // One TXT key so a TXT record always exists (DNS-SD resolvers expect SRV + TXT); it carries no identity.
    PCWSTR keys[1] = {L"txtvers"};
    PCWSTR values[1] = {L"1"};
    impl->instance = DnsServiceConstructInstance(impl->wname.c_str(), impl->whost.c_str(), have_ip ? &ip : nullptr, nullptr,
                                                 port, 0, 0, 1, keys, values);
    if (!impl->instance) {
        if (err) *err = "DnsServiceConstructInstance failed";
        return false;
    }
    impl->req.Version = DNS_QUERY_REQUEST_VERSION1;
    impl->req.InterfaceIndex = 0;
    impl->req.pServiceInstance = impl->instance;
    impl->req.pRegisterCompletionCallback = &Impl::onRegistered;
    impl->req.pQueryContext = impl.get();
    impl->req.unicastEnabled = FALSE;
    const DWORD rc = DnsServiceRegister(&impl->req, &impl->cancel);
    if (rc != DNS_REQUEST_PENDING) {
        if (err) *err = "DnsServiceRegister failed (" + std::to_string(rc) + ")";
        DnsServiceFreeInstance(impl->instance);
        return false;
    }
    {
        std::unique_lock<std::mutex> lk(impl->m);
        impl->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return impl->done; });
        if (!impl->done || impl->status != ERROR_SUCCESS) {
            const std::string why = impl->done ? ("status " + std::to_string(impl->status)) : "timed out";
            const bool finished = impl->done;
            lk.unlock();
            if (!finished) {
                DnsServiceRegisterCancel(&impl->cancel);
                std::unique_lock<std::mutex> lk2(impl->m);
                impl->cv.wait_for(lk2, std::chrono::milliseconds(1000), [&] { return impl->done; });
            }
            DnsServiceFreeInstance(impl->instance);
            impl->instance = nullptr;
            if (err) *err = "mDNS registration " + why;
            return false;
        }
    }
    impl->registered = true;
    impl_ = std::move(impl);
    return true;
}

void MdnsAdvertisement::stop() {
    if (!impl_) return;
    if (impl_->registered) {
        {
            std::lock_guard<std::mutex> lk(impl_->m);
            impl_->done = false;
        }
        if (DnsServiceDeRegister(&impl_->req, nullptr) == DNS_REQUEST_PENDING) {
            std::unique_lock<std::mutex> lk(impl_->m);
            impl_->cv.wait_for(lk, std::chrono::milliseconds(3000), [&] { return impl_->done; });
        }
        impl_->registered = false;
    }
    if (impl_->instance) {
        DnsServiceFreeInstance(impl_->instance);
        impl_->instance = nullptr;
    }
    impl_.reset();
}

std::string primaryLanIPv4() {
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    // Any public address picks the default route; this asks the routing table only (GetBestInterfaceEx sends nothing).
    inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
    DWORD if_index = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&dst), &if_index) != NO_ERROR) return "127.0.0.1";
    ULONG len = 16 * 1024;
    std::vector<uint8_t> buf(len);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, aa, &len);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(len);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, aa, &len);
    }
    if (rc != NO_ERROR) return "127.0.0.1";
    for (auto* p = aa; p; p = p->Next) {
        if (p->IfIndex != if_index) continue;
        for (auto* u = p->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            if (sin->sin_family != AF_INET) continue;
            char ip[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            if (std::string(ip).rfind("169.254.", 0) == 0) continue;
            return ip;
        }
    }
    return "127.0.0.1";
}

} // namespace kvhost
