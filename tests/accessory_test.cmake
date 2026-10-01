# Accessory data (rcore rev 7, link 2.1; docs/CORE_ABI.md "Accessory data"):
# the VRU on fake_core, which drains accessory_poll at every frame boundary,
# logs each message as FAKE_ACCESSORY frame=K ..., echoes it back through
# accessory_notify, and logs the bound seat's connected flag. CASE picks:
#
#   headless        --vru1 --accessory-script: a message named for frame 3 is
#                   seen in frame 3 and in no other; its echo prints as
#                   ACCESSORY_NOTIFY; seat 0 reads connected=0 although the
#                   headless sink puts a controller there
#   link            the same over the link: --accessory-send before grant 3
#                   lands in frame 3, the notify comes back; the second drain
#                   is the first (no FAKE_ACCESSORY_UNSTABLE)
#   replay          --replay-at 2: the second run of frame 3 sees the message
#                   again (the script is the frame's sequence, both times)
#   refused_type    fake_pkg_core declares neither accessory_data nor the
#                   type: --vru1 is refused, exit 2
#   refused_seat    --vru5 is not a flag this runner has: refused, exit 2
#   script_unbound  a script line for a seat nothing is plugged into: exit 2
#
# EMULATOR is CMAKE_CROSSCOMPILING_EMULATOR (wine, for a MinGW build), if any.
foreach(v LINK_TEST RUNNER PLAIN_CORE PKG_CORE PACKAGE ROM OUT CASE)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "accessory_test.cmake: -D${v}= is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
string(ASCII 9 TAB)

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

# `text` must match every pattern in MATCH and none in NOMATCH.
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

set(msg "{\"cmd\":\"vru_submit\",\"epoch\":1,\"slot\":4}")
set(msg2 "{\"cmd\":\"vru_speech\",\"event\":\"start\"}")
set(script "${OUT}/accessory.tsv")
# Two messages for frame 3, in order, and one for frame 5; a comment line.
file(WRITE "${script}" "# frame seat slot bytes\n3${TAB}0${TAB}0${TAB}${msg}\n3${TAB}0${TAB}0${TAB}${msg2}\n5${TAB}0${TAB}0${TAB}${msg}\n")

if(CASE STREQUAL "headless")
    run("${RUNNER}" --core "${PLAIN_CORE}" --rom "${ROM}" --frames 6 --out "${OUT}/run"
        --vru1 --accessory-script "${script}")
    expect_exit(0)
    file(READ "${OUT}/run/core.log" log)
    # Frame 3 sees both, in order; frame 5 sees one; no other frame sees any.
    string(REGEX MATCHALL "FAKE_ACCESSORY frame=[0-9]+" hits "${log}")
    if(NOT hits STREQUAL "FAKE_ACCESSORY frame=3;FAKE_ACCESSORY frame=3;FAKE_ACCESSORY frame=5")
        message(FATAL_ERROR "the messages landed in frames: ${hits}")
    endif()
    expect(log MATCH "FAKE_ACCESSORY frame=3 seat=0 slot=0 ${msg}\n.*FAKE_ACCESSORY frame=3 seat=0 slot=0 ${msg2}\n"
               NOMATCH "FAKE_ACCESSORY_UNSTABLE")
    # The bound seat reads no pad, although the headless sink connects seat 0.
    string(REGEX MATCHALL "FAKE_VRU_SEAT seat=0 connected=0 buttons=0" masked "${log}")
    list(LENGTH masked n_masked)
    if(NOT n_masked EQUAL 6)
        message(FATAL_ERROR "seat 0 read connected=0 in ${n_masked} of 6 frames")
    endif()
    expect(log NOMATCH "connected=1")
    # The echoes, on stderr, in order.
    expect(err MATCH "ACCESSORY_NOTIFY 0 0 {\"echo\":${msg},\"frame\":3}\nACCESSORY_NOTIFY 0 0 {\"echo\":${msg2},\"frame\":3}\nACCESSORY_NOTIFY 0 0 {\"echo\":${msg},\"frame\":5}\n")
