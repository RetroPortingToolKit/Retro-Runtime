# link_runner_crash_keeps_save: the fake core kills its runner at frame 5
# (FAKE_CORE_CRASH_AT). The hub side must see the runner end, and the save it
# writes must be exactly what frames 1-4 left in the battery region.
# EMULATOR is CMAKE_CROSSCOMPILING_EMULATOR (wine, for a MinGW build), if any.
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
execute_process(
    COMMAND ${EMULATOR} "${LINK_TEST}" --runner "${RUNNER}" --core "${CORE}" --rom "${ROM}"
            --frames 10 --out "${OUT}" --save "battery=${OUT}/battery.sav"
            --env FAKE_CORE_CRASH_AT=5
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")
if(rc EQUAL 0)
    message(FATAL_ERROR "the link test passed, but the runner was meant to die at frame 5")
endif()
if(NOT out MATCHES "4 frame\\(s\\) granted and done")
    message(FATAL_ERROR "expected the session to end after frame 4")
endif()
if(NOT EXISTS "${OUT}/battery.sav")
    message(FATAL_ERROR "no save was written after the crash")
endif()
file(READ "${OUT}/battery.sav" got HEX)
# Erase value 0xFF everywhere, then frame k wrote k at offset k.
set(want "ff01020304")
foreach(i RANGE 5 63)
    string(APPEND want "ff")
endforeach()
if(NOT got STREQUAL want)
    message(FATAL_ERROR "the save after the crash is wrong:\n  got  ${got}\n  want ${want}")
endif()
message("the save survived the crash, byte for byte")
