# Windows host: CLI, tests (and later the viewer and iPhone streaming host).
# Built only when WIN32; included from the top-level CMakeLists.txt.

# libfreenect2's runtime DLLs (freenect2, libusb, turbojpeg, glfw) are copied next to the programs
# once, by one target that every program depending on them waits for. All of these executables
# land in this directory (none sets RUNTIME_OUTPUT_DIRECTORY). A POST_BUILD copy per target used to
# race in a clean parallel build: kinect-cli and kinect-viewer wrote the same DLLs at the same time
# and one failed with "permission denied".
# Exactly the four THIRD_PARTY_NOTICES.md names, never whatever else that bin folder holds. One that isn't there
# is reported and left out (a libfreenect2 built without one of its pipelines); the programs then say what is
# missing when they start, as before.
set(KV_FREENECT2_RUNTIME_DLLS "")
foreach(dll_name freenect2.dll libusb-1.0.dll turbojpeg.dll glfw3.dll)
    if(EXISTS "${FREENECT2_PREFIX}/bin/${dll_name}")
        list(APPEND KV_FREENECT2_RUNTIME_DLLS "${FREENECT2_PREFIX}/bin/${dll_name}")
    else()
        message(WARNING "${FREENECT2_PREFIX}/bin/${dll_name} isn't there, so it isn't copied next to the programs")
    endif()
endforeach()
set(KV_FREENECT2_RUNTIME_COPIES "")
foreach(dll ${KV_FREENECT2_RUNTIME_DLLS})
    get_filename_component(dll_name "${dll}" NAME)
    set(dll_copy "${CMAKE_CURRENT_BINARY_DIR}/${dll_name}")
    add_custom_command(OUTPUT "${dll_copy}"
        COMMAND ${CMAKE_COMMAND} -E copy "${dll}" "${dll_copy}"
        COMMAND ${CMAKE_COMMAND} -E touch "${dll_copy}" # newer than its source, so it isn't redone every build
        DEPENDS "${dll}"
        VERBATIM)
    list(APPEND KV_FREENECT2_RUNTIME_COPIES "${dll_copy}")
endforeach()
add_custom_target(kv_freenect2_runtime DEPENDS ${KV_FREENECT2_RUNTIME_COPIES})
function(kv_copy_freenect2_runtime target)
    add_dependencies(${target} kv_freenect2_runtime)
endfunction()

function(kv_windows_target target)
    target_compile_definitions(${target} PRIVATE NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W3 /permissive-)
        # Hardening: Control Flow Guard, the SDL checks (which also make some warnings errors), and CET shadow-stack
        # compatibility. /Qspectre isn't here: it needs the Spectre-mitigated runtime libraries, which this
        # toolchain's install doesn't have. A static library takes the compile flags only.
        target_compile_options(${target} PRIVATE /guard:cf /sdl)
        get_target_property(kv_type ${target} TYPE)
        if(NOT kv_type STREQUAL "STATIC_LIBRARY")
            target_link_options(${target} PRIVATE /guard:cf /CETCOMPAT)
        endif()
    endif()
endfunction()

