file(MAKE_DIRECTORY "${OUT}")
file(REMOVE "${OUT}/anchor.route" "${OUT}/anchor.route.partial")
set(base --core "${CORE}" --rom "${CMAKE_CURRENT_LIST_DIR}/fake_package.txt")
execute_process(COMMAND "${LINK}" --runner "${RUNNER}" ${base} --frames 12
    --record-start-at "3:${OUT}/anchor.route" --record-stop-at 10 --out "${OUT}/record"
    RESULT_VARIABLE rc OUTPUT_VARIABLE output ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT output MATCHES "start at 3 state 1" OR NOT output MATCHES "stop at 10 state 2 fields 7")
    message(FATAL_ERROR "anchor controls: ${rc} ${output} ${err}")
endif()
file(READ "${OUT}/anchor.route" movie)
if(NOT movie MATCHES "\nA [0-9a-f]+ [0-9a-f]+\n")
    message(FATAL_ERROR "anchor is missing")
endif()
execute_process(COMMAND "${RUNNER}" ${base} --frames 20 --play-route "${OUT}/anchor.route"
    --out "${OUT}/play" RESULT_VARIABLE rc OUTPUT_VARIABLE output ERROR_VARIABLE err)
file(READ "${OUT}/play/route-verdict.json" verdict)
if(NOT rc EQUAL 0 OR NOT verdict MATCHES "in_sync" OR NOT verdict MATCHES "\"frames\": 7")
    message(FATAL_ERROR "anchor replay: ${rc} ${verdict} ${output} ${err}")
endif()
# Starting on an existing destination must refuse and leave it unchanged.
file(SHA256 "${OUT}/anchor.route" original)
execute_process(COMMAND "${LINK}" --runner "${RUNNER}" ${base} --frames 5
    --record-start-at "2:${OUT}/anchor.route" --out "${OUT}/existing"
    RESULT_VARIABLE rc OUTPUT_VARIABLE output ERROR_VARIABLE err)
file(SHA256 "${OUT}/anchor.route" after)
if(NOT rc EQUAL 0 OR NOT output MATCHES "state 5.*already exists" OR NOT original STREQUAL after)
    message(FATAL_ERROR "existing replay protection: ${rc} ${output} ${err}")
endif()
execute_process(COMMAND "${LINK}" --runner "${RUNNER}" ${base} --restart-route "${OUT}/anchor.route"
    --frames 20 --expect-route-end --out "${OUT}/selected"
    RESULT_VARIABLE rc OUTPUT_VARIABLE output ERROR_VARIABLE err)
file(READ "${OUT}/selected/route-verdict.json" verdict)
if(NOT rc EQUAL 0 OR NOT verdict MATCHES "in_sync" OR NOT output MATCHES "7 frame.s. granted and done")
    message(FATAL_ERROR "selected replay restart: ${rc} ${verdict} ${output} ${err}")
endif()
