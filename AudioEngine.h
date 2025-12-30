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

    #define SDCARD_CS_PIN    BUILTIN_SDCARD

    // ------------------ SAMPLER ------------------
    #define NUM_SAMPLER_VOICES 4    // RAM-limited
    #define MAX_SAMPLES 4          // SD sample pool

    struct SamplerVoice {
        AudioPlaySdRaw      player;     // plays sample from RAM
        AudioMixer4         mixSample;       // per voice mix
        uint8_t trackId; 
        uint8_t note;  
        bool active;           // pad/track mapping
        int sampleIndex;           // which sample is loaded
    };

    struct Sample {
        const char* filename;       // SD filename
    };

    extern SamplerVoice samplerVoices[NUM_SAMPLER_VOICES];
    extern Sample samplePool[MAX_SAMPLES];
    
    // Track→voice mapping
    extern int8_t samplerTrackVoiceMap[MAX_TRACKS][128];

    // ------------------ FUNCTIONS ------------------
    void loadAndAssignPad(const char* filename, uint8_t padId);
    bool loadSample(int idx);

    int findFreeSamplerVoice();

    void samplerNoteOn(uint8_t trackId, uint8_t note, uint8_t vel);
    void samplerNoteOff(uint8_t trackId, uint8_t note);
    void muteSamplerTrack(uint8_t trackId);

    // ------------------ SYNTH ------------------
    #define MAX_ENGINES 4
    #define VOICES_PER_ENGINE 4
    #define NUM_VOICES (MAX_ENGINES * VOICES_PER_ENGINE)

    struct Voice {
        AudioSynthWaveform  oscA;
        AudioSynthWaveform  oscB;
        AudioMixer4         oscMix;
        AudioEffectEnvelope env;

        uint8_t trackId; 
        uint8_t note;
        bool    active;
    };

    struct SynthEngine {
        AudioMixer4 mix;                     // sums voices
        AudioEffectBitcrusher crusher;       // shared
        AudioFilterStateVariable filter;      // shared
    };

    extern uint8_t voiceNote[NUM_VOICES];
    float midiToFreq(uint8_t note);
    int findFreeVoice();
    void noteOn(uint8_t trackId, uint8_t note, uint8_t vel);
    void noteOff(uint8_t trackId, uint8_t note);
    void muteTrack(uint8_t trackId);
    void allNotesOff();
    
    // ------------------ ENGINES UNIFY ------------------
    void trackNoteOn(uint8_t trackId, uint8_t note, uint8_t vel);
    void trackNoteOff(uint8_t trackId, uint8_t note);

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
    // ------------------ METRO ------------------
    extern const float METRO_VOLUME;
    void Metro(uint32_t tick);
    // ------------------ INITIALIZATION ------------------
    void init();

} // namespace AudioEngine

#endif