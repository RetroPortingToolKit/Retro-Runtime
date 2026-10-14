execute_process(COMMAND "${LINK_TEST}" --runner "${RUNNER}" --core "${CORE}"
    --rom "${ROM}" --out "${OUT}" --frames 4 --presentation-test RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "presentation link run failed: ${rc}")
endif()
file(READ "${OUT}/core.log" log)
foreach(expected
    "frame=1 known=1 size=1280x720"
    "frame=2 known=1 size=2560x1080"
    "frame=3 known=0 size=0x0"
    "frame=4 known=1 size=2560x1080")
    string(FIND "${log}" "FAKE_PRESENTATION ${expected}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "missing callback result: ${expected}\n${log}")
    endif()
endforeach()
