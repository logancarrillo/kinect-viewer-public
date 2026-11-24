# kv-render-icon makes the app icon from design/logo.svg and nothing else: any other file (even an
# existing SVG or image elsewhere in the repo) is refused with exit 3 before it is read, and no .ico
# is written.
set(CASES
    "${SRC}/design/icon-sheet.png"
    "${SRC}/macos/Assets.xcassets/AppIcon.appiconset/icon_16x16.png"
    "${SRC}/logo.svg"
    "${SRC}/design/logo.svg.bak")
set(out "${OUTDIR}/refused-icon.ico")
foreach(input IN LISTS CASES)
    file(REMOVE "${out}")
    execute_process(COMMAND "${TOOL}" "${input}" "${out}" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE why TIMEOUT 30)
    if(NOT rc EQUAL 3)
        message(FATAL_ERROR "${input}: expected exit 3 (refused), got ${rc}")
    endif()
    if(EXISTS "${out}")
        message(FATAL_ERROR "${input}: an icon was written")
    endif()
    message(STATUS "${input}: refused (${why})")
endforeach()
# The logo itself still works.
execute_process(COMMAND "${TOOL}" "${SRC}/design/logo.svg" "${out}" RESULT_VARIABLE rc OUTPUT_QUIET TIMEOUT 60)
if(NOT rc EQUAL 0 OR NOT EXISTS "${out}")
    message(FATAL_ERROR "the logo itself was not rendered (rc=${rc})")
endif()
file(REMOVE "${out}")
