#include "host_state.hpp"

#include "crypto_identity.hpp"
#include "pairing_payload.hpp"
#include "state_files.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <wincrypt.h>
#include <dpapi.h> // CryptProtectData (crypt32)

#ifdef KV_TESTING
#include <atomic>
#include <functional>
#include <mutex>
#endif

namespace kvhost {

// pairing.json's JSON (toJson / fromJson) is in host_state_json.cpp, which the viewer's --state-dir
// check links without the rest of the host.

std::wstring defaultHostStateDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".host-state";
    return std::wstring(buf, n) + L"\\kinect-viewer\\.host-state";
}

std::string newHostCommonName() {
    const std::string cn = randomHexId(8);
    return cn.empty() ? std::string("host") : cn; // no random bytes: generate() fails on its own key anyway
}

namespace {

std::wstring currentUserSidString() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return {};
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    std::vector<uint8_t> buf(len);
    std::wstring out;
    if (GetTokenInformation(tok, TokenUser, buf.data(), len, &len)) {
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
            out = s;
            LocalFree(s);
        }
    }
    CloseHandle(tok);
    return out;
}

std::string lastErrorText(const char* what) {
    return std::string(what) + " (Win32 error " + std::to_string(GetLastError()) + ")";
}

// The host key at rest: wrapped with DPAPI for this PC (machine scope, CRYPTPROTECT_LOCAL_MACHINE) with this
// app's entropy. A copy of the state folder alone, taken to another PC or Windows install (a backup of it), holds
// no key that can be used; a full image of the system disk carries this PC's DPAPI secrets with it. On this PC the
// folder's owner-only ACL keeps non-admin users from reading the file; an administrator can read it.
const char kHostKeyEntropy[] = "kinect-viewer host identity key v1";

#ifdef KV_TESTING
// Only a test program's own copy of this file, built with KV_TESTING, has these (host_state.hpp).
// setHostKeyDpapiErrorsForTest: 0 is DPAPI's own answer.
std::atomic<unsigned long> g_wrap_error_for_test{0};
std::atomic<unsigned long> g_unwrap_error_for_test{0};
// setHostKeyBeforePlainSaveForTest: run between a plain save's check and its write.
std::mutex g_before_plain_save_mutex;
std::function<void()> g_before_plain_save_for_test;
// setHostKeyReplaceFaultForTest: the n-th pairing.json replace from then on fails (0: none armed).
std::atomic<int> g_replace_fault_call_for_test{0};
std::atomic<unsigned long> g_replace_fault_error_for_test{0};
#endif

// This process's pairing.json.tmp.<pid>: two hosts on the same folder never write each other's temp file.
std::wstring pairingTmpPath(const std::wstring& dir) {
    return dir + L"\\pairing.json.tmp." + std::to_wstring(GetCurrentProcessId());
}

// ReplaceFileW for pairing.json. Under KV_TESTING, setHostKeyReplaceFaultForTest can make one call fail as Windows
// can leave it midway: ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 with a backup name (the replaced file moved to it, the new
// one not in), or ERROR_UNABLE_TO_MOVE_REPLACEMENT without one (the replaced file gone, the new one not in).
BOOL replacePairingFile(const std::wstring& replaced, const std::wstring& replacement, const wchar_t* backup) {
#ifdef KV_TESTING
    if (g_replace_fault_call_for_test.load() > 0 && g_replace_fault_call_for_test.fetch_sub(1) == 1) {
        const unsigned long e = g_replace_fault_error_for_test.load();
        if (e == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 && backup) {
            MoveFileExW(replaced.c_str(), backup, 0);
        } else if (e == ERROR_UNABLE_TO_MOVE_REPLACEMENT && !backup) {
            DeleteFileW(replaced.c_str());
        }
        SetLastError(e);
        return FALSE;
    }
#endif
    return ReplaceFileW(replaced.c_str(), replacement.c_str(), backup, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr,
                        nullptr);
}

