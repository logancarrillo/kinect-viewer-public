# kinect-cli serve --outside-port (W8, protocol v3 §8.1, §10): outside listening on, on the port given, for the
# host's own state. It needs sharing on (refused with the reason otherwise, and serve goes on); in a test run
# (--synthetic, --loopback-only) the listener binds 127.0.0.1 only and no router is ever used, so the status is
# manual with the steps; the switch and the port are saved in pet-camera.json. Synthetic, loopback, build tree.
set(base "${OUTDIR}/test-state/cli-outside-port")
file(REMOVE_RECURSE "${base}")
file(MAKE_DIRECTORY "${base}")
set(state "${base}/state")
# A port unlikely to be in use (the listener binds 127.0.0.1 only).
string(RANDOM LENGTH 4 ALPHABET 0123456789 r)
math(EXPR port "40000 + (${r} % 9000)")

# A port outside 1024-65535 is refused before anything starts.
execute_process(
    COMMAND "${CLI}" serve --synthetic --loopback-only --duration 1 --state-dir "${state}" --outside-port 80
    TIMEOUT 60
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE so
    ERROR_VARIABLE se)
if(rc EQUAL 0 OR NOT se MATCHES "--outside-port needs a port from 1024 to 65535")
    message(FATAL_ERROR "kinect-cli serve --outside-port 80: expected a refusal, got rc=${rc}\n${so}\n${se}")
endif()

# Sharing off (a fresh state): refused with the reason; serve itself runs.
execute_process(
    COMMAND "${CLI}" serve --synthetic --loopback-only --duration 2 --state-dir "${state}" --outside-port ${port}
    TIMEOUT 60
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE so
    ERROR_VARIABLE se)
message(STATUS "serve --outside-port with sharing off: rc=${rc}")
if(NOT rc EQUAL 0 OR NOT se MATCHES "OUTSIDE_REFUSED: .*Share the camera")
    message(FATAL_ERROR "kinect-cli serve --outside-port (sharing off): expected the refusal, rc=${rc}\n${so}\n${se}")
endif()
if(so MATCHES "outside: listening")
    message(FATAL_ERROR "kinect-cli serve --outside-port (sharing off): it listened anyway\n${so}")
endif()

# Sharing on in this host's pet-camera.json: it listens on 127.0.0.1, and with no router in a test run the status
# is manual, saying so. serve stops as soon as the status is printed as a result (--until-outside-reported); the
# --duration is only the deadline. The manual steps name the test run's fixed LAN address (v3 §10), never this PC's.
# A random port can still be held on this PC (a loopback connection in TIME_WAIT from an earlier test): serve then
# says so (OUTSIDE_NOT_LISTENING, the port-in-use sentence), and another port is tried, up to five.
foreach(try RANGE 1 5)
    file(WRITE "${state}/pet-camera.json" "{\"sharingEnabled\":true,\"privacyMode\":false}")
    execute_process(
        COMMAND "${CLI}" serve --synthetic --loopback-only --duration 60 --until-outside-reported --state-dir "${state}"
                --outside-port ${port}
        TIMEOUT 60
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE so
        ERROR_VARIABLE se)
    message(STATUS "serve --outside-port ${port} with sharing on: rc=${rc}")
    if(NOT se MATCHES "OUTSIDE_NOT_LISTENING: Port ${port} can't be opened on this PC")
        break()
    endif()
    string(RANDOM LENGTH 4 ALPHABET 0123456789 r)
    math(EXPR port "40000 + (${r} % 9000)")
endforeach()
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "kinect-cli serve --outside-port: expected exit 0, got ${rc}\n${so}\n${se}")
endif()
if(NOT so MATCHES "outside: listening on 127\\.0\\.0\\.1:${port} \\(a test run: no router is used\\)")
    message(FATAL_ERROR "kinect-cli serve --outside-port: not listening on loopback\n${so}\n${se}")
endif()
if(NOT so MATCHES "outside: status=manual port=${port} Automatic port forwarding \\(UPnP\\) isn't used in a test run")
    message(FATAL_ERROR "kinect-cli serve --outside-port: the status isn't manual for a test run\n${so}\n${se}")
endif()
if(NOT so MATCHES "Forward TCP port ${port} to 192\\.168\\.77\\.10 ")
    message(FATAL_ERROR "kinect-cli serve --outside-port: the test run didn't use the test LAN address\n${so}\n${se}")
endif()
file(READ "${state}/pet-camera.json" saved)
if(NOT saved MATCHES "\"allowOutside\":true" OR NOT saved MATCHES "\"outsidePort\":${port}")
    message(FATAL_ERROR "kinect-cli serve --outside-port: the setting wasn't saved\n${saved}")
endif()
file(REMOVE_RECURSE "${base}")
