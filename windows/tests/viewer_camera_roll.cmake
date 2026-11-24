# The camera roll on the PC (pet-camera.md §4, §6; protocol v3 §10 "Test isolation"), synthetic only. Every
# launch passes --synthetic and a --state-dir under the build tree; the roll, when there is one, records only into
# the --camera-roll-dir under the build tree. Nothing here reads, lists or creates the real camera roll
# (either default folder), the recordings folder or the real .host-state: the refusals are checked by the path
# alone.
#
# MODE=HOOK: --test-camera-roll. The panel's roll settings (on, motion sensitivity, a schedule row, Keep, the folder
#   "Change..." refusing the recordings folder and saving a test folder for after a restart, the D6 note); depth
#   frames marked as not the live sensor's never start a clip; the viewer's own depth feed starts a motion clip; a
#   manual clip through "Record a clip" and "Stop", and a second one; the limits beside the switch and Confirm;
#   the D6 note and "Preparing..." following the host's predicates; the browser lists every clip, found by
#   id in whichever day it landed, the manual one with its thumbnail and "This PC"; a star; the player, with the
#   sensor running, decodes on its worker: every frame the host is handed meanwhile is a live-source frame, and
#   a scrub drag doesn't stall them; a seek lands; with the sensor off (Motion greys out) the host is handed
#   nothing while it decodes; the player pins its clip, and a phone owner's delete of it closes the player first
#   the screenshot hook refuses while the player or the thumbnails are on screen and allows again once they
#   close; the browser's Delete of the clip the player has open. Then what the run left on disk.
# MODE=ISOLATION: a --state-dir whose pet-camera.json has everything on and a rollFolder of its own, launched with
#   --synthetic and no --camera-roll-dir: no roll, and that folder is never created.
# MODE=UNREADABLE: pet-camera.json can't be read (a folder stands in its place): the roll's settings are refused with
#   the reason, as the other pet-camera controls are.
# MODE=REFUSALS: --camera-roll-dir naming a folder under the build tree made to look like a real camera roll (day
#   folders, the roll's markers, a clip; no .kinect-viewer-test-roll), and --test-camera-roll without
#   --camera-roll-dir, are refused (exit 2) before anything starts, and the fake roll is left as it was. No
#   launch names the real camera roll or the recordings folder, not even to see it refused: test_viewer_model
#   checks those path-text refusals by parsing, as it does the same flags without --synthetic, so no test
#   launches the viewer without --synthetic or at a real folder.
# HOOK, ISOLATION and UNREADABLE need the viewer's exit code to be 0, so a crash or a hang at exit fails them.
cmake_minimum_required(VERSION 3.20)

function(run_viewer out_var)
    execute_process(COMMAND "${VIEWER}" ${ARGN} TIMEOUT 240 RESULT_VARIABLE rc)
    set(${out_var} ${rc} PARENT_SCOPE)
endfunction()

# The patterns are checked against ${report} in the caller.
function(expect)
    foreach(want IN LISTS ARGN)
        if(NOT report MATCHES "${want}")
            message(FATAL_ERROR "expected '${want}' in the report")
        endif()
    endforeach()
endfunction()

# A --state-dir the host takes: its marker, and (optionally) a pet-camera.json.
function(seed_state dir)
    file(REMOVE_RECURSE "${dir}")
    file(MAKE_DIRECTORY "${dir}")
    file(WRITE "${dir}/.kinect-viewer-host-state" "kinect-viewer host state\n")
endfunction()

