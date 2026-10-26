# Deep Drift — a RatMode pipe.
#
# This folder lives under pipes/ in the SDK checkout; the top-level CMakeLists
# picks it up automatically:
#
#     tools/ratpipe build          # or: cmake -S . -B build && cmake --build build
#     tools/ratpipe validate DeepDrift
#
# add_ratpipe() produces build/DeepDrift.ratpipe (universal on macOS, ad-hoc signed).
# `ID` must equal the id in src/Pipe.cpp — the build stops if they differ.
add_ratpipe(DeepDrift
    ID       com.alecwhiting.deepdrift
    SOURCES  src/Pipe.cpp)
