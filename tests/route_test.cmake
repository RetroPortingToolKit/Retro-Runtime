file(MAKE_DIRECTORY "${OUT}")
file(REMOVE "${OUT}/test.route" "${OUT}/test.route.partial" "${OUT}/link.route" "${OUT}/link.route.partial")
set(base --core "${CORE}" --rom "${CMAKE_CURRENT_LIST_DIR}/fake_package.txt")
execute_process(COMMAND "${RUNNER}" ${base} --frames 8 --record-route "${OUT}/test.route"
    --route-mark 4:menu --out "${OUT}/record" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "record: ${rc} ${err}")
endif()
execute_process(COMMAND "${RUNNER}" ${base} --play-route "${OUT}/test.route"
    --out "${OUT}/play" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "play: ${rc} ${err}")
endif()
file(READ "${OUT}/play/route-verdict.json" verdict)
if(NOT verdict MATCHES "in_sync" OR NOT EXISTS "${OUT}/play/route-4.ppm")
    message(FATAL_ERROR "missing successful verdict or marker picture")
endif()
execute_process(COMMAND "${RUNNER}" ${base} --play-route "${OUT}/test.route"
    --opt audio.mute=1 --out "${OUT}/identity" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "identity mismatch")
    message(FATAL_ERROR "changed identity did not refuse: ${rc} ${err}")
endif()
file(READ "${OUT}/test.route" movie)
string(FIND "${movie}" "\nEND " end)
math(EXPR end "${end}+1")
string(SUBSTRING "${movie}" 0 ${end} prefix)
file(WRITE "${OUT}/truncated.route" "${prefix}")
execute_process(COMMAND "${RUNNER}" ${base} --play-route "${OUT}/truncated.route"
    --out "${OUT}/truncated" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "incomplete")
    message(FATAL_ERROR "truncated route did not refuse: ${rc} ${err}")
endif()
# Reseal an intentionally wrong first checkpoint to exercise the actual
# runtime divergence verdict, rather than only the file checksum check.
string(REPLACE "F 1 0 0 0 0 2d " "F 1 0 0 0 0 626164 " changed "${prefix}")
string(SHA256 digest "${changed}")
file(WRITE "${OUT}/rewrite.tmp" "${changed}END 8 ${digest}\n")
configure_file("${OUT}/rewrite.tmp" "${OUT}/divergence.route" @ONLY NEWLINE_STYLE UNIX)
execute_process(COMMAND "${RUNNER}" ${base} --play-route "${OUT}/divergence.route"
    --out "${OUT}/divergence" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 5 OR NOT err MATCHES "field 1")
    message(FATAL_ERROR "changed checkpoint did not diverge: ${rc} ${err}")
endif()
# A semantically invalid field with a valid checksum must not reach the core.
string(REPLACE "F 1 " "F 9 " changed "${prefix}")
string(SHA256 digest "${changed}")
file(WRITE "${OUT}/rewrite.tmp" "${changed}END 8 ${digest}\n")
configure_file("${OUT}/rewrite.tmp" "${OUT}/malformed.route" @ONLY NEWLINE_STYLE UNIX)
execute_process(COMMAND "${RUNNER}" ${base} --play-route "${OUT}/malformed.route"
    --out "${OUT}/malformed" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "field order mismatch")
    message(FATAL_ERROR "malformed route did not refuse: ${rc} ${err}")
endif()
# A real link session uses precisely the same recorder as headless playback.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "RETRO_RECORD_ROUTE=${OUT}/link.route"
    "${LINK}" --runner "${RUNNER}" ${base} --frames 8 --out "${OUT}/link-record"
    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "link recording: ${rc} ${err}")
endif()
execute_process(COMMAND "${RUNNER}" ${base} --play-route "${OUT}/link.route"
    --out "${OUT}/link-play" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "link to headless replay: ${rc} ${err}")
endif()
set(segment "${OUT}/interactive-state.route")
set(state "${OUT}/interactive-state.rstate")
file(REMOVE "${segment}" "${segment}.partial" "${state}")
execute_process(COMMAND "${LINK}" --runner "${RUNNER}" ${base} --frames 12
    --diagnostic-record-start-at "1:${segment}" --state-save-at "5:${state}" --state-load-at "9:${state}"
    --out "${OUT}/interactive-state" RESULT_VARIABLE rc OUTPUT_VARIABLE trace ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT EXISTS "${segment}" OR NOT EXISTS "${state}"
   OR NOT trace MATCHES "state: load at frame 9 ok")
    message(FATAL_ERROR "interactive recording blocked a player state operation: ${rc} ${err}")
endif()
execute_process(COMMAND "${RUNNER}" ${base} --play-route "${segment}"
    --out "${OUT}/interactive-state-replay" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "interactive segment before state save did not replay: ${rc} ${err}")
endif()
# An unavailable recording destination must leave the same game session alive.
set(blocked "${OUT}/recording-parent-is-a-file")
file(WRITE "${blocked}" "keep")
execute_process(COMMAND "${LINK}" --runner "${RUNNER}" ${base} --frames 8
    --diagnostic-record-start-at "1:${blocked}/fail.route"
    --out "${OUT}/interactive-failed-destination"
    RESULT_VARIABLE rc OUTPUT_VARIABLE trace ERROR_VARIABLE err)
file(READ "${blocked}" contents)
if(NOT rc EQUAL 0 OR NOT trace MATCHES "replay: start at 1 state 5"
   OR NOT trace MATCHES "8 frame\\(s\\) granted and done" OR NOT contents STREQUAL "keep")
    message(FATAL_ERROR "failed recording destination interrupted play: ${rc} ${trace} ${err}")
endif()
message(STATUS "routes: fresh process, link recording, identity/truncation/grammar refusals and first divergence pass")