if(MODE STREQUAL "REFUSALS")
    set(base "${OUTDIR}/test-state/roll-refusals")
    set(fake "${OUTDIR}/test-state/roll-refusals-looks-real")
    file(REMOVE_RECURSE "${base}" "${fake}")
    # A fake roll under the build tree, shaped like a real one (pet-camera.md §6.3, §6.5): day folders, the
    # markers the host leaves in a roll it prepares, a clip and its sidecar; but not the test-roll marker.
    file(MAKE_DIRECTORY "${fake}/2026-11-17" "${fake}/2026-11-18")
    file(WRITE "${fake}/.nosync" "")
    file(WRITE "${fake}/.metadata_never_index" "")
    file(WRITE "${fake}/2026-11-18/2026-11-18_101500_motion.mp4" "not a real clip")
    file(WRITE "${fake}/2026-11-18/2026-11-18_101500_motion.json" "{\"clipId\":\"2026-11-18_101500_motion\"}")
    file(GLOB_RECURSE fake_before LIST_DIRECTORIES true RELATIVE "${fake}" "${fake}/*")
    set(names looks-real hook-no-dir)
    foreach(name IN LISTS names)
        set(out "${OUTDIR}/roll-refused-${name}.png")
        file(REMOVE "${out}" "${out}.txt" "${out}.refused.txt")
        if(name STREQUAL "hook-no-dir")
            set(args --synthetic --mode depth --test-camera-roll)
        else()
            set(args --synthetic --mode depth --test-camera-roll --camera-roll-dir "${fake}")
        endif()
        run_viewer(rc ${args} --state-dir "${base}/state" --screenshot "${out}" --screenshot-after-ms 300 --exit-after-screenshot)
        if(NOT rc EQUAL 2)
            message(FATAL_ERROR "${name}: expected exit 2 (refused), got ${rc}")
        endif()
        if(EXISTS "${out}" OR EXISTS "${out}.txt")
            message(FATAL_ERROR "${name}: the viewer ran and captured")
        endif()
        if(NOT EXISTS "${out}.refused.txt")
            message(FATAL_ERROR "${name}: no refusal reason written")
        endif()
        file(READ "${out}.refused.txt" why)
        message(STATUS "${name}: ${why}")
        if(name STREQUAL "hook-no-dir")
            if(NOT why MATCHES "needs --camera-roll-dir")
                message(FATAL_ERROR "${name}: unexpected reason: ${why}")
            endif()
        elseif(NOT why MATCHES "must be a new or empty folder, or one a test made \\(it holds \\.kinect-viewer-test-roll\\)")
            message(FATAL_ERROR "${name}: unexpected reason: ${why}")
        endif()
        file(REMOVE "${out}.refused.txt")
    endforeach()
    if(EXISTS "${base}")
        message(FATAL_ERROR "a refused run started the streaming host (its state folder exists)")
    endif()
    # The fake roll is as it was: nothing created, prepared or deleted in it.
    file(GLOB_RECURSE fake_after LIST_DIRECTORIES true RELATIVE "${fake}" "${fake}/*")
    if(NOT fake_before STREQUAL fake_after)
        message(FATAL_ERROR "the refused run changed the fake roll: before ${fake_before}, after ${fake_after}")
    endif()
    file(READ "${fake}/2026-11-18/2026-11-18_101500_motion.mp4" clip)
    if(NOT clip STREQUAL "not a real clip")
        message(FATAL_ERROR "the refused run changed the fake roll's clip")
    endif()
    file(REMOVE_RECURSE "${fake}")
    return()
endif()

# The run ended cleanly (the report is printed first, so a failure shows what the run saw).
function(expect_clean_exit rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "the viewer's exit code was ${rc}, not 0 (a crash, a hang at exit, or a refusal)")
    endif()
endfunction()

