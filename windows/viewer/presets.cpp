#include "presets.hpp"

#include "json_lite.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace kvwin {

namespace json = kvhost::json;

namespace {

std::wstring envVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return (n > 0 && n < MAX_PATH * 2) ? std::wstring(buf, n) : std::wstring();
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

ViewerPreset make(const char* name, kv_view_mode_t mode, float gamma, float ema, float lo, float hi, float man_min,
                  float man_max, kv_colormap_kind_t cmap, float dmin, float dmax, kv_exposure_mode_t exp, float semi_ms,
                  float man_ms, float gain) {
    ViewerPreset p;
    p.name = name;
    p.built_in = true;
    ViewerSettings& s = p.settings;
    s.mode = mode;
    s.sbs_layout = KV_SBS_IR_DEPTH;
    s.pipeline = KV_PIPELINE_OPENGL;
    s.ir_auto_scale = true;
    s.ir_gamma = gamma;
    s.ir_ema_alpha = ema;
    s.ir_low_percentile = lo;
    s.ir_high_percentile = hi;
    s.ir_manual_min = man_min;
    s.ir_manual_max = man_max;
    s.depth_colormap = cmap;
    s.depth_min_mm = dmin;
    s.depth_max_mm = dmax;
    s.depth_invert = true;
    s.exposure_mode = exp;
    s.auto_exposure_compensation = 0.0f;
    s.semi_auto_pseudo_ms = semi_ms;
    s.manual_integration_ms = man_ms;
    s.manual_analog_gain = gain;
    s.mirror_horizontal = false;
    s.rotate_180 = false;
    return p;
}

std::string presetJson(const ViewerPreset& p) {
    const ViewerSettings& s = p.settings;
    json::ObjectWriter w;
    w.field("name", p.name)
        .field("isBuiltIn", p.built_in)
        .field("modeRaw", static_cast<int>(s.mode))
        .field("sbsLayoutRaw", static_cast<int>(s.sbs_layout))
        .field("pipelineRaw", static_cast<int>(s.pipeline))
        .field("irAutoScale", s.ir_auto_scale)
        .doubleField("irGamma", s.ir_gamma)
        .doubleField("irSmoothingAlpha", s.ir_ema_alpha)
        .doubleField("irLowPercentile", s.ir_low_percentile)
        .doubleField("irHighPercentile", s.ir_high_percentile)
        .doubleField("irManualMin", s.ir_manual_min)
        .doubleField("irManualMax", s.ir_manual_max)
        .field("depthColormapRaw", static_cast<int>(s.depth_colormap))
        .doubleField("depthMinMm", s.depth_min_mm)
        .doubleField("depthMaxMm", s.depth_max_mm)
        .field("depthInvertNearWarm", s.depth_invert)
        .field("exposureModeRaw", static_cast<int>(s.exposure_mode))
        .doubleField("autoExposureComp", s.auto_exposure_compensation)
        .doubleField("semiAutoPseudoMs", s.semi_auto_pseudo_ms)
        .doubleField("manualIntegrationMs", s.manual_integration_ms)
        .doubleField("manualAnalogGain", s.manual_analog_gain)
        .field("mirrorHorizontal", s.mirror_horizontal)
        .field("rotate180", s.rotate_180);
    const PresetCutoffAndPoints& x = p.cutoff_points;
    if (x.cutoff_enabled) w.field("cutoffEnabled", *x.cutoff_enabled);
    if (x.cutoff_near_mm) w.doubleField("cutoffNearMm", *x.cutoff_near_mm);
    if (x.cutoff_far_mm) w.doubleField("cutoffFarMm", *x.cutoff_far_mm);
    if (x.point_size) w.doubleField("pointCloudSize", *x.point_size);
    if (x.point_color_source) w.field("pointCloudColorSourceRaw", static_cast<int>(*x.point_color_source));
    return w.finish();
}

// An optional number: absent is fine, present but not a number makes the entry bad.
bool optDouble(const json::Value& v, const char* key, std::optional<float>& out) {
    out.reset();
    if (!v.get(key)) return true;
    double d = 0;
    if (!json::getDouble(v, key, d) || !std::isfinite(d)) return false;
    out = static_cast<float>(d);
    return true;
}

bool presetFromJson(const json::Value& v, ViewerPreset& p) {
    ViewerSettings& s = p.settings;
    int mode = 0, sbs = 0, pipeline = 0, cmap = 0, exp = 0;
    double g = 0, ema = 0, lo = 0, hi = 0, mn = 0, mx = 0, dmin = 0, dmax = 0, ev = 0, semi = 0, man = 0, gain = 0;
    if (!json::getString(v, "name", p.name) || !json::getBool(v, "isBuiltIn", p.built_in) || !json::getInt(v, "modeRaw", mode) ||
        !json::getInt(v, "sbsLayoutRaw", sbs) || !json::getInt(v, "pipelineRaw", pipeline) ||
        !json::getBool(v, "irAutoScale", s.ir_auto_scale) || !json::getDouble(v, "irGamma", g) ||
        !json::getDouble(v, "irSmoothingAlpha", ema) || !json::getDouble(v, "irLowPercentile", lo) ||
        !json::getDouble(v, "irHighPercentile", hi) || !json::getDouble(v, "irManualMin", mn) ||
        !json::getDouble(v, "irManualMax", mx) || !json::getInt(v, "depthColormapRaw", cmap) ||
        !json::getDouble(v, "depthMinMm", dmin) || !json::getDouble(v, "depthMaxMm", dmax) ||
        !json::getBool(v, "depthInvertNearWarm", s.depth_invert) || !json::getInt(v, "exposureModeRaw", exp) ||
        !json::getDouble(v, "autoExposureComp", ev) || !json::getDouble(v, "semiAutoPseudoMs", semi) ||
        !json::getDouble(v, "manualIntegrationMs", man) || !json::getDouble(v, "manualAnalogGain", gain) ||
        !json::getBool(v, "mirrorHorizontal", s.mirror_horizontal) || !json::getBool(v, "rotate180", s.rotate_180)) {
        return false;
    }
    PresetCutoffAndPoints& x = p.cutoff_points;
    std::optional<int> src;
    if (!json::getOptBool(v, "cutoffEnabled", x.cutoff_enabled) || !optDouble(v, "cutoffNearMm", x.cutoff_near_mm) ||
        !optDouble(v, "cutoffFarMm", x.cutoff_far_mm) || !optDouble(v, "pointCloudSize", x.point_size) ||
        !json::getOptInt(v, "pointCloudColorSourceRaw", src)) {
        return false;
    }
    x.point_color_source.reset();
    if (src && *src >= KV_POINT_COLOR_DEPTH && *src <= KV_POINT_COLOR_REGISTERED_RGB) {
        x.point_color_source = static_cast<kv_point_color_source_t>(*src);
    }
    // Values this viewer doesn't have (an unknown mode or pipeline) fall back.
    s.mode = (mode >= KV_MODE_IR && mode <= KV_MODE_POINT_CLOUD_3D) ? static_cast<kv_view_mode_t>(mode) : KV_MODE_IR;
    s.sbs_layout = (sbs >= KV_SBS_IR_DEPTH && sbs <= KV_SBS_COLOR_IR) ? static_cast<kv_sbs_layout_t>(sbs) : KV_SBS_IR_DEPTH;
    s.pipeline = (pipeline == KV_PIPELINE_CPU || pipeline == KV_PIPELINE_OPENCL) ? static_cast<kv_pipeline_kind_t>(pipeline)
                                                                                  : KV_PIPELINE_OPENGL;
    s.depth_colormap = (cmap >= 0 && cmap <= KV_COLORMAP_GRAY) ? static_cast<kv_colormap_kind_t>(cmap) : KV_COLORMAP_TURBO;
    s.exposure_mode = (exp >= 0 && exp <= KV_EXPOSURE_MANUAL) ? static_cast<kv_exposure_mode_t>(exp) : KV_EXPOSURE_AUTO;
    s.ir_gamma = static_cast<float>(g);
    s.ir_ema_alpha = static_cast<float>(ema);
    s.ir_low_percentile = static_cast<float>(lo);
    s.ir_high_percentile = static_cast<float>(hi);
    s.ir_manual_min = static_cast<float>(mn);
    s.ir_manual_max = static_cast<float>(mx);
    s.depth_min_mm = static_cast<float>(dmin);
    s.depth_max_mm = static_cast<float>(dmax);
    s.auto_exposure_compensation = static_cast<float>(ev);
    s.semi_auto_pseudo_ms = static_cast<float>(semi);
    s.manual_integration_ms = static_cast<float>(man);
    s.manual_analog_gain = static_cast<float>(gain);
    sanitize(s);
    return true;
}

} // namespace

