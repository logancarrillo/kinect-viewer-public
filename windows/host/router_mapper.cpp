#include "router_mapper.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <natupnp.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <xmllite.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cctype>
#include <mutex>
#include <thread>
#include <vector>

namespace kvhost::outside {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

class Bstr {
public:
    explicit Bstr(const std::wstring& s) : b_(SysAllocString(s.c_str())) {}
    Bstr() = default;
    ~Bstr() { SysFreeString(b_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    BSTR get() const { return b_; }
    BSTR* out() {
        SysFreeString(b_);
        b_ = nullptr;
        return &b_;
    }

private:
    BSTR b_ = nullptr;
};

template <class T> class Com {
public:
    Com() = default;
    ~Com() {
        if (p_) p_->Release();
    }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    T* get() const { return p_; }
    T** out() {
        if (p_) p_->Release();
        p_ = nullptr;
        return &p_;
    }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

class NatUpnpSession final : public RouterSession {
public:
    explicit NatUpnpSession(Com<IStaticPortMappingCollection> c) : c_(std::move(c)) {}

    std::optional<RouterMapping> find(uint16_t port) override {
        Com<IStaticPortMapping> m;
        Bstr tcp(L"TCP");
        if (FAILED(c_->get_Item(port, tcp.get(), m.out())) || !m) return std::nullopt;
        RouterMapping r;
        Bstr client, external, description;
        long internalPort = 0;
        VARIANT_BOOL enabled = VARIANT_FALSE;
        if (FAILED(m->get_InternalClient(client.out())) || FAILED(m->get_InternalPort(&internalPort)) ||
            FAILED(m->get_Enabled(&enabled)))
            return std::nullopt;
        if (SUCCEEDED(m->get_ExternalIPAddress(external.out()))) r.externalAddress = narrow(external.get());
        if (SUCCEEDED(m->get_Description(description.out()))) r.description = narrow(description.get());
        r.internalClient = narrow(client.get());
        r.internalPort = internalPort > 0 && internalPort <= 65535 ? static_cast<uint16_t>(internalPort) : 0;
        r.enabled = enabled != VARIANT_FALSE;
        return r;
    }

    bool add(uint16_t port, const std::string& internalClient, const std::string& description) override {
        Com<IStaticPortMapping> m;
        Bstr tcp(L"TCP"), client(widen(internalClient)), desc(widen(description));
        return SUCCEEDED(c_->Add(port, tcp.get(), port, client.get(), VARIANT_TRUE, desc.get(), m.out())) && m;
    }

    bool remove(uint16_t port) override {
        Bstr tcp(L"TCP");
        return SUCCEEDED(c_->Remove(port, tcp.get()));
    }

private:
    Com<IStaticPortMappingCollection> c_;
};

uint64_t tickMs() { return GetTickCount64(); }
void sleepFor(uint64_t ms) { Sleep(static_cast<DWORD>(ms > ULONG_MAX ? ULONG_MAX : ms)); }

const char* kTaken = "That port is already in use on the router. Pick another port.";

} // namespace

// --- COM and the real router ---

ComScope::ComScope() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    owned_ = SUCCEEDED(hr);
    ok_ = owned_ || hr == RPC_E_CHANGED_MODE; // already initialised on this thread (STA): usable, not ours
}

ComScope::~ComScope() {
    if (owned_) CoUninitialize();
}

std::unique_ptr<RouterSession> openNatUpnpSession() {
    Com<IUPnPNAT> nat;
    if (FAILED(CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUPnPNAT),
                                reinterpret_cast<void**>(nat.out()))) ||
        !nat)
        return nullptr;
    Com<IStaticPortMappingCollection> c;
    if (FAILED(nat->get_StaticPortMappingCollection(c.out())) || !c) return nullptr;
    return std::make_unique<NatUpnpSession>(std::move(c));
}

GatewayVerdict judgeGateway(const GatewayIdentity& pin, const GatewayReading& now) {
    if (!pin.complete()) return GatewayVerdict::NoPin;
    // Positive evidence first: a part read now that differs from what it should be.
    if (!now.gateway.ip.empty() && now.gateway.ip != pin.ip) return GatewayVerdict::OtherGateway;
    if (!now.gateway.mac.empty() && now.gateway.mac != pin.mac) return GatewayVerdict::OtherGateway;
    const bool agrees = std::find(now.gatewayWans.begin(), now.gatewayWans.end(), now.reportedWan) != now.gatewayWans.end();
    if (!now.reportedWan.empty() && !now.gatewayWans.empty() && !agrees) return GatewayVerdict::Contradicted;
    // Then a part that couldn't be read: not evidence either way.
    if (!now.gateway.complete() || now.reportedWan.empty() || now.gatewayWans.empty()) return GatewayVerdict::Unreadable;
    return GatewayVerdict::Match;
}

const char* gatewayPinKind(const GatewayIdentity& pin) { return pin.complete() ? "address and hardware address" : ""; }

