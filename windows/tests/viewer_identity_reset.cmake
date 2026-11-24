# pairing.json fails closed (the rule for this PC's identity), from the PC's side. Synthetic only;
# the --state-dir is under the build tree, never the real %USERPROFILE%\kinect-viewer\.host-state.
#
# The folder holds the host's marker and a pairing.json that can't be parsed:
# 1. kinect-cli serve fails with the error and exit code 1, and makes no new identity.
# 2. The viewer starts with the host off: its report and status line carry the error, and pairing.json is as
#    it was (kept as pairing.json.bad).
# 3. The viewer's --test-identity-reset hook checks the Pet camera panel's error and its two buttons, presses
#    "Try again" (still unusable, nothing written), then "Reset this PC's identity" past its confirm dialog: a new
#    pairing.json is written, the host serves, and the hook's fake device pairs and connects over loopback.
cmake_minimum_required(VERSION 3.20)

function(expect)
    foreach(want IN LISTS ARGN)
        if(NOT report MATCHES "${want}")
            message(FATAL_ERROR "expected '${want}' in the report")
        endif()
    endforeach()
endfunction()

function(expect_pairing_unchanged who)
    file(SHA256 "${STATE_DIR}/pairing.json" now)
    if(NOT now STREQUAL corrupt_sha)
        message(FATAL_ERROR "${who}: pairing.json was changed")
    endif()
    if(NOT EXISTS "${STATE_DIR}/pairing.json.bad")
        message(FATAL_ERROR "${who}: no pairing.json.bad")
    endif()
    file(SHA256 "${STATE_DIR}/pairing.json.bad" bad)
    if(NOT bad STREQUAL corrupt_sha)
        message(FATAL_ERROR "${who}: pairing.json.bad isn't the original")
    endif()
endfunction()

file(REMOVE_RECURSE "${STATE_DIR}")
file(MAKE_DIRECTORY "${STATE_DIR}")
file(WRITE "${STATE_DIR}/.kinect-viewer-host-state" "kinect-viewer host state\n")
file(WRITE "${STATE_DIR}/pairing.json" "{\"serverIdentity\":{\"privateKeyRaw\":\"AAAA\",\"certificateDER\":")
file(SHA256 "${STATE_DIR}/pairing.json" corrupt_sha)

# ---- 1. kinect-cli serve -------------------------------------------------------------------------------------
execute_process(
    COMMAND "${CLI}" serve --synthetic --loopback-only --duration 1 --state-dir "${STATE_DIR}"
    TIMEOUT 60
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE so
    ERROR_VARIABLE se)
message(STATUS "kinect-cli serve: rc=${rc} ${se}")
if(NOT rc EQUAL 1)
    message(FATAL_ERROR "kinect-cli serve: exit code ${rc}, not 1, over a pairing.json it couldn't parse\n${so}")
endif()
if(NOT se MATCHES "IDENTITY_UNUSABLE: pairing.json [^\n]*couldn't be parsed")
    message(FATAL_ERROR "kinect-cli serve: the error doesn't say pairing.json is unusable\n${se}")
endif()
expect_pairing_unchanged("kinect-cli serve")

# ---- 2. the viewer, host off ---------------------------------------------------------------------------------
file(REMOVE "${OUT}" "${OUT}.txt" "${OUT}-off.png" "${OUT}-off.png.txt")
execute_process(COMMAND "${VIEWER}" --synthetic --mode depth --loopback-only --state-dir "${STATE_DIR}"
                        --screenshot "${OUT}-off.png" --screenshot-after-ms 800 --exit-after-screenshot
                TIMEOUT 120 RESULT_VARIABLE rc)
if(NOT EXISTS "${OUT}-off.png.txt")
    message(FATAL_ERROR "the viewer wrote no report (rc=${rc})")
endif()
file(READ "${OUT}-off.png.txt" report)
message(STATUS "${report}")
expect("ok=1\n" "state_dir_temp=0\n" "host_identity_unusable=1\n"
       "host_status=Streaming host off: pairing.json [^\n]*couldn't be parsed")
expect_pairing_unchanged("the viewer's start")

# ---- 3. the hook: the panel, Try again, Reset ----------------------------------------------------------------
execute_process(COMMAND "${VIEWER}" --synthetic --mode depth --state-dir "${STATE_DIR}" --test-identity-reset
                        --screenshot "${OUT}" --exit-after-screenshot
                TIMEOUT 120 RESULT_VARIABLE rc)
if(NOT EXISTS "${OUT}.txt")
    message(FATAL_ERROR "the viewer wrote no report (rc=${rc})")
endif()
file(READ "${OUT}.txt" report)
message(STATUS "${report}")
expect("ok=1\n" "state_dir_temp=0\n"
       "id_ran=1\n" "id_failed=\n" "id_done=1\n"
       # the host is off with the error, in the panel (with both buttons, the pet controls hidden) and the status
       "id_unusable_at_start=1\n" "id_panel_shown=1\n" "id_panel_text=[^\n]*pairing.json[^\n]*couldn't be parsed"
       "id_status_text=Streaming host off: pairing.json"
       # Try again: still unusable, and nothing was written
       "id_retry_still_unusable=1\n" "id_retry_left_file=1\n"
       # Reset, past its confirm: a new identity, the host serves, the panel is back to normal
       "id_reset_ok=1\n" "id_reset_note=pairing.json.bad already held it\n" "id_running_after=1\n"
       "id_panel_hidden_after=1\n" "id_device_paired=1\n" "id_device_connected=1\n"
       "host_identity_unusable=0\n" "pet_devices=1\n")
if(NOT EXISTS "${OUT}")
    message(FATAL_ERROR "missing capture ${OUT}")
endif()
# The new pairing.json: an identity the host serves with (its fingerprint is in the report), not the old bytes.
# Its key is wrapped for this PC (DPAPI) and never plain.
file(SHA256 "${STATE_DIR}/pairing.json" now)
if(now STREQUAL corrupt_sha)
    message(FATAL_ERROR "pairing.json wasn't replaced by the reset")
endif()
file(READ "${STATE_DIR}/pairing.json" pj)
if(NOT pj MATCHES "\"serverIdentity\":\\{\"privateKeyWrapped\":\"[^\"]+\",\"certificateDER\":\"[^\"]+\"\\},\"serviceId\":\"[0-9a-f]+\"")
    message(FATAL_ERROR "the new pairing.json doesn't hold an identity with a wrapped key: ${pj}")
endif()
if(pj MATCHES "privateKeyRaw")
    message(FATAL_ERROR "the new pairing.json holds the key plain: ${pj}")
endif()
file(SHA256 "${STATE_DIR}/pairing.json.bad" bad)
if(NOT bad STREQUAL corrupt_sha)
    message(FATAL_ERROR "pairing.json.bad isn't the original after the reset")
endif()
