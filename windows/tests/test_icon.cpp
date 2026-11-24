// The Windows app icon from design/logo.svg (the Sensor and Beam mark since the real-app PR; phase 6's
// contour lens before it). It's rendered from the logo at build time; here it's rendered again and
// must match the build's .ico byte for byte, has every size, looks like the logo (transparent outside
// the rounded square, the light sensor bar with its dark lens, the near-to-far depth bands, the dark
// field around them), and kinect-viewer.exe carries it as resource 1.
#include "kv_test.hpp"

#include "icon_render.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace kvicon;

namespace {

const std::vector<IconImage>& rendered() {
    static std::vector<IconImage> images;
    static bool done = false;
    if (!done) {
        done = true;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::string err;
        if (!renderSvgIcons(KV_LOGO_SVG, images, &err)) std::printf("  render failed: %s\n", err.c_str());
    }
    return images;
}

const IconImage* bySize(int size) {
    for (const auto& i : rendered())
        if (i.size == size) return &i;
    return nullptr;
}

struct Px {
    int b, g, r, a;
};
Px px(const IconImage& img, int x, int y) {
    const uint8_t* p = img.bgra.data() + (static_cast<size_t>(y) * img.size + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

uint32_t le32(const std::vector<uint8_t>& f, size_t at) {
    return f[at] | (f[at + 1] << 8) | (f[at + 2] << 16) | (static_cast<uint32_t>(f[at + 3]) << 24);
}

} // namespace

KV_TEST(every_size_renders_from_the_logo) {
    const auto& imgs = rendered();
    KV_REQUIRE(imgs.size() == iconSizes().size());
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (size_t i = 0; i < imgs.size(); ++i) {
        KV_CHECK(imgs[i].size == iconSizes()[i]);
        KV_CHECK(imgs[i].bgra.size() == static_cast<size_t>(imgs[i].size) * imgs[i].size * 4);
        KV_CHECK(imgs[i].png.size() > 8 && std::memcmp(imgs[i].png.data(), sig, 8) == 0);
    }
    for (int s : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256}) KV_CHECK(bySize(s) != nullptr);
}

KV_TEST(the_256_icon_looks_like_the_logo) {
    const IconImage* big = bySize(256);
    KV_REQUIRE(big);
    // Outside the rounded square (it starts 64/1024 in, with a 201.6/1024 corner radius): transparent.
    for (auto [x, y] : {std::pair{2, 2}, std::pair{253, 2}, std::pair{2, 253}, std::pair{253, 253}, std::pair{8, 128}}) {
        KV_CHECK_MSG(px(*big, x, y).a == 0, std::to_string(x) + "," + std::to_string(y));
    }
    // Points below are the logo's 1024 coordinates divided by 4.
    // The sensor bar (#E9EEF7, x 189-835, y 225-378), left of its lens: (333, 301).
    const Px bar = px(*big, 83, 75);
    std::printf("  bar    b=%d g=%d r=%d a=%d\n", bar.b, bar.g, bar.r, bar.a);
    KV_CHECK(bar.a == 255 && bar.r > 200 && bar.g > 200 && bar.b > 200);
    // The lens: a dark ring (#0B1530, r 49) around a teal centre (#22B8CF, r 20) at (512, 301).
    const Px lens = px(*big, 128, 75);
    const Px ring = px(*big, 137, 75);
    std::printf("  lens   b=%d g=%d r=%d a=%d / ring b=%d g=%d r=%d\n", lens.b, lens.g, lens.r, lens.a, ring.b, ring.g, ring.r);
    KV_CHECK(lens.a == 255 && lens.g > 150 && lens.b > 150 && lens.r < 120);
    KV_CHECK(ring.a == 255 && ring.r < 60 && ring.g < 60 && ring.b < 90);
    // The depth bands down the middle: near amber (#FFB238) at y 490, teal (#22B8CF) at 637,
    // far blue (#3D63DD) at 781.
    const Px nearBand = px(*big, 128, 122);
    const Px midBand = px(*big, 128, 159);
    const Px farBand = px(*big, 128, 195);
    std::printf("  bands  near r=%d g=%d b=%d / mid r=%d g=%d b=%d / far r=%d g=%d b=%d\n", nearBand.r, nearBand.g,
                nearBand.b, midBand.r, midBand.g, midBand.b, farBand.r, farBand.g, farBand.b);
    KV_CHECK(nearBand.a == 255 && nearBand.r > 220 && nearBand.g > 140 && nearBand.b < 100);
    KV_CHECK(midBand.a == 255 && midBand.g > 150 && midBand.b > 170 && midBand.r < 80);
    KV_CHECK(farBand.a == 255 && farBand.b > 190 && farBand.r < 100 && farBand.b > farBand.g);
    // Inside the square, beside the bands: the dark blue field (x 136, y 512).
    const Px field = px(*big, 34, 128);
    std::printf("  field  b=%d g=%d r=%d a=%d\n", field.b, field.g, field.r, field.a);
    KV_CHECK(field.a == 255 && field.b > field.r && field.r < 60 && field.g < 110);
}

KV_TEST(small_sizes_keep_the_shape) {
    for (int s : {16, 32, 48}) {
        const IconImage* img = bySize(s);
        KV_REQUIRE(img);
        KV_CHECK(px(*img, 0, 0).a == 0 && px(*img, s - 1, s - 1).a == 0); // the rounded corners
        // The pixel holding (333, 301) of 1024 is all sensor bar: light.
        const Px bar = px(*img, 333 * s / 1024, 301 * s / 1024);
        KV_CHECK_MSG(bar.a == 255 && bar.r > 170 && bar.g > 170 && bar.b > 170, std::to_string(s));
        // The pixel holding (512, 490) of 1024 is all near band: warm.
        const Px nearBand = px(*img, 512 * s / 1024, 490 * s / 1024);
        KV_CHECK_MSG(nearBand.a == 255 && nearBand.r > 200 && nearBand.r > nearBand.b + 100, std::to_string(s));
    }
}

KV_TEST(ico_layout_and_the_builds_icon_matches) {
    const auto& imgs = rendered();
    KV_REQUIRE(!imgs.empty());
    const std::vector<uint8_t> ico = makeIco(imgs);
    KV_REQUIRE(ico.size() > 6 + 16 * imgs.size());
    KV_CHECK(ico[0] == 0 && ico[1] == 0 && ico[2] == 1 && ico[3] == 0);
    KV_CHECK((ico[4] | (ico[5] << 8)) == static_cast<int>(imgs.size()));
    size_t total = 6 + 16 * imgs.size();
    for (size_t i = 0; i < imgs.size(); ++i) {
        const size_t e = 6 + 16 * i;
        const int w = ico[e] == 0 ? 256 : ico[e];
        KV_CHECK(w == imgs[i].size && ico[e + 1] == ico[e]);
        KV_CHECK((ico[e + 6] | (ico[e + 7] << 8)) == 32);
        KV_CHECK(le32(ico, e + 8) == imgs[i].png.size() && le32(ico, e + 12) == total);
        total += imgs[i].png.size();
    }
    KV_CHECK(total == ico.size());
    // The build's icon is this one: it follows the SVG.
    std::ifstream in(KV_APP_ICON, std::ios::binary);
    KV_REQUIRE(in.good());
    const std::vector<uint8_t> built((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::printf("  icon: %zu sizes, %zu bytes\n", imgs.size(), built.size());
    KV_CHECK(built == ico);
}

namespace {
void checkCarriesIcon(const wchar_t* exe) {
    HMODULE m = LoadLibraryExW(exe, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    KV_REQUIRE(m != nullptr);
    HRSRC group = FindResourceW(m, MAKEINTRESOURCEW(1), RT_GROUP_ICON);
    KV_CHECK(group != nullptr);
    if (group) {
        // GRPICONDIR: reserved, type 1, count.
        const auto* g = static_cast<const uint8_t*>(LockResource(LoadResource(m, group)));
        KV_CHECK(g && g[2] == 1 && (g[4] | (g[5] << 8)) == static_cast<int>(iconSizes().size()));
    }
    FreeLibrary(m);
}
} // namespace

KV_TEST(the_viewer_carries_the_icon) { checkCarriesIcon(KV_VIEWER_EXE); }
KV_TEST(the_cli_carries_the_icon) { checkCarriesIcon(KV_CLI_EXE); }

KV_TEST(only_the_logo_is_rendered) {
    KV_CHECK(isLogoPath(L"D:\\x\\design\\logo.svg") && isLogoPath(L"d:/X/Design/LOGO.SVG") && isLogoPath(KV_LOGO_SVG));
    for (const wchar_t* p : {L"D:\\x\\design\\other.svg", L"D:\\x\\logo.svg", L"D:\\x\\design\\logo.svg.png", L"logo.svg",
                             L"D:\\x\\notdesign\\logo.svg", L""}) {
        std::string key;
        for (const wchar_t* c = p; *c; ++c) key += static_cast<char>(*c);
        KV_CHECK_MSG(!isLogoPath(p), key);
    }
    std::vector<IconImage> out;
    std::string err;
    KV_CHECK(!renderSvgIcons(L"D:\\x\\design\\other.svg", out, &err) && out.empty() && err.find("refused") == 0);
}

KV_TEST_MAIN()