// After a replace that failed: Windows can have moved pairing.json away without moving the new file in. When
// pairing.json is missing and pairing.json.prev is there, .prev goes back as pairing.json, so no start ever reads
// the folder as a first run (a new identity, and every paired device lost). True when pairing.json is there after.
bool putPrevBackIfPairingGone(const std::wstring& final_path, const std::wstring& prev_path) {
    if (fileState(final_path) == FileState::Missing && fileState(prev_path) == FileState::Present) {
        MoveFileExW(prev_path.c_str(), final_path.c_str(), MOVEFILE_WRITE_THROUGH);
    }
    return fileState(final_path) == FileState::Present;
}

DATA_BLOB hostKeyEntropy() {
    return DATA_BLOB{static_cast<DWORD>(sizeof(kHostKeyEntropy) - 1),
                     reinterpret_cast<BYTE*>(const_cast<char*>(kHostKeyEntropy))};
}

bool wrapHostKey(const std::vector<uint8_t>& key, std::vector<uint8_t>& out, std::string* err) {
    out.clear();
    if (key.empty()) {
        if (err) *err = "there is no key to wrap";
        return false;
    }
#ifdef KV_TESTING
    if (const unsigned long forced = g_wrap_error_for_test.load()) {
        if (err) *err = "the key couldn't be wrapped: CryptProtectData (Win32 error " + std::to_string(forced) + ")";
        return false;
    }
#endif
    DATA_BLOB in{static_cast<DWORD>(key.size()), const_cast<BYTE*>(key.data())};
    DATA_BLOB entropy = hostKeyEntropy();
    DATA_BLOB blob{};
    if (!CryptProtectData(&in, nullptr, &entropy, nullptr, nullptr,
                          CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &blob)) {
        if (err) *err = lastErrorText("the key couldn't be wrapped: CryptProtectData");
        return false;
    }
    out.assign(blob.pbData, blob.pbData + blob.cbData);
    LocalFree(blob.pbData);
    return true;
}

// error: the Win32 error when it fails (0 for an empty input).
bool unwrapHostKey(const std::vector<uint8_t>& wrapped, std::vector<uint8_t>& out, unsigned long* error = nullptr) {
    out.clear();
    if (error) *error = 0;
    if (wrapped.empty()) return false;
#ifdef KV_TESTING
    if (const unsigned long forced = g_unwrap_error_for_test.load()) {
        if (error) *error = forced;
        return false;
    }
#endif
    DATA_BLOB in{static_cast<DWORD>(wrapped.size()), const_cast<BYTE*>(wrapped.data())};
    DATA_BLOB entropy = hostKeyEntropy();
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
        if (error) *error = GetLastError();
        return false;
    }
    out.assign(plain.pbData, plain.pbData + plain.cbData);
    SecureZeroMemory(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
    return true;
}

bool unwrapForCheck(const std::vector<uint8_t>& wrapped, std::vector<uint8_t>& out) {
    return unwrapHostKey(wrapped, out);
}

using ReadBack = bool (*)(const std::vector<uint8_t>& wrapped, std::vector<uint8_t>& key);

// A wrap of key that unwraps to key again, or false (err).
bool wrapAndCheck(const std::vector<uint8_t>& key, std::vector<uint8_t>& wrapped, std::string* err) {
    if (!wrapHostKey(key, wrapped, err)) return false;
    std::vector<uint8_t> again;
    if (!unwrapHostKey(wrapped, again) || again != key) {
        if (err) *err = "the wrapped key didn't unwrap to the same key";
        wrapped.clear();
        return false;
    }
    return true;
}

// Writes this process's pairing.json.tmp.<pid> (owner-only) with text, flushed to disk.
bool writePairingTmp(const std::wstring& dir, const std::string& text, std::string* err) {
    if (!ensureOwnerOnlyDirectory(dir, err)) return false;
    const std::wstring tmp_path = pairingTmpPath(dir);
    HANDLE h = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = lastErrorText("CreateFileW pairing.json.tmp");
        return false;
    }
    DWORD written = 0;
    const BOOL wrote = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    // A flush that fails is a write that failed: the rename below must never put a file in place that isn't on disk.
    const BOOL flushed = wrote && FlushFileBuffers(h);
    CloseHandle(h);
    if (!flushed || !applyOwnerOnlyAcl(tmp_path, false, err)) {
        DeleteFileW(tmp_path.c_str());
        if (err && err->empty()) *err = wrote ? "flushing pairing.json.tmp failed" : "write failed";
        return false;
    }
    return true;
}

