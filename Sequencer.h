
#ifndef SEQUENCER_H
#define SEQUENCER_H

#include <Arduino.h>
#include <uClock.h>
#include <EasyNextionLibrary.h>
#include "AudioEngine.h"
#include "Config.h"

enum class QuantizeType {
        OFF,
        QUARTER,
        SIXTEENTH,
        THIRTYSECOND
    };

struct NoteRepeatVoice {
        bool     active;
        bool     noteOn;
        uint8_t  note;
        uint32_t nextTick;
        uint32_t offTick;
    };

namespace Sequencer {

    // ------------------ CONFIG ------------------ //
    #define PPQN               96
    #define BEATS_PER_BAR       4
    #define STEPS_PER_BAR      16
    #define TICKS_PER_STEP (PPQN * BEATS_PER_BAR / STEPS_PER_BAR)  // 24
    #define TICKS_PER_BAR  (PPQN * BEATS_PER_BAR)

    #define MAX_SEQ_BARS 16
    #define MAX_PATTERN_TICKS  (MAX_SEQ_BARS * TICKS_PER_BAR)
    #define MAX_PATTERN_STEPS  (MAX_SEQ_BARS * STEPS_PER_BAR)

    #define NOTE_RANGE 96 // Standard 88-key piano --> MIDI notes 21 (A0) to 108 (C8). C0 - C8 --> 96

    #define DISPLAY_STEPS          32
    #define MAX_NOTES_DISPLAY      12 
    #define DISPLAY_PIXELS        512
    #define X_OFFSET              144

    // ------------------ SEQUENCER--------------------- //
    // SEQ LENGTH
    extern uint8_t seqLength; // number of bars (runtime variable)
    void setSeqLength(uint8_t bars);
    uint8_t getSeqLength();
    uint32_t getMaxTicks();
    uint16_t getTotalSteps();

    // BPM
    float getBPM();
    void setBPM(float bpm);
    float getStartNote();

    // VEL
    uint8_t getDefaultVelocity();
    void setDefaultVelocity(uint8_t v);

    // CLOCK
    void onTick(uint32_t tick);
    void onStep(uint32_t stepIndex);
    void handleClockContinue();

    // TRANSPORT
    enum TransportState { STOPPED, PREROLL, PLAYING, PAUSED };
    extern TransportState transport;
    extern bool isPlaying;
    extern bool isRecording;
    extern bool scrubMode;
    extern uint32_t prerollTick;
    extern bool prerollActive;
    extern volatile uint32_t tickOffset;

    void onPlayFromStart();
    void onPlayPause();
    void onStop();
    void onRecord();

    // PATTERN
    struct NoteEvent { uint8_t note; uint8_t vel; };
    struct Tick { NoteEvent events[NUM_VOICES]; uint8_t count = 0; };

    extern Tick pattern[MAX_PATTERN_TICKS];

    extern bool stepNoteHasEvent[MAX_PATTERN_STEPS][NOTE_RANGE];
    extern uint8_t lastCellState[DISPLAY_STEPS * MAX_NOTES_DISPLAY];

    void recordNoteEvent(uint8_t note, uint8_t vel);
    void clearPattern();
    void setQuantizeMode(QuantizeType mode);
    void markStepEvent(uint32_t tick, uint8_t note, uint8_t vel);

    QuantizeType getQuantizeMode();

    // NOTE REPEAT
    enum class NoteRepeatRate {
        OFF,
        QUARTER,
        EIGHTH,
        SIXTEENTH,
        SIXTEENTHT,
        THIRTYSECOND,
        THIRTYSECONDT
    };

    static constexpr uint8_t MAX_REPEAT_VOICES = 4;
    extern NoteRepeatVoice repeatVoices[MAX_REPEAT_VOICES];
    extern NoteRepeatRate noteRepeatRate;

    extern uint32_t noteRepeatLastTick;

