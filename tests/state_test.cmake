# Savestates over the link (link 1.1) and headless, with the fake core, whose
# picture after frame k is k & 0xff everywhere.
#
#   roundtrip  save after frame 4, load after frame 8: frames 9 and 10 run from
#              4 again, so the last picture is 6
#   refused    the same state into a session with other content: refused by
#              the load rule, naming both hashes; the session runs on
#   headless   headless --load-state takes the envelope
# (A corrupt state -- bytes that no longer match their recorded hash -- is
# checked in retro-overlay-test, which can write arbitrary bytes.)
foreach(v LINK_TEST RUNNER CORE ROM OTHER_ROM OUT)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "state_test.cmake: -D${v}= is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(state "${OUT}/slot01.rstate")

function(run_expect name)
    cmake_parse_arguments(A "" "" "ARGS;EXPECT" ${ARGN})
    execute_process(COMMAND ${EMULATOR} ${A_ARGS}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    foreach(want ${A_EXPECT})
        if(NOT out MATCHES "${want}")
            message(FATAL_ERROR "${name}: expected /${want}/ (rc ${rc})\nstdout:\n${out}\nstderr:\n${err}")
        endif()
    endforeach()
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${name}: exit ${rc}\nstdout:\n${out}\nstderr:\n${err}")
    endif()
    message(STATUS "${name}: ok")
endfunction()

run_expect(roundtrip
    ARGS "${LINK_TEST}" --runner "${RUNNER}" --core "${CORE}" --rom "${ROM}" --frames 10
         --out "${OUT}/roundtrip" --state-save-at "4:${state}" --state-load-at "8:${state}"
    EXPECT "state: save at frame 4 ok \\(12 bytes\\)" "state: load at frame 8 ok \\(12 bytes\\)"
           "picture: first byte 6" "10 frame\\(s\\) granted and done, runner exit 0")

run_expect(refused
    ARGS "${LINK_TEST}" --runner "${RUNNER}" --core "${CORE}" --rom "${OTHER_ROM}" --frames 3
         --out "${OUT}/refused" --state-load-at "2:${state}"
    EXPECT "state: load at frame 2 failed: refused: content SHA-256 differs: the state has [0-9a-f]+, this session has [0-9a-f]+"
           "picture: first byte 3" "runner exit 0")

run_expect(headless
    ARGS "${RUNNER}" --core "${CORE}" --rom "${ROM}" --frames 1 --out "${OUT}/headless"
         --load-state "${state}"
    EXPECT "state: loaded .*slot01.rstate \\(envelope, 12 bytes\\)")
