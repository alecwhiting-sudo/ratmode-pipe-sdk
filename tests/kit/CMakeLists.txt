# Kit + pipe_base unit tests. Meant to be pulled in from the top-level
# CMakeLists.txt with `add_subdirectory(tests/kit)` after `enable_testing()`;
# tests/kit/build.sh builds the same sources without CMake.
cmake_minimum_required(VERSION 3.22)

add_executable(ratpipe_kit_tests
    main.cpp
    test_kit.cpp
    test_pipe_base.cpp
    smoke_pipe.cpp)
target_include_directories(ratpipe_kit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_compile_features(ratpipe_kit_tests PRIVATE cxx_std_17)
target_compile_options(ratpipe_kit_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion>)
option(RATPIPE_KIT_TESTS_SANITIZE "Build the kit tests with ASan + UBSan" ON)
if (RATPIPE_KIT_TESTS_SANITIZE AND NOT MSVC)
    target_compile_options(ratpipe_kit_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(ratpipe_kit_tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME ratpipe_kit_tests COMMAND ratpipe_kit_tests)

# The smoke pipe as a plain shared library (not a .ratpipe bundle): proves the
# base + kit build with the bundle flags and export only ratpipe_entry.
add_library(ratpipe_smoke_pipe SHARED smoke_pipe.cpp)
target_include_directories(ratpipe_smoke_pipe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_compile_features(ratpipe_smoke_pipe PRIVATE cxx_std_17)
set_target_properties(ratpipe_smoke_pipe PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX "")
