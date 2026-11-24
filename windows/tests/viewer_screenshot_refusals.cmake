# The viewer's --screenshot test hook writes the whole window to disk, so it is refused unless the
# source is synthetic and the view has no colour in it. Each refusal must exit with 3 before
# anything is opened or captured: no PNG, no report, only the .refused.txt reason.
# The real-source case also passes --no-autostart, so even a regression couldn't open the sensor.
set(CASES
    "real|--no-autostart --mode ir"
    "colour|--synthetic --mode color"
    "sbs-colour-depth|--synthetic --mode sbs --sbs-layout color-depth"
    "sbs-colour-ir|--synthetic --mode sbs --sbs-layout color-ir"
    "blend-depth|--synthetic --mode depth --blend 50"
    "blend-ir|--synthetic --mode ir --blend 0"
    "3d-rgb-points|--synthetic --mode 3d --point-color rgb"
    "3d-real|--no-autostart --mode 3d")
# A refused run exits before the host starts, so it never even creates this folder (v3 §10).
file(REMOVE_RECURSE "${OUTDIR}/test-state/refusals")
foreach(case IN LISTS CASES)
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 name)
    list(GET parts 1 args)
    separate_arguments(args)
    set(out "${OUTDIR}/refused-${name}.png")
    file(REMOVE "${out}" "${out}.txt" "${out}.refused.txt" "${out}.pairing.png")
    execute_process(
        COMMAND "${VIEWER}" ${args} --state-dir "${OUTDIR}/test-state/refusals" --screenshot "${out}"
                --screenshot-after-ms 300 --exit-after-screenshot
        TIMEOUT 30
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 3)
        message(FATAL_ERROR "${name}: expected exit 3 (screenshot refused), got ${rc}")
    endif()
    if(EXISTS "${out}" OR EXISTS "${out}.txt" OR EXISTS "${out}.pairing.png")
        message(FATAL_ERROR "${name}: something was captured")
    endif()
    if(NOT EXISTS "${out}.refused.txt")
        message(FATAL_ERROR "${name}: no refusal reason written")
    endif()
    file(READ "${out}.refused.txt" why)
    message(STATUS "${name}: rc=${rc} ${why}")
    file(REMOVE "${out}.refused.txt")
endforeach()
if(EXISTS "${OUTDIR}/test-state/refusals")
    message(FATAL_ERROR "a refused run started the streaming host")
endif()
