# FM Kick — a drum pipe: FM kick with pitch envelope, click and tanh drive.
#
#     tools/ratpipe build
#     tools/ratpipe validate drum_fm_kick
#     tools/ratpipe render   drum_fm_kick --pattern drum16 --out out/kick.wav
#
# `ID` must equal the id in src/Pipe.cpp — the build stops if they differ.
add_ratpipe(drum_fm_kick
    ID       dev.ratmode.examples.fm_kick
    SOURCES  src/Pipe.cpp)
