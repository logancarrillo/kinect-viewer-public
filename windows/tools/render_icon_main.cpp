// kv-render-icon <logo.svg> <out.ico>: the build makes the app icon from the logo with this.
#include "icon_render.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: kv-render-icon <logo.svg> <out.ico>\n");
        return 2;
    }
    // Only the logo: this tool makes the app icon and nothing else (exit 3, nothing written).
    if (!kvicon::isLogoPath(argv[1])) {
        std::fprintf(stderr, "kv-render-icon: refused: it renders design/logo.svg only\n");
        return 3;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<kvicon::IconImage> images;
    std::string err;
    if (!kvicon::renderSvgIcons(argv[1], images, &err)) {
        std::fprintf(stderr, "kv-render-icon: %s\n", err.c_str());
        return 1;
    }
    const std::vector<uint8_t> ico = kvicon::makeIco(images);
    FILE* f = _wfopen(argv[2], L"wb");
    if (!f || std::fwrite(ico.data(), 1, ico.size(), f) != ico.size()) {
        if (f) std::fclose(f);
        std::fprintf(stderr, "kv-render-icon: can't write the .ico\n");
        return 1;
    }
    std::fclose(f);
    std::printf("kv-render-icon: %zu sizes, %zu bytes\n", images.size(), ico.size());
    CoUninitialize();
    return 0;
}
