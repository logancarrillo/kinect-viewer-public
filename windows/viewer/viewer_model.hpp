// Viewer settings model: the UI state, its mapping to the core's C structs, slider scales,
// presets and launch flags. Pure logic (no window), so it is unit-tested.
#pragma once

#include "kinect_core_c.h"

#include <optional>
#include <string>
#include <vector>

namespace kvhost {
struct RemoteControlCommandMessage;
}

namespace kvwin {

struct ViewerSettings {
    bool synthetic = false;
    kv_pipeline_kind_t pipeline = KV_PIPELINE_OPENGL;

    kv_view_mode_t mode = KV_MODE_IR;
    kv_sbs_layout_t sbs_layout = KV_SBS_IR_DEPTH;
    bool clock_overlay = false; // reaches the core only on the synthetic source

    bool ir_auto_scale = true;
    float ir_low_percentile = 0.01f;
    float ir_high_percentile = 0.995f;
    float ir_ema_alpha = 0.12f;
    float ir_gamma = 0.45f;
    float ir_manual_min = 0.0f;
    float ir_manual_max = 20000.0f;

    kv_colormap_kind_t depth_colormap = KV_COLORMAP_TURBO;
    float depth_min_mm = 500.0f;
    float depth_max_mm = 4500.0f;
    bool depth_invert = true; // near = warm

    kv_exposure_mode_t exposure_mode = KV_EXPOSURE_AUTO;
    float auto_exposure_compensation = 0.0f; // EV, [-2, 2]
    float semi_auto_pseudo_ms = 16.0f;       // (0, 66]
    float manual_integration_ms = 16.6f;     // (0, 66]
    float manual_analog_gain = 1.5f;         // [1, 4]

    // Orientation (phase 1: M mirrors, R rotates 180 degrees), done in the core's mapping.
    bool mirror_horizontal = false;
    bool rotate_180 = false;

