// The real-app details both programs carry: version info Explorer shows (product, company, version,
// each program's own name and description) and, for the viewer, the manifest (per-monitor v2 DPI,
// asInvoker, Common-Controls 6 kept from the linker's manifest).
#include "kv_test.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace {

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (c < 128) ? static_cast<char>(c) : '?';
    return s;
}

struct VersionInfo {
    bool ok = false;
    VS_FIXEDFILEINFO fixed{};
    std::vector<uint8_t> data;
    std::wstring str(const wchar_t* key) const {
        if (!ok) return L"";
        std::wstring path = std::wstring(L"\\StringFileInfo\\040904B0\\") + key;
        wchar_t* v = nullptr;
        UINT len = 0;
        if (!VerQueryValueW(data.data(), path.c_str(), reinterpret_cast<void**>(&v), &len) || !v) return L"";
        return std::wstring(v, len ? len - 1 : 0);
    }
};

VersionInfo readVersion(const wchar_t* exe) {
    VersionInfo vi;
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe, &handle);
    if (!size) return vi;
    vi.data.resize(size);
    if (!GetFileVersionInfoW(exe, 0, size, vi.data.data())) return vi;
    VS_FIXEDFILEINFO* f = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(vi.data.data(), L"\\", reinterpret_cast<void**>(&f), &len) || !f || len < sizeof(*f)) return vi;
    vi.fixed = *f;
    vi.ok = true;
    return vi;
}

void checkVersion(const wchar_t* exe, const wchar_t* name, const wchar_t* description) {
    const VersionInfo vi = readVersion(exe);
    KV_REQUIRE(vi.ok);
    std::printf("  %s: %s %s, %s\n", narrow(name).c_str(), narrow(vi.str(L"ProductName")).c_str(),
                narrow(vi.str(L"ProductVersion")).c_str(), narrow(vi.str(L"FileDescription")).c_str());
    KV_CHECK(vi.fixed.dwSignature == 0xFEEF04BD);
    KV_CHECK(vi.fixed.dwFileType == VFT_APP);
    KV_CHECK(vi.str(L"ProductName") == L"Kinect Viewer");
    KV_CHECK(vi.str(L"CompanyName") == L"The Kinect Viewer Authors");
    KV_CHECK(vi.str(L"ProductVersion") == KV_APP_VERSION);
    KV_CHECK(vi.str(L"FileDescription") == description);
    KV_CHECK(vi.str(L"InternalName") == name);
    KV_CHECK(vi.str(L"OriginalFilename") == std::wstring(name) + L".exe");
    // The fixed version starts with the product version's major.minor.
    const std::wstring mm = std::to_wstring(HIWORD(vi.fixed.dwProductVersionMS)) + L"." +
                            std::to_wstring(LOWORD(vi.fixed.dwProductVersionMS));
    KV_CHECK(mm == KV_APP_VERSION);
    KV_CHECK(LOWORD(vi.fixed.dwFileVersionLS) >= 1); // the build number
}

std::string manifestOf(const wchar_t* exe) {
    HMODULE m = LoadLibraryExW(exe, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) return "";
    std::string text;
    if (HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(1), RT_MANIFEST)) {
        const DWORD n = SizeofResource(m, r);
        if (const auto* p = static_cast<const char*>(LockResource(LoadResource(m, r)))) text.assign(p, n);
    }
    FreeLibrary(m);
    return text;
}

} // namespace

KV_TEST(the_viewer_has_version_info) { checkVersion(KV_VIEWER_EXE, L"kinect-viewer", L"Kinect Viewer"); }
KV_TEST(the_cli_has_version_info) { checkVersion(KV_CLI_EXE, L"kinect-cli", L"Kinect Viewer command line"); }

KV_TEST(the_viewer_manifest_is_dpi_aware_and_as_invoker) {
    const std::string m = manifestOf(KV_VIEWER_EXE);
    KV_REQUIRE(!m.empty());
    KV_CHECK(m.find("PerMonitorV2") != std::string::npos);
    KV_CHECK(m.find("true/pm") != std::string::npos);
    KV_CHECK(m.find("level=\"asInvoker\"") != std::string::npos || m.find("level='asInvoker'") != std::string::npos);
    KV_CHECK(m.find("requireAdministrator") == std::string::npos && m.find("highestAvailable") == std::string::npos);
    KV_CHECK(m.find("Microsoft.Windows.Common-Controls") != std::string::npos); // the linker's part stays
    KV_CHECK(m.find("8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a") != std::string::npos);
}

KV_TEST_MAIN()
