# FuKit_BD — a RatMode pipe.
#
# This folder lives under pipes/ in the SDK checkout; the top-level CMakeLists
# picks it up automatically:
#
#     tools/ratpipe build          # or: cmake -S . -B build && cmake --build build
#     tools/ratpipe validate FuKit_BD
#
# add_ratpipe() produces build/FuKit_BD.ratpipe (universal on macOS, ad-hoc signed).
# `ID` must equal the id in src/Pipe.cpp — the build stops if they differ.
add_ratpipe(FuKit_BD
    ID       com.alecwhiting.fukit_bd
    SOURCES  src/Pipe.cpp)
