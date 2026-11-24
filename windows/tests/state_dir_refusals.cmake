# --state-dir may only name a folder that doesn't exist yet, an empty one, or one that already holds host
# state: the host gives its state folder a protected, inheritable owner-only DACL, which would reach every
# file already in it. A folder of other files is refused before the host starts: exit 2, the reason, and
# the folder left exactly as it was (nothing written into it). Drive roots are refused too; that case is
# a unit test only (test_viewer_model), so a regression could never re-ACL a real drive from here.
set(dir "${OUTDIR}/test-state/refused-state-dir")
file(REMOVE_RECURSE "${dir}")
file(MAKE_DIRECTORY "${dir}")
file(WRITE "${dir}/notes.txt" "not host state\n")

function(expect_untouched who)
    file(GLOB left RELATIVE "${dir}" "${dir}/*")
    if(NOT left STREQUAL "notes.txt")
        message(FATAL_ERROR "${who}: the host wrote into a refused --state-dir: ${left}")
    endif()
endfunction()

# kinect-cli serve (synthetic, loopback only, one second if it ever started).
execute_process(
    COMMAND "${CLI}" serve --synthetic --loopback-only --duration 1 --state-dir "${dir}"
    TIMEOUT 60
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE so
    ERROR_VARIABLE se)
message(STATUS "kinect-cli: rc=${rc} ${se}")
if(NOT rc EQUAL 2)
    message(FATAL_ERROR "kinect-cli serve: expected exit 2 (state dir refused), got ${rc}\n${so}")
endif()
if(NOT se MATCHES "STATE_DIR_REFUSED: [^\n]*--state-dir")
    message(FATAL_ERROR "kinect-cli serve: no refusal reason on stderr")
endif()
expect_untouched("kinect-cli serve")

# The viewer, as a test-hook run: no dialog, exit 2, the reason in <screenshot>.refused.txt, no capture.
set(out "${OUTDIR}/refused-state-dir.png")
file(REMOVE "${out}" "${out}.txt" "${out}.refused.txt")
execute_process(
    COMMAND "${VIEWER}" --synthetic --mode depth --state-dir "${dir}" --screenshot "${out}" --screenshot-after-ms 300
            --exit-after-screenshot
    TIMEOUT 60
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 2)
    message(FATAL_ERROR "kinect-viewer: expected exit 2 (state dir refused), got ${rc}")
endif()
if(EXISTS "${out}" OR EXISTS "${out}.txt" OR NOT EXISTS "${out}.refused.txt")
    message(FATAL_ERROR "kinect-viewer: captured, or no refusal reason written")
endif()
file(READ "${out}.refused.txt" why)
message(STATUS "kinect-viewer: rc=${rc} ${why}")
if(NOT why MATCHES "--state-dir")
    message(FATAL_ERROR "kinect-viewer: the reason doesn't name --state-dir")
endif()
expect_untouched("kinect-viewer")
file(REMOVE "${out}.refused.txt")
file(REMOVE_RECURSE "${dir}")

# A folder that merely holds a file called devices.json (someone else's) is not host state: refused before
# the host starts, and left byte for byte as it was, its ACLs included. Before the fix the host took it,
# gave it the owner-only DACL and overwrote devices.json.
set(dir "${OUTDIR}/test-state/unrelated-devices-json")
file(REMOVE_RECURSE "${dir}")
file(MAKE_DIRECTORY "${dir}")
file(WRITE "${dir}/devices.json" "{\"devices\":[\"someone else's\"]}\n")
file(WRITE "${dir}/notes.txt" "not host state\n")
function(snapshot out_var)
    set(s "")
    foreach(f devices.json notes.txt)
        file(SHA256 "${dir}/${f}" h)
        string(APPEND s "${f}=${h}\n")
    endforeach()
    foreach(p "${dir}" "${dir}/devices.json")
        file(TO_NATIVE_PATH "${p}" np)
        execute_process(COMMAND icacls "${np}" OUTPUT_VARIABLE acl ERROR_VARIABLE acl_err RESULT_VARIABLE acl_rc)
        string(APPEND s "acl(${acl_rc})=${acl}\n")
    endforeach()
    file(GLOB names RELATIVE "${dir}" "${dir}/*" "${dir}/.*")
    list(SORT names)
    string(APPEND s "names=${names}\n")
    set(${out_var} "${s}" PARENT_SCOPE)
endfunction()
snapshot(before)
execute_process(
    COMMAND "${CLI}" serve --synthetic --loopback-only --duration 1 --state-dir "${dir}"
    TIMEOUT 60
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE so
    ERROR_VARIABLE se)
message(STATUS "kinect-cli (unrelated devices.json): rc=${rc} ${se}")
snapshot(after)
if(NOT rc EQUAL 2)
    message(FATAL_ERROR "kinect-cli serve: a folder with someone else's devices.json was taken (rc=${rc})\n${so}")
endif()
if(NOT before STREQUAL after)
    message(FATAL_ERROR "kinect-cli serve changed a refused --state-dir:\nbefore:\n${before}\nafter:\n${after}")
endif()
file(REMOVE_RECURSE "${dir}")
