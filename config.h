#pragma once

#define MAX_TRACKS 32
static constexpr uint8_t NUM_VOICES = 4;

    enum class EncParam {
        OSC1_PULSE,
        OSC2_DETUNE,
        FILTER_CUTOFF,
        FILTER_RESONANCE,
        BITCRUSH_BITS,
        ENV_ATT,
        ENV_DEC,
        ENV_SUS,
        ENV_REL,
        AMP_ATTACK,
        ARP_RATE,
        ARP_OCTAVES,
        ARP_MODE,
        ARP_GATE,
        MAIN_VOL,
        MAIN_2,
        MAIN_3,
        MAIN_4,
    };

