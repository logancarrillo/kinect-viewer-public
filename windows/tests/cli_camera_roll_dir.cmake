# kinect-cli serve --camera-roll-dir (W7, protocol v3 §10 "Test isolation"): the roll is recorded only into the
# folder named on the command line. Without the flag serve has no roll at all (it never picks a folder of its
# own, so a test can't reach the real camera roll); a folder the roll refuses (pet-camera.md §6.5: here one that
# holds other files) stops serve with exit 2 and the reason, and is left as it was. Synthetic, loopback only,
# in the build tree.
#
# W7: a synthetic or loopback-only serve is a test run, whose folder must be new,
# empty, or a test roll (it holds .kinect-viewer-test-roll, which the host writes when it prepares one). A temp folder
# made to look like a real roll is refused and left alone. No real path is ever passed here, not even to check a
# refusal: the path-text refusals are checked by parsing (test_viewer_model). The run that turns the roll on waits
# until the folder is prepared (--until-roll-prepared, with --duration as the deadline), not for a fixed time.
set(base "${OUTDIR}/test-state/camera-roll-dir")
file(REMOVE_RECURSE "${base}")
file(MAKE_DIRECTORY "${base}")

# A new folder, with the roll off (a fresh state, no pet-camera.json): accepted and named in the output, but
# left alone. W7: with the roll off the host creates and changes nothing there.
set(roll "${base}/roll")
set(state "${base}/state") # this host's own state, so the next run can turn the roll on in its pet-camera.json
execute_process(
    COMMAND "${CLI}" serve --synthetic --loopback-only --duration 1 --state-dir "${state}" --camera-roll-dir "${roll}"
    TIMEOUT 60
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE so
    ERROR_VARIABLE se)