bool validGatewayIp(const std::string& s) {
    if (s.empty() || s.size() > 15) return false;
    in_addr a{};
    if (inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
    char canon[INET_ADDRSTRLEN] = {0};
    return inet_ntop(AF_INET, &a, canon, sizeof(canon)) && s == canon; // dotted decimal exactly as written
}

bool validGatewayMac(const std::string& s) {
    if (s.size() != 17) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i % 3 == 2) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::optional<GatewayUrl> crackGatewayUrl(const std::string& url, const std::string& gatewayIp) {
    if (url.empty() || url.size() > 2048 || !validGatewayIp(gatewayIp)) return std::nullopt;
    for (const unsigned char c : url) {
        if (c <= 0x20 || c >= 0x7f) return std::nullopt; // no spaces, controls or anything outside ASCII
    }
    const std::wstring w(url.begin(), url.end());
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUserNameLength = uc.dwPasswordLength = uc.dwUrlPathLength =
        uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(w.c_str(), static_cast<DWORD>(w.size()), 0, &uc)) return std::nullopt;
    if (uc.nScheme != INTERNET_SCHEME_HTTP) return std::nullopt;
    if (uc.dwUserNameLength != 0 || uc.dwPasswordLength != 0) return std::nullopt;
    if (!uc.lpszHostName || uc.dwHostNameLength == 0) return std::nullopt;
    std::string host;
    for (DWORD i = 0; i < uc.dwHostNameLength; ++i) host.push_back(static_cast<char>(uc.lpszHostName[i]));
    if (!validGatewayIp(host) || host != gatewayIp) return std::nullopt; // an IPv4 literal: the gateway's own
    GatewayUrl g;
    g.host = host;
    g.port = uc.nPort;
    for (DWORD i = 0; uc.lpszUrlPath && i < uc.dwUrlPathLength; ++i) g.path.push_back(static_cast<char>(uc.lpszUrlPath[i]));
    for (DWORD i = 0; uc.lpszExtraInfo && i < uc.dwExtraInfoLength; ++i) g.path.push_back(static_cast<char>(uc.lpszExtraInfo[i]));
    if (g.path.empty() || g.path[0] != '/') g.path.insert(g.path.begin(), '/');
    return g;
}

std::string ssdpLocation(const std::string& reply) { return ssdpHeader(reply, "location"); }

std::string ssdpHeader(const std::string& reply, const std::string& lowerName) {
    size_t at = 0;
    bool first = true;
    while (at < reply.size()) {
        size_t end = reply.find('\n', at);
        if (end == std::string::npos) end = reply.size();
        std::string line = reply.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) {
            first = false;
            if (line.rfind("HTTP/1.1 200", 0) != 0 && line.rfind("HTTP/1.0 200", 0) != 0) return {};
            continue;
        }
        if (line.empty()) break; // the end of the headers
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        for (auto& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (name != lowerName) continue;
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
        return value;
    }
    return {};
}

std::string gatewayLocationFrom(const std::string& sourceIp, const std::string& reply, const std::string& gatewayIp) {
    if (!validGatewayIp(gatewayIp) || sourceIp != gatewayIp) return {};
    const std::string location = ssdpLocation(reply);
    return crackGatewayUrl(location, gatewayIp) ? location : std::string();
}

namespace {

// An XML document from memory, read with XmlLite: no DTD (refused), elements at most 32 deep. f(kind, localName,
// namespaceUri, value) for each element start ('<'), end ('>') and text ('t'). False when the document can't be read
// whole.
template <class F> bool readXml(const std::string& xml, F&& f) {
    if (xml.empty() || xml.size() > 256 * 1024) return false;
    IStream* stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(xml.data()), static_cast<UINT>(xml.size()));
    if (!stream) return false;
    IXmlReader* reader = nullptr;
    bool ok = SUCCEEDED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(&reader), nullptr)) && reader &&
              SUCCEEDED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) &&
              SUCCEEDED(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 32)) && SUCCEEDED(reader->SetInput(stream));
    XmlNodeType type = XmlNodeType_None;
    while (ok) {
        const HRESULT hr = reader->Read(&type);
        if (hr == S_FALSE) break;
        if (FAILED(hr)) {
            ok = false;
            break;
        }
        const wchar_t* name = nullptr;
        const wchar_t* value = nullptr;
        const wchar_t* uri = nullptr;
        if (type == XmlNodeType_Element) {
            if (FAILED(reader->GetLocalName(&name, nullptr)) || FAILED(reader->GetNamespaceUri(&uri, nullptr))) ok = false;
            else {
                const std::string n = narrow(name), ns = narrow(uri);
                f('<', n, ns, std::string());
                if (reader->IsEmptyElement()) f('>', n, ns, std::string());
            }
        } else if (type == XmlNodeType_EndElement) {
            if (FAILED(reader->GetLocalName(&name, nullptr)) || FAILED(reader->GetNamespaceUri(&uri, nullptr))) ok = false;
            else f('>', narrow(name), narrow(uri), std::string());
        } else if (type == XmlNodeType_Text || type == XmlNodeType_CDATA) {
            if (FAILED(reader->GetValue(&value, nullptr))) ok = false;
            else f('t', std::string(), std::string(), narrow(value));
        }
    }
    if (reader) reader->Release();
    stream->Release();
    return ok;
}