    extern bool noteRepeatActive;
    extern uint8_t noteRepeatNote;           // pad note currently repeating
    extern uint32_t noteRepeatInterval;      // interval in ticks
    extern uint32_t noteRepeatNextTick;      // next tick to trigger repeat
    extern uint32_t noteRepeatNextTickOff;   // when to turn off the note
    extern uint32_t noteRepeatDurationTicks; // how long each repeat note sounds
    extern bool noteRepeatNoteOn;    

    static constexpr uint8_t NOTE_REPEAT_GATE_PERCENT = 50;

    uint32_t getNoteRepeatIntervalTicks(NoteRepeatRate rate);
    void updateNoteRepeatLabel(NoteRepeatRate rate);

    void startNoteRepeat(uint8_t note);
    void stopNoteRepeat(uint8_t note);

    // ARPEGGIATOR
    extern uint8_t numHeldNotes;
    struct ArpVoice {
        bool active = false;
        uint8_t note = 0;
        bool noteOn = false;
        uint32_t nextTick = 0;
        uint32_t offTick = 0;
        uint8_t stepIndex = 0;
    };

    extern ArpVoice arpVoice;  

    enum class ArpMode {OFF, UP_OCTAVE, HELD_NOTES };
    enum class ArpRate : uint8_t {QUARTER, EIGHTH, SIXTEENTH, SIXTEENTHT, THIRTYSECOND, THIRTYSECONDT };

    extern ArpMode arpMode;
    extern ArpRate arpRate;
    extern uint8_t arpOctaves;
    extern float arpGate;

    void recalcArpTiming();
    
    void startArp(uint8_t note);
    void stopArp(uint8_t note);
    void processArp(uint32_t tick);
    void updateArpLabel(ArpMode);

    void onOverdub();
    void onRecord();

    // ------------------ VIEW ----------------------- //
    struct ViewPort {
        uint32_t startStep;
        uint16_t steps;
        uint16_t stepsPerColumn;
        float    barsOnDisplay;   // now float to support half-bar
        uint8_t  startNote;
        uint8_t  notesOnDisplay;
    };
    extern ViewPort view;
    extern bool viewportRedrawPending;
    void initView(uint8_t zoomBars);
    void updateDisplayPlayhead();

    // GRID
    namespace Grid {
        constexpr uint16_t startX       = X_OFFSET+2;
        constexpr uint16_t startY       = 121;
        constexpr uint16_t rowHeight    = 18;
        constexpr uint8_t  spacingY     = 2;
        constexpr uint8_t  spacingX     = 4;
        constexpr uint8_t  stepsVisible = DISPLAY_STEPS;
        constexpr uint16_t stepW        = DISPLAY_PIXELS / stepsVisible - spacingX;
        constexpr uint16_t fgOn         = 65535;
        constexpr uint16_t fgOff        = 0;
    }
    inline void drawGridCellPrebuilt(uint8_t col, uint8_t row, bool noteActive);
    void drawBarRuler();

    // ZOOM
    enum class ZoomLevel : uint8_t {
        X0 = 128,
        X1 = 1,
        X2 = 2,
        X4 = 4
    };
    void setZoom(ZoomLevel z);
    ZoomLevel getZoom();
    void cycleZoom(int8_t dir);

    //PLAYHEAD
    extern volatile uint32_t playheadTick;
    extern int16_t lastPhStep;
    extern uint16_t lastViewStartStep;
    void updatePlayhead(uint32_t stepIndex);
    void movePlayheadColumns(int8_t delta);
    void alignViewportToPlayhead(uint32_t stepIndex);

    // PIANO ROLL
    void initPianoRollXSTR();
    void drawPianoRoll();
    void scrollNotes(int8_t delta);

    // PROCESS
    void processDisplay();
    void updateSequencerDisplay(uint32_t playTick);

    // ------------------ INIT --------------------- //
    void init(); // sets display and starts clock

    // Testpattern
    void testPatternGumball(float noteLengthFraction);

} // namespace Sequencer

#endif