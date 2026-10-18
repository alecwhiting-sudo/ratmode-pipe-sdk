# Worked example pipes. Each folder is a complete pipe the way `ratpipe new`
# would lay one out (CMakeLists.txt, src/Pipe.cpp, README.md); copy one to
# pipes/<Name> to start from it.
#
# add_ratpipe() already registers a quick validation (20 fuzz seeds) per
# bundle; the examples additionally get the full run (200 seeds, with the
# V13 source audit) so `ctest` proves them against the whole contract.
set(_ratpipe_examples mono_fm_wobble drum_fm_kick)

foreach (_ex IN LISTS _ratpipe_examples)
    add_subdirectory(${_ex})
endforeach()

if (RATPIPE_ADD_TESTS AND TARGET ratpipe-host)
    foreach (_ex IN LISTS _ratpipe_examples)
        get_target_property(_bundle ${_ex} RATPIPE_BUNDLE_DIR)
        add_test(NAME validate.full.${_ex}
                 COMMAND ratpipe-host validate "${_bundle}" --src "${CMAKE_CURRENT_SOURCE_DIR}/${_ex}")
    endforeach()
endif()
