
#ifndef SEQUENCER_H
#define SEQUENCER_H

#include <Arduino.h>
#include <uClock.h>
#include "AudioEngine.h"
#include "Config.h"

namespace Sequencer {

    // ------------------ CONFIG ------------------ //
    #define MAX_SEQ_BARS 32

    #define MAX_SEQUENCES 32
    #define MAX_PATTERN_SLOTS 16

    #define PPQN               96
    #define BEATS_PER_BAR       4
    #define STEPS_PER_BAR      16
    #define TICKS_PER_STEP (PPQN * BEATS_PER_BAR / STEPS_PER_BAR)  // 24
    #define TICKS_PER_BAR  (PPQN * BEATS_PER_BAR)

    #define MAX_PATTERN_TICKS  (MAX_SEQ_BARS * TICKS_PER_BAR)
    #define MAX_PATTERN_STEPS  (MAX_SEQ_BARS * STEPS_PER_BAR)

    #define NOTE_RANGE 96 // Standard 88-key piano --> MIDI notes 21 (A0) to 108 (C8). C0 - C8 --> 96

    #define DISPLAY_STEPS          32
    #define MAX_NOTES_DISPLAY      12 
    #define DISPLAY_PIXELS        512
    #define X_OFFSET              144

    // ------------------ DATA STRUCTURE--------------------- //
    constexpr uint16_t MAX_EVENTS_PER_PATTERN = 1024;
    enum class EventType { NOTE_ON, NOTE_OFF, CC };

    struct Event {
        uint32_t tick;       // absolute tick
        EventType type;
        uint8_t note;        // or CC id
        uint8_t value;       // velocity or CC value
    };

    struct PatternSlot {
        Event events[MAX_EVENTS_PER_PATTERN];
        bool used;
    };
    
    struct Pattern {
        Event* events;       // dynamically allocated (or DMAMEM)
        uint16_t count;      // current number of events
        uint16_t maxEvents;  // max capacity
        int8_t slotIndex = -1; 
    };

    struct TrackEventBuffer {
        EventType* events;         // pointer to allocated pool for this track
        uint16_t  count;           // number of events currently recorded
        uint16_t  maxEvents;       // max events this buffer can hold
    };

    struct Track {
        bool active = false;
        bool mute   = false;
        uint8_t type = 0;
        uint8_t midiCh = 1;

        Pattern pattern;  // pattern with sparse events
    };

    struct Sequence {
        uint8_t lengthBars = 4;
        float   bpm        = 120.0f;
        Track   tracks[MAX_TRACKS];
    };

    enum TrackType {
        SYNTH = 0,
        SAMPLER = 1,
        //GRANULAR = 2,
        //PERC = 3
    };

    // ------------------ FUNCTIONS --------------------- //
    // CLOCK
    void onTick(uint32_t tick);
    void onStep(uint32_t stepIndex);
    void handleClockContinue();

    // SEQUENCE & TRACK 
    extern Sequence sequences[MAX_SEQUENCES];
    extern uint8_t currentSequence;
    extern uint8_t currentTrack;

    inline Sequence& curSeq() {return sequences[currentSequence];}
    inline Track& curTrack() {return curSeq().tracks[currentTrack];}
    
    void setCurrentTrack(uint8_t t);
    uint8_t getCurrentTrack();
    void setTrackType(TrackType type);
    void toggleTrackMute(uint8_t track);
    extern bool isTrackMuted(uint8_t track);
    void initTrack(Track& tr, uint8_t index);

    // PATTERN
    extern bool trackHasPatternData(uint8_t trackIndex);
    void clearPattern(uint8_t track);
    void initPattern(Track& tr);

    // EVENT
    inline Event makeEvent(uint32_t tick, uint8_t note, uint8_t vel) {
        Event e;
        e.tick  = tick;
        e.type  = (vel > 0) ? EventType::NOTE_ON : EventType::NOTE_OFF;
        e.note  = note;
        e.value = vel;
        return e;
    }
    void markStepEvent(uint32_t tick, uint8_t note, uint8_t vel);
    extern bool stepNoteHasEvent[MAX_PATTERN_STEPS][NOTE_RANGE];

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

    // RECORD
    void recordNoteEvent(uint8_t trackId, uint8_t note, uint8_t vel);
    void onOverdub();
    void onRecord();

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

    // TIMING DIVISION
    enum class TimingDivision {
        QUARTER,
        EIGHTH,
        SIXTEENTH,
        SIXTEENTHT,
        THIRTYSECOND,
        THIRTYSECONDT
    };
    uint32_t divisionToTicks(TimingDivision rate);

    extern TimingDivision noteRepeatRate;
    extern TimingDivision arpRate;

    // QUANTIZE
    void setQuantizeDivision(TimingDivision rate);
    void setQuantizeEnabled(bool on);

    // NOTE REPEAT
    struct NoteRepeatVoice {
        bool     active;
        bool     noteOn;
        uint8_t  note;
        uint32_t nextTick;
        uint32_t offTick;
        uint8_t  trackId;
    };

    constexpr uint8_t MAX_REPEAT_VOICES = 4;
    extern NoteRepeatVoice repeatVoices[MAX_REPEAT_VOICES];

    extern uint32_t noteRepeatLastTick;
    extern bool noteRepeatActive;
    extern uint8_t noteRepeatNote;           // pad note currently repeating
    extern uint32_t noteRepeatInterval;      // interval in ticks
    extern uint32_t noteRepeatNextTick;      // next tick to trigger repeat
    extern uint32_t noteRepeatNextTickOff;   // when to turn off the note
    extern uint32_t noteRepeatDurationTicks; // how long each repeat note sounds
    extern bool noteRepeatNoteOn;    

    void setRepeatDivision(TimingDivision rate);
    void startNoteRepeat(uint8_t note);
    void stopNoteRepeat(uint8_t note);

    // ARPEGGIATOR
    enum class ArpMode {OFF, UP_OCTAVE, HELD_NOTES };
    struct ArpVoice {
        bool active = false;
        uint8_t note = 0;
        bool noteOn = false;
        uint32_t nextTick = 0;
        uint32_t offTick = 0;
        uint8_t stepIndex = 0;
        uint8_t  trackId;
    };

    extern ArpMode arpMode;
    extern uint8_t arpOctaves;
    extern float arpGate;
    extern uint8_t numHeldNotes;
    extern ArpVoice arpVoice;

    void recalcArpTiming();
    void startArp(uint8_t note);
    void stopArp(uint8_t note);
    void processArp(uint32_t tick);
    void setArpMode(ArpMode mode);

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
    extern uint8_t lastCellState[DISPLAY_STEPS * MAX_NOTES_DISPLAY];

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
    void initTimingControls() ;
    void init(); // sets display and starts clock

    // Testpattern
    void testPatternGumball(float noteLengthFraction);

} // namespace Sequencer

#endif