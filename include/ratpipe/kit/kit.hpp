// ratpipe kit — umbrella header. Every block is header-only, JUCE-free and
// clean-room; the only allocation in the whole kit is DelayLine::prepare().
#pragma once
#include "finite_guard.hpp"
#include "denormal.hpp"
#include "rng.hpp"
#include "noise.hpp"
#include "smoother.hpp"
#include "dc_block.hpp"
#include "pitch.hpp"
#include "osc_polyblep.hpp"
#include "sat_tanh.hpp"
#include "svf.hpp"
#include "ladder_lite.hpp"
#include "adsr.hpp"
#include "lfo.hpp"
#include "delay_line.hpp"
