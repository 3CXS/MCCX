
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
    #define NOTE_RANGE 96 // Standard 88-key piano --> MIDI notes 21 (A0) to 108 (C8). C0 - C8 --> 96
    #define MAX_VISIBLE_STEPS 32
    #define MAX_NOTES_DISPLAY 12 
    #define DISPLAY_STEPS_PER_BAR 16
    #define DISPLAY_PIXELS        512
    #define X_OFFSET              144

    float getBPM();
    void setBPM(float bpm);
    float getStartNote();

    // ------------------ TYPES ------------------
    struct NoteEvent { uint8_t note; uint8_t vel; };
    struct Tick { NoteEvent events[NUM_VOICES]; uint8_t count = 0; };
    struct ViewPort {
        uint32_t startStep;
        uint16_t steps;
        uint16_t barsOnDisplay;
        uint8_t  startNote;
        uint8_t  notesOnDisplay;
    };

    // ------------------ VARIABLES ------------------
    extern ViewPort view;

    extern volatile uint32_t playheadTick;
    extern volatile uint32_t tickOffset;
    extern bool isPlaying;
    extern bool isRecording;
    extern bool scrubMode;
    extern uint32_t prerollTick;
    extern bool prerollActive;
    extern bool viewportRedrawPending;

    extern int16_t lastPhStep;
    extern uint16_t lastViewStartStep;

    extern Tick pattern[TICKS_PER_PATTERN];
    extern bool stepNoteHasEvent[TOTAL_STEPS][NOTE_RANGE];
    extern uint8_t lastCellState[MAX_VISIBLE_STEPS * MAX_NOTES_DISPLAY];

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

    // Viewport
    void initView(uint16_t zoomBars);
    void alignViewportToPlayhead(uint32_t stepIndex);
    void markStepEvent(uint32_t tick, uint8_t note, uint8_t vel);
    void updateSequencerDisplay(uint32_t playTick);
    void updatePlayhead(uint16_t stepIndex);
    void drawPianoPattern(uint8_t startNote);
    void redrawStepColumn(uint16_t columnIndex);
    void processDisplay();
    void ensureNoteVisible(uint8_t note);
    void scrollNotes(int8_t delta);

    // INIT
    void init(uint16_t zoomBars); // sets display and starts clock

    // Testpattern
    void testPattern16th(float noteLengthFraction);

} // namespace Sequencer

#endif