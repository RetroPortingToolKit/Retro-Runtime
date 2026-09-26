# Retro-Runtime: the rcore contract, the host <-> runner link, and
# retro-core-runner (docs/HOST_LIFECYCLE.md, docs/CORE_ABI.md, docs/CORE_LINK.md,
# docs/CORE_RUNNER.md).
#
# Built standalone (CI, releases), or pulled into a host with add_subdirectory():
#
#   retro_rcore          INTERFACE  include/rcore/rcore.h, the contract
#   retro_core_support   STATIC     a core's sidecar manifest reader and loader
#                                   (hosts read sidecars; the runner loads cores)
#   retro_corelink       STATIC     the host side of the link
#   retro-core-runner    exe        the child process that runs a core
#   retro-core-link-test exe        drives a core through the link, as a host does
#   rcore_fake_core      shared     the smallest core, for tests (never installed)
#
# Linux, macOS and Windows. The link's transport differs per OS
# (docs/LINK_TRANSPORTS.md); the protocol above it does not.
cmake_minimum_required(VERSION 3.20)
project(RetroRuntime VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)

option(RETRO_RUNTIME_TOOLS "Build retro-core-runner, the link test and the fake core" ON)
# A host that already resolved SDL3 passes its target here; otherwise this
# looks for SDL3 itself. SDL3 is only used to lend a core a GL context.
set(RETRO_RUNTIME_SDL3_TARGET "" CACHE STRING "SDL3 target to link the runner against")

# The version is compiled in (retro-core-runner --version), so a copy that has
# left its archive still says what it is. Releases pass the tag's version;
# anything else is "dev", never a stale number.
set(RETRO_RUNTIME_VERSION "dev" CACHE STRING "Version compiled into retro-core-runner")
set(RETRO_RUNTIME_COMMIT "" CACHE STRING "Commit compiled into retro-core-runner (default: git HEAD)")
set(_retro_commit "${RETRO_RUNTIME_COMMIT}")
if(NOT _retro_commit)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                        OUTPUT_VARIABLE _retro_commit OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET)
    endif()
endif()
if(NOT _retro_commit)
    set(_retro_commit "unknown")
endif()
set(RETRO_RUNTIME_COMMIT_RESOLVED "${_retro_commit}")
configure_file(runner/runtime_version.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/runtime_version.h @ONLY)