// Writes pairing.json.tmp.<pid> (owner-only) with text, then atomically replaces pairing.json.
bool writePairingText(const std::wstring& dir, const std::string& text, std::string* err) {
    if (!writePairingTmp(dir, text, err)) return false;
    const std::wstring final_path = dir + L"\\pairing.json";
    const std::wstring tmp_path = pairingTmpPath(dir);
    if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (err) *err = lastErrorText("MoveFileExW");
        DeleteFileW(tmp_path.c_str());
        return false;
    }
    return true;
}

// A plain save's write (DPAPI not working; saveHostStateAtomically). Another start of the host on the same folder
// can wrap the key between the save's check and this write (its migration), and a plain key must never land over
// that wrap. So pairing.json is replaced keeping what it replaced as pairing.json.prev, and when that holds a
// wrapped key and no plain one, it goes back as it was and the save fails. With no pairing.json at the check, it's a
// move that never replaces a pairing.json written meanwhile. A replace that fails midway never leaves the folder
// without pairing.json while .prev holds it (putPrevBackIfPairingGone).
bool writePlainPairingTextRechecked(const std::wstring& dir, const std::string& text, bool file_was_there,
                                    std::string* err) {
    if (!writePairingTmp(dir, text, err)) return false;
    const std::wstring final_path = dir + L"\\pairing.json";
    const std::wstring tmp_path = pairingTmpPath(dir);
    const std::wstring prev_path = dir + L"\\pairing.json.prev";
    if (!file_was_there) {
        if (MoveFileExW(tmp_path.c_str(), final_path.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        const DWORD e = GetLastError();
        DeleteFileW(tmp_path.c_str());
        if (err) {
            // The refusal is the file being there now, whatever the error said.
            *err = fileState(final_path) == FileState::Present
                       ? "a pairing.json was written while the key was being saved plain, so that save is refused "
                         "and the new file is as it was"
                       : "MoveFileExW (Win32 error " + std::to_string(e) + ")";
        }
        return false;
    }
    DeleteFileW(prev_path.c_str());
    if (!replacePairingFile(final_path, tmp_path, prev_path.c_str())) {
        const DWORD e = GetLastError();
        DeleteFileW(tmp_path.c_str());
        const bool there = putPrevBackIfPairingGone(final_path, prev_path);
        if (err) {
            *err = "ReplaceFileW pairing.json (Win32 error " + std::to_string(e) + ")" +
                   (there ? ": pairing.json is as it was" : ": pairing.json is missing");
        }
        return false;
    }
    std::string before;
    HostPersistedState was;
    const bool wrapped_meanwhile = readWholeFile(prev_path, before) && fromJson(before, was) &&
                                   was.privateKeyRaw.empty() && !was.privateKeyWrapped.empty();
    if (!wrapped_meanwhile) {
        DeleteFileW(prev_path.c_str());
        return true;
    }
    if (!replacePairingFile(final_path, prev_path, nullptr)) {
        const DWORD e = GetLastError();
        putPrevBackIfPairingGone(final_path, prev_path);
        if (err) {
            *err = fileState(prev_path) == FileState::Missing
                       ? "pairing.json was wrapped (another start of the host on this folder) while the key was being "
                         "saved plain, so that save is refused and the wrapped file is back as it was"
                       : "pairing.json was wrapped while the key was being saved plain, and putting the wrapped one "
                         "back failed (ReplaceFileW, Win32 error " + std::to_string(e) +
                             "), so it is kept as pairing.json.prev; the next start sorts it out";
        }
        return false;
    }
    if (err) {
        *err = "pairing.json was wrapped (another start of the host on this folder) while the key was being saved "
               "plain, so that save is refused and the wrapped file is back as it was";
    }
    return false;
}
// pairing.json read back from disk: its wrapped key unwraps (read_back) to key, and, when want_plain is set,
// its plain key is key too; without it, the file must hold no plain key. False (err) otherwise.
bool readBackChecks(const std::wstring& dir, const std::vector<uint8_t>& key, bool want_plain, ReadBack read_back,
                    std::string* err) {
    std::string text;
    HostPersistedState back;
    if (!readWholeFile(dir + L"\\pairing.json", text)) {
        if (err) *err = lastErrorText("pairing.json couldn't be read back");
        return false;
    }
    if (!fromJson(text, back)) {
        if (err) *err = "pairing.json didn't parse when read back";
        return false;
    }
    if (want_plain ? back.privateKeyRaw != key : !back.privateKeyRaw.empty()) {
        if (err) *err = want_plain ? "pairing.json's plain key wasn't the key when read back"
                                   : "pairing.json still held the key plain when read back";
        return false;
    }
    std::vector<uint8_t> unwrapped;
    if (!read_back(back.privateKeyWrapped, unwrapped) || unwrapped != key) {
        if (err) *err = "pairing.json's wrapped key didn't unwrap to the key when read back";
        return false;
    }
    return true;
}

// pairing.json read back from disk holds key plain (a save while DPAPI isn't working). False (err) otherwise.
bool readBackPlain(const std::wstring& dir, const std::vector<uint8_t>& key, std::string* err) {
    std::string text;
    HostPersistedState back;
    if (!readWholeFile(dir + L"\\pairing.json", text) || !fromJson(text, back) || back.privateKeyRaw != key) {
        if (err) *err = "pairing.json didn't hold the key when read back";
        return false;
    }
    return true;
}

} // namespace

bool applyOwnerOnlyAcl(const std::wstring& path, bool is_directory, std::string* err) {
    const std::wstring sid = currentUserSidString();
    if (sid.empty()) {
        if (err) *err = "cannot read the current user SID";
        return false;
    }
    // Protected DACL (no inherited ACEs), full control for the current user only.
    const std::wstring sddl = L"D:P(A;" + std::wstring(is_directory ? L"OICI" : L"") + L";FA;;;" + sid + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        if (err) *err = lastErrorText("ConvertStringSecurityDescriptorToSecurityDescriptorW");
        return false;
    }
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    const DWORD rc = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                           DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                           nullptr, dacl, nullptr);
    LocalFree(sd);
    if (rc != ERROR_SUCCESS) {
        if (err) *err = "SetNamedSecurityInfoW failed (" + std::to_string(rc) + ")";
        return false;
    }
    return true;
}

