// W7 camera roll on the wire (docs/protocol-v3-pet-camera.md §5.1, §5.2, §5.6-5.9, §7.1, §9): the
// checks on what a device sends, after its JSON has decoded.
//
// The split follows v3 §5.10. wire_protocol's fromJson checks shape only: bad JSON, a wrong type or
// a missing required key fails, and the message is ignored without an answer. A message that decodes
// is well-formed and gets exactly one answer (§5), so the functions here return the §9 code for its
// 0x0D, or nullptr when the request may go on to the roll. They never touch the disk: whether a clip
// or a day is in the host's index is the roll store's question, asked only after these pass.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "wire_protocol.hpp"

namespace kvhost {

// §5.6: limit is 1-100 and defaults to 50.
constexpr int kRollListMinLimit = 1;
constexpr int kRollListMaxLimit = 100;
constexpr int kRollListDefaultLimit = 50;
// A cursor is the host's own base64url of "<startUnixMs>:<clipId>" (at most 53 bytes, so 72
// characters) or of "<day>" (14 characters). Anything much longer can't have come from a nextCursor.
constexpr size_t kMaxRollCursorChars = 128;
// RollClip.startedBy for a clip started on the PC (pet-camera.md §6.3).
constexpr const char* kStartedByThisPc = "This PC";

// §5.6: ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ and a real date (month 1-12, a day that month has, leap years
// included).
bool isValidRollDay(const std::string& day);

// §7.1: ^[0-9]{4}-[0-9]{2}-[0-9]{2}/[0-9]{2}-[0-9]{2}-[0-9]{2}_(manual|motion|schedule)(-[0-9]{1,3})?$,
// with a real date and a time of day (00-23, 00-59, 00-59). The pattern alone keeps a device's id
// from ever naming a path outside the roll (one '/', no '.', no '\\'); the date and time checks fail
// closed on ids no clip can have, which the index would refuse anyway.
bool isValidClipId(const std::string& clipId);

// The day part of a valid clip id ("2026-11-18" of "2026-11-18/14-03-22_motion"), or "" when the id
// isn't valid.
std::string clipIdDay(const std::string& clipId);

// §5.6 cursor shape, as far as the wire can tell: 1 to kMaxRollCursorChars characters of the base64url
// alphabet (A-Z a-z 0-9 - _), with at most two '=' of padding at the end. Decoding it, and checking
// the decoded form against the clip-id or day pattern, is the roll store's job; it answers "invalid"
// there too.
bool isWellShapedRollCursor(const std::string& cursor);

// 0x12: the 0x0D code for a well-formed request, or nullptr.
//  - day present and not a valid day: not_found (§5.6);
//  - cursor present and not well shaped: invalid (§5.6);
//  - limit present and outside 1-100: invalid (decided here; the spec gives the range but not the
//    code, and "invalid" is what set_motion's out-of-range sensitivity gets).
// Checked in that order, so a request with several faults gets the first.
const char* checkRollListRequest(const RollListRequestMessage& m);
// limit, or 50 when it was left out. Only meaningful after checkRollListRequest passed.
int rollListLimit(const RollListRequestMessage& m);

// 0x14: not_found for a clip id that isn't valid (§5.8, §7.1), otherwise nullptr.
const char* checkRollThumbRequest(const RollThumbRequestMessage& m);

// 0x16 (§5.9), before any state is looked at:
//  - an unknown action: unsupported (§1 rule 6);
//  - start without clipId, any other action without streamId, or seek without positionSec: invalid
//    (decided here: the keys are optional in the JSON, so the message is well-formed and is
//    answered, and "invalid" names a request that can't be acted on);
//  - start with a clip id that isn't valid: not_found (§7.1).
// Whether streamId is this session's current stream (otherwise not_found) is the session's question.
const char* checkPlaybackRequest(const PlaybackRequestMessage& m);
// §5.9: positionSec clamped to [0, durationSec]. A start without one starts at 0.
double clampPlaybackPosition(double positionSec, double durationSec);

// ---- 0x0C, the roll actions (§5.2) -------------------------------------------------------------

enum class RollCommandKind { SetRoll, SetMotion, StartClip, StopClip, Star, Unstar, Delete };

// A 0x0C roll action with its keys checked, ready for the host's role checks and the roll.
struct RollCommand {
    RollCommandKind kind = RollCommandKind::StartClip;
    std::optional<bool> enabled;    // SetRoll (always set); SetMotion (when sent)
    std::optional<int> sensitivity; // SetMotion (when sent; 1-10)
    std::string clipId;             // Star, Unstar, Delete (a valid §7.1 id)
};

// set_roll, set_motion, start_clip, stop_clip, star_clip and delete_clip.
bool isRollAction(const std::string& action);

// For a 0x0C whose action is a roll action: the 0x0D code, or nullptr with out filled in.
//  - set_roll without enabled: invalid (as W6 answers set_sharing and set_privacy without it);
//  - set_motion with neither key, or with sensitivity outside 1-10: invalid (§5.2);
//  - star_clip without clipId or starred, delete_clip without clipId: invalid;
//  - star_clip or delete_clip with a clip id that isn't valid: not_found (§7.1);
//  - start_clip and stop_clip take no keys; any sent are ignored.
// Any other action gets "unsupported" here; the caller sends only roll actions.
const char* parseRollCommand(const PetCameraCommandMessage& m, RollCommand& out);

// ---- answers ----------------------------------------------------------------------------------

// A failed 0x0D: ok false, the code, and the code's §9 sentence when it has one.
CommandResultMessage makeCommandError(uint64_t requestId, const std::string& code);

// 0x0B (§5.1): sets rollBlockedReason and its §9 rollBlockedText together, or clears both when
// reason is unset, so the two can't disagree.
void setRollBlocked(PetCameraStateMessage& m, const std::optional<std::string>& reason);

} // namespace kvhost