std::string trimmed(std::string s) {
    while (!s.empty() && isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

bool startsWithNoCase(const std::string& s, const char* prefix) {
    for (size_t i = 0; prefix[i]; ++i) {
        if (i >= s.size() || tolower(static_cast<unsigned char>(s[i])) != prefix[i]) return false;
    }
    return true;
}

const char* const kWanServices[] = {"urn:schemas-upnp-org:service:WANIPConnection:1",
                                    "urn:schemas-upnp-org:service:WANIPConnection:2",
                                    "urn:schemas-upnp-org:service:WANPPPConnection:1"};

// A control URL against its base: absolute as it is; "/x" on the base's host and port; "x" beside the base's path.
std::optional<GatewayUrl> resolveControl(const std::string& control, const std::string& base, const std::string& gatewayIp) {
    if (control.empty()) return std::nullopt;
    if (startsWithNoCase(control, "http://") || startsWithNoCase(control, "https://")) return crackGatewayUrl(control, gatewayIp);
    const std::optional<GatewayUrl> b = crackGatewayUrl(base, gatewayIp);
    if (!b) return std::nullopt;
    std::string path = control;
    if (path[0] != '/') {
        const std::string dir = b->path.substr(0, b->path.find_last_of('/', b->path.find('?')) + 1);
        path = (dir.empty() ? std::string("/") : dir) + path;
    }
    return crackGatewayUrl("http://" + b->host + ":" + std::to_string(b->port) + path, gatewayIp);
}

} // namespace

std::vector<GatewayService> gatewayServices(const std::string& descriptionXml, const std::string& location,
                                            const std::string& gatewayIp) {
    std::vector<GatewayService> out;
    std::vector<std::string> path; // the open elements
    std::string urlBase, type, control;
    bool inService = false;
    std::vector<std::pair<std::string, std::string>> found; // (type, control URL as written)
    bool upnpRoot = false; // the document is a UPnP device description: <root> in the device namespace
    const bool read = readXml(descriptionXml, [&](char kind, const std::string& name, const std::string& ns,
                                                  const std::string& value) {
        if (kind == '<') {
            if (path.empty()) upnpRoot = name == "root" && ns == "urn:schemas-upnp-org:device-1-0";
            path.push_back(name);
            if (name == "service") {
                inService = true;
                type.clear();
                control.clear();
            }
        } else if (kind == '>') {
            if (name == "service" && inService) {
                inService = false;
                if (found.size() < 32) found.emplace_back(trimmed(type), trimmed(control));
            }
            if (!path.empty()) path.pop_back();
        } else if (!path.empty()) {
            if (path.back() == "URLBase" && path.size() == 2) urlBase += value;
            else if (inService && path.back() == "serviceType") type += value;
            else if (inService && path.back() == "controlURL") control += value;
        }
    });
    if (!read || !upnpRoot) return out;
    const std::string base = trimmed(urlBase).empty() ? location : trimmed(urlBase);
    for (const char* wanted : kWanServices) {
        for (const auto& [t, c] : found) {
            if (t != wanted) continue;
            if (const auto u = resolveControl(c, base, gatewayIp)) out.push_back(GatewayService{t, *u});
        }
    }
    return out;
}

namespace {
// urn:schemas-upnp-org:service:WANIPConnection:<n> or WANPPPConnection:<n>, any version.
bool wanConnectionNamespace(const std::string& ns) {
    for (const char* prefix : {"urn:schemas-upnp-org:service:WANIPConnection:", "urn:schemas-upnp-org:service:WANPPPConnection:"}) {
        const std::string p = prefix;
        if (ns.size() <= p.size() || ns.compare(0, p.size(), p) != 0) continue;
        return std::all_of(ns.begin() + static_cast<std::ptrdiff_t>(p.size()), ns.end(), [](char c) { return c >= '0' && c <= '9'; });
    }
    return false;
}
} // namespace

std::string soapExternalAddress(const std::string& responseXml) {
    // Envelope > Body > GetExternalIPAddressResponse (in a WAN connection service's namespace) > NewExternalIPAddress.
    static const std::string kEnvelope = "http://schemas.xmlsoap.org/soap/envelope/";
    std::vector<std::pair<std::string, std::string>> path; // (name, namespace)
    std::string address;
    int seen = 0;
    bool inside = false;
    const bool read = readXml(responseXml, [&](char kind, const std::string& name, const std::string& ns,
                                               const std::string& value) {
        if (kind == '<') {
            path.emplace_back(name, ns);
            inside = path.size() == 4 && path[0] == std::make_pair(std::string("Envelope"), kEnvelope) &&
                     path[1] == std::make_pair(std::string("Body"), kEnvelope) &&
                     path[2].first == "GetExternalIPAddressResponse" && wanConnectionNamespace(path[2].second) &&
                     name == "NewExternalIPAddress";
            if (inside) ++seen;
        } else if (kind == '>') {
            if (!path.empty()) path.pop_back();
            inside = false;
        } else if (inside) {
            address += value;
        }
    });
    address = trimmed(address);
    return read && seen == 1 && validGatewayIp(address) ? address : std::string();
}

namespace {

// One HTTP exchange with the gateway: no proxy, no redirects (one could lead off the gateway), no credentials
// sent, a 200 status, and a body of at most maxBytes, all before deadline (GetTickCount64): each phase gets at most
// 3 s of what is left, and the read loop checks the deadline too. False on anything else, an option that can't be
// set included.
bool gatewayHttp(const GatewayUrl& u, const wchar_t* verb, const std::wstring& headers, const std::string& body,
                 size_t maxBytes, uint64_t deadline, std::string& out) {
    out.clear();
    const auto left = [&]() -> int {
        const uint64_t now = GetTickCount64();
        return now >= deadline ? 0 : static_cast<int>(std::min<uint64_t>(deadline - now, 3'000));
    };
    struct Handle {
        HINTERNET h = nullptr;
        ~Handle() {
            if (h) WinHttpCloseHandle(h);
        }
    } session, connection, request;
    session.h = WinHttpOpen(L"KinectViewer", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return false;
    const int t = left();
    if (t == 0 || !WinHttpSetTimeouts(session.h, t, t, t, t)) return false;
    connection.h = WinHttpConnect(session.h, widen(u.host).c_str(), u.port, 0);
    if (!connection.h) return false;
    request.h = WinHttpOpenRequest(connection.h, verb, widen(u.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request.h) return false;
    DWORD noRedirects = WINHTTP_DISABLE_REDIRECTS;
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirects, sizeof(noRedirects)) ||
        !WinHttpSetOption(request.h, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon)))
        return false;
    if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return false;
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX) ||
        status != 200)
        return false;
    for (;;) {
        if (left() == 0) return false; // a body that drips in can't outlast the deadline
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) return false;
        if (available == 0) break;
        if (out.size() + available > maxBytes) return false;
        const size_t at = out.size();
        out.resize(at + available);
        DWORD got = 0;
        if (!WinHttpReadData(request.h, out.data() + at, available, &got)) return false;
        out.resize(at + got);
        if (got == 0) break;
    }
    return true;
}

} // namespace

