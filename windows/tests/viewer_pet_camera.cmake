# The viewer's pet camera (protocol v3 §10 "Test isolation", pet-camera.md §4), synthetic only.
#
# Default: a --state-dir under the build tree is seeded with two synthetic devices (devices.json) and
# "Share the camera" on, privacy off (pet-camera.json). A --synthetic --test-pet-camera launch must load
# them, then its hook pairs a fake v3 device on loopback through the pairing window, and checks the
# "Watching now" banner, privacy mode, sharing and its schedule, the Devices dialog (captured), a rename
# and a revoke, each through the panel's own code. The state it leaves behind is checked too.
#
# ISOLATION: a test-hook launch WITHOUT --state-dir reports a temp state folder of its own (never the
# real %USERPROFILE%\kinect-viewer\.host-state, which this test never reads) and removes it on exit.
cmake_minimum_required(VERSION 3.20)

function(run_viewer out_var)
    execute_process(COMMAND "${VIEWER}" ${ARGN} TIMEOUT 120 RESULT_VARIABLE rc)
    set(${out_var} ${rc} PARENT_SCOPE)
endfunction()

# The patterns are checked against ${report} in the caller (passed by name: a report line may hold a ";").
function(expect)
    foreach(want IN LISTS ARGN)
        if(NOT report MATCHES "${want}")
            message(FATAL_ERROR "expected '${want}' in the report")
        endif()
    endforeach()
endfunction()

if(ISOLATION)
    file(REMOVE "${OUT}" "${OUT}.txt")
    run_viewer(rc --synthetic --mode depth --screenshot "${OUT}" --screenshot-after-ms 1500 --exit-after-screenshot)
    if(NOT EXISTS "${OUT}.txt")
        message(FATAL_ERROR "the viewer wrote no report (rc=${rc})")
    endif()
    file(READ "${OUT}.txt" report)
    message(STATUS "${report}")
    expect("ok=1" "state_dir_temp=1\n" "state_dir=[^\n]*\\\\kinect-viewer-test-[0-9]+\\\\\\.host-state\n")
    string(REGEX MATCH "state_dir=([^\n]*)\n" _ "${report}")
    set(dir "${CMAKE_MATCH_1}")
    file(TO_CMAKE_PATH "${dir}" dir_cm)
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}/kinect-viewer" real_root)
    string(TOLOWER "${dir_cm}" dir_lc)
    string(TOLOWER "${real_root}" real_lc)
    string(FIND "${dir_lc}" "${real_lc}" at)
    if(at EQUAL 0)
        message(FATAL_ERROR "a test-hook launch used the real state folder: ${dir}")
    endif()
    file(TO_CMAKE_PATH "$ENV{TEMP}" temp_cm)
    string(TOLOWER "${temp_cm}" temp_lc)
    string(FIND "${dir_lc}" "${temp_lc}" at)
    if(NOT at EQUAL 0)
        message(FATAL_ERROR "the state folder isn't under %TEMP%: ${dir}")
    endif()
    get_filename_component(parent "${dir_cm}" DIRECTORY)
    if(EXISTS "${parent}")
        message(FATAL_ERROR "the run's temp state folder was left behind: ${parent}")
    endif()
    message(STATUS "isolated in ${dir} (removed on exit)")
    return()
endif()

# ---- seed the --state-dir -------------------------------------------------------------------------------
file(REMOVE_RECURSE "${STATE_DIR}")
file(REMOVE "${OUT}" "${OUT}.txt" "${OUT}.devices.png" "${OUT}.pairing.png" "${OUT}-first.png" "${OUT}-first.png.txt")
# A first launch makes the host's identity and an empty device list there (pairing.json, devices.json).
run_viewer(rc --synthetic --mode depth --state-dir "${STATE_DIR}" --screenshot "${OUT}-first.png" --screenshot-after-ms 800
           --exit-after-screenshot)
if(NOT EXISTS "${STATE_DIR}/devices.json" OR NOT EXISTS "${STATE_DIR}/pairing.json")
    message(FATAL_ERROR "the first launch left no state in --state-dir (rc=${rc})")
endif()
file(READ "${OUT}-first.png.txt" first)
file(TO_NATIVE_PATH "${STATE_DIR}" state_native)
string(REPLACE "\\" "\\\\" state_re "${state_native}")
string(REPLACE "." "\\." state_re "${state_re}")
set(report "${first}")
expect("state_dir_temp=0\n" "pet_devices=0\n")
string(TOLOWER "${first}" first_lc)
string(TOLOWER "state_dir=${state_re}\n" want_lc)
if(NOT first_lc MATCHES "${want_lc}")
    message(FATAL_ERROR "the report doesn't name --state-dir (${state_native})")
