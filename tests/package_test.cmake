# Game packages (docs/CORE_RUNNER.md, "--package"): a GAME_PACKAGE core runs
# only with --package, any other core refuses one, and the runner logs the
# package's SHA-256 beside the core's. CASE picks the scenario:
#
#   headless       fake_pkg_core + --package, headless: runs, hash printed
#   link           the same through retro-core-link-test: hash in runner.log
#   missing        fake_pkg_core without --package: refused, exit 2
#   link_missing   the same over the link: the runner ends before Ready, exit 2
#   unwanted       fake_core (no game_package) given --package: refused, exit 2
#   title          fake_pkg_core whose sidecar carries [title]: refused, exit 2
#
# EMULATOR is CMAKE_CROSSCOMPILING_EMULATOR (wine, for a MinGW build), if any.
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(SHA256 "${PACKAGE}" want_sha)

function(run)
    execute_process(COMMAND ${EMULATOR} ${ARGN}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    message("${out}${err}")
    set(rc "${rc}" PARENT_SCOPE)
    set(out "${out}" PARENT_SCOPE)
    set(err "${err}" PARENT_SCOPE)
endfunction()

function(expect_file_has file what)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "${file} was not written")
    endif()
    file(READ "${file}" text)
    string(FIND "${text}" "${what}" at)
    if(at LESS 0)
        message(FATAL_ERROR "${file} lacks '${what}':\n${text}")
    endif()
endfunction()

function(expect_refused pattern)
    if(NOT rc EQUAL 2)
        message(FATAL_ERROR "expected a refusal (exit 2), got exit ${rc}")
    endif()
    if(NOT "${out}${err}" MATCHES "${pattern}")
        message(FATAL_ERROR "the refusal does not say '${pattern}'")
    endif()
endfunction()

if(CASE STREQUAL "headless")
    run("${RUNNER}" --core "${PKG_CORE}" --package "${PACKAGE}" --rom "${ROM}"
        --frames 10 --out "${OUT}")
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "the runner exited ${rc}")
    endif()
    string(FIND "${out}" "package: ${PACKAGE} sha256 ${want_sha}" at)
    if(at LESS 0)
        message(FATAL_ERROR "the runner did not print the package hash ${want_sha}")
    endif()
    if(NOT out MATCHES "10 frame\\(s\\) submitted[^\n]*frame rate 50/1")
        message(FATAL_ERROR "expected 10 frames at the stated 50/1")
    endif()
    expect_file_has("${OUT}/core.log" "FAKE_PACKAGE ok ${PACKAGE}")
elseif(CASE STREQUAL "link")
    run("${LINK_TEST}" --runner "${RUNNER}" --core "${PKG_CORE}" --package "${PACKAGE}"
        --rom "${ROM}" --frames 10 --out "${OUT}")
    if(NOT rc EQUAL 0 OR NOT out MATCHES "10 frame\\(s\\) granted and done, runner exit 0")
        message(FATAL_ERROR "the link session did not run 10 frames cleanly (exit ${rc})")
    endif()
    expect_file_has("${OUT}/runner.log" "package: ${PACKAGE} sha256 ${want_sha}")
    expect_file_has("${OUT}/core.log" "FAKE_PACKAGE ok ${PACKAGE}")
elseif(CASE STREQUAL "missing")
    run("${RUNNER}" --core "${PKG_CORE}" --rom "${ROM}" --frames 1 --out "${OUT}")
    expect_refused("declares game_package: --package <library>[^\n]* is required")
elseif(CASE STREQUAL "link_missing")
    run("${LINK_TEST}" --runner "${RUNNER}" --core "${PKG_CORE}" --rom "${ROM}"
        --frames 1 --out "${OUT}")
    expect_refused("the runner ended before it was ready \\(exit 2")
    expect_file_has("${OUT}/runner.log" "declares game_package: --package <library>")
elseif(CASE STREQUAL "unwanted")
    run("${RUNNER}" --core "${PLAIN_CORE}" --package "${PACKAGE}" --rom "${ROM}"
        --frames 1 --out "${OUT}")
    expect_refused("does not declare game_package, so it takes no package")
elseif(CASE STREQUAL "title")
    # A copy of fake_pkg_core whose sidecar claims a title.
    get_filename_component(lib "${PKG_CORE}" NAME)
    get_filename_component(stem "${PKG_CORE}" NAME_WE)
    get_filename_component(dir "${PKG_CORE}" DIRECTORY)
    file(COPY "${PKG_CORE}" DESTINATION "${OUT}")
    file(READ "${dir}/${stem}.rcore.toml" sidecar)
    string(REPLACE "[build]" "[title]\nid = \"fake\"\n\n[build]" sidecar "${sidecar}")
    file(WRITE "${OUT}/${stem}.rcore.toml" "${sidecar}")
    run("${RUNNER}" --core "${OUT}/${lib}" --package "${PACKAGE}" --rom "${ROM}"
        --frames 1 --out "${OUT}")
    expect_refused("\\[title\\]: present, but the library declares game_package")
else()
    message(FATAL_ERROR "unknown CASE '${CASE}'")
endif()
