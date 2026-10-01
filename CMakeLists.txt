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
#   retro_state          STATIC     the savestate envelope (the runner writes and
#                                   checks it; hosts read its header to list slots)
#   retro_overlay        STATIC     what a host draws over a running core: FPS,
#                                   TURBO, volume, toasts, the save-state browser
#   retro-core-runner    exe        the child process that runs a core
#   retro-core-link-test exe        drives a core through the link, as a host does
#   rcore_fake_core      shared     the smallest core, for tests (never installed)
#   rcore_fake_pkg_core  shared     the same, declaring game_package (tests only)
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
add_library(retro_corelink STATIC corelink/core_link.cpp corelink/runner_probe.cpp
            ${_retro_transport})
target_include_directories(retro_corelink PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/corelink)
target_link_libraries(retro_corelink PUBLIC retro_rcore)
if(WIN32)
    target_link_libraries(retro_corelink PUBLIC advapi32 bcrypt shell32)
endif()

add_library(retro_state STATIC state/state_envelope.cpp)
target_include_directories(retro_state PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/state)
target_link_libraries(retro_state PUBLIC retro_core_support)

# No SDL, no GL, no clock: a host passes the time in and draws the images
# (docs/OVERLAY.md).
add_library(retro_overlay STATIC overlay/overlay_font.cpp overlay/overlay_raster.cpp
            overlay/osd.cpp overlay/savestate_menu.cpp)