    // Phase 3 (the Mac's HostController). The depth cut-off blacks out everything
    // outside [near, far] in every mode, 3D included; "Track me" is 500-2000 mm.
    bool cutoff_enabled = false;
    float cutoff_near_mm = 500.0f;  // [200, 5000]
    float cutoff_far_mm = 2000.0f;  // [400, 8000], and at least near + 50
    // The depth colours faded over the registered live colour: live only, so it is colour (never
    // recorded, never captured, never in a preset) and it is off during a playback.
    bool depth_color_blend_enabled = false;
    float depth_color_blend_percent = 50.0f; // [0, 100]
    // The 3D point cloud: what colours the points (registered RGB is live colour), and their size.
    kv_point_color_source_t point_color_source = KV_POINT_COLOR_DEPTH;
    float point_size = 2.5f; // px, [1, 10]
};

enum class DepthPreset { Desk, Spec, Room };

kv_display_params_t toDisplayParams(const ViewerSettings& s);
kv_color_controls_t toColorControls(const ViewerSettings& s);

bool modeNeedsColor(kv_view_mode_t mode, kv_sbs_layout_t sbs);
// Whether the view shows anything made from the live colour: a colour mode, the depth/colour blend,
// or the 3D point cloud coloured by registered RGB (the core's modeUsesColor says the same).
bool settingsNeedColor(const ViewerSettings& s);
// True when the engine runs without the RGB stream but the selected view needs it.
bool needsRestartForColor(bool running, bool rgb_active, const ViewerSettings& s);
// The blend replaces the 2D picture (512x424, the depth image's layout) in every mode but 3D.
bool blendShown(const ViewerSettings& s);
// The layout of the shown picture, for the inspection tools: Depth's while the blend is shown.
kv_view_mode_t imageLayoutMode(const ViewerSettings& s);
// "Track me": the cut-off on, 500-2000 mm.
void applyTrackMe(ViewerSettings& s);

// What a phone's view command (0x08 select_mode or set_cutoff) does to the settings, as the viewer
// applies it. select_mode: any mode 0-4 (3D included), with the layout when it's a valid one; an
// unknown mode is refused whole. set_cutoff: applyTrackMePreset wins, else cutoffEnabled turns it on
// or off. Other actions aren't view commands. Returns what changed (None when refused or no-op).
enum class PhoneViewChange { None, Mode, Cutoff };
PhoneViewChange applyPhoneViewCommand(ViewerSettings& s, const kvhost::RemoteControlCommandMessage& cmd);

void applyDepthPreset(ViewerSettings& s, DepthPreset p);
void resetIrDefaults(ViewerSettings& s);
void sanitize(ViewerSettings& s);

// Integer trackbar scales.
constexpr int kGammaSliderMin = 20, kGammaSliderMax = 140;  // gamma x100
constexpr int kEmaSliderMin = 5, kEmaSliderMax = 100;       // alpha x100
constexpr int kEvSliderMin = -200, kEvSliderMax = 200;      // EV x100
constexpr int kMsSliderMin = 10, kMsSliderMax = 660;        // ms x10
constexpr int kGainSliderMin = 100, kGainSliderMax = 400;   // gain x100
constexpr int kDepthSliderMin = 300, kDepthSliderMax = 8000; // mm
constexpr int kIrSliderMin = 0, kIrSliderMax = 65535;
constexpr int kCutNearSliderMin = 200, kCutNearSliderMax = 5000; // mm, in 50s
constexpr int kCutFarSliderMin = 400, kCutFarSliderMax = 8000;   // mm, in 50s
constexpr int kBlendSliderMin = 0, kBlendSliderMax = 100;        // percent
constexpr int kPointSizeSliderMin = 2, kPointSizeSliderMax = 20; // px x2 (1.0-10.0 in 0.5 steps)

int cutoffToSlider(float mm); // to the nearest 50 mm
float sliderToCutoff(int v);
int pointSizeToSlider(float px);
float sliderToPointSize(int v);
// A cut-off slider moved: the other one follows so far stays at least 50 mm past near (the Mac's).
void setCutoffNear(ViewerSettings& s, float mm);
void setCutoffFar(ViewerSettings& s, float mm);

int gammaToSlider(float g);
float sliderToGamma(int v);
int emaToSlider(float a);
float sliderToEma(int v);
int evToSlider(float ev);
float sliderToEv(int v);
int msToSlider(float ms);
float sliderToMs(int v);
int gainToSlider(float g);
float sliderToGain(int v);

struct LaunchOptions {
    bool autostart = true;
    std::wstring screenshot_path;     // test hook: PrintWindow capture of the main window
    int screenshot_after_ms = 2000;
    bool exit_after_screenshot = false;
    bool serve_loopback_only = false; // W3 tests: bind the stream listeners to 127.0.0.1 only
    // Where the streaming host keeps its state (pairing.json, devices.json, pet-camera.json). Empty:
    // %USERPROFILE%\kinect-viewer\.host-state, except that a --synthetic or test-hook launch never
    // touches it and uses a fresh temp folder instead (v3 §10 "Test isolation").
    std::wstring state_dir;
    bool pair_on_start = false;       // W3 test hook: open the pairing window at launch
    bool show_health = false;         // start with the health display on (H)
    // Test hook (with --screenshot, so synthetic only): before the capture, a region box and a ruler
    // are placed on the depth image and the view is frozen; the report carries the inspector line.
    bool test_tools = false;
    // Test hook (with --screenshot, so synthetic and colour-free only): the second-display output opens
    // as a small window (never over a whole display); at the capture it's captured live, then blacked
    // out through B's own code and captured again.
    bool test_second_display = false;
    // Test hook (with --screenshot, so synthetic only; implies --loopback-only): the pet camera panel
    // (protocol v3, W6). A fake v3 device on this PC pairs through the pairing window and watches; the
    // hook toggles privacy, sharing and a sharing schedule through the panel's own code, opens the
    // Devices dialog and captures it, renames and revokes the device, and reports each step.
    bool test_pet_camera = false;
    // Test hook (with --screenshot, so synthetic only; implies --loopback-only), for a --state-dir whose
    // pairing.json can't be used: checks the Pet camera panel's error and its buttons, presses "Try again",
    // then "Reset this PC's identity" past its confirm dialog, and pairs the fake v3 device once the host serves.
    bool test_identity_reset = false;
    // W7, the camera roll (v3 §10 "Test isolation"). --camera-roll-dir: the folder a test run's roll records
    // into, with --synthetic only; a test launch without it has no roll at all, whatever pet-camera.json says.
    // A real launch takes the rollFolder setting, or realLaunchRollFolder's default.
    std::wstring camera_roll_dir;
    // Test hook (with --screenshot, --synthetic and --camera-roll-dir; implies --loopback-only): the camera
    // roll's panel settings, a motion clip and a manual clip through the panel's own code, the roll browser
    // and its player window, and the screenshot rule while a clip is on screen.
    bool test_camera_roll = false;
    // W8 test hook (with --screenshot, --synthetic and --state-dir; implies --loopback-only): the Pet camera
    // panel's "Outside the home" section against a scripted fake router the viewer hands the host
    // (Options::router_factory), never the real one: each status's sentence, the manual address field, an invite
    // made and cancelled (its token never shown or written: only that it was shown and its length), the
    // address-changed flow and re-share, the connection log window, and off disabling everything (v3 §8, §10).
    bool test_outside = false;
    std::wstring error;
    // The screenshot hook was refused (see screenshotAllowed): the viewer exits with
    // kScreenshotRefusedExit and no message box, before it opens the sensor or captures anything.
    bool screenshot_refused = false;
    // --state-dir named a folder the host may not take (see stateDirAcceptable): nothing starts.
    bool state_dir_refused = false;
    // --camera-roll-dir or --test-camera-roll was refused (see cameraRollFlagsAcceptable): nothing starts.
    bool camera_roll_refused = false;
    // --test-outside was refused (see outsideTestFlagsAcceptable): nothing starts.
    bool outside_refused = false;
};

// pet-camera.md §4: the camera roll's default folder when the rollFolder setting is empty (a real launch only):
// %USERPROFILE%\kinect-viewer-camera-roll, beside the depth/IR recordings and outside OneDrive (which "Change..."
// refuses). Empty when USERPROFILE isn't set: no roll then, rather than a guess.
std::wstring defaultCameraRollDir();
// %USERPROFILE%\kinect-viewer-recordings, the depth/IR recordings folder (kvrec::defaultRecordingsDir, which
// test_recorder checks this against): the roll refuses it, a folder inside it and a folder holding it (§6.5).
std::wstring knownRecordingsDir();

constexpr int kScreenshotRefusedExit = 3;

// Parses --synthetic, --real, --no-autostart, --mode, --sbs-layout, --colormap, --pipeline,
// --clock-overlay, --screenshot PATH, --screenshot-after-ms N, --exit-after-screenshot,
// --loopback-only, --pair-on-start, and phase 3's --mode 3d, --point-color depth|ir|rgb,
// --point-size PX, --track-me, --blend PERCENT; W6's --state-dir DIR, --test-pet-camera and
// --test-identity-reset; W7's --camera-roll-dir DIR and --test-camera-roll; W8's --test-outside. Refuses (false,
// camera_roll_refused) roll flags that cameraRollFlagsAcceptable turns down, before anything else, and (false,
// outside_refused) --test-outside that outsideTestFlagsAcceptable turns down. With
// --screenshot, refuses (false, screenshot_refused) unless screenshotAllowed. Refuses (false,
// state_dir_refused) a --state-dir that stateDirAcceptable turns down.
bool parseLaunchArgs(int argc, const wchar_t* const* argv, LaunchOptions& lo, ViewerSettings& s);

// v3 §10 "Test isolation": a launch with --synthetic or any test hook (--screenshot, --test-*,
// --exit-after-*, --loopback-only, --pair-on-start, --camera-roll-dir). Without --state-dir such a run keeps
// its host state in a fresh temp folder of its own and never touches the real .host-state; and its camera
// roll is only ever --camera-roll-dir's folder.
bool isTestLaunch(const ViewerSettings& s, const LaunchOptions& lo);

// v3 §10 and pet-camera.md §6.6 "Test hooks": --camera-roll-dir and --test-camera-roll are for synthetic test
// runs only. --camera-roll-dir needs --synthetic; --test-camera-roll needs both; and the folder must not be,
// hold or sit inside a real or known folder (cameraRollTestDirAcceptable). why: the reason, when false.
bool cameraRollFlagsAcceptable(const ViewerSettings& s, const LaunchOptions& lo, std::wstring* why);
// W8 (v3 §10): --test-outside drives the outside listener's panel against a fake router, so it needs --synthetic
// and a --state-dir of the test's own (pet-camera.json, invite.json and the connection log stay in it).
bool outsideTestFlagsAcceptable(const ViewerSettings& s, const LaunchOptions& lo, std::wstring* why);
// A test's roll folder (already normalized) is checked by its path alone, so the real folders are never
// opened to decide: not the default camera roll (defaultCameraRollDir), the depth/IR recordings folder
// (knownRecordingsDir) or %USERPROFILE%\kinect-viewer (the real host state), nor a folder inside or holding
// one of them, nor a drive root, a network or a \\?\ path.
bool cameraRollTestDirAcceptable(const std::wstring& dir, std::wstring* why);
// True when a is b or a folder inside it (both full paths; case-insensitive, either separator).
bool pathIsSameOrInside(const std::wstring& a, const std::wstring& b);
// The file a test's roll folder holds once the host has prepared it as a test roll.
constexpr const wchar_t* kTestRollMarker = L".kinect-viewer-test-roll";
// The folder as the file system resolves it: the nearest part of the path that is there, through
// GetFinalPathNameByHandleW (8.3 names, subst drives, junctions and links resolved), with the parts not made yet
// after it. Only attributes are opened. nullopt (why) when it can't be resolved.
std::optional<std::wstring> resolvedFolderPath(const std::wstring& dir, std::wstring* why);
// In a test run --camera-roll-dir must be new, empty, or hold kTestRollMarker; and its resolved path must
// pass cameraRollTestDirAcceptable too (the path-text refusals, as a second line). Called only for a folder whose
// own path text passed, so a real folder named outright is never opened.
bool cameraRollTestDirContentsAcceptable(const std::wstring& dir, std::wstring* why);

// A real launch's camera roll (pet-camera.md §4): the rollFolder setting (UTF-8) when it is set, used as it is;
// otherwise default_dir (defaultCameraRollDir()). Empty: no roll at all.
std::wstring realLaunchRollFolder(const std::string& roll_folder_setting, const std::wstring& default_dir);
// The same with this PC's defaultCameraRollDir().
std::wstring realLaunchRollFolderNow(const std::string& roll_folder_setting);

// Where this launch's camera roll records (pet-camera.md §4, v3 §10). A test launch: --camera-roll-dir, or
// none (empty: no roll at all); the setting is never read for it. A real launch: realLaunchRollFolderNow.
std::wstring cameraRollDirForLaunch(bool test_launch, const std::wstring& camera_roll_dir, const std::string& roll_folder_setting);

// The PC's "Change..." for the camera roll's folder (pet-camera.md §4, §6.5), checked before it is saved; the
// folder itself is prepared only by the host at its next start. Refuses a network path or drive, a drive that
// isn't there, a drive root, a folder inside OneDrive (onedrive_roots), and the depth/IR recordings folder, a
// folder inside it or one holding it. drive_type: GetDriveTypeW of the folder's root (the caller asks; the
// rule stays pure). why: the reason, when false.
bool rollFolderChoiceAcceptable(const std::wstring& dir, const std::wstring& recordings_dir,
                                const std::vector<std::wstring>& onedrive_roots, unsigned drive_type, std::wstring* why);
// %OneDrive%, %OneDriveConsumer% and %OneDriveCommercial%, those that are set.
std::vector<std::wstring> oneDriveRootsFromEnvironment();

// The state folder as given on the command line, made absolute with backslashes and no trailing one
// (CTest passes forward slashes). Empty stays empty.
std::wstring normalizeStateDir(const std::wstring& dir);
// %TEMP%\<prefix>-<pid>\.host-state: a test run's own state folder (not created here).
std::wstring temporaryStateDir(const wchar_t* prefix);
// Deletes %TEMP%\<prefix>-<pid> with everything in it, but only when state_dir is exactly
// temporaryStateDir(prefix): never anything a caller passed in. True when it is gone.
bool removeTemporaryStateDir(const std::wstring& state_dir, const wchar_t* prefix);
// temporaryStateDir(prefix), with whatever a killed run of an earlier process with the same PID left
// there removed first (the same exact-match rule as removeTemporaryStateDir). Empty, with why set, when
// a leftover folder can't be removed: the run must not start on someone else's state.
std::wstring freshTemporaryStateDir(const wchar_t* prefix, std::wstring* why);
// Whether a --state-dir (already normalized) may be used. The host makes the folder owner-only with a
// protected, inheritable DACL that also reaches everything already in it, so only a folder that doesn't
// exist yet, an empty one, or the host's own (its marker file, or a pairing.json that reads as the
// host's: an install from before the marker) is taken. A file name alone (a devices.json) is not enough.
// Never a drive root or a share's root, and never a junction or link. why: the reason, when false.
bool stateDirAcceptable(const std::wstring& dir, std::wstring* why);

// The screenshot test hook writes the whole window to disk, so it may only capture the synthetic
// source, in a view with no colour in it (settingsNeedColor: not Colour, a Side-by-Side layout with
// colour, the depth/colour blend, nor 3D with RGB points), and never while a depth/IR recording
// replays in the view (v3 §10: those are real frames). It is checked at launch and again at the
// moment of capture (the source, the view or a replay may have changed).
// W7 (pet-camera.md §6.6, v3 §10): also refused while a camera-roll clip is on screen (roll_on_screen: the
// player window is open, or the roll browser shows clips' thumbnails), whatever the main view shows.
bool screenshotAllowed(const ViewerSettings& s, std::wstring* why = nullptr);
bool screenshotAllowed(const ViewerSettings& s, bool replaying, std::wstring* why = nullptr);
bool screenshotAllowed(const ViewerSettings& s, bool replaying, bool roll_on_screen, std::wstring* why);
// W8 (v3 §10): also refused while the invite window is open (invite_on_screen): it shows a live token, which no
// capture may hold.
bool screenshotAllowed(const ViewerSettings& s, bool replaying, bool roll_on_screen, bool invite_on_screen, std::wstring* why);

// ---- recording rules (phase 0), shared by the viewer and its tests ----
// IR and Depth only (kvrec::Recorder::canRecord says the same; test_recorder checks they agree).
bool recordingAllowedInMode(kv_view_mode_t mode);
// The mode allows it and the blend (live colour) is off, as the Mac's canRecordCurrentMode.
bool recordingAllowed(const ViewerSettings& s);
// Why a recording can't start now (0x07 recordingDisabledReason, and the PC's note): the colour
// sentence in a colour mode or with the blend on, the playback one while a recording plays;
// nullopt when it can.
std::optional<std::string> recordingDisabledReason(kv_view_mode_t mode, bool replaying);
std::optional<std::string> recordingDisabledReason(const ViewerSettings& s, bool replaying);
constexpr const char* kExitPlaybackFirst = "Exit recording playback before starting a new live recording.";
// A recording made in `during` stops when the view becomes `after` with a different mode or
// layout, or when the blend comes on.
bool viewChangeStopsRecording(const ViewerSettings& during, const ViewerSettings& after);

} // namespace kvwin
