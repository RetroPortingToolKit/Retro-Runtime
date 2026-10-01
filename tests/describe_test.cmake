# --describe (docs/CORE_RUNNER.md, "--describe"): what a core declares, as TAB
# records, with no ROM and no session. The fake cores declare the options and
# inputs below (tests/rcore_fake_core.c); stdout must be exactly these lines.
# CASE picks the scenario:
#
#   plain          fake_core, no --rom: the records, byte for byte, with its
#                  n64.vru accessory record
#   package        fake_pkg_core + --package: the same records, its own id,
#                  and no accessory (the package build declares none)
#   package_none   fake_pkg_core without --package: still described
#   unwanted       fake_core given --package: refused, exit 2
#   no_core        a core that does not exist: refused, exit 2, stdout empty
#
# EMULATOR is CMAKE_CROSSCOMPILING_EMULATOR (wine, for a MinGW build), if any.
string(ASCII 9 TAB)

function(run)
    execute_process(COMMAND ${EMULATOR} ${ARGN}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    message("${out}${err}")
    set(rc "${rc}" PARENT_SCOPE)
    set(out "${out}" PARENT_SCOPE)
    set(err "${err}" PARENT_SCOPE)
endfunction()

# The expected stdout for core id `id`. <T> is a TAB; everything else is
# literal, so \n here is a backslash and an n -- the escaped field.
function(expected id var)
    set(lines
        [=[describe<T>1]=]
        [=[core<T>@ID@<T>1.0<T>test]=]
        [=[option<T>video.mode<T>enum<T>restart,netplay<T>1<T>fast<T>0<T>0<T>Video mode<T>Line one\nline two\twith a back\\slash]=]
        [=[value<T>video.mode<T>fast]=]
        [=[value<T>video.mode<T>accurate]=]
        [=[value<T>video.mode<T>tab\there]=]
        [=[option<T>audio.mute<T>bool<T>-<T>1<T>0<T>0<T>0<T>Mute<T>]=]
        [=[option<T>cpu.overclock<T>int<T>developer<T>0<T><T>-5<T>1000000000000<T>Overclock<T>Percent over stock]=]
        [=[option<T>debug.trace<T>string<T>restart,netplay,developer<T>0<T><T>0<T>0<T>Trace\rfile<T>]=]
        [=[input<T>1<T>0<T>0<T>A]=]
        [=[input<T>0<T>3<T>-1<T>C-Left]=]
        [=[input<T>0<T>1<T>0<T>Stick]=])
    if(id STREQUAL "fake")
        list(APPEND lines [=[accessory<T>n64.vru<T>VRU Microphone<T>netplay<T>f<T>1]=])
    endif()
    string(JOIN "\n" text ${lines})
    string(REPLACE "<T>" "${TAB}" text "${text}")
    string(REPLACE "@ID@" "${id}" text "${text}")
    set(${var} "${text}\n" PARENT_SCOPE)
endfunction()

function(expect_described id)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "--describe exited ${rc}")
    endif()
    expected("${id}" want)
    if(NOT out STREQUAL want)
        message(FATAL_ERROR "--describe printed:\n${out}\nexpected:\n${want}")
    endif()
endfunction()

function(expect_refused pattern)
    if(NOT rc EQUAL 2)
        message(FATAL_ERROR "expected a refusal (exit 2), got exit ${rc}")
    endif()
    if(NOT out STREQUAL "")
        message(FATAL_ERROR "a refusal printed to stdout:\n${out}")
    endif()
    if(NOT err MATCHES "${pattern}")
        message(FATAL_ERROR "the refusal does not say '${pattern}'")
    endif()
endfunction()

if(CASE STREQUAL "plain")
    run("${RUNNER}" --describe --core "${PLAIN_CORE}")
    expect_described(fake)
elseif(CASE STREQUAL "package")
    run("${RUNNER}" --describe --core "${PKG_CORE}" --package "${PACKAGE}")
    expect_described(fake_pkg)
elseif(CASE STREQUAL "package_none")
    run("${RUNNER}" --describe --core "${PKG_CORE}")
    expect_described(fake_pkg)
elseif(CASE STREQUAL "unwanted")
    run("${RUNNER}" --describe --core "${PLAIN_CORE}" --package "${PACKAGE}")
    expect_refused("does not declare game_package, so it takes no package")
elseif(CASE STREQUAL "no_core")
    run("${RUNNER}" --describe --core "${PLAIN_CORE}.does-not-exist")
    expect_refused("retro-core-runner: ")
else()
    message(FATAL_ERROR "unknown CASE '${CASE}'")
endif()
