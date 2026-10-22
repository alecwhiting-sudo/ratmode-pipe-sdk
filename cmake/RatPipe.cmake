# cmake/RatPipe.cmake — build a RatMode pipe as a `.ratpipe` bundle.
#
#   include(path/to/cmake/RatPipe.cmake)
#
#   add_ratpipe(MyBass
#       ID       com.me.mybass            # reverse-DNS; MUST equal the descriptor id
#       SOURCES  src/Pipe.cpp
#       [INCLUDE_DIRECTORIES dir...]      # the SDK include/ dir is always added
#       [COMPILE_DEFINITIONS X=1 ...]
#       [COMPILE_OPTIONS     -Wfoo ...]
#       [LINK_LIBRARIES      lib ...]     # libm-style system libs only; see SIGNING.md
#       [MIN_RATMODE_VERSION 0.1.8])
#
# Produces   <RATPIPE_BUNDLE_OUTPUT_DIR>/MyBass.ratpipe/
#              Contents/Info.plist                 generated from the descriptor
#              Contents/MacOS/com.me.mybass.dylib  MH_DYLIB, universal on macOS (".so" on Linux)
#              Contents/Resources/manifest.json    generated from the descriptor
# and, on macOS, ad-hoc signs it (or signs with RATPIPE_CODESIGN_IDENTITY).
#
# Why the flags look the way they do (RatMode's Source/Ext/ExtMachOCheck.cpp
# refuses the bundle at load otherwise — this mirrors the in-repo test pipes
# that are known to load):
#   * add_library(SHARED), never MODULE      -> MH_DYLIB; MH_BUNDLE is refused
#   * no rpath of any kind                   -> LC_RPATH is refused
#   * -fvisibility=hidden + exported-symbol  -> exactly one export: ratpipe_entry (M12)
#   * -Wl,-dead_strip                        -> unreferenced code cannot import anything
#   * no -ffast-math                         -> would fold std::isfinite to true (M3)
#   * universal x86_64;arm64, macOS 11       -> M14 (set in the top-level CMakeLists)
#
# Variables you may set BEFORE calling add_ratpipe:
#   RATPIPE_HOST_EXECUTABLE   path to ratpipe-host (default: the in-tree target)
#   RATPIPE_BUNDLE_OUTPUT_DIR where bundles are assembled (default: ${CMAKE_BINARY_DIR})
#   RATPIPE_CODESIGN_IDENTITY Developer ID / identity for codesign (default "-" = ad-hoc)
#   RATPIPE_INSTALL_DIR       target of install_<name> (default: RatMode's per-user Extensions folder)
#   RATPIPE_MIN_RATMODE_VERSION  default for MIN_RATMODE_VERSION (default 0.1.8)
#   RATPIPE_ADD_TESTS         add a ctest entry that runs the validator (default ON)

if (DEFINED _RATPIPE_CMAKE_INCLUDED)
    return()
endif()
set(_RATPIPE_CMAKE_INCLUDED TRUE)

