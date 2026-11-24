# kinect-cli list honours a held lock: with --lock-path naming a lock held by a live process (pid 4,
# the System process, as verify-lock uses), it reports the holder and exits 3 without enumerating
# (it never opens the Kinect).
set(lock "${OUTDIR}/kv-cli-list-lock.txt")
file(WRITE "${lock}" "agent: cli_list test - held by the System process\npid: 4\n")
execute_process(COMMAND "${CLI}" list --lock-path "${lock}" --state-dir "${OUTDIR}/test-state/cli" TIMEOUT 30 RESULT_VARIABLE rc OUTPUT_VARIABLE so ERROR_VARIABLE se)
file(REMOVE "${lock}")
message(STATUS "rc=${rc} ${so}${se}")
if(NOT rc EQUAL 3)
    message(FATAL_ERROR "expected exit 3 (lock held), got ${rc}")
endif()
if(NOT so MATCHES "lock=held by another program: agent: cli_list test" OR NOT so MATCHES "enumeration skipped: the Kinect lock is held")
    message(FATAL_ERROR "the held lock was not reported")
endif()
if(so MATCHES "libfreenect2_devices=")
    message(FATAL_ERROR "it enumerated despite the lock")
endif()