endif()
# Two synthetic devices: a viewer seen two days ago and an owner not seen for 40 days (highlighted).
string(TIMESTAMP now_s "%s" UTC)
math(EXPR paired_ms "(${now_s} - 90 * 86400) * 1000")
math(EXPR second_seen_ms "(${now_s} - 2 * 86400) * 1000")
math(EXPR ipad_seen_ms "(${now_s} - 40 * 86400) * 1000")
set(devices "[{\"certSHA256\":\"ERERERERERERERERERERERERERERERERERERERERERE=\",\"name\":\"Second phone\",\"role\":\"viewer\",\"kind\":\"iphone\",\"pairedAtUnixMs\":${paired_ms},\"lastSeenUnixMs\":${second_seen_ms},\"lastProtocolVersion\":3},{\"certSHA256\":\"IiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiI=\",\"name\":\"Old iPad\",\"role\":\"owner\",\"kind\":\"ipad\",\"pairedAtUnixMs\":${paired_ms},\"lastSeenUnixMs\":${ipad_seen_ms},\"lastProtocolVersion\":3}]")
file(READ "${STATE_DIR}/devices.json" dj)
string(FIND "${dj}" "\"devices\":[]" at)
if(at LESS 0)
    message(FATAL_ERROR "unexpected devices.json from the first launch: ${dj}")
endif()
string(REPLACE "\"devices\":[]" "\"devices\":${devices}" dj "${dj}")
file(WRITE "${STATE_DIR}/devices.json" "${dj}")
file(WRITE "${STATE_DIR}/pet-camera.json" "{\"sharingEnabled\":true,\"sharingSchedule\":[],\"privacyMode\":false}")

# ---- the hook ---------------------------------------------------------------------------------------------
run_viewer(rc --synthetic --mode depth --show-health --state-dir "${STATE_DIR}" --test-pet-camera --screenshot "${OUT}"
           --exit-after-screenshot)
if(NOT EXISTS "${OUT}.txt")
    message(FATAL_ERROR "the viewer wrote no report (rc=${rc})")
endif()
file(READ "${OUT}.txt" report)
message(STATUS "${report}")
expect("ok=1\n" "running=1\n" "state_dir_temp=0\n"
       "pet_ran=1\n" "pet_failed=\n" "pet_done=1\n"
       # loaded from --state-dir, before the hook changed anything
       "pet_loaded_devices=2\n" "pet_loaded_names=Second phone.Old iPad\n" "pet_loaded_sharing=1\n"
       "pet_loaded_privacy=0\n" "pet_loaded_stale_owners=1\n" "pet_sharing_turned_on=0\n"
       # paired through the window as a viewer (the default), watching: the banner names it
       "pet_device_role=viewer\n" "pet_banner_shown=1\n" "pet_banner_text=Watching now: Test viewer\n"
       "pet_tier_high_sessions=1\n" "pet_health_tiers_shown=1\n" "pet_tiers=Live tiers: high [^\n]*1 session"
       # privacy mode: the banner goes, the button shows it, no frame while on, back when off
       "pet_privacy_hid_banner=1\n" "pet_privacy_button_on=1\n" "pet_privacy_frames_while_on=0\n" "pet_privacy_off_back=1\n"
       # sharing off stops a viewer; a schedule row covering now does too
       "pet_sharing_off_hid=1\n" "pet_sharing_on_back=1\n"
       "pet_schedule_row_added=1\n" "pet_schedule_hid=1\n" "pet_schedule_removed=1\n" "pet_schedule_back=1\n"
       # the Devices dialog: a rename reaches the banner; three rows, the stale owner highlighted; captured
       "pet_renamed=1\n" "pet_renamed_banner=Watching now: Garden phone\n" "pet_devices_dialog_rows=3\n"
       "pet_devices_highlighted=1\n" "pet_devices_captured=1\n"
       # the dialog's Revoke: 0x0F revoked, out of the list, the banner gone
       "pet_revoked_notice=1\n" "pet_devices_after_revoke=2\n" "pet_devices_dialog_rows_after=2\n" "pet_banner_after_revoke=0\n"
       # what the panel shows at the end
       "pet_devices=2\n" "pet_sharing=1\n" "pet_privacy=0\n" "pet_schedule_rows=0\n" "watching_banner=\n")
foreach(png "${OUT}" "${OUT}.devices.png")
    if(NOT EXISTS "${png}")
        message(FATAL_ERROR "missing capture ${png}")
    endif()
    file(SIZE "${png}" sz)
    if(sz LESS 3000)
        message(FATAL_ERROR "capture too small: ${png} (${sz} B)")
    endif()
endforeach()
# What the run left in --state-dir: the settings as the hook ended them, the revoked device gone.
file(READ "${STATE_DIR}/pet-camera.json" pc)
file(READ "${STATE_DIR}/devices.json" dj)
if(NOT pc MATCHES "\"sharingEnabled\":true" OR NOT pc MATCHES "\"privacyMode\":false" OR NOT pc MATCHES "\"sharingSchedule\":\\[\\]")
    message(FATAL_ERROR "pet-camera.json: ${pc}")
endif()
if(NOT dj MATCHES "Second phone" OR NOT dj MATCHES "Old iPad" OR dj MATCHES "Garden phone" OR dj MATCHES "\"revoked\":\\[\\]")
    message(FATAL_ERROR "devices.json: ${dj}")
endif()