add_library(retro_rcore INTERFACE)
target_include_directories(retro_rcore INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(retro_core_support STATIC
    runner/core_manifest.cpp
    runner/core_library.cpp
    runner/sha256.cpp
)
target_include_directories(retro_core_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/runner)
target_link_libraries(retro_core_support PUBLIC retro_rcore ${CMAKE_DL_LIBS})
set_target_properties(retro_core_support PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The link's transport is per OS (docs/LINK_TRANSPORTS.md); everything above
# it is one implementation.
if(WIN32)
    set(_retro_transport corelink/transport_win32.cpp)
else()
    set(_retro_transport corelink/transport_posix.cpp)
endif()
add_library(retro_corelink STATIC corelink/core_link.cpp ${_retro_transport})
target_include_directories(retro_corelink PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/corelink)
target_link_libraries(retro_corelink PUBLIC retro_rcore)
if(WIN32)
    target_link_libraries(retro_corelink PUBLIC advapi32 bcrypt shell32)
endif()

if(RETRO_RUNTIME_TOOLS)
    set(_sdl "${RETRO_RUNTIME_SDL3_TARGET}")
    if(NOT _sdl)
        find_package(SDL3 QUIET)
        if(TARGET SDL3::SDL3)
            set(_sdl SDL3::SDL3)
        else()
            find_package(PkgConfig QUIET)
            if(PkgConfig_FOUND)
                pkg_check_modules(RETRO_SDL3 IMPORTED_TARGET sdl3)
                if(TARGET PkgConfig::RETRO_SDL3)
                    set(_sdl PkgConfig::RETRO_SDL3)
                endif()
            endif()
        endif()
    endif()

    add_executable(retro-core-runner
        runner/runner_main.cpp
        runner/runner_link.cpp
        runner/host_session.cpp
    )
    target_link_libraries(retro-core-runner PRIVATE retro_core_support retro_corelink)
    target_include_directories(retro-core-runner PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    if(_sdl)
        target_link_libraries(retro-core-runner PRIVATE ${_sdl})
        target_compile_definitions(retro-core-runner PRIVATE RETRO_RUNNER_HAVE_SDL3=1)
    else()
        message(STATUS "retro-core-runner: no SDL3, so --gl (lending a GL context) is unavailable")
    endif()
    install(TARGETS retro-core-runner RUNTIME DESTINATION bin)

    add_executable(retro-core-link-test corelink/link_test_main.cpp)
    target_link_libraries(retro-core-link-test PRIVATE retro_corelink)

    # The smallest core: silent, states 50/1 after load(), one 64x48 frame
    # per run_frame. Its sidecar is written beside it.
    add_library(rcore_fake_core SHARED tests/rcore_fake_core.c)
    target_link_libraries(rcore_fake_core PRIVATE retro_rcore)
    set_target_properties(rcore_fake_core PROPERTIES PREFIX "" OUTPUT_NAME fake_core
                          C_VISIBILITY_PRESET hidden)
    file(GENERATE OUTPUT $<TARGET_FILE_DIR:rcore_fake_core>/fake_core.rcore.toml CONTENT
"# Written by CMake for the rcore_fake_core test fixture.\n\n[core]\nabi_major       = 0\ndraft_revision  = 5\nid              = \"fake\"\nversion         = \"1.0\"\nlibrary         = \"$<TARGET_FILE_NAME:rcore_fake_core>\"\nplatforms       = [\"test\"]\ncapabilities    = [\"run_frame\"]\n\n[build]\nengine_commit   = \"\"\nengine_dirty    = false\ntoolchain       = \"\"\ngenerated_utc   = \"\"\n")

    enable_testing()
    # Headless: the runner loads the fake core, checks its sidecar, runs 10
    # frames and reports the stated rate.
    add_test(NAME runner_headless_fake_core
        COMMAND retro-core-runner --core $<TARGET_FILE:rcore_fake_core>
                --rom ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE --frames 10
                --out ${CMAKE_CURRENT_BINARY_DIR}/test-headless)
    set_tests_properties(runner_headless_fake_core PROPERTIES
        PASS_REGULAR_EXPRESSION "frame rate 50/1")
    # --version: what release packaging and hosts read back from the binary.
    add_test(NAME runner_version COMMAND retro-core-runner --version)
    set_tests_properties(runner_version PROPERTIES PASS_REGULAR_EXPRESSION
        "version ${RETRO_RUNTIME_VERSION}\ncommit [^\n]+\nlink_protocol [0-9]+\\.[0-9]+\n")
    # Link: the same, driven through retro_corelink the way a host does.
    add_test(NAME link_fake_core
        COMMAND retro-core-link-test --runner $<TARGET_FILE:retro-core-runner>
                --core $<TARGET_FILE:rcore_fake_core>
                --rom ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE --frames 10
                --out ${CMAKE_CURRENT_BINARY_DIR}/test-link)
    set_tests_properties(link_fake_core PROPERTIES
        PASS_REGULAR_EXPRESSION "10 frame\\(s\\) granted and done, runner exit 0")
    # A crash mid-session: the runner dies at frame 5 with no unload. The hub
    # must see it end and still write the save as frames 1-4 left it -- the
    # save memory is the hub's (docs/CORE_LINK.md).
    add_test(NAME link_runner_crash_keeps_save
        COMMAND ${CMAKE_COMMAND}
                "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                -DLINK_TEST=$<TARGET_FILE:retro-core-link-test>
                -DRUNNER=$<TARGET_FILE:retro-core-runner>
                -DCORE=$<TARGET_FILE:rcore_fake_core>
                -DROM=${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/test-crash
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/link_crash_test.cmake)
endif()
