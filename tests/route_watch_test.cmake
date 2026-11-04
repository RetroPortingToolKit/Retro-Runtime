file(MAKE_DIRECTORY "${OUT}")
file(REMOVE "${OUT}/watch.route" "${OUT}/watch.route.partial")
set(base --core "${CORE}" --rom "${CMAKE_CURRENT_LIST_DIR}/fake_package.txt")
execute_process(COMMAND "${RUNNER}" ${base} --frames 8 --record-route "${OUT}/watch.route"
    --out "${OUT}/record" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "watch fixture record: ${rc} ${err}")
endif()
string(REPEAT "u" 64 original_save)
file(WRITE "${OUT}/battery" "${original_save}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "RETRO_PLAY_ROUTE=${OUT}/watch.route"
    "${LINK}" --runner "${RUNNER}" ${base} --frames 20 --expect-route-end
    --save "battery=${OUT}/battery" --input-script 1:a+left --out "${OUT}/complete"
    RESULT_VARIABLE rc OUTPUT_VARIABLE stdout ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT stdout MATCHES "8 frame.s. granted and done")
    message(FATAL_ERROR "watch completion: ${rc} ${stdout} ${err}")
endif()
file(READ "${OUT}/complete/route-verdict.json" verdict)
file(READ "${OUT}/battery" saved)
if(NOT verdict MATCHES "in_sync" OR NOT saved STREQUAL original_save)
    message(FATAL_ERROR "watch changed save or failed: ${verdict}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "RETRO_PLAY_ROUTE=${OUT}/watch.route"
    "${LINK}" --runner "${RUNNER}" ${base} --frames 3 --out "${OUT}/stop"
    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
file(READ "${OUT}/stop/route-verdict.json" verdict)
if(NOT rc EQUAL 0 OR NOT verdict MATCHES "stopped")
    message(FATAL_ERROR "watch Quit: ${rc} ${verdict} ${err}")
endif()
foreach(input IN ITEMS a left)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "RETRO_PLAY_ROUTE=${OUT}/watch.route" RETRO_ROUTE_TAKEOVER=1
        "${LINK}" --runner "${RUNNER}" ${base} --frames 12
        --input-script "1:a,3:none,5:${input}" --out "${OUT}/takeover-${input}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE stdout ERROR_VARIABLE err)
    file(READ "${OUT}/takeover-${input}/route-verdict.json" verdict)
    if(NOT rc EQUAL 0 OR NOT verdict MATCHES "taken_over" OR NOT verdict MATCHES "\"takeover_frame\": 5"
       OR NOT stdout MATCHES "12 frame.s. granted and done" OR NOT stdout MATCHES "picture: first byte 12")
        message(FATAL_ERROR "watch takeover ${input}: ${rc} ${verdict} ${stdout} ${err}")
    endif()
endforeach()
file(READ "${OUT}/watch.route" movie)
string(FIND "${movie}" "\nEND " boundary)
math(EXPR boundary "${boundary}+1")
string(SUBSTRING "${movie}" 0 ${boundary} prefix)
string(REPLACE "F 1 0 0 0 0 2d " "F 1 0 0 0 0 626164 " changed "${prefix}")
string(SHA256 digest "${changed}")
file(WRITE "${OUT}/rewrite.tmp" "${changed}END 8 ${digest}\n")
configure_file("${OUT}/rewrite.tmp" "${OUT}/wrong.route" @ONLY NEWLINE_STYLE UNIX)
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "RETRO_PLAY_ROUTE=${OUT}/wrong.route"
    "${LINK}" --runner "${RUNNER}" ${base} --frames 20 --expect-route-end --out "${OUT}/mismatch"
    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
file(READ "${OUT}/mismatch/route-verdict.json" verdict)
if(rc EQUAL 0 OR NOT verdict MATCHES "diverged" OR NOT verdict MATCHES "\"first_divergence\": 1")
    message(FATAL_ERROR "watch mismatch: ${rc} ${verdict} ${err}")
endif()
message(STATUS "watch: end, Quit, input authority, private saves, neutral arming and button/analog takeover pass")