std::vector<std::string> askGatewayForWanAt(const std::string& gatewayIp, const std::string& location, uint32_t deadlineMs,
                                            const GatewayTrace& trace) {
    const uint64_t deadline = GetTickCount64() + deadlineMs;
    const auto note = [&](const std::string& line) {
        if (trace) trace(line);
    };
    std::vector<std::string> answers;
    const std::optional<GatewayUrl> description = crackGatewayUrl(location, gatewayIp);
    if (!description) {
        if (trace) note("description " + location + ": not an http URL on the gateway's own address, not fetched");
        return answers;
    }
    std::string xml;
    if (!gatewayHttp(*description, L"GET", L"", "", 64 * 1024, deadline, xml)) {
        if (trace) note("description " + location + ": no usable answer (no connection, not 200, over 64 KB, or past the deadline)");
        return answers;
    }
    const std::vector<GatewayService> services = gatewayServices(xml, location, gatewayIp);
    if (trace) {
        std::string kinds;
        for (const GatewayService& s : services) kinds += (kinds.empty() ? "" : ", ") + s.type;
        note("description " + location + ": " + std::to_string(xml.size()) + " bytes, " + std::to_string(services.size()) +
             " WAN connection service(s) on the gateway" + (kinds.empty() ? "" : ": " + kinds));
    }
    // Every WAN connection service is asked: one may answer 0.0.0.0 or a private address while another holds the
    // public one (the caller keeps only public answers).
    for (const GatewayService& s : services) {
        if (GetTickCount64() >= deadline) {
            if (trace) note("the deadline passed before " + s.type + " was asked");
            break;
        }
        const std::string body = "<?xml version=\"1.0\"?>\r\n"
                                 "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                                 "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                                 "<u:GetExternalIPAddress xmlns:u=\"" +
                                 s.type + "\"></u:GetExternalIPAddress></s:Body></s:Envelope>\r\n";
        const std::wstring headers = L"Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + widen(s.type) +
                                     L"#GetExternalIPAddress\"\r\n";
        std::string answer;
        const auto at = [&] { return "http://" + s.control.host + ":" + std::to_string(s.control.port) + s.control.path; };
        if (!gatewayHttp(s.control, L"POST", headers, body, 16 * 1024, deadline, answer)) {
            if (trace) note(s.type + " at " + at() + ": no usable answer (no connection, not 200, over 16 KB, or past the deadline)");
            continue;
        }
        const std::string wan = soapExternalAddress(answer);
        if (trace)
            note(s.type + " at " + at() + ": " +
                 (wan.empty() ? "answered, but not with one dotted IPv4 in a GetExternalIPAddressResponse" : "answered " + wan));
        if (!wan.empty() && std::find(answers.begin(), answers.end(), wan) == answers.end()) answers.push_back(wan);
    }
    return answers;
}

std::vector<std::string> askGatewayForWan(const std::string& gatewayIp, const std::string& lanIPv4, uint32_t deadlineMs,
                                          std::string& lastGoodLocation, const GatewayTrace& trace) {
    const uint64_t deadline = GetTickCount64() + deadlineMs;
    const auto note = [&](const std::string& line) {
        if (trace) trace(line);
    };
    std::vector<std::string> answers;
    // The description that answered last time, straight to the gateway over TCP: a forged SSDP reply can't stand in
    // the way of it. At most kLastGoodTryMs of the deadline, so an old port that has gone dead (a router restarted
    // on a new port) can't starve the search that would find the new one.
    if (crackGatewayUrl(lastGoodLocation, gatewayIp)) {
        if (trace) note("first, the description that answered last time, straight to the gateway");
        answers = askGatewayForWanAt(gatewayIp, lastGoodLocation, std::min(deadlineMs, kLastGoodTryMs), trace);
        if (!answers.empty()) return answers;
    } else {
        if (trace) note("no description has answered before: searching");
    }
    in_addr lan{}, gateway{};
    if (!validGatewayIp(gatewayIp) || inet_pton(AF_INET, lanIPv4.c_str(), &lan) != 1 ||
        inet_pton(AF_INET, gatewayIp.c_str(), &gateway) != 1) {
        if (trace) note("search: the gateway's or this PC's address isn't a dotted IPv4, nothing sent");
        return answers;
    }
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (trace) note("search: Winsock didn't start");
        return answers;
    }
    SOCKET s = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET && trace) note("search: no UDP socket");
    if (s != INVALID_SOCKET) {
        sockaddr_in from{};
        from.sin_family = AF_INET;
        from.sin_addr = lan;
        DWORD ttl = 2;
        const bool ok = bind(s, reinterpret_cast<const sockaddr*>(&from), sizeof(from)) == 0 &&
                        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&lan), sizeof(lan)) == 0 &&
                        setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl)) == 0;
        const std::string search = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                                   "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
        if (ok) {
            // To the SSDP group on the forward's interface, and to the gateway itself (UPnP 1.1's unicast search).
            sockaddr_in group{};
            group.sin_family = AF_INET;
            group.sin_port = htons(1900);
            inet_pton(AF_INET, "239.255.255.250", &group.sin_addr);
            sockaddr_in direct = group;
            direct.sin_addr = gateway;
            const int toGroup = sendto(s, search.data(), static_cast<int>(search.size()), 0,
                                       reinterpret_cast<const sockaddr*>(&group), sizeof(group));
            const int toGateway = sendto(s, search.data(), static_cast<int>(search.size()), 0,
                                         reinterpret_cast<const sockaddr*>(&direct), sizeof(direct));
            if (trace) note(std::string("search: sent from ") + lanIPv4 + " to the SSDP group (" +
                 (toGroup > 0 ? "sent" : "failed") + ") and to the gateway " + gatewayIp + " (" +
                 (toGateway > 0 ? "sent" : "failed") + ")");
        } else {
            if (trace) note("search: the socket couldn't be bound to " + lanIPv4 + " or set for multicast");
        }
        int replies = 0, counted = 0;
        // Replies until the deadline. Only one sent from the gateway, about the gateway, counts. A UDP source can be
        // forged on the home network, so every distinct location is tried: a forged one at a port that refuses costs
        // little, but one that drops the connection costs up to 3 s, so enough of them can use up the deadline (the
        // pass then reads as unread, and confirms nothing). The first that gives answers ends the search and is kept
        // for next time (the host saves it, so a restart asks it first).
        const uint64_t searchEnd = deadline;
        std::vector<std::string> tried;
        while (ok && answers.empty()) {
            const uint64_t now = GetTickCount64();
            if (now >= searchEnd) break;
            fd_set r;
            FD_ZERO(&r);
            FD_SET(s, &r);
            timeval tv{};
            const uint64_t wait = searchEnd - now;
            tv.tv_sec = static_cast<long>(wait / 1000);
            tv.tv_usec = static_cast<long>((wait % 1000) * 1000);
            if (select(0, &r, nullptr, nullptr, &tv) <= 0) break;
            char buf[2048];
            sockaddr_in src{};
            int srcLen = sizeof(src);
            const int n = recvfrom(s, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&src), &srcLen);
            if (n <= 0) continue;
            char ip[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
            ++replies;
            const std::string reply(buf, static_cast<size_t>(n));
            const std::string location = gatewayLocationFrom(ip, reply, gatewayIp);
            if (location.empty()) {
                if (trace) note(std::string("search reply from ") + ip + ": doesn't count (" +
                     (std::string(ip) != gatewayIp      ? "not sent from the gateway"
                      : ssdpLocation(reply).empty() ? "no LOCATION in a 200 reply"
                                                    : "its LOCATION isn't on the gateway's own address") +
                     ")");
                continue;
            }
            if (std::find(tried.begin(), tried.end(), location) != tried.end()) continue;
            ++counted;
            if (trace) note(std::string("search reply from ") + ip + ": counts, LOCATION " + location);
            tried.push_back(location);
            const uint64_t remaining = deadline > GetTickCount64() ? deadline - GetTickCount64() : 0;
            if (remaining == 0) break;
            answers = askGatewayForWanAt(gatewayIp, location, static_cast<uint32_t>(remaining), trace);
            if (!answers.empty()) lastGoodLocation = location;
        }
        if (trace) note("search: " + std::to_string(replies) + " repl" + (replies == 1 ? "y" : "ies") + ", " + std::to_string(counted) +
             " distinct LOCATION(s) from the gateway tried; " + (answers.empty() ? "no answer by the deadline" : "answered"));
        closesocket(s);
    }
    WSACleanup();
    return answers;
}