if(MODE STREQUAL "ISOLATION")
    set(state "${STATE_DIR}-isolation")
    set(chosen "${STATE_DIR}-isolation-roll-folder")
    file(REMOVE_RECURSE "${chosen}")
    seed_state("${state}")
    file(TO_NATIVE_PATH "${chosen}" chosen_native)
    string(REPLACE "\\" "\\\\" chosen_json "${chosen_native}")
    file(WRITE "${state}/pet-camera.json"
         "{\"sharingEnabled\":true,\"sharingSchedule\":[],\"privacyMode\":false,\"rollEnabled\":true,\"rollFolder\":\"${chosen_json}\","
         "\"rollKeepDays\":30,\"rollCapBytes\":200000000000,\"motionEnabled\":true,\"motionSensitivity\":9,"
         "\"recordScheduleEnabled\":true,\"recordSchedule\":[{\"days\":127,\"start\":\"00:00\",\"minutes\":60}]}")
    file(REMOVE "${OUT}" "${OUT}.txt")
    run_viewer(rc --synthetic --mode depth --state-dir "${state}" --screenshot "${OUT}" --screenshot-after-ms 2500
               --exit-after-screenshot)
    if(NOT EXISTS "${OUT}.txt")
        message(FATAL_ERROR "the viewer wrote no report (rc=${rc})")
    endif()
    file(READ "${OUT}.txt" report)
    message(STATUS "${report}")
    expect_clean_exit("${rc}")
    # The settings were read (everything on), but this run has no roll at all.
    expect("ok=1\n" "running=1\n" "roll_enabled=1\n" "roll_motion=1\n" "roll_schedule_on=1\n" "roll_available=0\n" "roll_test_run=1\n"
           "roll_dir=\n" "roll_error=a test run without --camera-roll-dir has no camera roll" "roll_clips_finalized=0\n"
           "motion_fed=0\n")
    if(EXISTS "${chosen}")
        message(FATAL_ERROR "a test run without --camera-roll-dir created the rollFolder setting's folder: ${chosen}")
    endif()
    return()
endif()

if(MODE STREQUAL "UNREADABLE")
    set(state "${STATE_DIR}-unreadable")
    set(roll "${ROLL_DIR}-unreadable")
    file(REMOVE_RECURSE "${roll}")
    seed_state("${state}")
    file(MAKE_DIRECTORY "${state}/pet-camera.json") # there, but it can't be read as a file
    file(REMOVE "${OUT}" "${OUT}.txt")
    run_viewer(rc --synthetic --mode depth --state-dir "${state}" --camera-roll-dir "${roll}" --test-camera-roll
               --screenshot "${OUT}" --exit-after-screenshot)
    if(NOT EXISTS "${OUT}.txt")
        message(FATAL_ERROR "the viewer wrote no report (rc=${rc})")
    endif()
    file(READ "${OUT}.txt" report)
    message(STATUS "${report}")
    expect_clean_exit("${rc}")
    expect("rt_ran=1\n" "rt_unreadable=1\n" "rt_refused_unreadable=1\n" "pet_privacy=1\n" "roll_enabled=0\n"
           "roll_note=Record to the camera roll wasn't changed: [^\n]*pet-camera")
    file(REMOVE_RECURSE "${state}" "${roll}")
    return()
endif()

# ---- the hook --------------------------------------------------------------------------------------------------
set(state "${STATE_DIR}")
set(roll "${ROLL_DIR}")
set(next "${ROLL_DIR}-next") # the folder "Change..." saves: for a later, real start; this run never creates it
seed_state("${state}")
file(REMOVE_RECURSE "${roll}" "${next}")
file(REMOVE "${OUT}" "${OUT}.txt")
run_viewer(rc --synthetic --mode depth --state-dir "${state}" --camera-roll-dir "${roll}" --test-camera-roll
           --screenshot "${OUT}" --exit-after-screenshot)
if(NOT EXISTS "${OUT}.txt")
    message(FATAL_ERROR "the viewer wrote no report (rc=${rc})")