bool isOwnerOnly(const std::wstring& path) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr,
                              &sd) != ERROR_SUCCESS) {
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL ctrl = 0;
    DWORD rev = 0;
    GetSecurityDescriptorControl(sd, &ctrl, &rev);
    bool ok = dacl != nullptr && (ctrl & SE_DACL_PROTECTED) != 0 && dacl->AceCount >= 1;
    const std::wstring me = currentUserSidString();
    for (DWORD i = 0; ok && i < dacl->AceCount; ++i) {
        void* ace = nullptr;
        if (!GetAce(dacl, i, &ace)) { ok = false; break; }
        auto* hdr = static_cast<ACE_HEADER*>(ace);
        if (hdr->AceType != ACCESS_ALLOWED_ACE_TYPE) { ok = false; break; }
        auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        LPWSTR s = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<PSID>(&allowed->SidStart), &s)) { ok = false; break; }
        ok = (me == s);
        LocalFree(s);
    }
    LocalFree(sd);
    return ok;
}

bool ensureOwnerOnlyDirectory(const std::wstring& dir, std::string* err) {
    // Create parents first (they keep their normal ACLs), then the leaf with the owner-only DACL.
    for (size_t pos = dir.find(L'\\', 3); pos != std::wstring::npos; pos = dir.find(L'\\', pos + 1)) {
        CreateDirectoryW(dir.substr(0, pos).c_str(), nullptr);
    }
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        if (err) *err = lastErrorText("CreateDirectoryW");
        return false;
    }
    return applyOwnerOnlyAcl(dir, true, err);
}

bool hostKeyUnwrapErrorIsData(unsigned long win32) {
    switch (win32) {
        case ERROR_INVALID_DATA:      // damaged, or wrapped with other entropy
        case ERROR_INVALID_PARAMETER: // not a whole wrap (truncated)
        case static_cast<unsigned long>(NTE_BAD_DATA):
        case static_cast<unsigned long>(NTE_BAD_KEY):
        case static_cast<unsigned long>(NTE_BAD_KEY_STATE): // its master key isn't this PC's
            return true;
        default:
            return false;
    }
}