std::vector<SsdpSeen> ssdpSurvey(const std::string& lanIPv4, const std::string& gatewayIp, const std::string& st,
                                 uint32_t deadlineMs) {
    std::vector<SsdpSeen> seen;
    in_addr lan{}, gateway{};
    if (inet_pton(AF_INET, lanIPv4.c_str(), &lan) != 1 || inet_pton(AF_INET, gatewayIp.c_str(), &gateway) != 1) return seen;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return seen;
    SOCKET s = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s != INVALID_SOCKET) {
        sockaddr_in from{};
        from.sin_family = AF_INET;
        from.sin_addr = lan;
        DWORD ttl = 2;
        const bool ok = bind(s, reinterpret_cast<const sockaddr*>(&from), sizeof(from)) == 0 &&
                        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&lan), sizeof(lan)) == 0 &&
                        setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl)) == 0;
        if (ok) {
            const std::string search = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                                       "ST: " + st + "\r\n\r\n";
            sockaddr_in group{};
            group.sin_family = AF_INET;
            group.sin_port = htons(1900);
            inet_pton(AF_INET, "239.255.255.250", &group.sin_addr);
            sockaddr_in direct = group;
            direct.sin_addr = gateway;
            sendto(s, search.data(), static_cast<int>(search.size()), 0, reinterpret_cast<const sockaddr*>(&group), sizeof(group));
            sendto(s, search.data(), static_cast<int>(search.size()), 0, reinterpret_cast<const sockaddr*>(&direct), sizeof(direct));
            const uint64_t deadline = GetTickCount64() + deadlineMs;
            for (;;) {
                const uint64_t now = GetTickCount64();
                if (now >= deadline) break;
                fd_set r;
                FD_ZERO(&r);
                FD_SET(s, &r);
                timeval tv{};
                tv.tv_sec = static_cast<long>((deadline - now) / 1000);
                tv.tv_usec = static_cast<long>(((deadline - now) % 1000) * 1000);
                if (select(0, &r, nullptr, nullptr, &tv) <= 0) break;
                char buf[2048];
                sockaddr_in src{};
                int srcLen = sizeof(src);
                const int n = recvfrom(s, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&src), &srcLen);
                if (n <= 0) continue;
                char ip[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
                seen.push_back(SsdpSeen{ip, ssdpHeader(std::string(buf, static_cast<size_t>(n)), "st")});
            }
        }
        closesocket(s);
    }
    WSACleanup();
    return seen;
}

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isHex(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// The length of a dotted IPv4 at line[i] (four runs of 1-3 digits joined by dots, no digit just after), 0 if none.
size_t ipv4At(const std::string& line, size_t i) {
    size_t at = i;
    for (int part = 0; part < 4; ++part) {
        if (part > 0) {
            if (at >= line.size() || line[at] != '.') return 0;
            ++at;
        }
        size_t digits = 0;
        while (at < line.size() && isDigit(line[at]) && digits < 3) {
            ++at;
            ++digits;
        }
        if (digits == 0) return 0;
    }
    return at < line.size() && isDigit(line[at]) ? 0 : at - i;
}

// True when a hardware address ("aa-bb-cc-dd-ee-ff" or with colons, either case) starts at line[i], no hex digit
// just after.
bool macAt(const std::string& line, size_t i) {
    if (i + 17 > line.size()) return false;
    const char sep = line[i + 2];
    if (sep != '-' && sep != ':') return false;
    for (size_t k = 0; k < 17; ++k) {
        if (k % 3 == 2 ? line[i + k] != sep : !isHex(line[i + k])) return false;
    }
    return i + 17 == line.size() || !isHex(line[i + 17]);
}

// True when a UUID (8-4-4-4-12 hex digits) starts at line[i], no hex digit just after.
bool uuidAt(const std::string& line, size_t i) {
    static const size_t kGroups[] = {8, 4, 4, 4, 12};
    size_t at = i;
    for (size_t g = 0; g < 5; ++g) {
        if (g > 0) {
            if (at >= line.size() || line[at] != '-') return false;
            ++at;
        }
        for (size_t k = 0; k < kGroups[g]; ++k, ++at) {
            if (at >= line.size() || !isHex(line[at])) return false;
        }
    }
    return at == line.size() || !isHex(line[at]);
}

} // namespace