# ---- W5 stage 6: the app icon from phase 6's logo ------------------------------------------------
# kv-render-icon draws design/logo.svg with Windows' Direct2D SVG renderer and WIC (no new dependency)
# into kinect-viewer.ico at build time; both programs carry it as resource 1.
add_library(kv_icon STATIC tools/icon_render.cpp)
kv_windows_target(kv_icon)
target_include_directories(kv_icon PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tools")
target_link_libraries(kv_icon PUBLIC d2d1 windowscodecs shlwapi ole32)
add_executable(kv-render-icon tools/render_icon_main.cpp)
kv_windows_target(kv-render-icon)
target_link_libraries(kv-render-icon PRIVATE kv_icon)
set(KV_LOGO_SVG "${CMAKE_SOURCE_DIR}/design/logo.svg")
set(KV_APP_ICON "${CMAKE_CURRENT_BINARY_DIR}/kinect-viewer.ico")
add_custom_command(OUTPUT "${KV_APP_ICON}"
    COMMAND kv-render-icon "${KV_LOGO_SVG}" "${KV_APP_ICON}"
    DEPENDS kv-render-icon "${KV_LOGO_SVG}"
    COMMENT "Rendering the app icon from design/logo.svg"
    VERBATIM)
add_custom_target(kv_app_icon DEPENDS "${KV_APP_ICON}")
file(TO_CMAKE_PATH "${KV_APP_ICON}" KV_APP_ICON_RC) # forward slashes: the resource compiler takes them
# The version Explorer shows (Properties > Details) and the installer records; the Mac and iOS apps
# carry the same number (CFBundleShortVersionString / CFBundleVersion).
set(KV_APP_VERSION_MAJOR 1)
set(KV_APP_VERSION_MINOR 0)
set(KV_APP_VERSION "${KV_APP_VERSION_MAJOR}.${KV_APP_VERSION_MINOR}")
set(KV_APP_BUILD 1)
# Each program gets its own .rc: the icon plus version info with its own name and description.
function(kv_app_icon target description)
    set(KV_RC_NAME "${target}")
    set(KV_RC_DESCRIPTION "${description}")
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/viewer/app_icon.rc.in" "${rc}" @ONLY)
    set_source_files_properties("${rc}" PROPERTIES OBJECT_DEPENDS "${KV_APP_ICON}")
    target_sources(${target} PRIVATE "${rc}")
    add_dependencies(${target} kv_app_icon)
endfunction()

# ---- W1: CLI ----------------------------------------------------------------------------------
add_executable(kinect-cli cli/kinect_cli.cpp)
kv_app_icon(kinect-cli "Kinect Viewer command line")
kv_windows_target(kinect-cli)
target_include_directories(kinect-cli PRIVATE "${FREENECT2_INCLUDE_DIR}")
target_link_libraries(kinect-cli PRIVATE kinect_core winmm)
kv_copy_freenect2_runtime(kinect-cli)

# ---- W1: tests --------------------------------------------------------------------------------
add_executable(test_lock_guard tests/test_lock_guard.cpp)
kv_windows_target(test_lock_guard)
target_link_libraries(test_lock_guard PRIVATE kinect_core)
kv_copy_freenect2_runtime(test_lock_guard)
add_test(NAME lock_guard COMMAND test_lock_guard)

add_executable(test_core_synthetic tests/test_core_synthetic.cpp)
kv_windows_target(test_core_synthetic)
target_link_libraries(test_core_synthetic PRIVATE kinect_core winmm)
kv_copy_freenect2_runtime(test_core_synthetic)
add_test(NAME core_synthetic COMMAND test_core_synthetic)
# Its 30 fps row's inter-frame p95 is a timing floor judged against the desk (KV_FLOOR): skipped, never passed, when busy.
set_tests_properties(core_synthetic PROPERTIES SKIP_RETURN_CODE 77)

# v3 §10 "Test isolation": every test that launches the viewer or the CLI passes a state folder under
# the build tree (--state-dir), so no test run ever reads or writes the real .host-state.
set(KV_TEST_STATE "${CMAKE_CURRENT_BINARY_DIR}/test-state")
# Exit code 4 means a barcode mismatch (or no frames checked).
add_test(NAME cli_synthetic_barcode
    COMMAND kinect-cli run --synthetic --duration 3 --verify-barcode --state-dir ${KV_TEST_STATE}/cli)
add_test(NAME cli_synthetic_bench_short
    COMMAND kinect-cli bench --synthetic --duration 5 --verify-barcode --state-dir ${KV_TEST_STATE}/cli)
set_tests_properties(cli_synthetic_bench_short PROPERTIES PASS_REGULAR_EXPRESSION "BENCHMARK=PASS")
add_test(NAME cli_verify_lock COMMAND kinect-cli verify-lock --state-dir ${KV_TEST_STATE}/cli)
set_tests_properties(cli_verify_lock PROPERTIES PASS_REGULAR_EXPRESSION "LOCK_VERIFY=PASS")
# list reads the real Kinect's USB descriptors when the lock is free, and fails (by design) when another
# program holds it: a hardware test, off by default (-DKV_HARDWARE_TESTS=ON to run it). What runs by
# default is that it honours a held lock without enumerating anything.
option(KV_HARDWARE_TESTS "Run the tests that touch the real Kinect" OFF)
if(KV_HARDWARE_TESTS)
    add_test(NAME cli_list COMMAND kinect-cli list)
    set_tests_properties(cli_list PROPERTIES LABELS hardware)
endif()
add_test(NAME cli_list_respects_a_held_lock
    COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:kinect-cli> -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_list_lock.cmake)
# Next to the programs, exactly the four third-party DLLs THIRD_PARTY_NOTICES.md names.
add_test(NAME runtime_dlls_are_the_named_four
    COMMAND ${CMAKE_COMMAND} -DDIR=$<TARGET_FILE_DIR:kinect-cli> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_dlls_named.cmake)
# Both programs carry Control Flow Guard and CET compatibility in their headers (kv_windows_target's flags).
add_test(NAME programs_are_built_hardened
    COMMAND ${CMAKE_COMMAND} "-DPROGRAMS=$<TARGET_FILE:kinect-cli>|$<TARGET_FILE:kinect-viewer>"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/programs_hardened.cmake)

# The open-source export (git archive) never includes docs/internal/, nor any word its list
# (docs/internal/export-forbidden-words.txt) names. Only the private repository's checkout can say: a tree made by
# the export, and a clone of the published repository, have no docs/internal/ to leave out (the export has no .git
# either). So the row is there only in a git checkout that has the list.
if(EXISTS "${CMAKE_SOURCE_DIR}/.git" AND EXISTS "${CMAKE_SOURCE_DIR}/docs/internal/export-forbidden-words.txt")
    find_package(Git REQUIRED)
    add_test(NAME export_leaves_out_internal
        COMMAND ${CMAKE_COMMAND} -DGIT=${GIT_EXECUTABLE} -DREPO=${CMAKE_SOURCE_DIR} -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/export_leaves_out_internal.cmake)
endif()

# ---- W2: viewer (Win32 + Direct3D 11) -----------------------------------------------------------
# host/host_power.cpp (the Kinect power model: viewer Start/Stop and phone On/Off) lives here, not in
# kv_host, so the viewer's own Start/Stop uses it even without the streaming host. It needs only the
# header-only enum from host/wire_protocol.hpp.
# host/json_lite.cpp (Swift-compatible JSON) on its own, for the streaming host and the viewer's presets;
# with pairing.json's JSON (host/host_state_json.cpp), which the viewer's --state-dir check reads.
add_library(kv_json STATIC host/json_lite.cpp host/host_state_json.cpp)
kv_windows_target(kv_json)
target_include_directories(kv_json PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/host")

add_library(kv_viewer_model STATIC viewer/viewer_model.cpp viewer/inspection.cpp viewer/presets.cpp viewer/point_cloud_camera.cpp
    viewer/pet_camera_model.cpp
    viewer/outside_panel_model.cpp
    viewer/display_targets.cpp
    host/host_power.cpp)
kv_windows_target(kv_viewer_model)
target_include_directories(kv_viewer_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/viewer" "${CMAKE_CURRENT_SOURCE_DIR}/host")
target_link_libraries(kv_viewer_model PUBLIC kv_json)
target_link_libraries(kv_viewer_model PUBLIC kinect_core)

# Stage 5: the 3D point cloud on Direct3D 11 (offscreen 960x720, read back).
add_library(kv_point_cloud STATIC viewer/point_cloud_renderer.cpp)
kv_windows_target(kv_point_cloud)
target_link_libraries(kv_point_cloud PUBLIC kv_viewer_model d3d11 dxgi d3dcompiler)

add_executable(kinect-viewer WIN32
    viewer/viewer_main.cpp
    viewer/viewer_app.cpp
    viewer/d3d_preview.cpp
    viewer/window_capture.cpp
    viewer/second_display.cpp
    viewer/pet_camera_ui.cpp
)
kv_windows_target(kinect-viewer)
kv_app_icon(kinect-viewer "Kinect Viewer")
# Per-monitor v2 DPI and asInvoker from the manifest (merged with the linker's, so the
# Common-Controls 6 dependency viewer_app.cpp asks for stays).
target_sources(kinect-viewer PRIVATE viewer/kinect-viewer.manifest)
target_link_libraries(kinect-viewer PRIVATE kv_viewer_model kv_point_cloud kinect_core
    d3d11 dxgi d3dcompiler comctl32 windowscodecs winmm ole32 shell32 dwmapi)
kv_copy_freenect2_runtime(kinect-viewer)

# The orbit camera's math, the renderer on synthetic depth (placement, flips, point size, no fallback
# intrinsics), the core's cut-off and colour sources, and the 3D frame rate on the synthetic source.
add_executable(test_point_cloud tests/test_point_cloud.cpp)
kv_windows_target(test_point_cloud)
target_link_libraries(test_point_cloud PRIVATE kv_point_cloud kinect_core winmm)
kv_copy_freenect2_runtime(test_point_cloud)
add_test(NAME point_cloud COMMAND test_point_cloud)

add_executable(test_viewer_model tests/test_viewer_model.cpp)
kv_windows_target(test_viewer_model)
target_link_libraries(test_viewer_model PRIVATE kv_viewer_model)
kv_copy_freenect2_runtime(test_viewer_model)
add_test(NAME viewer_model COMMAND test_viewer_model)

add_executable(test_inspection tests/test_inspection.cpp)
kv_windows_target(test_inspection)
target_link_libraries(test_inspection PRIVATE kv_viewer_model kinect_core)
kv_copy_freenect2_runtime(test_inspection)
add_test(NAME inspection COMMAND test_inspection)

# Screenshot smoke test: launches the viewer on the synthetic source, captures its own window with
# PrintWindow after 2.5 s and exits. No input is synthesised. Checks the report the hook writes.
add_test(NAME viewer_synthetic_screenshot
    COMMAND ${CMAKE_COMMAND}
        -DVIEWER=$<TARGET_FILE:kinect-viewer>
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/viewer-smoke.png
        -DSTATE_DIR=${KV_TEST_STATE}/viewer-smoke
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_smoke.cmake)

# Synthetic, with the health display (H) on: the strip above the image is laid out and filled.
add_test(NAME viewer_health_screenshot
    COMMAND ${CMAKE_COMMAND}
        -DVIEWER=$<TARGET_FILE:kinect-viewer>
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/viewer-health.png
        -DSTATE_DIR=${KV_TEST_STATE}/viewer-health
        -DHEALTH=1
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_smoke.cmake)

# Synthetic: a region box and a ruler on the depth half, then a freeze (the --test-tools hook goes
# through the same code as the mouse and Space); the report carries the inspector line.
add_test(NAME viewer_tools_screenshot
    COMMAND ${CMAKE_COMMAND}
        -DVIEWER=$<TARGET_FILE:kinect-viewer>
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/viewer-tools.png
        -DSTATE_DIR=${KV_TEST_STATE}/viewer-tools
        -DTOOLS=1
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_smoke.cmake)

# Stage 5: the 3D point cloud on the synthetic source (depth-coloured points, Track me), and the same
# frozen after an orbit with the camera still moving (the --test-tools hook in 3D).
add_test(NAME viewer_3d_screenshot
    COMMAND ${CMAKE_COMMAND}
        -DVIEWER=$<TARGET_FILE:kinect-viewer>
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/viewer-3d.png
        -DSTATE_DIR=${KV_TEST_STATE}/viewer-3d
        -DMODE3D=1
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_smoke.cmake)
add_test(NAME viewer_3d_frozen_orbit_screenshot
    COMMAND ${CMAKE_COMMAND}
        -DVIEWER=$<TARGET_FILE:kinect-viewer>
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/viewer-3d-tools.png
        -DSTATE_DIR=${KV_TEST_STATE}/viewer-3d-tools
        -DMODE3D=1
        -DTOOLS=1
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_smoke.cmake)

# Stage 6: the icon follows the logo (rendered again and compared with the build's), its sizes and
# pixels, and kinect-viewer.exe carries it.
add_executable(test_icon tests/test_icon.cpp)
kv_windows_target(test_icon)
target_link_libraries(test_icon PRIVATE kv_icon)
target_compile_definitions(test_icon PRIVATE
    "KV_LOGO_SVG=L\"${KV_LOGO_SVG}\""
    "KV_APP_ICON=L\"${KV_APP_ICON_RC}\""
    "KV_VIEWER_EXE=L\"$<TARGET_FILE:kinect-viewer>\""
    "KV_CLI_EXE=L\"$<TARGET_FILE:kinect-cli>\"")
add_dependencies(test_icon kinect-viewer kinect-cli kv_app_icon)
add_test(NAME app_icon COMMAND test_icon)
# The version info and manifest both programs carry, and the per-user install and uninstall
# (windows/packaging), run only against a temporary folder and a temporary HKCU key.
add_executable(test_app_info tests/test_app_info.cpp)
kv_windows_target(test_app_info)
target_link_libraries(test_app_info PRIVATE version)
target_compile_definitions(test_app_info PRIVATE
    "KV_APP_VERSION=L\"${KV_APP_VERSION}\""
    "KV_VIEWER_EXE=L\"$<TARGET_FILE:kinect-viewer>\""
    "KV_CLI_EXE=L\"$<TARGET_FILE:kinect-cli>\"")
add_dependencies(test_app_info kinect-viewer kinect-cli)
add_test(NAME app_info COMMAND test_app_info)
find_program(KV_POWERSHELL powershell.exe REQUIRED)
add_test(NAME packaging_install_uninstall
    COMMAND ${KV_POWERSHELL} -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/packaging_install.ps1
        -From $<TARGET_FILE_DIR:kinect-viewer>)
# kv-render-icon renders design/logo.svg only: anything else exits 3 and writes nothing.
add_test(NAME render_icon_refuses_other_files
    COMMAND ${CMAKE_COMMAND}
        -DTOOL=$<TARGET_FILE:kv-render-icon>
        -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}
        -DSRC=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/render_icon_refusal.cmake)

# Stage 6: the second-display output (as a small window) shows the live view, and B blacks it out.
add_test(NAME viewer_second_display_screenshot
    COMMAND ${CMAKE_COMMAND}
        -DVIEWER=$<TARGET_FILE:kinect-viewer>
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/viewer-second.png
        -DSTATE_DIR=${KV_TEST_STATE}/viewer-second
        -DSECOND=1
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_smoke.cmake)
# The same with a region box and a ruler on the main window (--test-tools): the output stays clean.
add_test(NAME viewer_second_display_with_tools_screenshot
    COMMAND ${CMAKE_COMMAND}
        -DVIEWER=$<TARGET_FILE:kinect-viewer>
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/viewer-second-tools.png
        -DSTATE_DIR=${KV_TEST_STATE}/viewer-second-tools
        -DSECOND=1
        -DTOOLS=1
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_smoke.cmake)

# The screenshot hook refuses the real source, Colour, Side-by-Side layouts with colour, the blend and
# RGB 3D points (exit 3, nothing captured).
add_test(NAME viewer_screenshot_refusals
    COMMAND ${CMAKE_COMMAND}
        -DVIEWER=$<TARGET_FILE:kinect-viewer>
        -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_screenshot_refusals.cmake)

# ---- W3: iPhone streaming host (OpenSSL 3 mutual TLS 1.3, mDNS, Media Foundation H.264) -----------
find_package(OpenSSL 3)
if(NOT OpenSSL_FOUND)
    message(WARNING "OpenSSL 3 not found: skipping the iPhone streaming host. Build with windows/scripts/build.cmd (vcpkg).")
    return()
endif()

add_library(kv_host STATIC
    host/wire_protocol.cpp
    host/crypto_identity.cpp
    host/pairing_payload.cpp
    host/host_state.cpp
    host/video_convert.cpp
    host/mf_h264_encoder.cpp
    host/mf_platform.cpp
    host/live_tiers.cpp
    host/tls_conn.cpp
    host/mdns.cpp
    host/stream_server.cpp
    host/host_controller.cpp
    host/state_files.cpp
    host/device_store.cpp
    host/pet_settings.cpp
    host/roll_media.cpp
    host/folder_protection.cpp
    host/roll_store.cpp
    host/roll_wire.cpp
    host/roll_triggers.cpp
    host/roll_thumbnail.cpp
    host/roll_recorder.cpp
    host/roll_playback.cpp
    # W8 (outside the home): the address rules, the router mapping (NATUPnP behind a seam), remote invites, the
    # admission limiter and the connection log (Phase A), wired into the host in Phase B, with the mapper's worker.
    host/outside_address.cpp
    host/keep_running.cpp
    host/crash_recorder.cpp
    host/outside_limiter.cpp
    host/remote_invite.cpp
    host/router_mapper.cpp
    host/connection_log.cpp
    host/outside_mapper.cpp
)
kv_windows_target(kv_host)
target_include_directories(kv_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/host" "${CMAKE_SOURCE_DIR}/core/include")
target_link_libraries(kv_host PUBLIC kv_json OpenSSL::SSL OpenSSL::Crypto ws2_32 crypt32 advapi32 user32
    mfplat mfuuid mf strmiids ole32 oleaut32 dnsapi iphlpapi winhttp xmllite shlwapi)
# W7 roll_media: the camera roll's IMFSinkWriter and IMFSourceReader live in mfreadwrite.
target_link_libraries(kv_host PUBLIC mfreadwrite)

# W8 (outside the home): the Phase A modules' own tests link this name; the modules now build into kv_host.
add_library(kv_outside INTERFACE)
target_link_libraries(kv_outside INTERFACE kv_host)

# Its own copy of host/host_state.cpp, built with KV_TESTING: the only build that has the host key's test switches
# (host_state.hpp). An object named here is linked whole and comes before kv_host, so kv_host's host_state.obj,
# built without them, is never pulled in. Only test_ targets may define KV_TESTING (a row there pins it).
add_executable(test_protocol_golden tests/test_protocol_golden.cpp host/host_state.cpp)
kv_windows_target(test_protocol_golden)
target_compile_definitions(test_protocol_golden PRIVATE KV_TESTING)
target_include_directories(test_protocol_golden PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_protocol_golden PRIVATE kv_host)
# The source tree, for the row that pins that no shipping source reaches the host key's DPAPI test switch.
target_compile_definitions(test_protocol_golden PRIVATE "KV_WINDOWS_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
add_test(NAME protocol_golden COMMAND test_protocol_golden)

# Timing floors are judged against the desk (tests/kv_desk.hpp): one missed while the rest of the PC was busy makes its
# runner exit 77, which SKIP_RETURN_CODE shows as "Not Run", never as passed. A row that uses KV_FLOOR sets it.
add_executable(test_desk_floor tests/test_desk_floor.cpp)
kv_windows_target(test_desk_floor)
target_include_directories(test_desk_floor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
# Only test_desk_floor honours KV_TEST_DESK_FORCE; every other binary ignores it (desk_unforced checks that).
target_compile_definitions(test_desk_floor PRIVATE KV_DESK_FORCE_ALLOWED)
add_test(NAME desk_floor COMMAND test_desk_floor)

add_executable(test_desk_unforced tests/test_desk_unforced.cpp)
kv_windows_target(test_desk_unforced)
target_include_directories(test_desk_unforced PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
add_test(NAME desk_unforced COMMAND test_desk_unforced)

add_executable(test_json_lite tests/test_json_lite.cpp)
kv_windows_target(test_json_lite)
target_include_directories(test_json_lite PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_json_lite PRIVATE kv_json)
add_test(NAME json_lite COMMAND test_json_lite)

add_executable(test_h264_encoder tests/test_h264_encoder.cpp)
kv_windows_target(test_h264_encoder)
target_include_directories(test_h264_encoder PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${CMAKE_SOURCE_DIR}/core/include")
target_link_libraries(test_h264_encoder PRIVATE kv_host kinect_core wmcodecdspuuid winmm)
kv_copy_freenect2_runtime(test_h264_encoder)
add_test(NAME h264_encoder COMMAND test_h264_encoder)
# Its frame counts are timing floors judged against the desk (KV_FLOOR): skipped, never passed, when the PC was busy.
set_tests_properties(h264_encoder PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_live_tiers tests/test_live_tiers.cpp)
kv_windows_target(test_live_tiers)
target_include_directories(test_live_tiers PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${CMAKE_SOURCE_DIR}/core/include")
target_link_libraries(test_live_tiers PRIVATE kv_host kinect_core wmcodecdspuuid winmm)
kv_copy_freenect2_runtime(test_live_tiers)
add_test(NAME live_tiers COMMAND test_live_tiers)
# Its encoder-close time is a timing floor judged against the desk (KV_FLOOR): skipped, never passed, when busy.
set_tests_properties(live_tiers PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_stream_e2e tests/test_stream_e2e.cpp)
kv_windows_target(test_stream_e2e)
target_include_directories(test_stream_e2e PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_stream_e2e PRIVATE kv_host kv_viewer_model kinect_core wmcodecdspuuid winmm)
kv_copy_freenect2_runtime(test_stream_e2e)
add_test(NAME stream_e2e_loopback COMMAND test_stream_e2e)
# Its power-off floor is judged against the desk (KV_FLOOR): skipped, never passed, when the PC was busy.
set_tests_properties(stream_e2e_loopback PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_mdns tests/test_mdns.cpp)
kv_windows_target(test_mdns)
target_link_libraries(test_mdns PRIVATE kv_host)
add_test(NAME mdns_register_browse COMMAND test_mdns)

# W8 Phase A: the outside modules' tests. A fake router, an injected clock and new temp folders only.
add_executable(test_outside
    tests/test_outside_main.cpp
    tests/test_outside_address.cpp
    tests/test_outside_limiter.cpp
    tests/test_remote_invite.cpp
    tests/test_router_mapper.cpp
    tests/test_connection_log.cpp
)
kv_windows_target(test_outside)
target_include_directories(test_outside PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_outside PRIVATE kv_outside)
# test_outside_address reads the shipping sources (no switch to a test address policy reaches them).
target_compile_definitions(test_outside PRIVATE "KV_WINDOWS_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
add_test(NAME outside_modules COMMAND test_outside)

# The keep-running watchdog and the crash recorder. Children of its own, this run's
# own named objects, a temp folder.
add_executable(test_keep_running tests/test_keep_running.cpp)
kv_windows_target(test_keep_running)
target_link_libraries(test_keep_running PRIVATE kv_host)
target_include_directories(test_keep_running PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${CMAKE_CURRENT_SOURCE_DIR}/host")
add_test(NAME keep_running COMMAND test_keep_running)

# W8 Phase B: the outside listener, invites, the limiter and the router mapping end to end. Loopback only, new temp
# state folders, a fake router behind the RouterSession seam, and "outside" sources through the source seam.
add_executable(test_outside_e2e tests/test_outside_e2e.cpp)
kv_windows_target(test_outside_e2e)
target_include_directories(test_outside_e2e PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_outside_e2e PRIVATE kv_host kinect_core)
add_test(NAME outside_e2e COMMAND test_outside_e2e)

# W6: the device list (devices.json, reconciliation with pairing.json, the mirror) and pet-camera.json.
add_executable(test_device_store tests/test_device_store.cpp)
kv_windows_target(test_device_store)
target_include_directories(test_device_store PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_device_store PRIVATE kv_host)
add_test(NAME device_store COMMAND test_device_store)

# W7 A1: the camera roll's pass-through MP4 writer, pre-roll ring and reader (pet-camera.md §6.1).
# Synthetic frames, temp folders only.
add_executable(test_roll_media tests/test_roll_media.cpp)
kv_windows_target(test_roll_media)
target_include_directories(test_roll_media PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_roll_media PRIVATE kv_host wmcodecdspuuid)
add_test(NAME roll_media COMMAND test_roll_media)
# W7 A2: the camera roll's files (names, sidecars, index.json, listing and cursors, stars, retention,
# the disk floor, folder protection). Fake .mp4 bytes in temp folders only.
add_executable(test_roll_store tests/test_roll_store.cpp)
kv_windows_target(test_roll_store)
target_include_directories(test_roll_store PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_roll_store PRIVATE kv_host)
add_test(NAME roll_store COMMAND test_roll_store)
# W7: the camera-roll messages on the wire (0x12-0x19, the roll keys of 0x0B and 0x0C) and their checks.
add_executable(test_roll_wire tests/test_roll_wire.cpp)
kv_windows_target(test_roll_wire)
target_include_directories(test_roll_wire PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_roll_wire PRIVATE kv_host)
add_test(NAME roll_wire COMMAND test_roll_wire)
# W7: the camera-roll triggers (depth motion detector, trigger set) and the WIC JPEG thumbnailer.
target_link_libraries(kv_host PUBLIC windowscodecs) # roll_thumbnail.cpp
target_link_libraries(kv_host PUBLIC dbghelp) # crash_recorder.cpp: MiniDumpWriteDump
add_executable(test_roll_triggers tests/test_roll_triggers.cpp)
kv_windows_target(test_roll_triggers)
target_include_directories(test_roll_triggers PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_roll_triggers PRIVATE kv_host)
add_test(NAME roll_triggers COMMAND test_roll_triggers)

# W7 B1: the camera roll end to end on a fake v3 device (synthetic source, temp state and roll folders).
add_executable(test_roll_e2e tests/test_roll_e2e.cpp)
kv_windows_target(test_roll_e2e)
target_include_directories(test_roll_e2e PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_roll_e2e PRIVATE kv_host kinect_core wmcodecdspuuid winmm)
add_test(NAME roll_e2e COMMAND test_roll_e2e)

# Streaming in the CLI (kinect-cli serve) and the viewer (Pair iPhone...).
find_package(unofficial-nayuki-qr-code-generator CONFIG REQUIRED)
target_compile_definitions(kinect-cli PRIVATE KV_WITH_STREAMING)
target_link_libraries(kinect-cli PRIVATE kv_host kv_viewer_model)
# W7: the camera roll's browser, player and schedule editor (roll_ui.cpp) need the host's store and its Media
# Foundation helper, so they are part of the streaming viewer only.
target_sources(kinect-viewer PRIVATE viewer/stream_hooks.cpp viewer/pet_test_device.cpp viewer/roll_ui.cpp)
# W8: the outside-the-home windows (invite, re-share, connection log), the QR drawing they share with the pairing
# window, and the --test-outside hook's fake router (the host takes it through Options::router_factory).
target_sources(kinect-viewer PRIVATE viewer/outside_ui.cpp viewer/qr_draw.cpp viewer/outside_test_router.cpp)
target_compile_definitions(kinect-viewer PRIVATE KV_WITH_STREAMING)
target_link_libraries(kinect-viewer PRIVATE kv_host unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator)
# Viewer with streaming: open the pairing window at launch (loopback only, nothing advertised) and
# capture both windows. The pairing PNG is a synthetic UI capture (QR of a loopback-only offer).
add_test(NAME viewer_pairing_window_screenshot
    COMMAND ${CMAKE_COMMAND}
        -DVIEWER=$<TARGET_FILE:kinect-viewer>
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/viewer-pairing.png
        -DSTATE_DIR=${KV_TEST_STATE}/viewer-pairing
        -DPAIR=1
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_smoke.cmake)
# W6 (protocol v3 §10, pet-camera.md §4): a --state-dir seeded with two synthetic devices and sharing on
# is loaded; then the --test-pet-camera hook pairs a fake v3 device on loopback and drives the panel,
# the "Watching now" banner, privacy, sharing and its schedule, and the Devices dialog (rename, revoke).
add_test(NAME viewer_pet_camera_panel
    COMMAND ${CMAKE_COMMAND}
        -DVIEWER=$<TARGET_FILE:kinect-viewer>
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/viewer-pet-camera.png
        -DSTATE_DIR=${KV_TEST_STATE}/viewer-pet-camera
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_pet_camera.cmake)
# A test-hook launch without --state-dir uses a temp folder of its own and removes it on exit.
add_test(NAME viewer_test_launch_isolated_state
    COMMAND ${CMAKE_COMMAND}
        -DVIEWER=$<TARGET_FILE:kinect-viewer>
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/viewer-isolated.png
        -DISOLATION=1
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_pet_camera.cmake)
# --state-dir naming a folder of other files is refused by kinect-cli serve and the viewer before the
# host starts (it would make the folder owner-only, down to every file already in it).
add_test(NAME state_dir_refusals
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:kinect-cli>
        -DVIEWER=$<TARGET_FILE:kinect-viewer>
        -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/state_dir_refusals.cmake)
# W7: kinect-cli serve --camera-roll-dir records only into the folder it names; without it there is no roll.
add_test(NAME cli_camera_roll_dir
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:kinect-cli>
        -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_camera_roll_dir.cmake)
# W8: kinect-cli serve --outside-port. Needs sharing on; a test run binds 127.0.0.1 and uses no router (manual).
add_test(NAME cli_outside_port
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:kinect-cli>
        -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_outside_port.cmake)
# W7 (pet-camera.md §4, §6; v3 §10): the viewer's camera roll, synthetic only, with --state-dir and --camera-roll-dir
# under the build tree: the panel's settings, motion and manual clips, the browser and its player (no frame of it
# reaches the host), the screenshot rule while a clip is on screen; no roll without --camera-roll-dir; settings
# refused while pet-camera.json can't be read; the real folders refused as --camera-roll-dir by their path alone.
foreach(mode IN ITEMS HOOK ISOLATION UNREADABLE REFUSALS)
    string(TOLOWER "${mode}" mode_lc)
    set(name viewer_camera_roll_${mode_lc})
    add_test(NAME ${name}
        COMMAND ${CMAKE_COMMAND}
            -DVIEWER=$<TARGET_FILE:kinect-viewer>
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/viewer-camera-roll-${mode_lc}.png
            -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}
            -DSTATE_DIR=${KV_TEST_STATE}/viewer-camera-roll
            -DROLL_DIR=${KV_TEST_STATE}/viewer-camera-roll-clips
            -DMODE=${mode}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_camera_roll.cmake)
endforeach()
# pairing.json fails closed (this PC's identity): kinect-cli serve and the viewer refuse to start over one that
# can't be parsed and leave it as it was; the viewer's Pet camera panel shows why, "Try again" and the reset (a
# new identity, every device pairs again) work, and the host then serves. Synthetic, loopback, build-tree folder.
add_test(NAME viewer_identity_reset
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:kinect-cli>
        -DVIEWER=$<TARGET_FILE:kinect-viewer>
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/viewer-identity-reset.png
        -DSTATE_DIR=${KV_TEST_STATE}/viewer-identity
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_identity_reset.cmake)
# W8 (pet-camera.md §4; protocol v3 §8, §10): the Pet camera panel's "Outside the home" section and its windows,
# synthetic only, with a --state-dir under the build tree and the viewer's own fake router (never the real one):
# every status's sentence, the manual address field, an invite shown (never its token) and cancelled, the
# address-changed flow and re-share, the connection log window, and off disabling everything.
add_test(NAME viewer_outside_panel
    COMMAND ${CMAKE_COMMAND}
        -DVIEWER=$<TARGET_FILE:kinect-viewer>
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/viewer-outside.png
        -DSTATE_DIR=${KV_TEST_STATE}/viewer-outside
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_outside.cmake)

# ---- W4: host-only recorder (16-bit IR/Depth only: FFV1 via ffmpeg, PNG16 via WIC; colour is live only) ----
add_library(kv_recorder STATIC recorder/recorder.cpp recorder/recordings.cpp)
kv_windows_target(kv_recorder)
target_include_directories(kv_recorder PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/recorder")
target_link_libraries(kv_recorder PUBLIC kv_host windowscodecs shlwapi)

add_executable(test_recorder tests/test_recorder.cpp)
kv_windows_target(test_recorder)
target_link_libraries(test_recorder PRIVATE kv_recorder kv_viewer_model kinect_core winmm)
kv_copy_freenect2_runtime(test_recorder)
add_test(NAME recorder COMMAND test_recorder)

# Recordings in the app (phase 2): the list, the 7-day deletion and its log, the colour purge, the reader
# (PNG16 and FFV1 chunks, bit exact) and playback through the core's replay pipeline. Temp folders only.
add_executable(test_recordings tests/test_recordings.cpp)
kv_windows_target(test_recordings)
target_link_libraries(test_recordings PRIVATE kv_recorder kinect_core winmm)
kv_copy_freenect2_runtime(test_recordings)
add_test(NAME recordings COMMAND test_recordings)

# Protocol v2 power model (synthetic engine, a temp lock file, a temp PNG16 recording).
add_executable(test_host_power tests/test_host_power.cpp)
kv_windows_target(test_host_power)
target_link_libraries(test_host_power PRIVATE kv_viewer_model kv_recorder kinect_core winmm)
kv_copy_freenect2_runtime(test_host_power)
add_test(NAME host_power COMMAND test_host_power)
target_compile_definitions(kinect-viewer PRIVATE KV_WITH_RECORDER)
target_link_libraries(kinect-viewer PRIVATE kv_recorder)
target_compile_definitions(kinect-cli PRIVATE KV_WITH_RECORDER)
target_link_libraries(kinect-cli PRIVATE kv_recorder)
# Phase 0: the CLI refuses to record colour before it opens anything: exit 2, the sentence, and no
# --out folder (in the build tree, so a regression could never write into the real recordings folder).
foreach(colour_mode color sbs)
    add_test(NAME cli_record_refuses_${colour_mode}
        COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:kinect-cli> -DMODE=${colour_mode}
            -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_record_refusal.cmake)
endforeach()
# The real sensor is recorded only in the viewer, after the reminder: record refuses it (IR here).
add_test(NAME cli_record_refuses_the_real_source
    COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:kinect-cli> -DMODE=ir -DREAL=1
        -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_record_refusal.cmake)
