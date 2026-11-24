#include "roll_wire.hpp"

namespace kvhost {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// The value of n digits at s[at], or -1 if any of them isn't a digit.
int digitsAt(const std::string& s, size_t at, size_t n) {
    if (at + n > s.size()) return -1;
    int v = 0;
    for (size_t i = at; i < at + n; ++i) {
        if (!isDigit(s[i])) return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

bool isLeapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int daysInMonth(int y, int m) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && isLeapYear(y) ? 29 : kDays[m - 1];
}

// "YYYY-MM-DD" at s[0..10), a real date.
bool dayPrefixValid(const std::string& s) {
    if (s.size() < 10 || s[4] != '-' || s[7] != '-') return false;
    const int y = digitsAt(s, 0, 4);
    const int mo = digitsAt(s, 5, 2);
    const int d = digitsAt(s, 8, 2);
    if (y < 1 || mo < 1 || mo > 12 || d < 1) return false; // year 0000 is no real date: refused, as roll_store did
    return d <= daysInMonth(y, mo);
}

bool isBase64UrlChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || isDigit(c) || c == '-' || c == '_';
}

} // namespace

bool isValidRollDay(const std::string& day) { return day.size() == 10 && dayPrefixValid(day); }

bool isValidClipId(const std::string& id) {
    // YYYY-MM-DD/HH-MM-SS_ is 20 characters; the shortest trigger ("manual", "motion") makes 26 and the
    // longest id ("schedule" and "-999") 32.
    if (id.size() < 26 || id.size() > 32 || !dayPrefixValid(id) || id[10] != '/' || id[13] != '-' || id[16] != '-' ||
        id[19] != '_') {
        return false;
    }
    const int hh = digitsAt(id, 11, 2);
    const int mm = digitsAt(id, 14, 2);
    const int ss = digitsAt(id, 17, 2);
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59) return false;
    size_t at = 20;
    bool trigger = false;
    for (const char* t : {"manual", "motion", "schedule"}) {
        const std::string name(t);
        if (id.compare(at, name.size(), name) == 0) {
            at += name.size();
            trigger = true;
            break;
        }
    }
    if (!trigger) return false;
    if (at == id.size()) return true;
    // (-[0-9]{1,3})?, exactly as §7.1 writes it.
    if (id[at] != '-') return false;
    const size_t n = id.size() - at - 1;
    return n >= 1 && n <= 3 && digitsAt(id, at + 1, n) >= 0;
}

std::string clipIdDay(const std::string& clipId) { return isValidClipId(clipId) ? clipId.substr(0, 10) : std::string(); }

bool isWellShapedRollCursor(const std::string& c) {
    if (c.empty() || c.size() > kMaxRollCursorChars) return false;
    size_t body = c.size();
    while (body > 0 && c.size() - body < 2 && c[body - 1] == '=') --body;
    if (body == 0) return false;
    for (size_t i = 0; i < body; ++i) {
        if (!isBase64UrlChar(c[i])) return false;
    }
    // Base64 never ends one character into a group, and padding only ever completes a group.
    if (body % 4 == 1) return false;
    return body == c.size() || c.size() % 4 == 0;
}

const char* checkRollListRequest(const RollListRequestMessage& m) {
    if (m.day && !isValidRollDay(*m.day)) return pet_error::kNotFound;
    if (m.cursor && !isWellShapedRollCursor(*m.cursor)) return pet_error::kInvalid;
    if (m.limit && (*m.limit < kRollListMinLimit || *m.limit > kRollListMaxLimit)) return pet_error::kInvalid;
    return nullptr;
}

int rollListLimit(const RollListRequestMessage& m) { return m.limit.value_or(kRollListDefaultLimit); }

const char* checkRollThumbRequest(const RollThumbRequestMessage& m) {
    return isValidClipId(m.clipId) ? nullptr : pet_error::kNotFound;
}

const char* checkPlaybackRequest(const PlaybackRequestMessage& m) {
    if (!playback_action::isKnown(m.action)) return pet_error::kUnsupported;
    if (m.action == playback_action::kStart) {
        if (!m.clipId) return pet_error::kInvalid;
        return isValidClipId(*m.clipId) ? nullptr : pet_error::kNotFound;
    }
    if (!m.streamId) return pet_error::kInvalid;
    if (m.action == playback_action::kSeek && !m.positionSec) return pet_error::kInvalid;
    return nullptr;
}

double clampPlaybackPosition(double positionSec, double durationSec) {
    // NaN can't arrive in JSON; the comparisons are written so it would land on 0 anyway.
    if (!(positionSec > 0.0)) return 0.0;
    if (!(durationSec > 0.0)) return 0.0;
    return positionSec < durationSec ? positionSec : durationSec;
}

bool isRollAction(const std::string& a) {
    return a == pet_action::kSetRoll || a == pet_action::kSetMotion || a == pet_action::kStartClip ||
           a == pet_action::kStopClip || a == pet_action::kStarClip || a == pet_action::kDeleteClip;
}

const char* parseRollCommand(const PetCameraCommandMessage& m, RollCommand& out) {
    out = RollCommand{};
    const std::string& a = m.action;
    if (a == pet_action::kSetRoll) {
        if (!m.enabled) return pet_error::kInvalid;
        out.kind = RollCommandKind::SetRoll;
        out.enabled = m.enabled;
        return nullptr;
    }
    if (a == pet_action::kSetMotion) {
        if (!m.enabled && !m.sensitivity) return pet_error::kInvalid;
        if (m.sensitivity && (*m.sensitivity < 1 || *m.sensitivity > 10)) return pet_error::kInvalid;
        out.kind = RollCommandKind::SetMotion;
        out.enabled = m.enabled;
        out.sensitivity = m.sensitivity;
        return nullptr;
    }
    if (a == pet_action::kStartClip || a == pet_action::kStopClip) {
        out.kind = a == pet_action::kStartClip ? RollCommandKind::StartClip : RollCommandKind::StopClip;
        return nullptr;
    }
    if (a == pet_action::kStarClip || a == pet_action::kDeleteClip) {
        const bool star = a == pet_action::kStarClip;
        if (!m.clipId || (star && !m.starred)) return pet_error::kInvalid;
        if (!isValidClipId(*m.clipId)) return pet_error::kNotFound;
        out.kind = !star ? RollCommandKind::Delete : (*m.starred ? RollCommandKind::Star : RollCommandKind::Unstar);
        out.clipId = *m.clipId;
        return nullptr;
    }
    return pet_error::kUnsupported;
}

CommandResultMessage makeCommandError(uint64_t requestId, const std::string& code) {
    CommandResultMessage r;
    r.requestId = requestId;
    r.ok = false;
    r.error = code;
    if (const char* s = text::sentenceFor(code)) r.message = s;
    return r;
}

void setRollBlocked(PetCameraStateMessage& m, const std::optional<std::string>& reason) {
    m.rollBlockedReason = reason;
    m.rollBlockedText.reset();
    if (reason) {
        if (const char* s = text::sentenceFor(*reason)) m.rollBlockedText = s;
    }
}

} // namespace kvhost
