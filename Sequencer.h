
#ifndef SEQUENCER_H
#define SEQUENCER_H

#include <Arduino.h>
#include <uClock.h>
#include <EasyNextionLibrary.h>
#include "AudioEngine.h"
#include "Config.h"

namespace Sequencer {

    // ------------------ CONFIG ------------------
    #define PPQN               96
    #define NUMBER_OF_BARS      4
    #define BEATS_PER_BAR       4
    #define STEPS_PER_BAR      16
    #define TICKS_PER_PATTERN (PPQN * BEATS_PER_BAR * NUMBER_OF_BARS)
    #define TICKS_PER_STEP (PPQN * BEATS_PER_BAR / STEPS_PER_BAR)  // 24
    const uint16_t TOTAL_STEPS = NUMBER_OF_BARS * STEPS_PER_BAR;   // 64 steps

    #define MAX_VISIBLE_STEPS 32
    #define DISPLAY_STEPS_PER_BAR 16
    #define DISPLAY_PIXELS        512
    #define X_OFFSET              144

    float getBPM();
    void setBPM(float bpm);

    // ------------------ TYPES ------------------
    struct NoteEvent { uint8_t note; uint8_t vel; };
    struct Tick { NoteEvent events[NUM_VOICES]; uint8_t count = 0; };
    struct ViewPort { uint32_t startStep; uint16_t steps; uint16_t barsOnDisplay; };

    // ------------------ VARIABLES ------------------
    extern ViewPort view;

    extern volatile uint32_t playheadTick;
    extern volatile uint32_t tickOffset;
    extern bool isPlaying;
    extern bool isRecording;
    extern bool scrubMode;
    extern uint32_t prerollTick;
    extern bool prerollActive;

    extern int16_t lastPhStep;

    extern Tick pattern[TICKS_PER_PATTERN];
    extern bool stepHasEvent[TICKS_PER_PATTERN];
    extern uint8_t lastCellState[MAX_VISIBLE_STEPS];
    extern char stepObjName[MAX_VISIBLE_STEPS][12];
    //extern EasyNex* display;

    enum TransportState { STOPPED, PREROLL, PLAYING, PAUSED };
    extern TransportState transport;

    // ------------------ FUNCTIONS ------------------
    uint8_t getDefaultVelocity();
    void setDefaultVelocity(uint8_t v);

    // Clock
    void onTick(uint32_t tick);
    void onStep(uint32_t stepIndex);
    void handleClockContinue();

    // Transport
    void onPlayFromStart();
    void onPlayPause();
    void onStop();
    void onRecord();

    // Pattern / Recording
    void recordNoteEvent(uint8_t note, uint8_t vel);
    void clearPattern();
    void clearpatterndisplay();

    // Viewport
    void initView(uint16_t zoomBars);
    void alignViewportToPlayhead(uint32_t stepIndex);
    void markStepEvent(uint32_t tick, uint8_t vel);
    void updateSequencerDisplay(uint32_t playTick);

    void updatePianoPlayhead(uint16_t stepIndex, uint8_t startNote);
    void drawPianoPattern(uint8_t startNote);
    void redrawStepColumn(uint16_t columnIndex);

    // INIT
    void init(uint16_t zoomBars); // sets display and starts clock

    // Testpattern
    void testPattern(float noteLengthFraction);
    void testPattern16th(float noteLengthFraction);

} // namespace Sequencer

#endif