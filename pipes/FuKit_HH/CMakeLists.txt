# FuKit_HH — a RatMode pipe.
#
# This folder lives under pipes/ in the SDK checkout; the top-level CMakeLists
# picks it up automatically:
#
#     tools/ratpipe build          # or: cmake -S . -B build && cmake --build build
#     tools/ratpipe validate FuKit_HH
#
# add_ratpipe() produces build/FuKit_HH.ratpipe (universal on macOS, ad-hoc signed).
# `ID` must equal the id in src/Pipe.cpp — the build stops if they differ.
add_ratpipe(FuKit_HH
    ID       com.alecwhiting.fukit_hh
    SOURCES  src/Pipe.cpp)