target_include_directories(retro_overlay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/overlay)
target_link_libraries(retro_overlay PUBLIC retro_rcore PRIVATE retro_state)

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
        runner/state_keeper.cpp
        runner/net_session.cpp
    )
    # Netplay (runner/net_session.hpp): recomp-net's episode driver and UDP
    # transport, compiled in from a checkout when one is named. Without it the
    # runner answers `netplay 0` and refuses --net-*.
    set(RETRO_RUNTIME_RECOMP_NET_DIR "" CACHE PATH
        "recomp-net checkout: build retro-core-runner with netplay")
    if(RETRO_RUNTIME_RECOMP_NET_DIR)
        set(RNET_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(RNET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(RNET_ENABLE_ICE OFF CACHE BOOL "" FORCE)
        add_subdirectory(${RETRO_RUNTIME_RECOMP_NET_DIR} recomp-net EXCLUDE_FROM_ALL)
        target_link_libraries(retro-core-runner PRIVATE recomp_net)
        target_compile_definitions(retro-core-runner PRIVATE RETRO_RUNNER_NETPLAY=1)
        message(STATUS "retro-core-runner: netplay from ${RETRO_RUNTIME_RECOMP_NET_DIR}")
    endif()
    # Threads: the savestate keeper hashes content off the core thread.
    find_package(Threads REQUIRED)
    target_link_libraries(retro-core-runner PRIVATE retro_core_support retro_corelink retro_state
                          Threads::Threads)
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
    # per run_frame, a 12-byte savestate. Its sidecar is written beside it.
    add_library(rcore_fake_core SHARED tests/rcore_fake_core.c)
    target_link_libraries(rcore_fake_core PRIVATE retro_rcore)
    set_target_properties(rcore_fake_core PROPERTIES PREFIX "" OUTPUT_NAME fake_core
                          C_VISIBILITY_PRESET hidden)
    file(GENERATE OUTPUT $<TARGET_FILE_DIR:rcore_fake_core>/fake_core.rcore.toml CONTENT
"# Written by CMake for the rcore_fake_core test fixture.\n\n[core]\nabi_major       = 0\ndraft_revision  = 7\nid              = \"fake\"\nversion         = \"1.0\"\nlibrary         = \"$<TARGET_FILE_NAME:rcore_fake_core>\"\nplatforms       = [\"test\"]\ncapabilities    = [\"run_frame\", \"savestate\", \"accessory_data\"]\n\n[build]\nengine_commit   = \"\"\nengine_dirty    = false\ntoolchain       = \"\"\ngenerated_utc   = \"\"\n")

    # The same core built as a generic one: GAME_PACKAGE, load() requires the
    # fixture package (tests/fake_package.txt). Its sidecar has no [title]: a
    # generic core's title comes from --package (docs/CORE_ABI.md).
    add_library(rcore_fake_pkg_core SHARED tests/rcore_fake_core.c)
    target_link_libraries(rcore_fake_pkg_core PRIVATE retro_rcore)
    target_compile_definitions(rcore_fake_pkg_core PRIVATE FAKE_GAME_PACKAGE=1)
    set_target_properties(rcore_fake_pkg_core PROPERTIES PREFIX "" OUTPUT_NAME fake_pkg_core
                          C_VISIBILITY_PRESET hidden)
    file(GENERATE OUTPUT $<TARGET_FILE_DIR:rcore_fake_pkg_core>/fake_pkg_core.rcore.toml CONTENT
"# Written by CMake for the rcore_fake_pkg_core test fixture.\n\n[core]\nabi_major       = 0\ndraft_revision  = 5\nid              = \"fake_pkg\"\nversion         = \"1.0\"\nlibrary         = \"$<TARGET_FILE_NAME:rcore_fake_pkg_core>\"\nplatforms       = [\"test\"]\ncapabilities    = [\"run_frame\", \"savestate\", \"game_package\"]\n\n[build]\nengine_commit   = \"\"\nengine_dirty    = false\ntoolchain       = \"\"\ngenerated_utc   = \"\"\n")

    # The plain core plus a capability bit from a newer revision (bit 40),
    # named "future_feature" in its sidecar: a runner warns and runs it.
    add_library(rcore_fake_future_core SHARED tests/rcore_fake_core.c)
    target_link_libraries(rcore_fake_future_core PRIVATE retro_rcore)
    target_compile_definitions(rcore_fake_future_core PRIVATE FAKE_FUTURE_CAP=1)
    set_target_properties(rcore_fake_future_core PROPERTIES PREFIX "" OUTPUT_NAME fake_future_core
                          C_VISIBILITY_PRESET hidden)
    file(GENERATE OUTPUT $<TARGET_FILE_DIR:rcore_fake_future_core>/fake_future_core.rcore.toml CONTENT
"# Written by CMake for the rcore_fake_future_core test fixture.\n\n[core]\nabi_major       = 0\ndraft_revision  = 99\nid              = \"fake_future\"\nversion         = \"1.0\"\nlibrary         = \"$<TARGET_FILE_NAME:rcore_fake_future_core>\"\nplatforms       = [\"test\"]\ncapabilities    = [\"run_frame\", \"savestate\", \"accessory_data\", \"future_feature\"]\n\n[build]\nengine_commit   = \"\"\nengine_dirty    = false\ntoolchain       = \"\"\ngenerated_utc   = \"\"\n")

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
        "version ${RETRO_RUNTIME_VERSION}\ncommit [^\n]+\nlink_protocol [0-9]+\\.[0-9]+\n.*gl [01]\ngame_package 1\ndescribe 1\n")
    # Link: the same, driven through retro_corelink the way a host does.
    add_test(NAME link_fake_core
        COMMAND retro-core-link-test --runner $<TARGET_FILE:retro-core-runner>
                --core $<TARGET_FILE:rcore_fake_core>
                --rom ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE --frames 10
                --out ${CMAKE_CURRENT_BINARY_DIR}/test-link)
    set_tests_properties(link_fake_core PROPERTIES
        PASS_REGULAR_EXPRESSION "10 frame\\(s\\) granted and done, runner exit 0")
    # probe_runner: what a host reads to choose and verify a runner.
    add_test(NAME runner_probe COMMAND retro-core-link-test --probe $<TARGET_FILE:retro-core-runner>)
    set_tests_properties(runner_probe PROPERTIES PASS_REGULAR_EXPRESSION
        "probe: version ${RETRO_RUNTIME_VERSION}, link [0-9]+\\.[0-9]+, rcore ABI [0-9]+, compatible, game_package 1, describe 1, accessory_data 1")
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
    # Savestates: saved and loaded over the link, refused by the load rule,
    # and read by headless --load-state (tests/state_test.cmake).
    add_test(NAME link_savestates
        COMMAND ${CMAKE_COMMAND}
                "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                -DLINK_TEST=$<TARGET_FILE:retro-core-link-test>
                -DRUNNER=$<TARGET_FILE:retro-core-runner>
                -DCORE=$<TARGET_FILE:rcore_fake_core>
                -DROM=${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
                -DOTHER_ROM=${CMAKE_CURRENT_SOURCE_DIR}/README.md
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/test-states
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/state_test.cmake)
    # The overlay and the envelope, without a window or a core.
    add_executable(retro-overlay-test tests/overlay_test.cpp)
    target_link_libraries(retro-overlay-test PRIVATE retro_overlay retro_state)
    add_test(NAME overlay COMMAND retro-overlay-test ${CMAKE_CURRENT_BINARY_DIR}/test-overlay)
    set_tests_properties(overlay PROPERTIES PASS_REGULAR_EXPRESSION "overlay-test: ok")
    # Game packages: fake_pkg_core with the fixture package, headless and over
    # the link; and the refusals (no --package for a package core, --package
    # for a plain one, [title] in a package core's sidecar).
    foreach(_case headless link missing link_missing unwanted title)
        add_test(NAME runner_package_${_case}
            COMMAND ${CMAKE_COMMAND}
                    "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                    -DCASE=${_case}
                    -DLINK_TEST=$<TARGET_FILE:retro-core-link-test>
                    -DRUNNER=$<TARGET_FILE:retro-core-runner>
                    -DPKG_CORE=$<TARGET_FILE:rcore_fake_pkg_core>
                    -DPLAIN_CORE=$<TARGET_FILE:rcore_fake_core>
                    -DPACKAGE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_package.txt
                    -DROM=${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
                    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/test-package-${_case}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_test.cmake)
    endforeach()
    # --describe: the fake cores' declarations as TAB records, without --rom;
    # and the refusals (--package for a plain core, a core that is not there).
    # Accessory data (rev 7, link 2.1): the VRU on fake_core, headless from a
    # script and over the link, with the frame each message lands in; and the
    # refusals (a core without the type, a seat the runner does not have).
    foreach(_case headless link replay refused_type refused_seat script_unbound)
        add_test(NAME runner_accessory_${_case}
            COMMAND ${CMAKE_COMMAND}
                    "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                    -DCASE=${_case}
                    -DLINK_TEST=$<TARGET_FILE:retro-core-link-test>
                    -DRUNNER=$<TARGET_FILE:retro-core-runner>
                    -DPKG_CORE=$<TARGET_FILE:rcore_fake_pkg_core>
                    -DPLAIN_CORE=$<TARGET_FILE:rcore_fake_core>
                    -DPACKAGE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_package.txt
                    -DROM=${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
                    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/test-accessory-${_case}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/accessory_test.cmake)
    endforeach()
    # A capability this runner does not know is warned about and ignored
    # (CORE_ABI.md): fake_future_core loads, runs, links and describes; a
    # KNOWN capability on one side only is still refused.
    foreach(_case future_headless future_describe future_link missing_known extra_known)
        add_test(NAME runner_manifest_${_case}
            COMMAND ${CMAKE_COMMAND}
                    "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                    -DCASE=${_case}
                    -DLINK_TEST=$<TARGET_FILE:retro-core-link-test>
                    -DRUNNER=$<TARGET_FILE:retro-core-runner>
                    -DFUTURE_CORE=$<TARGET_FILE:rcore_fake_future_core>
                    -DROM=${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
                    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/test-manifest-${_case}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/manifest_test.cmake)
    endforeach()
    foreach(_case plain package package_none unwanted no_core)
        add_test(NAME runner_describe_${_case}
            COMMAND ${CMAKE_COMMAND}
                    "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                    -DCASE=${_case}
                    -DRUNNER=$<TARGET_FILE:retro-core-runner>
                    -DPKG_CORE=$<TARGET_FILE:rcore_fake_pkg_core>
                    -DPLAIN_CORE=$<TARGET_FILE:rcore_fake_core>
                    -DPACKAGE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_package.txt
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/describe_test.cmake)
    endforeach()
endif()