ViewerPreset capturePreset(const std::string& name, const ViewerSettings& s) {
    ViewerPreset p;
    p.name = name;
    p.settings = s;
    p.settings.depth_color_blend_enabled = false; // live colour: never saved
    p.cutoff_points.cutoff_enabled = s.cutoff_enabled;
    p.cutoff_points.cutoff_near_mm = s.cutoff_near_mm;
    p.cutoff_points.cutoff_far_mm = s.cutoff_far_mm;
    p.cutoff_points.point_size = s.point_size;
    p.cutoff_points.point_color_source = s.point_color_source;
    return p;
}

void applyPreset(const ViewerPreset& p, ViewerSettings& s) {
    const ViewerSettings& f = p.settings;
    s.mode = f.mode;
    s.sbs_layout = f.sbs_layout;
    s.pipeline = f.pipeline;
    s.ir_auto_scale = f.ir_auto_scale;
    s.ir_low_percentile = f.ir_low_percentile;
    s.ir_high_percentile = f.ir_high_percentile;
    s.ir_ema_alpha = f.ir_ema_alpha;
    s.ir_gamma = f.ir_gamma;
    s.ir_manual_min = f.ir_manual_min;
    s.ir_manual_max = f.ir_manual_max;
    s.depth_colormap = f.depth_colormap;
    s.depth_min_mm = f.depth_min_mm;
    s.depth_max_mm = f.depth_max_mm;
    s.depth_invert = f.depth_invert;
    s.exposure_mode = f.exposure_mode;
    s.auto_exposure_compensation = f.auto_exposure_compensation;
    s.semi_auto_pseudo_ms = f.semi_auto_pseudo_ms;
    s.manual_integration_ms = f.manual_integration_ms;
    s.manual_analog_gain = f.manual_analog_gain;
    s.mirror_horizontal = f.mirror_horizontal;
    s.rotate_180 = f.rotate_180;
    const PresetCutoffAndPoints& x = p.cutoff_points;
    if (x.cutoff_enabled) s.cutoff_enabled = *x.cutoff_enabled;
    if (x.cutoff_near_mm) s.cutoff_near_mm = *x.cutoff_near_mm;
    if (x.cutoff_far_mm) s.cutoff_far_mm = std::max(s.cutoff_near_mm + 50.0f, *x.cutoff_far_mm);
    if (x.point_size) s.point_size = *x.point_size;
    if (x.point_color_source) s.point_color_source = *x.point_color_source;
    sanitize(s);
}

