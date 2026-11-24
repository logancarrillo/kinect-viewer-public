// Named viewer presets and the last-used settings (as on the Mac, macos/App/PresetsStore.swift).
// A preset holds the mode, pipeline, IR mapping, colour map, distance window,
// exposure and orientation; not the source. Built in: "Desk", "Room" and "Dark room IR", with the
// Mac's values. Custom presets and the last-used settings are kept in presets.json beside the
// streaming host's pairing state (%USERPROFILE%\kinect-viewer\.host-state, owner-only when the host
// made it).
#pragma once

#include "viewer_model.hpp"

#include <optional>
#include <string>
#include <vector>

namespace kvwin {

// The cut-off and point cloud preset fields, optional as on the Mac: a file from before they existed, and the
// built-ins, don't have them, and applying such a preset leaves the cut-off and the point cloud as they are.
// The depth/colour blend is never in a preset (it is live colour).
struct PresetCutoffAndPoints {
    std::optional<bool> cutoff_enabled;
    std::optional<float> cutoff_near_mm;
    std::optional<float> cutoff_far_mm;
    std::optional<float> point_size;
    std::optional<kv_point_color_source_t> point_color_source;
};

struct ViewerPreset {
    std::string name; // UTF-8
    bool built_in = false;
    ViewerSettings settings; // only the preset's fields are used (see applyPreset)
    PresetCutoffAndPoints cutoff_points;
};

// The preset's fields from s.
ViewerPreset capturePreset(const std::string& name, const ViewerSettings& s);
// Copies the preset's fields into s (keeps the source and the clock overlay), then sanitizes.
void applyPreset(const ViewerPreset& p, ViewerSettings& s);
const std::vector<ViewerPreset>& builtInPresets();

class PresetsStore {
public:
    explicit PresetsStore(std::wstring dir); // presets.json in dir
    void reload();
    std::vector<ViewerPreset> all() const; // built-ins first, then custom
    const std::vector<ViewerPreset>& custom() const { return custom_; }
    // False for an empty name or a built-in's name (any case). Same name (any case): replaced.
    bool saveCustom(const std::string& name, const ViewerSettings& s);
    void deleteCustom(const std::string& name);
    void saveLastUsed(const ViewerSettings& s);
    const std::optional<ViewerPreset>& lastUsed() const { return last_used_; }
    std::wstring path() const { return file_; }

    // The JSON, for tests: {"customPresets":[...],"lastUsedSettings":{...}}. fromJson fails only when the
    // file itself isn't that shape; a bad entry (or a bad last-used) is skipped and counted.
    static std::string toJson(const std::vector<ViewerPreset>& custom, const std::optional<ViewerPreset>& last);
    static bool fromJson(const std::string& json, std::vector<ViewerPreset>& custom, std::optional<ViewerPreset>& last,
                         int* skipped = nullptr);
    // A file that couldn't be read, or had entries skipped: it's moved aside (presets.json.unreadable-N)
    // before the first write, so nothing in it is lost.
    bool keepsUnreadableFile() const { return keep_aside_; }

private:
    bool persist();
    std::wstring dir_;
    std::wstring file_;
    std::vector<ViewerPreset> custom_;
    std::optional<ViewerPreset> last_used_;
    bool keep_aside_ = false;
};

std::wstring defaultPresetsDir(); // %USERPROFILE%\kinect-viewer\.host-state

} // namespace kvwin
