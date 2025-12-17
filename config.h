#pragma once

static constexpr uint8_t NUM_VOICES = 4;

    enum class SynthParam {
        OSC1_PULSE,
        OSC2_DETUNE,
        FILTER_CUTOFF,
        FILTER_RESONANCE,
        BITCRUSH_BITS,
        AMP_ATTACK
    };