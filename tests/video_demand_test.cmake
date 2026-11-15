execute_process(COMMAND "${LINK_TEST}" --runner "${RUNNER}" --core "${CORE}"
    --rom "${ROM}" --out "${OUT}" --frames 10 --video-every 3
    RESULT_VARIABLE rc OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "video-demand link run failed: ${rc}\n${output}\n${errors}")
endif()
foreach(expected "10 frame(s) granted and done, runner exit 0, 0 fault(s)"
                 "pictures received: 4" "picture: first byte 10" "frame stats: game 5 work measured")
    string(FIND "${output}" "${expected}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "missing result: ${expected}\n${output}")
    endif()
endforeach()
file(READ "${OUT}/core.log" log)
string(FIND "${log}" "FAKE_DONE frames=10" pos)
if(pos EQUAL -1)
    message(FATAL_ERROR "video demand lost simulation work: ${log}")
endif()

# Recording observes every field even when its host's grant declines pixels.
file(REMOVE "${OUT}/recording.route" "${OUT}/recording.route.partial")
execute_process(COMMAND "${LINK_TEST}" --runner "${RUNNER}" --core "${CORE}"
    --rom "${ROM}" --out "${OUT}/record" --frames 10 --video-every 0
    --record-start-at "1:${OUT}/recording.route" --record-stop-at 9
    RESULT_VARIABLE rc OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "recording demand run failed: ${rc}\n${output}\n${errors}")
endif()
foreach(expected "pictures received: 9" "stop at 9 state 2 fields 8"
                 "10 frame(s) granted and done, runner exit 0, 0 fault(s)")
    string(FIND "${output}" "${expected}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "recording did not override optional demand: ${expected}\n${output}")
    endif()
endforeach()
