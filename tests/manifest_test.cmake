# The sidecar check and capabilities from a newer draft revision
# (docs/CORE_ABI.md, "Capabilities are declared, never faked"). fake_future_core
# declares bit 40, which this runner cannot name, and its sidecar names it
# "future_feature". CASE picks:
#
#   future_headless  loads and runs 3 frames, exit 0, both warnings on stderr
#   future_describe  --describe exits 0 with its records; warnings on stderr only
#   future_link      the same core over the link, 3 frames
#   missing_known    a sidecar naming a KNOWN capability the library lacks
#                    (deterministic): refused, exit 2, naming both lists
#   extra_known      a sidecar lacking a known capability the library has
#                    (savestate): refused, exit 2
foreach(v LINK_TEST RUNNER FUTURE_CORE ROM OUT CASE)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "manifest_test.cmake: -D${v}= is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

function(run)
    execute_process(COMMAND ${EMULATOR} ${ARGN}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    message("${out}${err}")
    set(rc "${rc}" PARENT_SCOPE)
    set(out "${out}" PARENT_SCOPE)
    set(err "${err}" PARENT_SCOPE)
endfunction()

function(expect_exit want)
    if(NOT rc EQUAL ${want})
        message(FATAL_ERROR "expected exit ${want}, got ${rc}")
    endif()
endfunction()

function(expect text)
    cmake_parse_arguments(A "" "" "MATCH;NOMATCH" ${ARGN})
    foreach(want ${A_MATCH})
        if(NOT "${${text}}" MATCHES "${want}")
            message(FATAL_ERROR "${text}: expected /${want}/")
        endif()
    endforeach()
    foreach(bad ${A_NOMATCH})
        if("${${text}}" MATCHES "${bad}")
            message(FATAL_ERROR "${text}: did not expect /${bad}/")
        endif()
    endforeach()
endfunction()

set(warn_bits "WARN: fake_future_core.rcore.toml: capabilities: the library declares bits 0x10000000000 this runner cannot name \\(newer than this runner's rev [0-9]+\\); ignored")
set(warn_name "WARN: fake_future_core.rcore.toml: capabilities: the manifest names 'future_feature', which this runner does not know \\(newer than this runner's rev [0-9]+\\); ignored")

# A copy of the library with a sidecar of this test's own writing.
function(core_with_sidecar caps var)
    get_filename_component(name "${FUTURE_CORE}" NAME)
    file(COPY "${FUTURE_CORE}" DESTINATION "${OUT}/lib")
    file(WRITE "${OUT}/lib/fake_future_core.rcore.toml"
"[core]\nabi_major       = 0\ndraft_revision  = 99\nid              = \"fake_future\"\nversion         = \"1.0\"\nlibrary         = \"${name}\"\nplatforms       = [\"test\"]\ncapabilities    = [${caps}]\n\n[build]\nengine_commit   = \"\"\nengine_dirty    = false\ntoolchain       = \"\"\ngenerated_utc   = \"\"\n")
    set(${var} "${OUT}/lib/${name}" PARENT_SCOPE)
endfunction()

if(CASE STREQUAL "future_headless")
    run("${RUNNER}" --core "${FUTURE_CORE}" --rom "${ROM}" --frames 3 --out "${OUT}/run")
    expect_exit(0)
    expect(out MATCH "core: fake_future 1.0 platforms=test capabilities=0x10000000405 "
                     "manifest: fake_future_core.rcore.toml agrees"
                     "runner: 3 frame\\(s\\) submitted")
    expect(err MATCH "${warn_bits}" "${warn_name}")
elseif(CASE STREQUAL "future_describe")
    run("${RUNNER}" --describe --core "${FUTURE_CORE}")
    expect_exit(0)
    expect(out MATCH "^describe\t1\ncore\tfake_future\t1.0\ttest\n" "\naccessory\tn64.vru\t")
    expect(out NOMATCH "WARN")
    expect(err MATCH "${warn_bits}" "${warn_name}")
elseif(CASE STREQUAL "future_link")
    run("${LINK_TEST}" --runner "${RUNNER}" --core "${FUTURE_CORE}" --rom "${ROM}" --frames 3
        --out "${OUT}/run")
    expect_exit(0)
    expect(out MATCH "3 frame\\(s\\) granted and done, runner exit 0")
    file(READ "${OUT}/run/runner.log" rlog)
    expect(rlog MATCH "${warn_bits}" "${warn_name}")
elseif(CASE STREQUAL "missing_known")
    core_with_sidecar("\"run_frame\", \"savestate\", \"deterministic\", \"accessory_data\", \"future_feature\"" core)
    run("${RUNNER}" --core "${core}" --rom "${ROM}" --frames 1 --out "${OUT}/run")
    expect_exit(2)
    expect(err MATCH "capabilities: manifest 'run_frame,savestate,deterministic,accessory_data', library 'run_frame,savestate,accessory_data'")
elseif(CASE STREQUAL "extra_known")
    core_with_sidecar("\"run_frame\", \"accessory_data\", \"future_feature\"" core)
    run("${RUNNER}" --core "${core}" --rom "${ROM}" --frames 1 --out "${OUT}/run")
    expect_exit(2)
    expect(err MATCH "capabilities: manifest 'run_frame,accessory_data', library 'run_frame,savestate,accessory_data'")
else()
    message(FATAL_ERROR "unknown CASE '${CASE}'")
endif()
