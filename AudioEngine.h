#ifndef AUDIOENGINE_H
#define AUDIOENGINE_H

#include <Audio.h>
#include <Wire.h>
#include <SD.h>
#include <SPI.h>
#include <SerialFlash.h>
#include <math.h>

#include "Config.h"

namespace AudioEngine {

    #define MAX_ENGINES 4
    #define VOICES_PER_ENGINE 4
    #define NUM_VOICES (MAX_ENGINES * VOICES_PER_ENGINE)

    #define SDCARD_CS_PIN    BUILTIN_SDCARD
    #define SDCARD_MOSI_PIN  11
    #define SDCARD_SCK_PIN   13

    // ------------------ DATA STRUCT ------------------
    struct Voice {
        AudioSynthWaveform oscA;
        AudioSynthWaveform oscB;
        AudioMixer4        oscMix;
        AudioEffectEnvelope env;

        uint8_t engineId;
        uint8_t trackId; 
        uint8_t note;
        bool    active;
    };

    struct SynthEngine {
        AudioMixer4 mix;                     // sums voices
        AudioEffectBitcrusher crusher;       // shared
        AudioFilterStateVariable filter;      // shared
    };

    // ------------------ AUDIO OBJECTS ------------------
    extern AudioMixer4             mixMain;

    // ------------------ PARAMETERS  ------------------
    void setSynthParam(EncParam p, float value);
    void setMainParam(EncParam param, float value);

    // ------------------ PENDING BUFFER ------------------
    #define PEND_MASK 63
    
    extern volatile uint8_t pend_note[64];
    extern volatile uint8_t pend_vel[64];
    extern volatile uint8_t pend_track[64];

    extern volatile uint8_t pend_w;
    extern volatile uint8_t pend_r;

    bool popPending(uint8_t &trackId, uint8_t &note, uint8_t &vel);
    void pushPending(uint8_t trackId, uint8_t note, uint8_t vel);
    void processAudio();

    // ------------------ VOICE MANAGEMENT ------------------
    extern uint8_t voiceNote[NUM_VOICES];
    float midiToFreq(uint8_t note);
    int findFreeVoice();
    void noteOn(uint8_t trackId, uint8_t note, uint8_t vel);
    void noteOff(uint8_t trackId, uint8_t note);
    void muteTrack(uint8_t trackId);
    void allNotesOff();

    // ------------------ METRO ------------------
    extern const float METRO_VOLUME;
    void Metro(uint32_t tick);

    // ------------------ INITIALIZATION ------------------
    void init();

} // namespace AudioEngine

#endif