message(STATUS "serve with a roll, off: rc=${rc}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "kinect-cli serve --camera-roll-dir: expected exit 0, got ${rc}\n${so}\n${se}")
endif()
if(NOT so MATCHES "camera roll: ")
    message(FATAL_ERROR "kinect-cli serve --camera-roll-dir: the roll isn't on\n${so}")
endif()
if(EXISTS "${roll}")
    message(FATAL_ERROR "kinect-cli serve --camera-roll-dir: the folder was created with the roll off")
endif()

# The roll on in this host's own pet-camera.json: the folder is created and protected (its markers written).
file(WRITE "${state}/pet-camera.json" "{\"sharingEnabled\":false,\"privacyMode\":false,\"rollEnabled\":true}")
execute_process(
    COMMAND "${CLI}" serve --synthetic --loopback-only --until-roll-prepared --duration 45 --state-dir "${state}"
            --camera-roll-dir "${roll}"
    TIMEOUT 60
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE so
    ERROR_VARIABLE se)
message(STATUS "serve with a roll, on: rc=${rc}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "kinect-cli serve --camera-roll-dir (roll on): expected exit 0, got ${rc}\n${so}\n${se}")
endif()
if(NOT so MATCHES "camera roll prepared")
    message(FATAL_ERROR "kinect-cli serve --camera-roll-dir (roll on): it didn't say the folder was prepared\n${so}\n${se}")
endif()
if(NOT EXISTS "${roll}/.nosync" OR NOT EXISTS "${roll}/.metadata_never_index")
    message(FATAL_ERROR "kinect-cli serve --camera-roll-dir (roll on): the folder wasn't prepared\n${so}\n${se}")
endif()
# A test run's roll is marked as one when it is prepared.
if(NOT EXISTS "${roll}/.kinect-viewer-test-roll")
    message(FATAL_ERROR "kinect-cli serve --camera-roll-dir (roll on): the test roll wasn't marked")
endif()

# A folder holding the marker (the one just made) is accepted again.
execute_process(
    COMMAND "${CLI}" serve --synthetic --loopback-only --until-roll-prepared --duration 45 --state-dir "${state}"
            --camera-roll-dir "${roll}"
    TIMEOUT 60
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE so
    ERROR_VARIABLE se)
if(NOT rc EQUAL 0 OR NOT so MATCHES "camera roll prepared")
    message(FATAL_ERROR "kinect-cli serve: a marked test roll wasn't accepted: rc=${rc}\n${so}\n${se}")
endif()

# A temp folder made to look like a real roll (a day folder, the roll's markers and log, but no test marker): a test
# run refuses it, exit 2 with the reason, and leaves every file as it was.
set(fake "${base}/fake-real-roll")
file(MAKE_DIRECTORY "${fake}/2026-11-18")
file(WRITE "${fake}/.nosync" "")
file(WRITE "${fake}/.metadata_never_index" "")
file(WRITE "${fake}/deletion-log.txt" "")
file(WRITE "${fake}/2026-11-18/14-03-22_motion.json" "{\"version\":1}")
file(GLOB_RECURSE fake_before RELATIVE "${fake}" "${fake}/*")
execute_process(
    COMMAND "${CLI}" serve --synthetic --loopback-only --duration 1 --camera-roll-dir "${fake}"
    TIMEOUT 60
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE so
    ERROR_VARIABLE se)
message(STATUS "serve with a roll that looks real: rc=${rc} ${se}")
if(NOT rc EQUAL 2 OR NOT se MATCHES "CAMERA_ROLL_REFUSED: .*test run")
    message(FATAL_ERROR "kinect-cli serve: a folder that looks like a real roll wasn't refused: rc=${rc}\n${so}\n${se}")
endif()
file(GLOB_RECURSE fake_after RELATIVE "${fake}" "${fake}/*")
if(NOT fake_before STREQUAL fake_after)
    message(FATAL_ERROR "kinect-cli serve changed a refused roll: ${fake_before} -> ${fake_after}")
endif()

# A new subfolder of that roll is refused too, however it is named: by its own path, and through a
# junction (mklink /J) to the roll or to one of its days. serve resolves the path as the viewer does, and the host
# refuses a test roll inside a roll that isn't one. The roll is on here, so a folder accepted would be made and
# prepared (the host state from above has it on); the fixture must stay exactly as it was. The old code accepted all
# three and made the subfolder.
set(link "${base}/link-to-fake")
set(day_link "${base}/link-to-fake-day")
file(TO_NATIVE_PATH "${link}" link_native)
file(TO_NATIVE_PATH "${day_link}" day_link_native)
file(TO_NATIVE_PATH "${fake}" fake_native)
file(TO_NATIVE_PATH "${fake}/2026-11-18" day_native)
foreach(pair IN ITEMS "${link_native}|${fake_native}" "${day_link_native}|${day_native}")
    string(REPLACE "|" ";" pair_list "${pair}")
    list(GET pair_list 0 junction)
    list(GET pair_list 1 junction_target)
    execute_process(COMMAND cmd /c mklink /J "${junction}" "${junction_target}" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE se)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "mklink /J ${junction}: rc=${rc} ${se}")
    endif()
endforeach()
file(GLOB_RECURSE fake_before LIST_DIRECTORIES true RELATIVE "${fake}" "${fake}/*")
foreach(inside IN ITEMS "${fake}/newsub" "${link}/newsub" "${day_link}/newsub")
    execute_process(
        COMMAND "${CLI}" serve --synthetic --loopback-only --until-roll-prepared --duration 20 --state-dir "${state}"
                --camera-roll-dir "${inside}"
        TIMEOUT 60
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE so
        ERROR_VARIABLE se)
    message(STATUS "serve with a roll inside one that looks real (${inside}): rc=${rc} ${se}")
    if(NOT rc EQUAL 2 OR NOT se MATCHES "CAMERA_ROLL_REFUSED: ")
        message(FATAL_ERROR "kinect-cli serve: ${inside}, inside a roll that looks real, wasn't refused: rc=${rc}\n${so}\n${se}")
    endif()
endforeach()
file(GLOB_RECURSE fake_after LIST_DIRECTORIES true RELATIVE "${fake}" "${fake}/*")
# The junctions go first, as links (rmdir never follows one), so nothing below can reach through them.
execute_process(COMMAND cmd /c rmdir "${link_native}")
execute_process(COMMAND cmd /c rmdir "${day_link_native}")
if(NOT fake_before STREQUAL fake_after)
    message(FATAL_ERROR "kinect-cli serve changed a roll that looks real: ${fake_before} -> ${fake_after}")
endif()

# No flag: no roll, and nothing named.
execute_process(
    COMMAND "${CLI}" serve --synthetic --loopback-only --duration 1
    TIMEOUT 60
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE so
    ERROR_VARIABLE se)
if(NOT rc EQUAL 0 OR so MATCHES "camera roll: ")
    message(FATAL_ERROR "kinect-cli serve without --camera-roll-dir: rc=${rc}, or a roll is on\n${so}")
endif()

# A folder of other files: refused, exit 2 with the reason, and left exactly as it was.
set(other "${base}/other")
file(MAKE_DIRECTORY "${other}")
file(WRITE "${other}/notes.txt" "not a camera roll\n")
execute_process(
    COMMAND "${CLI}" serve --synthetic --loopback-only --duration 1 --camera-roll-dir "${other}"
    TIMEOUT 60
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE so
    ERROR_VARIABLE se)
message(STATUS "serve with a refused roll: rc=${rc} ${se}")
if(NOT rc EQUAL 2)
    message(FATAL_ERROR "kinect-cli serve: expected exit 2 (roll folder refused), got ${rc}\n${so}")
endif()
if(NOT se MATCHES "CAMERA_ROLL_REFUSED: ")
    message(FATAL_ERROR "kinect-cli serve: no refusal reason on stderr\n${se}")
endif()
file(GLOB left RELATIVE "${other}" "${other}/*")
if(NOT left STREQUAL "notes.txt")
    message(FATAL_ERROR "kinect-cli serve wrote into a refused roll folder: ${left}")
endif()
file(REMOVE_RECURSE "${base}")