const std::vector<ViewerPreset>& builtInPresets() {
    static const std::vector<ViewerPreset> presets = {
        make("Desk", KV_MODE_DEPTH, 0.45f, 0.12f, 0.01f, 0.995f, 0.0f, 20000.0f, KV_COLORMAP_TURBO, 400.0f, 2200.0f,
             KV_EXPOSURE_SEMI_AUTO, 16.0f, 16.6f, 1.5f),
        make("Room", KV_MODE_DEPTH, 0.45f, 0.12f, 0.01f, 0.995f, 0.0f, 20000.0f, KV_COLORMAP_VIRIDIS, 500.0f, 6000.0f,
             KV_EXPOSURE_AUTO, 16.0f, 16.6f, 1.5f),
        make("Dark room IR", KV_MODE_IR, 0.36f, 0.16f, 0.005f, 0.995f, 0.0f, 14000.0f, KV_COLORMAP_INFERNO, 500.0f, 4500.0f,
             KV_EXPOSURE_MANUAL, 33.0f, 33.3f, 3.0f),
    };
    return presets;
}

std::wstring defaultPresetsDir() {
    const std::wstring home = envVar(L"USERPROFILE");
    return (home.empty() ? std::wstring(L".") : home) + L"\\kinect-viewer\\.host-state";
}

PresetsStore::PresetsStore(std::wstring dir) : dir_(std::move(dir)), file_(dir_ + L"\\presets.json") {
    reload();
}

std::string PresetsStore::toJson(const std::vector<ViewerPreset>& custom, const std::optional<ViewerPreset>& last) {
    std::string arr = "[";
    for (size_t i = 0; i < custom.size(); ++i) {
        if (i) arr += ',';
        arr += presetJson(custom[i]);
    }
    arr += ']';
    json::ObjectWriter w;
    w.raw("customPresets", arr);
    if (last) w.raw("lastUsedSettings", presetJson(*last));
    return w.finish();
}