#ifdef KV_TESTING
void setHostKeyDpapiErrorsForTest(unsigned long wrap_error, unsigned long unwrap_error) {
    g_wrap_error_for_test.store(wrap_error);
    g_unwrap_error_for_test.store(unwrap_error);
}

void setHostKeyBeforePlainSaveForTest(std::function<void()> hook) {
    std::lock_guard<std::mutex> lk(g_before_plain_save_mutex);
    g_before_plain_save_for_test = std::move(hook);
}

void setHostKeyReplaceFaultForTest(int nth_call, unsigned long win32_error) {
    g_replace_fault_error_for_test.store(win32_error);
    g_replace_fault_call_for_test.store(nth_call);
}
#endif

HostStateLoad loadHostStateChecked(const std::wstring& dir, HostPersistedState& s, std::string* err, bool* passing) {
    if (err) err->clear();
    if (passing) *passing = false;
    const std::wstring path = dir + L"\\pairing.json";
    unsigned long state_error = 0;
    const FileState state = fileState(path, &state_error);
    if (state == FileState::Missing) {
        // No pairing.json, but the pairing.json.prev an interrupted save left: that is this PC's identity, which
        // couldn't be put back (recoverPairingLeftovers; another program may be holding it). Never a first run,
        // which would make a new identity and lose every paired device: it fails closed, as a fault that may pass.
        if (fileState(dir + L"\\pairing.json.prev") != FileState::Missing) {
            if (err) {
                *err = "pairing.json (this PC's identity) is missing, and the pairing.json.prev an interrupted save "
                       "left couldn't be put back (another program may be holding it; a restart lets go of it)";
            }
            if (passing) *passing = true;
            return HostStateLoad::Unusable;
        }
        return HostStateLoad::Missing;
    }
    if (state == FileState::Unknown) {
        if (err) *err = "pairing.json (this PC's identity) couldn't be checked (Win32 error " + std::to_string(state_error) + ")";
        return HostStateLoad::Unusable;
    }
    std::string text;
    if (!readWholeFile(path, text)) {
        const DWORD e = GetLastError();
        if (err) *err = "pairing.json (this PC's identity) couldn't be read (Win32 error " + std::to_string(e) + ")";
        return HostStateLoad::Unusable;
    }
    if (!fromJson(text, s)) {
        if (err) *err = "pairing.json (this PC's identity) couldn't be parsed";
        return HostStateLoad::Unusable;
    }
    // The key wrapped only (the usual case once migrated): unwrapped here. One that won't unwrap fails closed like a
    // file that can't be parsed. Only a data error (the wrap is damaged, or from another PC or Windows install) is
    // worth the owner's reset; anything else may pass, so the error says to try again first. A file that still
    // holds the key plain is used as it is.
    if (s.privateKeyRaw.empty()) {
        unsigned long e = 0;
        if (!unwrapHostKey(s.privateKeyWrapped, s.privateKeyRaw, &e)) {
            const bool data = hostKeyUnwrapErrorIsData(e);
            if (passing) *passing = !data;
            if (err) {
                *err = data ? "pairing.json (this PC's identity) holds a key that can't be unwrapped on this PC "
                              "(Win32 error " + std::to_string(e) + "): it was wrapped on another PC or Windows "
                              "install, or it is damaged. Reset this PC's identity to make a new one (every device "
                              "must pair again)"
                            : "this PC's key in pairing.json couldn't be unwrapped just now (Win32 error " +
                                  std::to_string(e) + "), which may pass: try again. Reset this PC's identity only "
                                  "if it still fails after a restart (every device must pair again)";
            }
            return HostStateLoad::Unusable;
        }
    }
    return HostStateLoad::Loaded;
}

bool loadHostState(const std::wstring& dir, HostPersistedState& s) {
    return loadHostStateChecked(dir, s) == HostStateLoad::Loaded;
}

