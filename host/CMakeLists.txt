# ratpipe-host — offline host + validator (pure C++17, macOS + Linux, no JUCE).
#
# Linux: the real-time guard (V5/V6) is symbol interposition compiled straight
#        into the executable (exported with -rdynamic so dlopen'ed pipes bind to it).
# macOS: the guard is a DYLD_INTERPOSE dylib (libratpipe_rtguard.dylib, next to
#        the executable); `ratpipe-host validate` re-execs itself with it inserted.

find_package(Threads REQUIRED)

set(RATPIPE_HOST_SOURCES
    src/main.cpp src/validator.cpp src/engine.cpp src/plugin.cpp src/patterns.cpp
    src/manifest.cpp src/exports.cpp src/rtguard_host.cpp)

if (APPLE)
    add_executable(ratpipe-host ${RATPIPE_HOST_SOURCES})
    add_library(ratpipe_rtguard SHARED src/rtguard_api.cpp src/rtguard_mac.cpp)
    set_target_properties(ratpipe_rtguard PROPERTIES
        OUTPUT_NAME ratpipe_rtguard
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}")
    add_dependencies(ratpipe-host ratpipe_rtguard)
else()
    add_executable(ratpipe-host ${RATPIPE_HOST_SOURCES} src/rtguard_api.cpp src/rtguard_linux.cpp)
    set_target_properties(ratpipe-host PROPERTIES ENABLE_EXPORTS ON)
endif()

target_include_directories(ratpipe-host PRIVATE "${RATPIPE_SDK_INCLUDE_DIR}")
target_compile_definitions(ratpipe-host PRIVATE RATPIPE_HOST=1)
target_compile_options(ratpipe-host PRIVATE -Wall -Wextra -fno-fast-math)
target_link_libraries(ratpipe-host PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(ratpipe-host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}")

set(RATPIPE_HOST_EXECUTABLE "$<TARGET_FILE:ratpipe-host>" CACHE INTERNAL "")

if (RATPIPE_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
