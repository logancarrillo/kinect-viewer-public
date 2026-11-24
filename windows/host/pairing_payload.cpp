#include "pairing_payload.hpp"

#include "crypto_identity.hpp"
#include "json_lite.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>

namespace kvhost {

namespace {

bool isUnreservedQueryChar(unsigned char c) {
    // Characters URLComponents leaves alone inside a query item value.
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~' || c == '!' || c == '$' || c == '\'' || c == '(' || c == ')' || c == '*' || c == ',' ||
           c == ';' || c == ':' || c == '@' || c == '/' || c == '?';
}

std::string percentEncodeQueryValue(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (isUnreservedQueryChar(c)) {
            o += static_cast<char>(c);
        } else {
            char b[4];
            std::snprintf(b, sizeof(b), "%%%02X", c);
            o += b;
        }
    }
    return o;
}

std::string percentDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const std::string hx = s.substr(i + 1, 2);
            char* e = nullptr;
            const long v = std::strtol(hx.c_str(), &e, 16);
            if (e && *e == 0) {
                o += static_cast<char>(v);
                i += 2;
                continue;
            }
        }
        o += s[i];
    }
    return o;
}

std::string trimWhitespace(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

} // namespace

std::string base64URL(const std::vector<uint8_t>& data) {
    std::string s = base64Encode(data.data(), data.size());
    for (char& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

bool fromBase64URL(const std::string& str, std::vector<uint8_t>& out) {
    std::string s = str;
    for (char& c : s) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    const size_t rem = s.size() % 4;
    if (rem > 0) s.append(4 - rem, '=');
    return base64Decode(s, out);
}

std::string QRPairingPayload::toURIString() const {
    return "kv2://pair?s=" + percentEncodeQueryValue(serviceId) + "&fp=" + percentEncodeQueryValue(base64URL(serverCertSHA256)) +
           "&tok=" + percentEncodeQueryValue(base64URL(oneTimeToken)) + "&h=" + percentEncodeQueryValue(hostIP) +
           "&p=" + std::to_string(pairPort);
}

bool QRPairingPayload::parse(const std::string& raw, QRPairingPayload& out) {
    const std::string uri = trimWhitespace(raw);
    const std::string prefix = "kv2://pair";
    if (uri.compare(0, prefix.size(), prefix) != 0) return false;
    std::string rest = uri.substr(prefix.size());
    if (!rest.empty() && rest[0] == '/') {
        // URLComponents puts "/..." in path; a path is allowed but ignored.
        const size_t q = rest.find('?');
        rest = (q == std::string::npos) ? std::string() : rest.substr(q);
    }
    if (rest.empty() || rest[0] != '?') return false;
    std::map<std::string, std::string> dict;
    size_t pos = 1;
    while (pos <= rest.size()) {
        const size_t amp = rest.find('&', pos);
        const std::string item = rest.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const size_t eq = item.find('=');
        if (eq != std::string::npos) dict[percentDecode(item.substr(0, eq))] = percentDecode(item.substr(eq + 1));
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    QRPairingPayload q;
    q.serviceId = dict["s"];
    q.hostIP = dict["h"];
    const std::string p = dict["p"];
    if (q.serviceId.empty() || q.hostIP.empty() || p.empty() || p.size() > 5) return false;
    for (char c : p) {
        if (c < '0' || c > '9') return false;
    }
    const long port = std::strtol(p.c_str(), nullptr, 10);
    if (port <= 0 || port > 65535) return false;
    q.pairPort = static_cast<uint16_t>(port);
    if (!fromBase64URL(dict["fp"], q.serverCertSHA256) || q.serverCertSHA256.size() != 32) return false;
    if (!fromBase64URL(dict["tok"], q.oneTimeToken) || q.oneTimeToken.size() < 16) return false;
    out = std::move(q);
    return true;
}

std::string normalizeShortCode(const std::string& code) {
    std::string d;
    for (char c : code) {
        if (c >= '0' && c <= '9') d += c;
    }
    return d;
}

void shortCodeFromBytes(const uint8_t b[4], std::string& formatted, std::string& normalized) {
    const uint32_t v = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
                       (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%08u", static_cast<unsigned>(v % 100000000u));
    normalized = digits;
    formatted = normalized.substr(0, 4) + "-" + normalized.substr(4);
}

bool generateShortCode(std::string& formatted, std::string& normalized) {
    uint8_t b[4];
    if (!randomBytes(b, 4)) return false;
    shortCodeFromBytes(b, formatted, normalized);
    return true;
}

std::string randomHexId(size_t n) {
    std::vector<uint8_t> b(n);
    if (!randomBytes(b.data(), n)) return {};
    return hexLower(b.data(), b.size());
}

} // namespace kvhost