bool keepPairingJsonAsBad(const std::wstring& dir, std::string* what) {
    return keepBadCopy(dir, L"pairing.json", what);
}

IdentityReset resetHostIdentity(const std::wstring& dir, std::string* err, std::string* note) {
    if (note) note->clear();
    const std::wstring path = dir + L"\\pairing.json";
    // Checked again now, not trusted from the failed start: one that loads now, with a key and certificate a start
    // can use (as HostStreamServer::start checks them), is used as it is.
    HostPersistedState now;
    const HostStateLoad load = loadHostStateChecked(dir, now);
    if (load == HostStateLoad::Loaded && CryptoIdentity::fromRecord(now.privateKeyRaw, now.certificateDER)) {
        if (note) *note = "pairing.json can be read now, so this PC's identity was kept";
        return IdentityReset::NotNeeded;
    }
    // No pairing.json, but the pairing.json.prev an interrupted save left: this PC's identity. It goes back first
    // (recoverPairingLeftovers). One that still can't be put back refuses the reset, since a new identity is never
    // made while the old one is there and not kept. One put back that loads is used as it is.
    if (fileState(path) == FileState::Missing && fileState(dir + L"\\pairing.json.prev") != FileState::Missing) {
        const std::string recovered = recoverPairingLeftovers(dir);
        if (fileState(path) == FileState::Missing) {
            if (err) {
                *err = "pairing.json.prev (this PC's identity, from an interrupted save) couldn't be put back as "
                       "pairing.json, so nothing was changed: try again in a moment, or after a restart";
            }
            if (note) *note = recovered;
            return IdentityReset::Refused;
        }
        HostPersistedState back;
        if (loadHostStateChecked(dir, back) == HostStateLoad::Loaded &&
            CryptoIdentity::fromRecord(back.privateKeyRaw, back.certificateDER)) {
            if (note) *note = "pairing.json.prev was put back as pairing.json, so this PC's identity was kept";
            return IdentityReset::NotNeeded;
        }
    }
    // The old identity first, for a human to look at (keepBadCopy: unless a kept copy already holds it).
    std::string kept;
    if (load == HostStateLoad::Missing) {
        kept = "there was no pairing.json to keep";
    } else if (!keepPairingJsonAsBad(dir, &kept)) {
        std::string current;
        if (readWholeFile(path, current)) {
            // It can be read, so the copy is all that is in the way: never write over the only copy of it.
            if (err) *err = "the old pairing.json couldn't be kept (" + kept + "), so it was left as it was";
            if (note) *note = kept;
            return IdentityReset::Refused;
        }
        // It can't be read, so no copy of it can be made at all: the owner asked for the reset; note says so.
    }
    if (note) *note = kept;
    std::string gen_err;
    auto identity = CryptoIdentity::generate(newHostCommonName(), &gen_err);
    if (!identity) {
        if (err) *err = "making a new identity failed" + (gen_err.empty() ? std::string() : ": " + gen_err);
        return IdentityReset::Failed;
    }
    HostPersistedState st; // a first run's settings
    st.privateKeyRaw = identity->privateKeyRaw();
    st.certificateDER = identity->certificateDER();
    st.serviceId = randomHexId(6);
    st.allowPhoneModeSwitch = true;
    std::string before, save_err;
    const bool had = readWholeFile(path, before);
    if (!saveHostStateAtomically(dir, st, &save_err)) {
        // Say which: the old file as it was (the write failed), or the new one there and not checking out.
        std::string after;
        const bool replaced = readWholeFile(path, after) && (!had || after != before);
        if (err) {
            *err = replaced ? "the new pairing.json was written but didn't check out (" + save_err + ")" +
                                  (had ? "; the old one is kept as its .bad copy" : "")
                            : "writing the new pairing.json failed: " + save_err +
                                  (had ? "; the old one is as it was" : "");
        }
        return IdentityReset::Failed;
    }
    return IdentityReset::Done;
}