elseif(CASE STREQUAL "link")
    run("${LINK_TEST}" --runner "${RUNNER}" --core "${PLAIN_CORE}" --rom "${ROM}" --frames 6
        --out "${OUT}/run" --vru1
        --accessory-send "3:0:0:${msg}" --accessory-send "3:0:0:${msg2}" --accessory-send "5:0:0:${msg}")
    expect_exit(0)
    expect(out MATCH "6 frame\\(s\\) granted and done, runner exit 0")
    file(READ "${OUT}/run/core.log" log)
    string(REGEX MATCHALL "FAKE_ACCESSORY frame=[0-9]+" hits "${log}")
    if(NOT hits STREQUAL "FAKE_ACCESSORY frame=3;FAKE_ACCESSORY frame=3;FAKE_ACCESSORY frame=5")
        message(FATAL_ERROR "the messages landed in frames: ${hits}")
    endif()
    expect(log NOMATCH "FAKE_ACCESSORY_UNSTABLE" "connected=1")
    # The link test grants seat 0 connected; the runner still reports 0.
    expect(log MATCH "FAKE_VRU_SEAT seat=0 connected=0 buttons=0")
    expect(out MATCH "accessory: notify 0 0 {\"echo\":${msg},\"frame\":3}\naccessory: notify 0 0 {\"echo\":${msg2},\"frame\":3}\n"
               "accessory: notify 0 0 {\"echo\":${msg},\"frame\":5}\n")
elseif(CASE STREQUAL "replay")
    run("${RUNNER}" --core "${PLAIN_CORE}" --rom "${ROM}" --frames 6 --out "${OUT}/run"
        --vru1 --accessory-script "${script}" --replay-at 2)
    expect_exit(0)
    expect(out MATCH "final frame IDENTICAL")
    file(READ "${OUT}/run/core.log" log)
    string(REGEX MATCHALL "FAKE_ACCESSORY frame=[0-9]+" hits "${log}")
    if(NOT hits STREQUAL "FAKE_ACCESSORY frame=3;FAKE_ACCESSORY frame=3;FAKE_ACCESSORY frame=5;FAKE_ACCESSORY frame=3;FAKE_ACCESSORY frame=3;FAKE_ACCESSORY frame=5")
        message(FATAL_ERROR "the messages landed in frames: ${hits}")
    endif()
elseif(CASE STREQUAL "refused_type")
    run("${RUNNER}" --core "${PKG_CORE}" --package "${PACKAGE}" --rom "${ROM}" --frames 1
        --out "${OUT}/run" --vru1)
    expect_exit(2)
    expect(err MATCH "accessory n64.vru: core 'fake_pkg' does not declare accessory_data")
    run("${LINK_TEST}" --runner "${RUNNER}" --core "${PKG_CORE}" --package "${PACKAGE}"
        --rom "${ROM}" --frames 1 --out "${OUT}/link" --vru1)
    expect_exit(2)
    expect(err MATCH "the runner ended before it was ready \\(exit 2")
    file(READ "${OUT}/link/runner.log" rlog)
    expect(rlog MATCH "does not declare accessory_data")
elseif(CASE STREQUAL "refused_seat")
    run("${RUNNER}" --core "${PLAIN_CORE}" --rom "${ROM}" --frames 1 --out "${OUT}/run" --vru5)
    expect_exit(2)
    expect(err MATCH "unknown argument --vru5")
elseif(CASE STREQUAL "script_unbound")
    file(WRITE "${script}" "2${TAB}1${TAB}0${TAB}${msg}\n")
    run("${RUNNER}" --core "${PLAIN_CORE}" --rom "${ROM}" --frames 2 --out "${OUT}/run"
        --vru1 --accessory-script "${script}")
    expect_exit(2)
    expect(err MATCH "accessory.tsv:1: nothing is plugged into seat 1 slot 0")
else()
    message(FATAL_ERROR "unknown CASE '${CASE}'")
endif()
