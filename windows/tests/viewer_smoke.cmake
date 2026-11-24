# Runs kinect-viewer.exe --synthetic with the screenshot test hook and checks the hook's report. Side by
# Side IR+Depth: the hook refuses any view with colour in it, and any source but the synthetic one.
# STATE_DIR (v3 §10 "Test isolation"): the host's state folder for this run, fresh each time, under the
# build tree; the report must name it.
file(REMOVE "${OUT}" "${OUT}.txt" "${OUT}.pairing.png" "${OUT}.second.png" "${OUT}.second-black.png")
set(EXTRA_ARGS "")
if(NOT STATE_DIR)
    message(FATAL_ERROR "viewer_smoke.cmake needs -DSTATE_DIR (a test never uses the real .host-state)")
endif()
file(REMOVE_RECURSE "${STATE_DIR}")
set(BASE_ARGS --mode sbs --sbs-layout ir-depth --clock-overlay)
set(EXPECT_FRAME "frame=1024x424")
if(MODE3D)
    # The 3D point cloud with depth-coloured points and Track me (no colour in the view).
    set(BASE_ARGS --mode 3d --track-me --point-size 3)
    set(EXPECT_FRAME "frame=960x720")
endif()
if(PAIR)
    set(EXTRA_ARGS --loopback-only --pair-on-start)
endif()
if(HEALTH)
    list(APPEND EXTRA_ARGS --show-health)
endif()
if(TOOLS)
    list(APPEND EXTRA_ARGS --test-tools)
endif()
if(SECOND)
    list(APPEND EXTRA_ARGS --test-second-display)
endif()
execute_process(
    COMMAND "${VIEWER}" --synthetic ${BASE_ARGS} --state-dir "${STATE_DIR}"
            --screenshot "${OUT}" --screenshot-after-ms 2500 --exit-after-screenshot ${EXTRA_ARGS}
    TIMEOUT 60
    RESULT_VARIABLE rc)
if(NOT EXISTS "${OUT}.txt")
    message(FATAL_ERROR "viewer did not write the screenshot report (rc=${rc})")
endif()
file(READ "${OUT}.txt" report)
message(STATUS "${report}")
if(NOT report MATCHES "ok=1" OR NOT report MATCHES "running=1" OR NOT report MATCHES "${EXPECT_FRAME}")
    message(FATAL_ERROR "unexpected screenshot report")
endif()
# The streaming host kept its state in STATE_DIR (it made its identity there), never anywhere else.
if(NOT report MATCHES "state_dir_temp=0\n" OR NOT EXISTS "${STATE_DIR}/pairing.json")
    message(FATAL_ERROR "the run didn't keep its host state in --state-dir ${STATE_DIR}")
endif()
if(MODE3D)
    if(NOT report MATCHES "mode=4" OR NOT report MATCHES "shown=960x720" OR NOT report MATCHES "pc_note=\n"
       OR NOT report MATCHES "cutoff=1" OR NOT report MATCHES "cutoff_near=500" OR NOT report MATCHES "cutoff_far=2000"
       OR NOT report MATCHES "point_source=0" OR NOT report MATCHES "blend=0")
        message(FATAL_ERROR "the 3D view did not render on the synthetic source")
    endif()
    string(REGEX MATCH "pc_rendered=([0-9]+)" _ "${report}")
    if(CMAKE_MATCH_1 LESS 20)
        message(FATAL_ERROR "too few 3D renders in 2.5 s (${CMAKE_MATCH_1})")
    endif()
    if(TOOLS)
        # The hook orbits (yaw 0.6, pitch 0.25), freezes, then zooms 1.2x: the camera moves on a frozen view.
        if(NOT report MATCHES "frozen=1" OR NOT report MATCHES "cam_yaw=0\\.600" OR NOT report MATCHES "cam_pitch=0\\.250"
           OR NOT report MATCHES "cam_distance=1\\.708")
            message(FATAL_ERROR "the 3D camera did not move on the frozen view")
        endif()
    endif()
