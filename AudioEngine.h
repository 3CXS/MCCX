#ifndef AUDIOENGINE_H
#define AUDIOENGINE_H

#include <Audio.h>
#include <Wire.h>
#include <SD.h>
#include <SPI.h>
#include <SerialFlash.h>
#include <math.h>
#include "Sequencer.h"
#include "Config.h"

namespace AudioEngine {

    #define SDCARD_CS_PIN    BUILTIN_SDCARD
    #define SDCARD_MOSI_PIN  11
    #define SDCARD_SCK_PIN   13

    // ------------------ AUDIO OBJECTS ------------------
    extern AudioMixer4             mixV1, mixV2, mixMetro, mixMain;
    extern AudioOutputI2S          i2s1;
    extern AudioControlSGTL5000    sgtl5000_1;

    // SYNTH VOICES
    extern AudioSynthWaveform      osc[NUM_VOICES];
    extern AudioEffectEnvelope     env[NUM_VOICES];
    extern AudioConnection*        patchCords[NUM_VOICES * 6];

    // METRO
    extern AudioSynthWaveform      metroOsc;
    extern AudioEffectEnvelope     metroEnv;
    extern AudioConnection         patchMetro1;
    extern AudioConnection         patchMetro2;
    extern AudioConnection         patchMetro3;

    // MAIN OUTPUT
    extern AudioConnection         patchMainL;
    extern AudioConnection         patchMainR;

    // VOICE TRACKING
    extern uint8_t voiceNote[NUM_VOICES];

    void setSynthParam(SynthParam p, float value);

    // ------------------ PENDING BUFFER ------------------
    #define PEND_MASK 63
    extern volatile uint8_t pend_note[64], pend_vel[64], pend_w, pend_r;
    bool popPending(uint8_t &note, uint8_t &vel);
    void pushPending(uint8_t note, uint8_t vel);
    void processPending();

    // ------------------ VOICE MANAGEMENT ------------------
    float midiToFreq(uint8_t note);
    int findFreeVoice();
    void noteOn(uint8_t note, uint8_t vel);
    void noteOff(uint8_t note);
    void allNotesOff();

    // ------------------ METRO ------------------
    extern const float METRO_VOLUME;
    void Metro(uint32_t tick);

    // ------------------ INITIALIZATION ------------------
    void init();

} // namespace AudioEngine

#endif