std::string maskProbeLine(const std::string& line, std::vector<std::pair<std::string, std::string>>& names,
                          AddressPolicy policy) {
    std::string out;
    size_t i = 0;
    while (i < line.size()) {
        const bool fresh = i == 0 || !isHex(line[i - 1]);
        if (fresh && uuidAt(line, i)) {
            out += "[uuid]";
            i += 36;
            continue;
        }
        if (fresh && macAt(line, i)) {
            out += "[hardware address]";
            i += 17;
            continue;
        }
        const size_t n = (i == 0 || !isDigit(line[i - 1])) ? ipv4At(line, i) : 0;
        if (n == 0) {
            // A run of 8 or more hex digits: a device id, a serial, a hardware address written without separators.
            size_t run = 0;
            while (fresh && i + run < line.size() && isHex(line[i + run])) ++run;
            if (run >= 8) {
                out += "[id]";
                i += run;
                continue;
            }
            out += line[i++];
            continue;
        }
        const std::string ip = line.substr(i, n);
        i += n;
        if (ip == "0.0.0.0") {
            out += ip;
            continue;
        }
        auto named = std::find_if(names.begin(), names.end(), [&](const auto& p) { return p.first == ip; });
        if (named == names.end()) {
            const std::string kind = checkWanAddress(ip, policy) == WanCheck::Public ? "public address" : "non-public address";
            const auto sameKind = std::count_if(names.begin(), names.end(),
                                                [&](const auto& p) { return p.second.rfind(kind + " ", 0) == 0; });
            names.emplace_back(ip, kind + " " + std::to_string(sameKind + 1));
            named = names.end() - 1;
        }
        out += "[" + named->second + "]";
    }
    return out;
}

GatewayIdentity readGatewayOn(const std::string& lanIPv4) {
    GatewayIdentity g;
    in_addr lan{};
    if (inet_pton(AF_INET, lanIPv4.c_str(), &lan) != 1) return g;
    // The interface that owns the forward's LAN address.
    MIB_UNICASTIPADDRESS_TABLE* table = nullptr;
    if (GetUnicastIpAddressTable(AF_INET, &table) != NO_ERROR || !table) return g;
    NET_IFINDEX index = 0;
    NET_LUID luid{};
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_UNICASTIPADDRESS_ROW& row = table->Table[i];
        if (row.Address.si_family == AF_INET && row.Address.Ipv4.sin_addr.s_addr == lan.s_addr) {
            index = row.InterfaceIndex;
            luid = row.InterfaceLuid;
            break;
        }
    }
    FreeMibTable(table);
    if (index == 0) return g;
    // Its default route's next hop (any address off the home network picks it; nothing is sent).
    SOCKADDR_INET source{};
    source.si_family = AF_INET;
    source.Ipv4.sin_family = AF_INET;
    source.Ipv4.sin_addr = lan;
    SOCKADDR_INET dest{};
    dest.si_family = AF_INET;
    dest.Ipv4.sin_family = AF_INET;
    inet_pton(AF_INET, "192.0.2.1", &dest.Ipv4.sin_addr);
    MIB_IPFORWARD_ROW2 route{};
    SOCKADDR_INET best{};
    if (GetBestRoute2(&luid, index, &source, &dest, 0, &route, &best) != NO_ERROR || route.NextHop.si_family != AF_INET ||
        route.NextHop.Ipv4.sin_addr.s_addr == 0)
        return g;
    char ip[INET_ADDRSTRLEN] = {0};
    if (!inet_ntop(AF_INET, &route.NextHop.Ipv4.sin_addr, ip, sizeof(ip))) return g;
    g.ip = ip;
    // Its hardware address on that interface (at most one ARP request).
    MIB_IPNET_ROW2 row{};
    row.Address = route.NextHop;
    row.InterfaceIndex = index;
    row.InterfaceLuid = luid;
    if (ResolveIpNetEntry2(&row, &source) == NO_ERROR && row.PhysicalAddressLength == 6) {
        char mac[18];
        std::snprintf(mac, sizeof(mac), "%02x-%02x-%02x-%02x-%02x-%02x", row.PhysicalAddress[0], row.PhysicalAddress[1],
                      row.PhysicalAddress[2], row.PhysicalAddress[3], row.PhysicalAddress[4], row.PhysicalAddress[5]);
        g.mac = mac;
    }
    return g;
}

bool usableInternalClient(const std::string& ip) {
    const auto a = parseIp(ip);
    if (!a || a->v6) return false;
    const auto& b = a->b;
    return b[0] == 10 || (b[0] == 172 && (b[1] & 0xf0) == 16) || (b[0] == 192 && b[1] == 168);
}

std::optional<std::string> internalClientIPv4() {
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                               GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return std::nullopt;
    std::optional<std::string> best;
    ULONG bestMetric = ULONG_MAX;
    for (auto* ad = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); ad; ad = ad->Next) {
        if (ad->OperStatus != IfOperStatusUp || !ad->FirstGatewayAddress) continue;
        for (auto* u = ad->FirstUnicastAddress; u; u = u->Next) {
            const auto a = fromSockaddr(u->Address.lpSockaddr);
            if (!a) continue;
            const std::string text = toText(*a);
            if (usableInternalClient(text) && ad->Ipv4Metric < bestMetric) {
                best = text;
                bestMetric = ad->Ipv4Metric;
            }
        }
    }
    return best;
}

const char* outsideStatusName(OutsideStatus s) {
    switch (s) {
        case OutsideStatus::Off: return "off";
        case OutsideStatus::Mapped: return "mapped";
        case OutsideStatus::Manual: return "manual";
        case OutsideStatus::Error: return "error";
        case OutsideStatus::CarrierNat: return "carrier_nat";
        case OutsideStatus::AddressChanged: return "address_changed";
    }
    return "error";
}

