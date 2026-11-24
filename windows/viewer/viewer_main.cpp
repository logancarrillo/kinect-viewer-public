// Entry point for kinect-viewer.exe (Win32 GUI subsystem).
#include "viewer_app.hpp"

#include <shellapi.h>

#ifdef KV_WITH_STREAMING
#include "stream_hooks.hpp"
#include "keep_running.hpp"
#include <string>
#endif

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
#ifdef KV_WITH_STREAMING
    // The keep-running watchdog (keep_running.hpp): no window, no sensor, no host; it only waits on the viewer.
    if (argv && argc >= 3 && std::wstring(argv[1]) == L"--keep-running") {
        const DWORD pid = static_cast<DWORD>(_wtoi(argv[2]));
        const std::wstring dir = argc >= 4 ? argv[3] : std::wstring();
        LocalFree(argv);
        return kvhost::runKeepRunningMain(pid, dir);
    }
#endif
    kvwin::ViewerApp app;
#ifdef KV_WITH_STREAMING
    kvwin::IphoneStreamHooks hooks;
    const int rc = app.run(inst, argc, argv, &hooks);
#else
    const int rc = app.run(inst, argc, argv, nullptr);
#endif
    LocalFree(argv);
    return rc;
}