get_filename_component(_RATPIPE_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(RATPIPE_SDK_INCLUDE_DIR "${_RATPIPE_SDK_ROOT}/include" CACHE INTERNAL "")
set(_RATPIPE_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# The SDK version (VERSION at the repo root) is stamped into every pipe's
# descriptor as sdk_version via RATPIPE_SDK_VERSION_STRING (pipe_base.hpp
# falls back to its own constant when the definition is absent).
if (EXISTS "${_RATPIPE_SDK_ROOT}/VERSION")
    file(STRINGS "${_RATPIPE_SDK_ROOT}/VERSION" _RATPIPE_SDK_VERSION LIMIT_COUNT 1)
    string(STRIP "${_RATPIPE_SDK_VERSION}" _RATPIPE_SDK_VERSION)
endif()
if (NOT _RATPIPE_SDK_VERSION)
    set(_RATPIPE_SDK_VERSION "0.0.0")
endif()
set(RATPIPE_SDK_VERSION "${_RATPIPE_SDK_VERSION}" CACHE INTERNAL "")

if (NOT DEFINED RATPIPE_BUNDLE_OUTPUT_DIR)
    set(RATPIPE_BUNDLE_OUTPUT_DIR "${CMAKE_BINARY_DIR}")
endif()
if (NOT DEFINED RATPIPE_CODESIGN_IDENTITY OR "${RATPIPE_CODESIGN_IDENTITY}" STREQUAL "")
    set(RATPIPE_CODESIGN_IDENTITY "-")
endif()
if (NOT DEFINED RATPIPE_INSTALL_DIR)
    set(RATPIPE_INSTALL_DIR "$ENV{HOME}/Library/Application Support/RatMode/Extensions")
endif()
if (NOT DEFINED RATPIPE_MIN_RATMODE_VERSION)
    set(RATPIPE_MIN_RATMODE_VERSION "0.1.8")
endif()
if (NOT DEFINED RATPIPE_ADD_TESTS)
    set(RATPIPE_ADD_TESTS ON)
endif()

function(add_ratpipe target)
    cmake_parse_arguments(RP "" "ID;MIN_RATMODE_VERSION"
        "SOURCES;INCLUDE_DIRECTORIES;COMPILE_DEFINITIONS;COMPILE_OPTIONS;LINK_LIBRARIES" ${ARGN})

    if (WIN32)
        message(FATAL_ERROR "add_ratpipe: Windows bundles are not supported yet (platform-neutral ABI; Windows later).")
    endif()
    if (NOT RP_ID)
        message(FATAL_ERROR "add_ratpipe(${target}): ID <reverse.dns.id> is required")
    endif()
    if (NOT RP_ID MATCHES "^[a-z0-9]+(\\.[a-z0-9_]+)+$")
        message(FATAL_ERROR "add_ratpipe(${target}): ID '${RP_ID}' must match ^[a-z0-9]+(\\.[a-z0-9_]+)+$ (lowercase reverse-DNS)")
    endif()
    if (NOT RP_SOURCES)
        message(FATAL_ERROR "add_ratpipe(${target}): SOURCES is required")
    endif()
    if (NOT RP_MIN_RATMODE_VERSION)
        set(RP_MIN_RATMODE_VERSION "${RATPIPE_MIN_RATMODE_VERSION}")
    endif()

    # -ffast-math lets the compiler fold isfinite() to true: the exact trap the
    # finite guards exist for. Refuse it anywhere we can see it.
    foreach (_f IN ITEMS "${CMAKE_C_FLAGS}" "${CMAKE_CXX_FLAGS}" "${RP_COMPILE_OPTIONS}")
        if (_f MATCHES "-ffast-math|-Ofast|-ffinite-math-only")
            message(FATAL_ERROR "add_ratpipe(${target}): -ffast-math / -Ofast / -ffinite-math-only are not allowed (they defeat the finite guards)")
        endif()
    endforeach()

    # Locate the host that writes manifest + Info.plist from the descriptor.
    if (TARGET ratpipe-host)
        set(_host "$<TARGET_FILE:ratpipe-host>")
        set(_host_dep ratpipe-host)
    elseif (RATPIPE_HOST_EXECUTABLE)
        set(_host "${RATPIPE_HOST_EXECUTABLE}")
        set(_host_dep "")
    else()
        message(FATAL_ERROR "add_ratpipe(${target}): no ratpipe-host. Build the SDK host first "
                            "(it is a target of this repo) or set RATPIPE_HOST_EXECUTABLE.")
    endif()

    if (APPLE)
        set(_suffix ".dylib")
    else()
        set(_suffix ".so")
    endif()

    set(_bundle "${RATPIPE_BUNDLE_OUTPUT_DIR}/${target}.ratpipe")
    set(_macos  "${_bundle}/Contents/MacOS")
    set(_binary "${_macos}/${RP_ID}${_suffix}")

    add_library(${target} SHARED ${RP_SOURCES})
    target_include_directories(${target} PRIVATE "${RATPIPE_SDK_INCLUDE_DIR}" ${RP_INCLUDE_DIRECTORIES})
    target_compile_definitions(${target} PRIVATE
        "RATPIPE_SDK_VERSION_STRING=\"${_RATPIPE_SDK_VERSION}\""
        ${RP_COMPILE_DEFINITIONS})
    target_compile_options(${target} PRIVATE
        -fno-fast-math -fvisibility=hidden
        $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>
        -Wall -Wextra
        ${RP_COMPILE_OPTIONS})
    if (RP_LINK_LIBRARIES)
        target_link_libraries(${target} PRIVATE ${RP_LINK_LIBRARIES})
    endif()

    if (APPLE)
        target_link_options(${target} PRIVATE
            -Wl,-dead_strip
            -Wl,-exported_symbol,_ratpipe_entry
            # Explicit install name: never leak the absolute build path into the binary.
            "-Wl,-install_name,@loader_path/${RP_ID}${_suffix}")
    else()
        target_link_options(${target} PRIVATE
            -Wl,--gc-sections
            -Wl,--exclude-libs,ALL
            "-Wl,--version-script=${_RATPIPE_CMAKE_DIR}/ratpipe_exports.map")
        target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
    endif()

    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME                 "${RP_ID}"
        PREFIX                      ""
        SUFFIX                      "${_suffix}"
        POSITION_INDEPENDENT_CODE   ON
        C_VISIBILITY_PRESET         hidden
        CXX_VISIBILITY_PRESET       hidden
        VISIBILITY_INLINES_HIDDEN   ON
        # No LC_RPATH, ever (ExtMachOCheck refuses it):
        MACOSX_RPATH                OFF
        SKIP_BUILD_RPATH            ON
        BUILD_RPATH                 ""
        INSTALL_RPATH               ""
        BUILD_WITH_INSTALL_RPATH    ON
        INSTALL_NAME_DIR            "@loader_path"
        BUILD_WITH_INSTALL_NAME_DIR ON
        LIBRARY_OUTPUT_DIRECTORY    "${_macos}"
        # Multi-config generators (Xcode) would otherwise append Debug/Release:
        LIBRARY_OUTPUT_DIRECTORY_DEBUG          "${_macos}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE        "${_macos}"
        LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${_macos}"
        LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL     "${_macos}")

    # Contents/Info.plist and manifest.json both come from the built
    # descriptor, so the three can never disagree (V8 / V12). The host exits
    # non-zero — failing the build — if the descriptor id is not RP_ID.
    set(_post
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bundle}/Contents/Resources"
        COMMAND "${_host}" --dump-manifest "$<TARGET_FILE:${target}>"
                --expect-id "${RP_ID}" --min-ratmode-version "${RP_MIN_RATMODE_VERSION}"
                --out "${_bundle}/Contents/Resources/manifest.json"
        COMMAND "${_host}" --dump-plist "$<TARGET_FILE:${target}>"
                --expect-id "${RP_ID}"
                --out "${_bundle}/Contents/Info.plist")
    if (APPLE)
        list(APPEND _post
            COMMAND xattr -cr "${_bundle}"
            COMMAND codesign --force --sign "${RATPIPE_CODESIGN_IDENTITY}" "${_bundle}"
            COMMAND lipo -archs "$<TARGET_FILE:${target}>")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD ${_post}
        COMMENT "Assembling ${target}.ratpipe"
        VERBATIM)
    if (_host_dep)
        add_dependencies(${target} ${_host_dep})
    endif()

    set_target_properties(${target} PROPERTIES RATPIPE_BUNDLE_DIR "${_bundle}" RATPIPE_ID "${RP_ID}")
    set_property(GLOBAL APPEND PROPERTY RATPIPE_BUNDLES "${_bundle}")

    # install_<target> / uninstall_<target>: copy into RatMode's Extensions
    # folder (Developer mode only — see docs/SIGNING.md).
    get_filename_component(_bundle_name "${_bundle}" NAME)
    add_custom_target(install_${target}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${RATPIPE_INSTALL_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${RATPIPE_INSTALL_DIR}/${_bundle_name}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_bundle}" "${RATPIPE_INSTALL_DIR}/${_bundle_name}"
        COMMAND "${CMAKE_COMMAND}" -E echo "Installed ${_bundle_name} into: ${RATPIPE_INSTALL_DIR}"
        DEPENDS ${target} VERBATIM)
    add_custom_target(uninstall_${target}
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${RATPIPE_INSTALL_DIR}/${_bundle_name}"
        COMMAND "${CMAKE_COMMAND}" -E echo "Removed ${_bundle_name} from: ${RATPIPE_INSTALL_DIR}"
        VERBATIM)

    if (RATPIPE_ADD_TESTS AND TARGET ratpipe-host)
        add_test(NAME validate.${target}
                 COMMAND ratpipe-host validate "${_bundle}" --quick)
    endif()
endfunction()