// --- the mapping flow ---

namespace {

// Ours: this PC is its internal client, on the same port, and it carries our description (the same check the
// start-up leftover removal makes). A mapping on our port to this PC with another
// description is another app's on this PC: "port in use", never adopted, never removed, on any path.
bool ours(const RouterMapping& m, uint16_t port, const std::string& client) {
    return m.internalClient == client && m.internalPort == port && m.description == kMappingDescription;
}

// §8.2 steps 3-4 on one session: reuse this PC's mapping, refuse someone else's, add and read back.
PortMapper::Outcome ensureMapping(RouterSession& s, uint16_t port, const std::string& client) {
    using O = PortMapper::Outcome;
    auto found = s.find(port);
    if (found && !ours(*found, port, client)) return O{O::Taken, *found};
    if (found && !found->enabled) {
        // This PC's own mapping, switched off on the router: put it back as it should be.
        s.remove(port);
        found.reset();
    }
    if (!found) {
        if (!s.add(port, client, kMappingDescription)) return O{O::AddRefused, {}};
        found = s.find(port);
        if (!found || !ours(*found, port, client)) return O{O::NotReadBack, {}};
    }
    return O{O::Mapped, *found};
}

// Removes the mapping only while it is this PC's (never someone else's on the same port).
PortMapper::Outcome removeMapping(RouterSession& s, uint16_t port, const std::string& client) {
    using O = PortMapper::Outcome;
    const auto found = s.find(port);
    if (found && ours(*found, port, client) && s.remove(port)) return O{O::Removed, *found};
    return O{O::Absent, {}};
}

// A leftover from an earlier run: ours only with this PC's internal client and our description (the
// start-up removal's rule: never someone else's mapping on that port).
PortMapper::Outcome removeLeftoverMapping(RouterSession& s, uint16_t port, const std::string& client) {
    using O = PortMapper::Outcome;
    const auto found = s.find(port);
    if (found && ours(*found, port, client) && s.remove(port)) {
        return O{O::Removed, *found};
    }
    return O{O::Absent, {}};
}

const char* kNoLan = "This PC has no home-network address the router can forward to.";

} // namespace

PortMapper::PortMapper(RouterOpener opener) : PortMapper(std::move(opener), Timing{}) {}

PortMapper::PortMapper(RouterOpener opener, Timing timing) : PortMapper(std::move(opener), std::move(timing), AddressPolicy{}) {}

PortMapper::PortMapper(RouterOpener opener, Timing timing, AddressPolicy policy)
    : opener_(std::move(opener)), timing_(std::move(timing)), policy_(policy) {
    if (!timing_.nowMs) timing_.nowMs = &tickMs;
    if (!timing_.sleepMs) timing_.sleepMs = &sleepFor;
    if (!timing_.lanAddress) timing_.lanAddress = &internalClientIPv4;
}

PortMapper::Outcome PortMapper::onWorker(std::function<Outcome(RouterSession&)> work) {
    if (stuck_->load()) return Outcome{Outcome::TimedOut, {}};
    struct Shared {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        bool abandoned = false;
        Outcome out;
    };
    auto shared = std::make_shared<Shared>();
    auto stuck = stuck_;
    auto onUnstuck = onUnstuck_;
    RouterOpener opener = opener_;
    std::thread worker([shared, stuck, onUnstuck, opener = std::move(opener), work = std::move(work)] {
        Outcome out;
        {
            ComScope com;
            std::unique_ptr<RouterSession> s = com.ok() ? opener() : nullptr;
            out = s ? work(*s) : Outcome{Outcome::NoRouter, {}};
        }
        bool wasAbandoned = false;
        {
            std::lock_guard<std::mutex> lock(shared->mu);
            shared->out = out;
            shared->done = true;
            wasAbandoned = shared->abandoned;
            if (wasAbandoned) stuck->store(false); // the router let go: calls may start again
            shared->cv.notify_all();
        }
        // A removal kept while this call was stuck can run now (on the owner's thread: it posts itself one).
        if (wasAbandoned && *onUnstuck) (*onUnstuck)();
    });
    std::unique_lock<std::mutex> lock(shared->mu);
    if (shared->cv.wait_for(lock, std::chrono::milliseconds(timing_.callDeadlineMs), [&] { return shared->done; })) {
        lock.unlock();
        worker.join();
        return shared->out;
    }
    // Given up: never waited on again. It ends when the router lets go of it.
    shared->abandoned = true;
    stuck_->store(true);
    lock.unlock();
    worker.detach();
    return Outcome{Outcome::TimedOut, {}};
}

MappingReport PortMapper::manual(const std::string& why) {
    MappingReport r;
    r.status = OutsideStatus::Manual;
    r.message = why + " Forward TCP port " + std::to_string(port_) + " to " + client_ +
                " in your router's settings, then type your public address here if you know it.";
    return r;
}

MappingReport PortMapper::error(const std::string& why) {
    MappingReport r;
    r.status = OutsideStatus::Error;
    r.message = why;
    return r;
}

MappingReport PortMapper::open(uint16_t port, const std::string& internalClient) {
    port_ = port;
    client_ = internalClient;
    holding_ = false;
    // Wanted again: a removal still kept for this very forward is dropped (open() maps it now).
    pendingRemovals_.erase(std::remove_if(pendingRemovals_.begin(), pendingRemovals_.end(),
                                          [&](const auto& p) { return p.first == port && p.second == internalClient; }),
                           pendingRemovals_.end());
    // §8.3: lastPublicWan_ and an unacknowledged change are kept across a re-open (the 10-minute check after a
    // failed one, a port change, outside listening turned off and on): the address devices were last told is
    // what a new one is compared with, whatever failed in between.
    if (!usableInternalClient(internalClient)) {
        report_ = error(kNoLan);
        return report_;
    }
    const uint64_t start = timing_.nowMs();
    const std::string client = client_;
    Outcome o;
    for (;;) {
        o = onWorker([port, client](RouterSession& s) { return ensureMapping(s, port, client); });
        if (o.kind != Outcome::NoRouter || timing_.nowMs() - start >= timing_.findRouterForMs) break;
        timing_.sleepMs(timing_.retryEveryMs);
    }
    report_ = afterMapping(o, false);
    return report_;
}

