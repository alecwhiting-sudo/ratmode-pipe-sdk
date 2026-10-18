# ratmode-pipe-sdk — top level.
#
#   cmake -S . -B build && cmake --build build
#
# Builds the offline host/validator (ratpipe-host), every pipe under
# examples/ and pipes/ (pipes/ is where `tools/ratpipe new` puts yours),
# and, if present, the kit unit tests.

cmake_minimum_required(VERSION 3.22)

# Must be decided before project(): RatMode is universal and targets macOS 11
# (Arch38 §7.2), so a bundle that loads in RatMode needs both slices.
if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures (RatMode is universal)")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS (matches RatMode)")
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RATPIPE_SDK_VERSION_STRING LIMIT_COUNT 1)
project(ratmode_pipe_sdk VERSION ${RATPIPE_SDK_VERSION_STRING} LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(RATPIPE_BUILD_HOST       "Build the offline host / validator (ratpipe-host)" ON)
option(RATPIPE_BUILD_EXAMPLES   "Build examples/*"                                  ON)
option(RATPIPE_BUILD_PIPES      "Build pipes/* (created by `ratpipe new`)"           ON)
option(RATPIPE_BUILD_TESTS      "Build the SDK's own tests (host fixtures, kit)"     ON)

set(RATPIPE_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")

if (RATPIPE_BUILD_TESTS)
    enable_testing()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RatPipe.cmake" OPTIONAL)

if (RATPIPE_BUILD_HOST AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/host/CMakeLists.txt")
    add_subdirectory(host)
endif()

if (RATPIPE_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/kit/CMakeLists.txt")
    add_subdirectory(tests/kit)
endif()

if (RATPIPE_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()

if (RATPIPE_BUILD_PIPES)
    file(GLOB _ratpipe_user_pipes LIST_DIRECTORIES false
         "${CMAKE_CURRENT_SOURCE_DIR}/pipes/*/CMakeLists.txt")
    foreach (_p IN LISTS _ratpipe_user_pipes)
        get_filename_component(_d "${_p}" DIRECTORY)
        get_filename_component(_n "${_d}" NAME)
        add_subdirectory("${_d}" "pipes/${_n}")
    endforeach()
endif()