bool PresetsStore::fromJson(const std::string& text, std::vector<ViewerPreset>& custom, std::optional<ViewerPreset>& last,
                            int* skipped) {
    custom.clear();
    last.reset();
    int bad = 0;
    json::Value v;
    if (!json::parse(text, v) || v.type != json::Value::Type::Object) return false;
    const json::Value* arr = v.get("customPresets");
    if (!arr || arr->type != json::Value::Type::Array) return false;
    for (const auto& item : arr->items) {
        ViewerPreset p;
        if (item.type != json::Value::Type::Object || !presetFromJson(item, p)) {
            ++bad; // skipped: one bad entry doesn't cost the others
            continue;
        }
        p.built_in = false; // a file can't make a built-in
        custom.push_back(std::move(p));
    }
    const json::Value* lu = v.get("lastUsedSettings");
    if (lu && lu->type != json::Value::Type::Null) {
        ViewerPreset p;
        if (lu->type == json::Value::Type::Object && presetFromJson(*lu, p)) {
            p.built_in = false;
            last = std::move(p);
        } else {
            ++bad;
        }
    }
    if (skipped) *skipped = bad;
    return true;
}

void PresetsStore::reload() {
    keep_aside_ = false;
    std::ifstream in(file_, std::ios::binary);
    if (!in) {
        custom_.clear();
        last_used_.reset();
        // No file yet; or one that exists but can't be opened, which is never written over.
        keep_aside_ = GetFileAttributesW(file_.c_str()) != INVALID_FILE_ATTRIBUTES;
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    int skipped = 0;
    if (!fromJson(ss.str(), custom_, last_used_, &skipped)) {
        custom_.clear();
        last_used_.reset();
        keep_aside_ = true; // unreadable: kept, never overwritten
    } else if (skipped > 0) {
        keep_aside_ = true; // the skipped entries are kept in the moved-aside copy
    }
}

std::vector<ViewerPreset> PresetsStore::all() const {
    std::vector<ViewerPreset> out = builtInPresets();
    out.insert(out.end(), custom_.begin(), custom_.end());
    return out;
}

bool PresetsStore::saveCustom(const std::string& name, const ViewerSettings& s) {
    const std::string clean = trim(name);
    if (clean.empty() || clean == "_last_used") return false;
    for (const auto& b : builtInPresets()) {
        if (lower(b.name) == lower(clean)) return false;
    }
    ViewerPreset p = capturePreset(clean, s);
    auto it = std::find_if(custom_.begin(), custom_.end(), [&](const ViewerPreset& c) { return lower(c.name) == lower(clean); });
    if (it != custom_.end()) *it = p;
    else custom_.push_back(p);
    return persist();
}

void PresetsStore::deleteCustom(const std::string& name) {
    custom_.erase(std::remove_if(custom_.begin(), custom_.end(), [&](const ViewerPreset& c) { return c.name == name; }),
                  custom_.end());
    persist();
}

void PresetsStore::saveLastUsed(const ViewerSettings& s) {
    last_used_ = capturePreset("_last_used", s);
    persist();
}

bool PresetsStore::persist() {
    // %USERPROFILE%\kinect-viewer and .host-state (the host makes it owner-only; made here, it
    // inherits the profile's ACL).
    const size_t cut = dir_.find_last_of(L'\\');
    if (cut != std::wstring::npos) CreateDirectoryW(dir_.substr(0, cut).c_str(), nullptr);
    CreateDirectoryW(dir_.c_str(), nullptr);
    if (keep_aside_) {
        // Move what couldn't be read out of the way first (a fresh name each time), never over it.
        bool moved = false;
        for (int i = 1; i < 1000 && !moved; ++i) {
            const std::wstring aside = file_ + L".unreadable-" + std::to_wstring(i);
            if (GetFileAttributesW(aside.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
            if (!MoveFileExW(file_.c_str(), aside.c_str(), MOVEFILE_WRITE_THROUGH)) return false;
            moved = true;
        }
        if (!moved) return false; // every name taken: write nothing rather than over the file
        keep_aside_ = false;
    }
    const std::string body = toJson(custom_, last_used_);
    const std::wstring tmp = file_ + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << body;
        if (!out) return false;
    }
    return MoveFileExW(tmp.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

} // namespace kvwin
