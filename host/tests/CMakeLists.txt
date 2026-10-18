# Validator tests: good fixtures (RatMode's own test pipes) must pass,
# deliberately bad ones must fail the expected check, and a pipe that
# SIGSEGVs must be reported as a failure, not take the host down.

set(RP_FIX "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(RP_FIX_INC "${RATPIPE_SDK_INCLUDE_DIR}/ratpipe")

# Good fixtures -> real bundles (also exercises add_ratpipe, manifest + plist generation).
add_ratpipe(fixture_test_tone ID dev.ratmode.test_tone SOURCES "${RP_FIX}/test_tone.c"
            INCLUDE_DIRECTORIES "${RP_FIX_INC}" LINK_LIBRARIES m)
add_ratpipe(fixture_test_drum ID dev.ratmode.test_drum SOURCES "${RP_FIX}/test_drum.c"
            INCLUDE_DIRECTORIES "${RP_FIX_INC}" LINK_LIBRARIES m)

# The crash fixture is a raw library (it SEGVs by design; no bundle needed).
add_library(fixture_crash SHARED "${RP_FIX}/test_crash.c")
target_include_directories(fixture_crash PRIVATE "${RP_FIX_INC}")
set_target_properties(fixture_crash PROPERTIES PREFIX "" OUTPUT_NAME "crash" C_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bad")

# Bad fixtures: one raw library per defect.
set(RP_BAD_NAMES
    NAN ALLOC IDS STRUCT DESC_SIZE PEAK LOCK IO STATE DRONE ROUNDTRIP BOUNDS OVERWRITE OOB EXPORT DC NONDET SLOW)
foreach (bad IN LISTS RP_BAD_NAMES)
    string(TOLOWER "${bad}" lc)
    set(tgt fixture_bad_${lc})
    add_library(${tgt} SHARED "${RP_FIX}/bad/bad_pipe.c")
    target_include_directories(${tgt} PRIVATE "${RATPIPE_SDK_INCLUDE_DIR}")
    target_compile_definitions(${tgt} PRIVATE BAD_${bad}=1)
    target_compile_options(${tgt} PRIVATE -O1 -fno-fast-math -fvisibility=hidden)
    target_link_libraries(${tgt} PRIVATE m Threads::Threads)
    if (NOT APPLE AND NOT bad STREQUAL "EXPORT")
        target_link_options(${tgt} PRIVATE "-Wl,--version-script=${RATPIPE_SDK_ROOT}/cmake/ratpipe_exports.map")
    endif()
    set_target_properties(${tgt} PROPERTIES PREFIX "" OUTPUT_NAME "bad_${lc}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bad")
    list(APPEND RP_BAD_TARGETS ${tgt})
endforeach()
# A well-behaved variant of the same source (control).
add_library(fixture_bad_control SHARED "${RP_FIX}/bad/bad_pipe.c")
target_include_directories(fixture_bad_control PRIVATE "${RATPIPE_SDK_INCLUDE_DIR}")
target_compile_options(fixture_bad_control PRIVATE -O1 -fno-fast-math -fvisibility=hidden)
target_link_libraries(fixture_bad_control PRIVATE m Threads::Threads)
if (NOT APPLE)
    target_link_options(fixture_bad_control PRIVATE "-Wl,--version-script=${RATPIPE_SDK_ROOT}/cmake/ratpipe_exports.map")
endif()
set_target_properties(fixture_bad_control PROPERTIES PREFIX "" OUTPUT_NAME "control"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bad")

find_package(Python3 COMPONENTS Interpreter)
if (Python3_FOUND)
    add_test(NAME validator.fixtures
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_validator.py"
                     "$<TARGET_FILE:ratpipe-host>" "${CMAKE_CURRENT_BINARY_DIR}/bad")
endif()
