# kinect-cli record refuses colour: exit 2, the exact sentence, and its --out folder is
# never created (the folder is in the build tree, so even a regression couldn't write into the real
# recordings folder).
set(out "${OUTDIR}/refused-recordings-${MODE}")
file(REMOVE_RECURSE "${out}")
if(REAL)
    # The real source: refused before the sensor or a folder is opened (synthetic only).
    set(SOURCE_ARGS)
    set(EXPECT "RECORD_REFUSED: kinect-cli records the synthetic source only")
else()
    set(SOURCE_ARGS --synthetic)
    set(EXPECT "RECORD_REFUSED: Colour is live only\\. Switch to IR or Depth to record\\.")
endif()
execute_process(
    COMMAND "${CLI}" record ${SOURCE_ARGS} --mode ${MODE} --duration 1 --out "${out}" --state-dir "${OUTDIR}/test-state/cli"
    TIMEOUT 30
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE so
    ERROR_VARIABLE se)
message(STATUS "rc=${rc} ${se}${so}")
if(NOT rc EQUAL 2)
    message(FATAL_ERROR "expected exit 2, got ${rc}")
endif()
if(NOT se MATCHES "${EXPECT}")
    message(FATAL_ERROR "refusal message missing")
endif()
if(EXISTS "${out}")
    message(FATAL_ERROR "the --out folder was created: ${out}")
endif()