elseif(TOOLS AND NOT SECOND) # (with SECOND the hook's Esc has cleared the tools before the report)
    if(NOT report MATCHES "frozen=1" OR NOT report MATCHES "inspect=FROZEN" OR NOT report MATCHES "Region 41x41: mean [0-9]+"
       OR NOT report MATCHES "Ruler: [0-9]+\\.[0-9] mm")
        message(FATAL_ERROR "the inspection tools did not run on the synthetic frame")
    endif()
endif()
if(SECOND)
    # Every step went through the keys' own code (B, Esc), with the output as a small window.
    foreach(want
            "second_ran=1"               # the hook ran (and was allowed: synthetic, no colour)
            "second_opened_blacked=1"    # B with the output off: it opened blacked out
            "second_took_focus=0"        # it never took the keyboard
            "second_click_through=1"     # layered + transparent
            "second_hit_through=1"       # and the mouse finds the window under it
            "second_frame_clean=1"       # the clean frame, never the main window's copy with tools drawn in
            "second_blackout_after_b=1"  # B blacked it out
            "second_black_lit=0\n"       # every captured pixel black
            "second_esc_stopped=1"       # Esc stopped the output covering the main window's display
            "switch_cleared=1")          # a mode change dropped the old frame at once, here and on the output
        if(NOT report MATCHES "${want}")
            message(FATAL_ERROR "second display: expected ${want}")
        endif()
    endforeach()
    string(REGEX MATCH "second_frames=([0-9]+)" _ "${report}")
    set(frames ${CMAKE_MATCH_1})
    string(REGEX MATCH "second_live_lit=([0-9]+)" _ "${report}")
    set(live_lit ${CMAKE_MATCH_1})
    string(REGEX MATCH "second_live_total=([0-9]+)" _ "${report}")
    set(live_total ${CMAKE_MATCH_1})
    string(REGEX MATCH "second_black_total=([0-9]+)" _ "${report}")
    set(black_total ${CMAKE_MATCH_1})
    string(REGEX MATCH "second_black_presents=([0-9]+)" _ "${report}")
    set(black_presents ${CMAKE_MATCH_1})
    message(STATUS "second display: ${frames} frames; live ${live_lit}/${live_total} pixels lit; black 0/${black_total}")
    if(frames LESS 20 OR black_presents LESS 1 OR black_total LESS 1000)
        message(FATAL_ERROR "the second display didn't run long enough, or the black capture was empty")
    endif()
    math(EXPR live_min "${live_total} / 4") # the synthetic picture fills well over a quarter of it
    if(live_lit LESS live_min)
        message(FATAL_ERROR "the live second display was mostly black (${live_lit}/${live_total})")
    endif()
    # "At once": black within 3 DWM compositions of B (about 50 ms at 60 Hz).
    string(REGEX MATCH "second_black_after_frames=([0-9]+)" _ "${report}")
    message(STATUS "second display: black after ${CMAKE_MATCH_1} composition(s)")
    if(CMAKE_MATCH_1 GREATER 3)
        message(FATAL_ERROR "the black-out took ${CMAKE_MATCH_1} compositions to show")
    endif()
    if(TOOLS AND NOT report MATCHES "second_tools_on_main=1")
        message(FATAL_ERROR "the tools weren't on the main window, so the clean-frame check proved nothing")
    endif()
endif()
if(HEALTH AND NOT report MATCHES "health_shown=1")
    message(FATAL_ERROR "the health display was not shown")
endif()
file(SIZE "${OUT}" png_size)
if(png_size LESS 10000)
    message(FATAL_ERROR "screenshot PNG too small (${png_size} B)")
endif()
if(PAIR)
    if(NOT EXISTS "${OUT}.pairing.png")
        message(FATAL_ERROR "pairing window was not captured")
    endif()
    file(SIZE "${OUT}.pairing.png" pair_size)
    if(pair_size LESS 3000)
        message(FATAL_ERROR "pairing screenshot too small (${pair_size} B)")
    endif()
    # Its QR code and code are live secrets: the window is kept out of screen capture (the test's own shot lifts
    # that for the shot only, after the report has read it).
    if(NOT report MATCHES "pairing_open=1" OR NOT report MATCHES "pairing_capture_excluded=1")
        message(FATAL_ERROR "the pairing window isn't kept out of screen capture")
    endif()
endif()
