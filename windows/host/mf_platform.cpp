#include "mf_platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>

#include <cstdio>
#include <mutex>

namespace kvhost {

namespace {
std::mutex g_mutex;
bool g_started = false;
} // namespace

// Why there is no MFShutdown anywhere in the host: a hardware encoder MFT (a GPU vendor's) can finish its own release
// on an MF work queue after the last reference to it goes. The host used to call MFShutdown when its last encoder
// closed, which tore that queue down under the MFT, and its work item crashed: an access violation on an RTWorkQ
// thread inside the vendor's MFT. The unpair e2e test hit it now and then under load, and it could happen whenever
// the last live tier closed (the last phone leaving). So MF is started once and left running; MF's
// own reference count still pairs any other MFStartup/MFShutdown in the process (a test's decoder), and the OS
// cleans up at exit. MFShutdown was the commonest trigger, not the whole cause: the encoder's own release could still
// race its queued work under load, so closing an encoder also drains it, shuts its activation object down and
// releases it a few seconds later (mf_h264_encoder.cpp, parkEncoder).
bool ensureMediaFoundation(std::string* err) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_started) return true;
    const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        if (err) {
            char b[64];
            std::snprintf(b, sizeof(b), "MFStartup failed (hr=0x%08lx)", static_cast<unsigned long>(hr));
            *err = b;
        }
        return false;
    }
    g_started = true;
    return true;
}

} // namespace kvhost
