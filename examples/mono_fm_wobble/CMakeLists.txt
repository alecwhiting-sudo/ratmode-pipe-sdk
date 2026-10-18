# FM Wobble — the README's "gritty FM bass with a wobble macro".
#
#     tools/ratpipe build
#     tools/ratpipe validate mono_fm_wobble
#     tools/ratpipe render   mono_fm_wobble --pattern acid16 --out out/wobble.wav
#
# `ID` must equal the id in src/Pipe.cpp — the build stops if they differ.
add_ratpipe(mono_fm_wobble
    ID       dev.ratmode.examples.fm_wobble
    SOURCES  src/Pipe.cpp)