MappingReport PortMapper::refresh() {
    if (!holding_) return report_;
    const uint16_t port = port_;
    const std::string old = client_;
    const auto lan = timing_.lanAddress();
    if (!lan || !usableInternalClient(*lan)) {
        removeOrKeep(port, old);
        holding_ = false;
        report_ = error(kNoLan);
        return report_;
    }
    const std::string now = *lan;
    Outcome o;
    if (now != old) {
        // This PC moved on the home network: its old forward goes, and the new address is mapped.
        o = onWorker([port, old, now](RouterSession& s) {
            removeMapping(s, port, old);
            return ensureMapping(s, port, now);
        });
        client_ = now;
    } else {
        o = onWorker([port, now](RouterSession& s) { return ensureMapping(s, port, now); });
    }
    report_ = afterMapping(o, true);
    return report_;
}

MappingReport PortMapper::afterMapping(const Outcome& o, bool fromRefresh) {
    switch (o.kind) {
        case Outcome::NoRouter:
            // A router that stops answering while this PC holds a forward is tried again next check.
            if (!fromRefresh) holding_ = false;
            return manual(fromRefresh ? "The router stopped answering for automatic port forwarding (UPnP)."
                                      : "No router that takes automatic port forwarding (UPnP) was found.");
        case Outcome::TimedOut:
            if (!fromRefresh) holding_ = false;
            return manual("The router didn't answer in time for automatic port forwarding (UPnP).");
        case Outcome::Taken:
            holding_ = false;
            return error(kTaken);
        case Outcome::AddRefused:
            if (!fromRefresh) holding_ = false;
            return manual("The router refused the port forward.");
        case Outcome::NotReadBack:
            if (!fromRefresh) holding_ = false;
            return manual("The router's port forward couldn't be read back.");
        case Outcome::Removed:
        case Outcome::Absent:
        case Outcome::Mapped:
            break;
    }
    MappingReport r;
    r.wanAddress = o.mapping.externalAddress;
    const WanCheck wan = checkWanAddress(r.wanAddress, policy_);
    if (wan != WanCheck::Public) {
        // §8.3: nothing outside can reach this PC, so no forward is left open on the router.
        removeOrKeep(port_, client_);
        holding_ = false;
        r.status = wan == WanCheck::NotConnected ? OutsideStatus::Error : OutsideStatus::CarrierNat;
        r.message = wanCheckSentence(wan, r.wanAddress);
        return r;
    }
    holding_ = true;
    if (!lastPublicWan_.empty() && lastPublicWan_ != r.wanAddress) changeUnacknowledged_ = true;
    lastPublicWan_ = r.wanAddress;
    r.status = changeUnacknowledged_ ? OutsideStatus::AddressChanged : OutsideStatus::Mapped;
    return r;
}

void PortMapper::seedAnnouncedAddress(const std::string& wan) {
    lastPublicWan_ = wan;
    changeUnacknowledged_ = false;
}

void PortMapper::forgetAddress() {
    lastPublicWan_.clear();
    changeUnacknowledged_ = false;
    if (report_.status == OutsideStatus::AddressChanged) report_.status = OutsideStatus::Mapped;
}

bool PortMapper::acknowledgeAddressChange(const std::string& wan) {
    if (!changeUnacknowledged_ || wan.empty() || wan != lastPublicWan_) return false;
    changeUnacknowledged_ = false;
    if (report_.status == OutsideStatus::AddressChanged) report_.status = OutsideStatus::Mapped;
    return true;
}

bool PortMapper::removeOrKeep(uint16_t port, const std::string& client) {
    const auto keep = [&] {
        for (const auto& p : pendingRemovals_) {
            if (p.first == port && p.second == client) return;
        }
        pendingRemovals_.emplace_back(port, client);
    };
    if (stuck_->load()) {
        keep(); // §8.2: an earlier call still holds the router; this runs when it returns
        return false;
    }
    const Outcome o = onWorker([port, client](RouterSession& s) { return removeMapping(s, port, client); });
    if (o.kind == Outcome::TimedOut) keep();
    return o.kind == Outcome::Removed;
}

void PortMapper::setOnUnstuck(std::function<void()> f) { *onUnstuck_ = std::move(f); }

bool PortMapper::retryPendingRemovals() {
    std::vector<std::pair<uint16_t, std::string>> todo;
    todo.swap(pendingRemovals_);
    for (const auto& p : todo) {
        // A port this PC holds again now (outside turned back on) is wanted: it isn't removed.
        if (holding_ && p.first == port_ && p.second == client_) continue;
        removeOrKeep(p.first, p.second);
    }
    return pendingRemovals_.empty();
}

uint16_t PortMapper::mayHoldPort() const {
    if (holding_ && port_ != 0) return port_;
    return pendingRemovals_.empty() ? 0 : pendingRemovals_.front().first;
}

PortMapper::Outcome PortMapper::removeLeftover(uint16_t port, const std::string& internalClient) {
    if (stuck_->load()) return Outcome{Outcome::TimedOut, {}};
    return onWorker([port, internalClient](RouterSession& s) { return removeLeftoverMapping(s, port, internalClient); });
}

bool PortMapper::remove() {
    bool removed = false;
    if (port_ != 0) removed = removeOrKeep(port_, client_);
    report_ = {};
    report_.status = OutsideStatus::Off;
    holding_ = false;
    // lastPublicWan_ and changeUnacknowledged_ stay (§8.3, see open()): a removal is not an acknowledgement.
    return removed;
}

} // namespace kvhost::outside
