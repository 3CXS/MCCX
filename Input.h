#ifndef INPUT_H
#define INPUT_H

#include <Arduino.h>
#include <Encoder.h>
#include <Adafruit_seesaw.h>
#include <Adafruit_NeoTrellis.h>
#include <seesaw_neopixel.h>

#include "ButtonManager.h"
#include "Config.h"

// Forward declare your external modules
namespace Sequencer { void recordNoteEvent(uint8_t note, uint8_t vel); }
namespace AudioEngine { void noteOn(uint8_t note, uint8_t vel); void noteOff(uint8_t note); void allNotesOff(); }

namespace Input {

    // ---------------- BUTTONS & MUX ----------------
    extern Mux16 mux;
    extern ButtonManager manager;

    extern uint8_t PLAY_FROM_START;
    extern uint8_t PLAY_PAUSE;
    extern uint8_t STOP;
    extern uint8_t RECORD;
    extern uint8_t ENCODER;
    extern uint8_t RSHIFT;
    extern bool shiftActive;
    extern bool f1Active;
    extern bool f2Active;

    void onPlayFromStart();
    void onPlayPause();
    void onStop();
    void onRecord();
    void onShift();
    void onEncoderButton();
    void onF2();
    void showF1PadHints();
    void clearF1PadHints();

    // ---------------- MAIN ENCODER ----------------
    extern Encoder MainEnc;
    void mainEncoder();

    // ---------------- QUAD ENCODERS ----------------
    extern Adafruit_seesaw encA; // optional: add encB if needed
    extern const uint8_t NUM_ENCODERS;
    extern int32_t encPos[];
    extern bool encButtonPrev[];
    extern const uint8_t encPins[];
    extern Adafruit_seesaw* encBoards[];
    extern const char* encNames[];

    void readEncoders();
    void setEncoderPage(uint8_t page);

    // ---------------- BUTTONPADS ----------------
    #define Y_DIM 4
    #define X_DIM 8
    extern Adafruit_NeoTrellis t_array[Y_DIM/4][X_DIM/4];
    extern Adafruit_MultiTrellis trellis;
    uint8_t keyToNote(uint8_t keyIndex);
    void readPads();
    TrellisCallback keyPress();
    void processTrellisLEDs();

    // ---------------- PROCESS  ----------------
    void processInputEvents();
    // ---------------- INIT  ----------------
    void init();


} //namespace InputModule

#endif