bool saveHostStateAtomically(const std::wstring& dir, const HostPersistedState& s, std::string* err) {
    // The key goes to disk wrapped: a new wrap, checked before anything is written.
    HostPersistedState out = s;
    std::string wrap_err;
    if (!wrapAndCheck(s.privateKeyRaw, out.privateKeyWrapped, &wrap_err)) {
        // DPAPI isn't working. While pairing.json is missing (a first run, a reset) or still holds the key plain,
        // the save goes on with it plain, as before the wrap, so a phone can still pair and a setting still save;
        // the next start's migration wraps it. Over a file that holds it wrapped, never: that save fails.
        std::string text;
        HostPersistedState onDisk;
        const std::wstring path = dir + L"\\pairing.json";
        const FileState state = fileState(path);
        const bool plainNow =
            state == FileState::Missing ||
            (state == FileState::Present && readWholeFile(path, text) && fromJson(text, onDisk) &&
             !onDisk.privateKeyRaw.empty());
        if (!plainNow || s.privateKeyRaw.empty()) {
            if (err) *err = wrap_err;
            return false;
        }
#ifdef KV_TESTING
        {
            std::function<void()> hook;
            {
                std::lock_guard<std::mutex> lk(g_before_plain_save_mutex);
                hook = g_before_plain_save_for_test;
            }
            if (hook) hook();
        }
#endif
        out = s;
        out.privateKeyWrapped.clear();
        // The write re-checks what it replaced: a wrap made meanwhile goes back as it was, and the save fails.
        if (!writePlainPairingTextRechecked(dir, toJson(out), state != FileState::Missing, err)) return false;
        return readBackPlain(dir, s.privateKeyRaw, err);
    }
    out.privateKeyRaw.clear();
    if (!writePairingText(dir, toJson(out), err)) return false;
    return readBackChecks(dir, s.privateKeyRaw, false, unwrapForCheck, err);
}

namespace {
// The key a pairing.json's text holds, plain or unwrapped here; nullopt when it holds none this PC can read.
std::optional<std::vector<uint8_t>> keyIn(const std::string& text) {
    HostPersistedState s;
    if (!fromJson(text, s)) return std::nullopt;
    if (!s.privateKeyRaw.empty()) return s.privateKeyRaw;
    std::vector<uint8_t> key;
    if (!unwrapHostKey(s.privateKeyWrapped, key)) return std::nullopt;
    return key;
}

// A process with this id is running (or can't be asked, which counts as running: its temp file stays).
bool processIsRunning(unsigned long pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return GetLastError() != ERROR_INVALID_PARAMETER;
    DWORD code = 0;
    const bool running = !GetExitCodeProcess(p, &code) || code == STILL_ACTIVE;
    CloseHandle(p);
    return running;
}
} // namespace