endif()
file(READ "${OUT}.txt" report)
message(STATUS "${report}")
expect_clean_exit("${rc}")
expect("ok=1\n" "rt_ran=1\n" "rt_failed=\n" "rt_done=1\n" "rt_available_at_start=1\n" "roll_available=1\n"
       # the settings, through the panel
       "rt_roll_on=1\n" "rt_sens_set=1\n" "rt_sched_row_added=1\n" "rt_sched_on=1\n" "rt_keep_set=1\n"
       "rt_folder_refused_recordings=1\n" "rt_folder_refusal=The camera roll can't be the depth/IR recordings folder"
       "rt_folder_changed=1\n" "rt_folder_note=[^\n]*applies after a restart"
       "rt_d6_note_shown=1\n" "rt_d6_note=Skipped while the camera roll is on" "rt_d6_mismatch=0\n" "rt_preparing_mismatch=0\n"
       # The limits beside the switch; Confirm while they aren't the owner's, gone once they are
       "rt_limits_text=Keeps clips 30 days, up to 200[.0]* GB\\.\n" "rt_confirm_shown=1\n" "rt_confirm_ok=1\n"
       "rt_limits_known_later=1\n" "rt_confirm_hidden_later=1\n" "roll_limits=Keeps clips 14 days, up to 50[.0]* GB\\.\n"
       # The host is told this is a test run
       "roll_test_run=1\n"
       # motion: frames that aren't the live sensor's never start a clip; the viewer's own depth feed does
       "rt_armed=1\n" "rt_motion_from_replay_frames_started=0\n" "rt_motion_viewer_clip=1\n" "rt_motion_clip_finalized=1\n"
       # a manual clip through the panel's buttons
       "rt_manual_started=1\n" "rt_manual_stopped=1\n" "rt_manual2_stopped=1\n" "rt_clips_finalized=3\n"
       # the sensor off: Motion greys out with the reason
       "rt_motion_greyed=1\n" "rt_motion_greyed_text=Motion needs the live sensor"
       # the browser: every day's clips, found by id (midnight-proof), the manual one with its thumbnail and
       # "This PC"; a star
       "rt_browser_days=[1-9]\n" "rt_browser_clips=3\n" "rt_browser_thumbs=[1-3]\n" "rt_manual_found=1\n"
       "rt_manual_has_thumb=1\n" "rt_manual_by_pc=1\n" "rt_motion_found=1\n" "rt_manual2_found=1\n" "rt_starred=1\n"
       # the player, with the sensor running: every frame the host was handed meanwhile was a live-source
       # frame, and some were; then with the sensor off: nothing reaches the host while the player decodes
       "rt_player_opened=1\n" "rt_player_seeked=1\n" "rt_player_live_frames=[1-9][0-9]*\n" "rt_player_foreign_frames=0\n"
       "rt_onframe_during_player=0\n" "rt_tier_frames_during_player=0\n" "rt_player_decoded_while_off=[1-9][0-9]*\n"
       # The player pins its clip and lets go when it closes; a phone owner's delete of the clip it has
       # open closes it first ("This clip was deleted."), it unpins, and the clip goes
       "rt_pins_while_open=1\n" "rt_pins_after_close=0\n" "rt_owner_delete=held=1 released=1 deleted=1\n"
       "rt_owner_delete_player_closed=1\n" "rt_owner_delete_notice=This clip was deleted\\.\n" "rt_owner_delete_pins_after=0\n"
       "rt_owner_delete_gone=1\n"
       # the screenshot rule while a roll clip is on screen, and after
       "rt_shot_refused_browser=1\n" "rt_shot_refused_player=1\n" "rt_shot_refused_browser_only=1\n" "rt_shot_allowed_after=1\n"
       # the browser's Delete of the clip the player has open: the player closes first
       "rt_deleted=1\n" "rt_pc_delete_closed_player=1\n" "rt_deleted_gone=1\n"
       # where it ended
       "roll_enabled=1\n" "roll_motion=0\n" "roll_sensitivity=7\n" "roll_schedule_on=1\n" "roll_schedule_rows=1\n"
       "roll_keep_days=14\n" "roll_cap_bytes=50000000000\n" "roll_folder_setting=[^\n]*-next\n")
# While the owner drags the scrub bar the UI thread only asks; the decode is the player's worker's. Live frames
# keep reaching the host during the drag at least half as often as during plain playback just before it, the drag's
# positions coalesce, and the last one asked for is decoded.
foreach(key rt_seek_burst_baseline_live rt_seek_burst_live rt_seek_burst_requests rt_seek_burst_completed)
    string(REGEX MATCH "${key}=([0-9]+)" _ "${report}")
    set(${key} "${CMAKE_MATCH_1}")
endforeach()
if(rt_seek_burst_baseline_live LESS 30)
    message(FATAL_ERROR "only ${rt_seek_burst_baseline_live} live frames reached the host in 3 s of playback")
