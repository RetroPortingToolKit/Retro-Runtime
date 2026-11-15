foreach(mode paced unpaced)
    set(args)
    set(pictures 8)
    set(repeats 0)
    if(mode STREQUAL unpaced)
        list(APPEND args --unpaced)
        set(pictures 4)
        set(repeats 4)
    endif()
    execute_process(COMMAND "${LINK_TEST}" --runner "${RUNNER}" --core "${CORE}"
        --rom "${ROM}" --out "${OUT}/${mode}" --frames 8 --cpu-repeat-test --pcm-hash ${args}
        RESULT_VARIABLE rc OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${mode} CPU repeat failed: ${rc}\n${output}\n${errors}")
    endif()
    foreach(expected "8 frame(s) granted and done, runner exit 0, 0 fault(s)"
                     "pictures received: ${pictures}" "picture: first byte 4"
                     "frame stats: game 4 work measured" "over 32 samples")
        string(FIND "${output}" "${expected}" pos)
        if(pos EQUAL -1)
            message(FATAL_ERROR "${mode} missing result ${expected}\n${output}")
        endif()
    endforeach()
    file(READ "${OUT}/${mode}/core.log" log)
    if(NOT log MATCHES "FAKE_CPU_REPEAT repeats=${repeats}")
        message(FATAL_ERROR "${mode} repeat branch was not selected correctly: ${log}")
    endif()
    string(REGEX MATCH "PCM_HASH [0-9a-f]+ over 32 samples" pcm "${output}")
    if(mode STREQUAL paced)
        set(reference_pcm "${pcm}")
    elseif(NOT pcm STREQUAL reference_pcm)
        message(FATAL_ERROR "repeats changed PCM: ${reference_pcm} vs ${pcm}")
    endif()
endforeach()
# The recorder sees each submitted field before an unchanged picture is
# elided. Playback without lending must match every pixel and audio digest.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "RETRO_RECORD_ROUTE=${OUT}/repeat.route"
    "${LINK_TEST}" --runner "${RUNNER}" --core "${CORE}" --rom "${ROM}"
    --out "${OUT}/record" --frames 8 --cpu-repeat-test --unpaced
    RESULT_VARIABLE rc OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT rc EQUAL 0 OR NOT output MATCHES "pictures received: 4")
    message(FATAL_ERROR "repeat route record failed: ${rc}\n${output}\n${errors}")
endif()
execute_process(COMMAND "${RUNNER}" --core "${CORE}" --rom "${ROM}"
    --play-route "${OUT}/repeat.route" --out "${OUT}/play"
    RESULT_VARIABLE rc OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "repeat route playback failed: ${rc}\n${output}\n${errors}")
endif()
file(READ "${OUT}/play/route-verdict.json" verdict)
if(NOT verdict MATCHES "in_sync")
    message(FATAL_ERROR "repeat per-field pixels/audio diverged: ${verdict}")
endif()