std::string recoverPairingLeftovers(const std::wstring& dir) {
    std::string note;
    const std::wstring final_path = dir + L"\\pairing.json";
    const std::wstring prev_path = dir + L"\\pairing.json.prev";
    if (fileState(prev_path) == FileState::Present) {
        const FileState pairing = fileState(final_path);
        if (pairing == FileState::Missing) {
            note = MoveFileExW(prev_path.c_str(), final_path.c_str(), MOVEFILE_WRITE_THROUGH)
                       ? "pairing.json was missing beside the pairing.json.prev an interrupted save left: that is "
                         "pairing.json again"
                       : lastErrorText("pairing.json is missing beside the pairing.json.prev an interrupted save left, "
                                       "which couldn't be put back, so the identity fails closed until it can be: "
                                       "MoveFileExW");
        } else if (pairing == FileState::Present) {
            // Removed only when it is the same identity as pairing.json (the same key): the file a save replaced.
            // Another key (a reset another start made meanwhile, whose put-back failed with both files left) may be
            // the only copy of that identity, so it is kept as a .bad copy first; so is one that can't be read.
            std::string now_text, prev_text;
            const auto now_key = readWholeFile(final_path, now_text) ? keyIn(now_text) : std::nullopt;
            const auto prev_key = readWholeFile(prev_path, prev_text) ? keyIn(prev_text) : std::nullopt;
            if (now_key && prev_key && *now_key == *prev_key) {
                if (DeleteFileW(prev_path.c_str())) {
                    note = "the pairing.json.prev an interrupted save left (the file it replaced, the same identity) "
                           "is removed";
                }
            } else {
                std::string kept;
                if (keepBadCopy(dir, L"pairing.json.prev", &kept) && DeleteFileW(prev_path.c_str())) {
                    note = "the pairing.json.prev an interrupted save left holds another identity than pairing.json, "
                           "or one that can't be read: " + kept + ", and .prev is removed";
                } else {
                    note = "the pairing.json.prev an interrupted save left holds another identity than pairing.json, "
                           "or one that can't be read, and is kept as it is (" + kept + ")";
                }
            }
        }
    }
    // Temp files of writes that never finished: pairing.json.tmp (an earlier build's name) and pairing.json.tmp.<pid>
    // of a process that is gone. Another process's that is running is its write in progress, and stays.
    int removed = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE fh = FindFirstFileW((dir + L"\\pairing.json.tmp*").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring name = fd.cFileName;
            const std::wstring prefix = L"pairing.json.tmp.";
            bool stale = name == L"pairing.json.tmp";
            if (name.size() > prefix.size() && name.size() <= prefix.size() + 10 && name.rfind(prefix, 0) == 0 &&
                name.find_first_not_of(L"0123456789", prefix.size()) == std::wstring::npos) {
                const unsigned long pid = wcstoul(name.c_str() + prefix.size(), nullptr, 10); // ULONG_MAX past range
                stale = pid != GetCurrentProcessId() && !processIsRunning(pid);
            }
            if (stale && DeleteFileW((dir + L"\\" + name).c_str())) ++removed;
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    if (removed > 0) {
        note += std::string(note.empty() ? "" : "; ") + std::to_string(removed) +
                (removed == 1 ? " pairing.json temp file" : " pairing.json temp files") +
                " a write that never finished left " + (removed == 1 ? "is" : "are") + " removed";
    }
    return note;
}

HostKeyMigration migrateHostKeyToWrapped(const std::wstring& dir, std::string* note, ReadBack read_back_for_test) {
    if (note) note->clear();
    const ReadBack read_back = read_back_for_test ? read_back_for_test : unwrapForCheck;
    std::string text;
    HostPersistedState plain;
    if (!readWholeFile(dir + L"\\pairing.json", text) || !fromJson(text, plain)) {
        if (note) *note = "pairing.json couldn't be read to wrap its key";
        return HostKeyMigration::Unreadable;
    }
    if (plain.privateKeyRaw.empty()) return HostKeyMigration::AlreadyWrapped; // nothing is written
    const std::vector<uint8_t> key = plain.privateKeyRaw;
    std::string why;
    // 1. Wrapped, and unwrapped again in memory.
    HostPersistedState both = plain;
    if (!wrapAndCheck(key, both.privateKeyWrapped, &why)) {
        if (note) *note = "this PC's key stays plain in pairing.json for now: " + why;
        return HostKeyMigration::KeptPlain;
    }
    // 2. Both on disk, so a failure from here on leaves a file that still holds the plain key.
    if (!writePairingText(dir, toJson(both), &why)) {
        if (note) *note = "this PC's key stays plain in pairing.json for now: writing it wrapped failed: " + why;
        return HostKeyMigration::KeptPlain;
    }
    // 3. Read back from disk: the wrapped key unwraps to the plain one beside it.
    if (!readBackChecks(dir, key, true, read_back, &why)) {
        if (note) *note = "this PC's key stays plain in pairing.json for now: " + why;
        return HostKeyMigration::KeptPlain;
    }
    // 4. Only now is the plain key removed: the same wrapped key alone, then read back and checked again.
    HostPersistedState wrapped = both;
    wrapped.privateKeyRaw.clear();
    if (!writePairingText(dir, toJson(wrapped), &why) || !readBackChecks(dir, key, false, read_back, &why)) {
        std::string undo;
        const bool restored = writePairingText(dir, toJson(both), &undo);
        if (note) {
            *note = "this PC's key stays plain in pairing.json for now: " + why +
                    (restored ? std::string() : "; putting the plain key back failed too: " + undo);
        }
        return HostKeyMigration::KeptPlain;
    }
    if (note) *note = "this PC's key in pairing.json is now wrapped for this PC (DPAPI)";
    return HostKeyMigration::Migrated;
}

} // namespace kvhost
