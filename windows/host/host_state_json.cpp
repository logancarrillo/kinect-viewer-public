// pairing.json's JSON, apart from host_state.cpp (ACLs, files) so that kv_json carries it: the viewer's
// --state-dir check reads a folder's pairing.json without linking the streaming host.
#include "host_state.hpp"

#include "json_lite.hpp"

namespace kvhost {

namespace {

// A key that may be absent (or null): absent reads as empty.
bool getOptKeyBytes(const json::Value& obj, const char* key, std::vector<uint8_t>& out) {
    out.clear();
    const json::Value* v = obj.get(key);
    if (!v || v->type == json::Value::Type::Null) return true;
    return v->asBytes(out);
}

} // namespace

std::string toJson(const HostPersistedState& s) {
    // The key as it is to be written: plain (privateKeyRaw), wrapped (privateKeyWrapped), or both while a
    // migration checks the wrapped one (host_state.cpp). Whichever is empty is left out.
    json::ObjectWriter id;
    if (!s.privateKeyRaw.empty()) id.bytes("privateKeyRaw", s.privateKeyRaw);
    if (!s.privateKeyWrapped.empty()) id.bytes("privateKeyWrapped", s.privateKeyWrapped);
    const std::string ident = id.bytes("certificateDER", s.certificateDER).finish();
    json::ObjectWriter w;
    w.raw("serverIdentity", ident).field("serviceId", s.serviceId);
    if (s.pinnedClientCertSHA256) w.bytes("pinnedClientCertSHA256", *s.pinnedClientCertSHA256);
    w.field("allowPhoneModeSwitch", s.allowPhoneModeSwitch)
        .field("allowPhoneSensorPowerControl", s.allowPhoneSensorPowerControl)
        .field("turnOffKinectOnPhoneDisconnect", s.turnOffKinectOnPhoneDisconnect);
    return w.finish();
}

bool fromJson(const std::string& text, HostPersistedState& s) {
    json::Value v;
    if (!json::parse(text, v) || v.type != json::Value::Type::Object) return false;
    const json::Value* ident = v.get("serverIdentity");
    // The key plain (as every build before the wrap wrote it), wrapped, or both; never neither. A wrapped one
    // is unwrapped by loadHostStateChecked, not here: this file is read by the viewer's --state-dir check too.
    if (!ident || ident->type != json::Value::Type::Object ||
        !getOptKeyBytes(*ident, "privateKeyRaw", s.privateKeyRaw) ||
        !getOptKeyBytes(*ident, "privateKeyWrapped", s.privateKeyWrapped) ||
        (s.privateKeyRaw.empty() && s.privateKeyWrapped.empty()) ||
        !json::getBytes(*ident, "certificateDER", s.certificateDER) || !json::getString(v, "serviceId", s.serviceId) ||
        !json::getBool(v, "allowPhoneModeSwitch", s.allowPhoneModeSwitch)) {
        return false;
    }
    // A file written before phase 7 has neither key: the defaults apply, as decodeIfPresent ?? does.
    std::optional<bool> allow_power, off_on_disconnect;
    if (!json::getOptBool(v, "allowPhoneSensorPowerControl", allow_power) ||
        !json::getOptBool(v, "turnOffKinectOnPhoneDisconnect", off_on_disconnect)) {
        return false;
    }
    s.allowPhoneSensorPowerControl = allow_power.value_or(true);
    s.turnOffKinectOnPhoneDisconnect = off_on_disconnect.value_or(false);
    const json::Value* pin = v.get("pinnedClientCertSHA256");
    if (pin && pin->type != json::Value::Type::Null) {
        std::vector<uint8_t> b;
        if (!pin->asBytes(b)) return false;
        s.pinnedClientCertSHA256 = std::move(b);
    } else {
        s.pinnedClientCertSHA256.reset();
    }
    return true;
}

} // namespace kvhost