endif()
math(EXPR burst_twice "${rt_seek_burst_live} * 2")
if(burst_twice LESS rt_seek_burst_baseline_live)
    message(FATAL_ERROR "the scrub stalled the live frames: ${rt_seek_burst_live} in 3 s of scrubbing, ${rt_seek_burst_baseline_live} before")
endif()
if(rt_seek_burst_requests LESS 100 OR rt_seek_burst_completed LESS 1 OR NOT rt_seek_burst_completed LESS rt_seek_burst_requests)
    message(FATAL_ERROR "the scrub's seeks: ${rt_seek_burst_requests} asked for, ${rt_seek_burst_completed} decoded")
endif()
string(REGEX MATCH "rt_player_frames=([0-9]+)" _ "${report}")
if(CMAKE_MATCH_1 LESS 15)
    message(FATAL_ERROR "the player decoded only ${CMAKE_MATCH_1} frames")
endif()
string(REGEX MATCH "rt_player_size=([0-9]+)x([0-9]+)" _ "${report}")
if(CMAKE_MATCH_1 LESS 64 OR CMAKE_MATCH_2 LESS 64)
    message(FATAL_ERROR "the player's frame size: ${CMAKE_MATCH_1}x${CMAKE_MATCH_2}")
endif()
string(REGEX MATCH "rt_player_seek_target=([0-9]+)" _ "${report}")
set(target "${CMAKE_MATCH_1}")
string(REGEX MATCH "rt_player_seek_pts=([0-9]+)" _ "${report}")
math(EXPR off "${CMAKE_MATCH_1} - ${target}")
if(off LESS -10000000 OR off GREATER 10000000) # within a second (100 ns units)
    message(FATAL_ERROR "the seek landed ${off} hns from its target")
endif()
if(NOT EXISTS "${OUT}")
    message(FATAL_ERROR "no capture")
endif()
file(SIZE "${OUT}" sz)
if(sz LESS 3000)
    message(FATAL_ERROR "capture too small (${sz} B)")
endif()

# What the run left on disk: the roll's markers; the manual clip (its .mp4, .jpg and starred sidecar); the motion
# clip deleted; the settings in pet-camera.json; and the folder "Change..." saved never created.
if(NOT EXISTS "${roll}/.nosync" OR NOT EXISTS "${roll}/.metadata_never_index")
    message(FATAL_ERROR "the roll folder wasn't prepared")
endif()
file(GLOB manual_mp4 "${roll}/*/*_manual.mp4")
file(GLOB manual_jpg "${roll}/*/*_manual.jpg")
file(GLOB manual_json "${roll}/*/*_manual.json")
file(GLOB motion_files "${roll}/*/*_motion*")
list(LENGTH manual_mp4 n_mp4)
list(LENGTH manual_jpg n_jpg)
list(LENGTH manual_json n_json)
list(LENGTH motion_files n_motion)
if(NOT n_mp4 EQUAL 1 OR NOT n_jpg EQUAL 1 OR NOT n_json EQUAL 1 OR NOT n_motion EQUAL 0)
    message(FATAL_ERROR "the roll holds ${n_mp4} manual .mp4, ${n_jpg} .jpg, ${n_json} .json and ${n_motion} motion files")
endif()
file(READ "${manual_json}" sidecar)
if(NOT sidecar MATCHES "\"starred\":true" OR NOT sidecar MATCHES "\"deviceName\":\"This PC\"")
    message(FATAL_ERROR "the manual clip's sidecar: ${sidecar}")
endif()
file(READ "${state}/pet-camera.json" pc)
foreach(want "\"rollEnabled\":true" "\"motionEnabled\":false" "\"motionSensitivity\":7" "\"recordScheduleEnabled\":true"
             "\"rollKeepDays\":14" "\"rollCapBytes\":50000000000" "\"rollFolder\":\"[^\"]*-next\"")
    if(NOT pc MATCHES "${want}")
        message(FATAL_ERROR "pet-camera.json lacks ${want}: ${pc}")
    endif()
endforeach()
if(EXISTS "${next}")
    message(FATAL_ERROR "the folder \"Change...\" saved was created by this run: ${next}")
endif()
