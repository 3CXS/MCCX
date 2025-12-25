#include "Sequencer.h"
#include "Display.h"

#define ATOMIC(X) noInterrupts(); X; interrupts();

namespace Sequencer {

    // SEQ LENGTH
    uint8_t seqLength = 8; // default

    void setSeqLength(uint8_t bars) {
        seqLength = constrain(bars, 1, MAX_SEQ_BARS);
        Display::writeNum("length.val", seqLength);
        if (playheadTick >= getMaxTicks()) {
            playheadTick = getMaxTicks() - 1;
        }
    }
    uint8_t getSeqLength() { return seqLength;}
    uint32_t getMaxTicks() { return seqLength * TICKS_PER_BAR;}
    uint16_t getTotalSteps() { return seqLength * STEPS_PER_BAR;}

    // VIEW
    ViewPort view;
    uint8_t display_steps_per_bar = 0;
    uint32_t getTicksPerColumn() {
        return (uint32_t)((TICKS_PER_BAR * view.barsOnDisplay) / DISPLAY_STEPS);
    }
    int16_t lastPhStep = -1;        
    uint16_t lastViewStartStep = 0;

    uint8_t lastCellState[DISPLAY_STEPS * MAX_NOTES_DISPLAY];

    // ZOOM
    static ZoomLevel zoomLevel = ZoomLevel::X2; // default = 2 bars
    inline float zoomBarsFromLevel(ZoomLevel z) {
        switch(z) {
            case ZoomLevel::X0:   return 0.5f;
            case ZoomLevel::X1:   return 1.0f;
            case ZoomLevel::X2:   return 2.0f;
            case ZoomLevel::X4:   return 4.0f;
        }
        return 1.0f; // fallback
    }
    ZoomLevel getZoom() {return zoomLevel;}

    // BPM
    static float bpm = 120.0f;
    static constexpr float BPM_MIN = 40.0f;
    static constexpr float BPM_MAX = 300.0f;
    
    float getBPM() { return bpm; }
    float getStartNote() { return view.startNote; }

    void setBPM(float v) {
        bpm = constrain(v, BPM_MIN, BPM_MAX);
        uClock.setTempo(bpm);
        Display::writeNum("bpm.val", bpm);
    }
    
    // VELOCITY
    static uint8_t defaultVelocity = 120;
    uint8_t getDefaultVelocity() {return defaultVelocity;}
    void setDefaultVelocity(uint8_t v) {defaultVelocity = constrain(v, 1, 127);}

    // TRANSPORT
    volatile uint32_t playheadTick = 0;  // current tick within pattern
    volatile uint32_t tickOffset = 0;    // offset to align playhead after preroll

    bool isPlaying = false;
    bool isRecording = false;
    bool scrubMode = false;

    uint32_t prerollTick = 0;
    bool prerollActive = false;

    bool viewportRedrawPending = true;
    TransportState transport = STOPPED;

    static constexpr uint8_t PREROLL_BEATS = 4;
    static constexpr uint32_t PREROLL_TICKS = PREROLL_BEATS * PPQN;
    static uint8_t pianoStartNote = 48; // C2 default

    enum class RecordMode {
        NORMAL,   // Clears pattern and records fresh
        OVERDUB,  // Adds notes to existing pattern
        // FUTURE: PUNCH_IN
    };
    RecordMode currentRecordMode = RecordMode::NORMAL;


    // PATTERN
    Tick pattern[MAX_PATTERN_TICKS];
    bool stepHasEvent[MAX_PATTERN_TICKS]; // true if any tick in the step has an event
    bool stepNoteHasEvent[MAX_PATTERN_STEPS][NOTE_RANGE];  // absolute MIDI notes

   // QUANTIZE
    uint32_t quantizeTicks(QuantizeType q) {
        switch(q) {
            case QuantizeType::QUARTER:     return PPQN;         // 1 quarter = 96 ticks
            case QuantizeType::SIXTEENTH:   return PPQN / 4;     // 1/16 = 24 ticks
            case QuantizeType::THIRTYSECOND:return PPQN / 8;     // 1/32 = 12 ticks
            case QuantizeType::OFF:
            default: return 1;  // no quantization
        }
    }

    static QuantizeType quantizeMode = QuantizeType::OFF;
    void setQuantizeMode(QuantizeType mode) {
        quantizeMode = mode;
        Display::writeStr("quant.txt", mode == QuantizeType::OFF ? "OFF" :
                                        mode == QuantizeType::QUARTER ? "1/4" :
                                        mode == QuantizeType::SIXTEENTH ? "1/16" : "1/32");
    }

    QuantizeType getQuantizeMode() {return quantizeMode;}

    // NOTE REPEAT

    NoteRepeatRate noteRepeatRate = NoteRepeatRate::OFF;

    uint32_t noteRepeatLastTick = 0;

    bool noteRepeatActive       = false;
    uint8_t noteRepeatNote      = 0;
    uint32_t noteRepeatInterval = 0;
    uint32_t noteRepeatNextTick = 0;
    uint32_t noteRepeatNextTickOff = 0;
    uint32_t noteRepeatDurationTicks = 0;
    bool noteRepeatNoteOn       = false;

    NoteRepeatVoice repeatVoices[MAX_REPEAT_VOICES];

    uint32_t getNoteRepeatIntervalTicks(NoteRepeatRate rate) {
        switch(rate) {
            case NoteRepeatRate::QUARTER:      return PPQN;
            case NoteRepeatRate::EIGHTH:       return PPQN / 2;
            case NoteRepeatRate::SIXTEENTH:    return PPQN / 4;
            case NoteRepeatRate::SIXTEENTHT:   return PPQN / 6;
            case NoteRepeatRate::THIRTYSECOND: return PPQN / 8;
            case NoteRepeatRate::THIRTYSECONDT:return PPQN / 12;
            case NoteRepeatRate::OFF:
            default: return 0;
        }
    }

    void updateNoteRepeatLabel(NoteRepeatRate rate) {
        const char* txt = "OFF";
        switch(rate) {
            case NoteRepeatRate::QUARTER:      txt = "1/4"; break;
            case NoteRepeatRate::EIGHTH:       txt = "1/8"; break;
            case NoteRepeatRate::SIXTEENTH:    txt = "1/16"; break;
            case NoteRepeatRate::SIXTEENTHT:   txt = "1/16T"; break;
            case NoteRepeatRate::THIRTYSECOND: txt = "1/32"; break;
            case NoteRepeatRate::THIRTYSECONDT:txt = "1/32T"; break;
            default: txt = "OFF"; break;
        }
        Display::writeStr("nr.txt", txt);
    }

    void startNoteRepeat(uint8_t note) {
        if (noteRepeatRate == NoteRepeatRate::OFF)
            return;

        // Find free voice
        for (uint8_t i = 0; i < NUM_VOICES; i++) {
            if (!repeatVoices[i].active) {
                repeatVoices[i] = {
                    .active   = true,
                    .noteOn   = false,
                    .note     = note,
                    .nextTick = playheadTick,   // start immediately
                    .offTick  = 0
                };
                return;
            }
        }
    }

    void stopNoteRepeat(uint8_t note) {
        for (uint8_t i = 0; i < NUM_VOICES; i++) {
            auto &v = repeatVoices[i];
            if (v.active && v.note == note) {

                if (v.noteOn) {
                    AudioEngine::noteOff(note);
                    if (isRecording)
                        recordNoteEvent(note, 0);
                }

                v.active = false;
                v.noteOn = false;
                return;
            }
        }
    }

    void processNoteRepeat(uint32_t tick) {
        uint32_t interval = getNoteRepeatIntervalTicks(noteRepeatRate);
        if (interval == 0) return;

        for (uint8_t i = 0; i < NUM_VOICES; i++) {
            auto &v = repeatVoices[i];
            if (!v.active) continue;

            // NOTE ON
            if (!v.noteOn && tick >= v.nextTick) {
                AudioEngine::noteOn(v.note, getDefaultVelocity());
                v.noteOn = true;

                uint32_t dur = interval * 8 / 10;
                v.offTick = v.nextTick + dur;
                v.nextTick += interval;

                if (isRecording)
                    recordNoteEvent(v.note, getDefaultVelocity());
            }

            // NOTE OFF
            if (v.noteOn && tick >= v.offTick) {
                AudioEngine::noteOff(v.note);
                v.noteOn = false;

                if (isRecording)
                    recordNoteEvent(v.note, 0);
            }
        }
    }

    // ARPEGGIATOR

    uint8_t numHeldNotes = 0;
    ArpVoice arpVoice = { false, false, 0, 0, 0 };
    ArpMode arpMode = ArpMode::OFF;
    ArpRate arpRate = ArpRate::SIXTEENTH;
    float arpGate = 0.8f;

    uint32_t getArpIntervalTicks(ArpRate rate) {
        switch(rate) {
            case ArpRate::QUARTER:      return PPQN;
            case ArpRate::EIGHTH:       return PPQN / 2;
            case ArpRate::SIXTEENTH:    return PPQN / 4;
            case ArpRate::SIXTEENTHT:   return PPQN / 6;
            case ArpRate::THIRTYSECOND: return PPQN / 8;
            case ArpRate::THIRTYSECONDT:return PPQN / 12;
            default: return PPQN / 4;
        }
    }

    void recalcArpTiming() {
        // This function can precompute interval ticks if needed
        // For now, we just call getArpIntervalTicks in processArp
    }

    static constexpr uint8_t MAX_ARP_VOICES = 1; // monophonic arp output
    uint8_t arpOctaves = 3;       // octave cycle range

    void updateArpLabel(ArpMode mode) {
        const char* txt = "OFF";
        switch(mode) {
            case ArpMode::UP_OCTAVE:      txt = "UP"; break;
            case ArpMode::HELD_NOTES:     txt = "HOLD"; break;
            default: txt = "OFF"; break;
        }
        Display::writeStr("arp.txt", txt);
    }

    // Held note pool
    static constexpr uint8_t MAX_HELD_NOTES = 8;
    static uint8_t heldNotes[MAX_HELD_NOTES];

    void addHeldNote(uint8_t note) {
        for (uint8_t i = 0; i < numHeldNotes; i++)
            if (heldNotes[i] == note) return; // already in pool
        if (numHeldNotes < MAX_HELD_NOTES)
            heldNotes[numHeldNotes++] = note;
    }

    void removeHeldNote(uint8_t note) {
        for (uint8_t i = 0; i < numHeldNotes; i++) {
            if (heldNotes[i] == note) {
                // shift remaining down
                for (uint8_t j = i; j < numHeldNotes - 1; j++)
                    heldNotes[j] = heldNotes[j + 1];
                numHeldNotes--;
                break;
            }
        }
    }

    void startArp(uint8_t note) {
        if (arpMode == ArpMode::OFF) return;

        addHeldNote(note);

        arpVoice.active   = true;
        arpVoice.noteOn   = false;
        arpVoice.stepIndex= 0;
        arpVoice.nextTick = playheadTick;  // start immediately
        arpVoice.offTick  = 0;
    }

    void stopArp(uint8_t note) {
        removeHeldNote(note);
        if (arpVoice.noteOn && arpVoice.active) {
            AudioEngine::noteOff(arpVoice.note);
            arpVoice.noteOn = false;
            if (isRecording)
                recordNoteEvent(arpVoice.note, 0);
        }
        if (numHeldNotes == 0) arpVoice.active = false;
    }

    void processArp(uint32_t tick) {
        if (!arpVoice.active || numHeldNotes == 0 || arpMode == ArpMode::OFF)
            return;

        uint32_t interval = getArpIntervalTicks(arpRate);
        if (interval == 0) return;

        // --- NOTE ON ---
        if (!arpVoice.noteOn && tick >= arpVoice.nextTick) {

            uint8_t noteToPlay = 0;
            if (arpMode == ArpMode::UP_OCTAVE) {
                uint8_t baseNote = heldNotes[0];
                noteToPlay = baseNote + (arpVoice.stepIndex % arpOctaves) * 12;
            } else { // HELD_NOTES
                noteToPlay = heldNotes[arpVoice.stepIndex % numHeldNotes];
            }

            AudioEngine::noteOn(noteToPlay, getDefaultVelocity());
            arpVoice.note = noteToPlay;
            arpVoice.noteOn = true;

            // fix gate calculation: multiply only, no division by 100
            arpVoice.offTick = tick + (uint32_t)(interval * arpGate);
            arpVoice.nextTick = tick + interval;
            arpVoice.stepIndex++;

            if (isRecording)
                recordNoteEvent(noteToPlay, getDefaultVelocity());
        }

        // --- NOTE OFF ---
        if (arpVoice.noteOn && tick >= arpVoice.offTick) {
            AudioEngine::noteOff(arpVoice.note);
            arpVoice.noteOn = false;

            if (isRecording)
                recordNoteEvent(arpVoice.note, 0);
        }
    }

    // ----------------------------------------------------------------------------------//
    //                                      SEQUENCER                                    //
    // ----------------------------------------------------------------------------------//

    // ------------------ CLOCK ------------------
    void onTick(uint32_t tick) {
        // PREROLL
        if (transport == PREROLL) {
            AudioEngine::Metro(prerollTick);
            prerollTick++;
            if (prerollTick >= PREROLL_TICKS) {
                transport = PLAYING;
                playheadTick = 0;
                isPlaying = true;
                prerollTick = 0;
                tickOffset = tick;
            }
            return;
        }

        if (!isPlaying) return;

        // NORMAL PLAY
        uint32_t patternTick = (tick - tickOffset) % getMaxTicks();
        playheadTick = patternTick;

        Tick* t = &pattern[patternTick];

        for (uint8_t i = 0; i < t->count; i++) {
            AudioEngine::pushPending(t->events[i].note, t->events[i].vel);
        }
        // NOTE REPEAT
        processNoteRepeat(playheadTick);
        processArp(playheadTick);

        if (isRecording) {
            AudioEngine::Metro(patternTick);
        }
    }

    void onStep(uint32_t stepIndex) {
        updateSequencerDisplay(playheadTick);
    }

    void handleClockContinue() { scrubMode = false; }

    // ------------------ TRANSPORT ------------------
    void onPlayFromStart() {
        playheadTick = 0;
        tickOffset = 0;
        prerollTick = 0;

        alignViewportToPlayhead(playheadTick);
        updateSequencerDisplay(playheadTick);

        if (isRecording) {
            transport = PREROLL;
            prerollActive = true;
        } else {
            transport = PLAYING;
            isPlaying = true;
            prerollActive = false;
        }

        uClock.start();  // start clock
    }

    void onPlayPause() {
        switch (transport) {
            case STOPPED:
                transport = PLAYING;
                isPlaying = true;
                uClock.start();
                break;
            case PLAYING:
                uClock.pause();
                transport = PAUSED;
                isPlaying = false;
                break;
            case PAUSED:
                uClock.pause();  // toggle continue
                transport = PLAYING;
                isPlaying = true;
                break;
            case PREROLL:
                uClock.pause();
                transport = PAUSED;
                break;
        }
    }

    void onStop() {
        uClock.stop();
        transport = STOPPED;
        isPlaying = false;
        isRecording = false;
        scrubMode = false;
        AudioEngine::allNotesOff();
        playheadTick = 0;
        updateSequencerDisplay(playheadTick);
        Display::writeStr("rec.txt", isRecording? "REC":" ");
    }



    // ------------------ PATTERN ------------------
    void onRecord() {
        isRecording = true;
        playheadTick = 0;
        //allNotesOff();
        clearPattern();
        transport = STOPPED;
        updateSequencerDisplay(playheadTick);
        Display::writeStr("rec.txt", "REC");
    }

    void onOverdub() {
        isRecording = true;
        playheadTick = 0;
        // Do NOT clear pattern, unlike normal record
        transport = STOPPED;
        updateSequencerDisplay(playheadTick);
        Display::writeStr("rec.txt", "OVER");
    }

    void startRecord(RecordMode mode) {
        isRecording = true;
        currentRecordMode = mode;

        switch(mode) {
            case RecordMode::NORMAL:
                playheadTick = 0;
                clearPattern();
                // allNotesOff(); // optional if you want silence
                Display::writeStr("rec.txt", "REC");
                break;

            case RecordMode::OVERDUB:
                // Keep playheadTick as is to continue from current step
                // Keep pattern intact
                // allNotesOff(); // optional
                Display::writeStr("rec.txt", "OVERDUB");
                break;

            default:
                break;
        }

        transport = PLAYING; // You probably want playback while recording
        updateSequencerDisplay(playheadTick);
    }

    void recordNoteEvent(uint8_t note, uint8_t vel) {
        uint32_t tick;
        ATOMIC(tick = playheadTick);

        // Quantize ONLY note-on
        if (vel > 0 && quantizeMode != QuantizeType::OFF) {
            uint32_t qTicks = quantizeTicks(quantizeMode);
            tick = ((tick + qTicks / 2) / qTicks) * qTicks;

            // Clamp to pattern end
            if (tick >= getMaxTicks())
                tick = getMaxTicks() - 1;
        }

        Tick &t = pattern[tick];

        if (t.count < NUM_VOICES) {
            t.events[t.count++] = { note, vel };
        }

        // Only mark steps for note-on
        if (vel > 0) {
            markStepEvent(tick, note, vel);
        }

        updateSequencerDisplay(playheadTick);
    }

    void clearPattern() {
        // Clear internal pattern data
        for (uint32_t i = 0; i < getMaxTicks(); i++) {
            pattern[i].count = 0;
            for (uint8_t e = 0; e < NUM_VOICES; e++) {
                pattern[i].events[e].note = 0;
                pattern[i].events[e].vel  = 0;
            }
        }

        // Reset step events
        for (uint16_t i = 0; i < getTotalSteps(); i++) {
            stepHasEvent[i] = false;
            for (uint8_t n = 0; n < NOTE_RANGE; n++)
                stepNoteHasEvent[i][n] = false;
        }

        // Clear visible cells (only previously active)
        for (uint8_t row = 0; row < MAX_NOTES_DISPLAY; row++) {
            for (uint8_t col = 0; col < Grid::stepsVisible; col++) {
                uint16_t cellIndex = row * Grid::stepsVisible + col;
                if (lastCellState[cellIndex]) {
                    drawGridCellPrebuilt(col, row, false);
                    lastCellState[cellIndex] = false;
                }
            }
        }

        // Reset playhead
        lastPhStep = -1;
        lastViewStartStep = view.startStep;
        updatePlayhead(view.startStep);
    }


    void markStepEvent(uint32_t tick, uint8_t note, uint8_t vel) {
        if (vel == 0) return;

        uint32_t step = tick / TICKS_PER_STEP;
        if (step >= getTotalSteps()) return;

        stepHasEvent[step] = true;
        stepNoteHasEvent[step][note] = true;
    }

    // ----------------------------------------------------------------------------------//
    //                                      VIEWPORT                                     //
    // ----------------------------------------------------------------------------------//
    void initView(uint8_t zoomBars) {

        display_steps_per_bar = (uint8_t)(DISPLAY_STEPS / view.barsOnDisplay + 0.5f);

        if (zoomBars < 1) zoomBars = 1;
        if (zoomBars > getSeqLength()) zoomBars = getSeqLength();

        view.barsOnDisplay  = zoomBarsFromLevel(zoomLevel);
        view.steps          = DISPLAY_STEPS;      // fixed 32 display columns
        view.startStep      = 0;
        view.startNote      = pianoStartNote;
        view.notesOnDisplay = MAX_NOTES_DISPLAY;

        // Clear previous cell states
        for (uint16_t i = 0; i < DISPLAY_STEPS * MAX_NOTES_DISPLAY; i++) lastCellState[i] = 0xFF;
        lastPhStep = -1;
        // recalc display steps per bar
        display_steps_per_bar = (uint8_t)(DISPLAY_STEPS / view.barsOnDisplay); // e.g., 32 / 0.5 = 64
        // Redraw piano roll
        initPianoRollXSTR();
        drawPianoRoll();
    }

    // ------------------ GRID ------------------
    char xstrCellOn[MAX_NOTES_DISPLAY][Grid::stepsVisible][64];
    char xstrCellOff[MAX_NOTES_DISPLAY][Grid::stepsVisible][64];

    void initGridXSTR() {
        for (uint8_t row = 0; row < MAX_NOTES_DISPLAY; row++) {
            for (uint8_t col = 0; col < Grid::stepsVisible; col++) {
                uint16_t x = Grid::startX + col * (Grid::stepW + Grid::spacingX);
                uint16_t y = Grid::startY + row * (Grid::rowHeight + Grid::spacingY);

                // ON version
                sprintf(xstrCellOn[row][col],
                        "xstr %u,%u,%u,%u,0,%u,0,1,1,1,\"X\"\xff\xff\xff",
                        x, y, Grid::stepW, Grid::rowHeight, Grid::fgOn);

                // OFF version
                sprintf(xstrCellOff[row][col],
                        "xstr %u,%u,%u,%u,0,%u,0,1,1,1,\" \"\xff\xff\xff",
                        x, y, Grid::stepW, Grid::rowHeight, Grid::fgOff);
            }
        }
    }

    inline void drawGridCellPrebuilt(uint8_t col, uint8_t row, bool noteActive) {
        if (noteActive) {
            Display::writeCmd(xstrCellOn[row][col]);
        } else {
            Display::writeCmd(xstrCellOff[row][col]);
        }
    }

    // ------------------ PIANO ROLL ------------------
    namespace PianoRoll {
        constexpr uint16_t startX       = 100;         // Left margin
        constexpr uint16_t startY       = 121;        // Align with grid
        constexpr uint16_t width        = 38;         // Label width
        constexpr uint16_t rowHeight    = 18;         // same as grid rowHeight
        constexpr uint8_t  spacingY     = 2;
        constexpr uint16_t fgColor      = 33840;  
        constexpr uint16_t bgColor      = 0;          // Black/erase
    }

    char xstrNoteLabel[MAX_NOTES_DISPLAY][NOTE_RANGE];

    void getNoteName(uint8_t note, char* out) {
        static const char* names[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        uint8_t nameIndex = note % 12;
        int8_t octave = (note / 12) - 2;

        // Always 4 chars
        if (octave < 0) {
            sprintf(out, "%-2s%d ", names[nameIndex], octave);  // e.g., "C -2 "
        } else {
            sprintf(out, "%-2s %d", names[nameIndex], octave);  // e.g., "C  2 "
        }
    }

    void initPianoRollXSTR() {
        for (uint8_t row = 0; row < view.notesOnDisplay; row++) {
            uint16_t y = PianoRoll::startY + row * (PianoRoll::rowHeight + PianoRoll::spacingY);
            uint8_t note = view.startNote + row;

            char noteName[5];  // 4 chars + null
            getNoteName(note, noteName);

            // Prebuild XSTR command for this note label
            sprintf(xstrNoteLabel[row],
                    "xstr %u,%u,%u,%u,1,%u,0,0,1,1,\"%s\"\xff\xff\xff",
                    PianoRoll::startX, y, PianoRoll::width, PianoRoll::rowHeight, PianoRoll::fgColor, noteName);
        }
    }

    void drawPianoRoll() {
        for (uint8_t row = 0; row < view.notesOnDisplay; row++) {
            Display::writeCmd(xstrNoteLabel[row]);
        }
    }

    void scrollNotes(int8_t delta) {
        int16_t newStart = (int16_t)view.startNote + (int16_t)delta;

        // Clamp
        if (newStart < 0) newStart = 0;
        if (newStart > NOTE_RANGE - view.notesOnDisplay)
            newStart = NOTE_RANGE - view.notesOnDisplay;

        // Only update if changed
        if (newStart != view.startNote) {
            view.startNote = (uint8_t)newStart;
            viewportRedrawPending = true;   
            initPianoRollXSTR();
        }
    }

    // ------------------ PLAYHEAD ------------------
    #define PLAYHEAD_COLOR 33840	 
    #define GRID_Y 121             
    #define GRID_H 236            
    #define STEP_W 16 

    char playheadCmd[DISPLAY_STEPS][64];
    char playheadEraseCmd[DISPLAY_STEPS][64];

    void initPlayheadCmds(uint16_t stepsVisible, uint16_t gridY, uint16_t gridH) {
        for (uint16_t col = 0; col < stepsVisible; col++) {
            uint16_t x = X_OFFSET + (DISPLAY_PIXELS / stepsVisible) * col;
            uint16_t xEnd = x + (DISPLAY_PIXELS / stepsVisible) - 2;

            // Draw playhead
            sprintf(playheadCmd[col],
                    "draw %u,%u,%u,%u,%u\xff\xff\xff",
                    x, gridY, xEnd, gridY + gridH, PLAYHEAD_COLOR);

            // Erase playhead
            sprintf(playheadEraseCmd[col],
                    "draw %u,%u,%u,%u,0\xff\xff\xff",
                    x, gridY, xEnd, gridY + gridH);
        }
    }

    void movePlayheadColumns(int8_t delta) {
        uint32_t ticksPerColumn = getTicksPerColumn();
        int32_t newTick = (int32_t)playheadTick + (int32_t)delta * ticksPerColumn;

        if (scrubMode) {
            // Clamp between 0 and max ticks
            int32_t maxTicks = getMaxTicks();
            if (newTick < 0) newTick = 0;
            if (newTick >= maxTicks) newTick = maxTicks - 1;
        } else {
            // Wrap around normally
            int32_t maxTicks = getMaxTicks();
            while (newTick < 0)        newTick += maxTicks;
            while (newTick >= maxTicks) newTick -= maxTicks;
        }

        playheadTick = (uint32_t)newTick;
        updateSequencerDisplay(playheadTick);
    }

    void updatePlayhead(uint32_t stepIndex) {
        int16_t newPhCol = (playheadTick - view.startStep * TICKS_PER_STEP) / getTicksPerColumn();

        if (lastPhStep >= 0 && lastPhStep < DISPLAY_STEPS) {
            Display::writeCmd(playheadEraseCmd[lastPhStep]);
        }

        if (newPhCol >= 0 && newPhCol < DISPLAY_STEPS) {
            Display::writeCmd(playheadCmd[newPhCol]);
            lastPhStep = newPhCol;
        } else {
            lastPhStep = -1;
        }
    }

    // ------------------ ZOOM ------------------
    void setZoom(ZoomLevel z) {
        // Erase playhead BEFORE changing zoom
        if (lastPhStep >= 0 && lastPhStep < DISPLAY_STEPS) {
            Display::writeCmd(playheadEraseCmd[lastPhStep]);
            lastPhStep = -1;
        }

        // Apply zoom
        zoomLevel = z;
        const uint8_t bars = static_cast<uint8_t>(z);
        initView(bars);
        alignViewportToPlayhead(playheadTick / TICKS_PER_STEP);
        updatePlayhead(playheadTick / TICKS_PER_STEP);

        // Update zoom label
        const char* txt = "X?";
        switch (z) {
            case ZoomLevel::X0:   txt = "X0"; break;
            case ZoomLevel::X1:   txt = "X1";  break;
            case ZoomLevel::X2:   txt = "X2";  break;
            case ZoomLevel::X4:   txt = "X4";  break;
        }
        Display::writeStr("zoom.txt", txt);
        viewportRedrawPending = true;
    }

    void cycleZoom(int8_t dir) {
        static const ZoomLevel zooms[] = {
            ZoomLevel::X0,
            ZoomLevel::X1,
            ZoomLevel::X2,
            ZoomLevel::X4
        };

        int idx = 0;
        for (int i = 0; i < 4; i++)
            if (zooms[i] == zoomLevel) idx = i;

        idx = constrain(idx + dir, 0, 3);
        setZoom(zooms[idx]);
    }
    
    // ------------------ COUNTER ------------------
    void counter(uint32_t playTick) {

        uint32_t bar = playTick / TICKS_PER_BAR;
        uint32_t tickInBar = playTick % TICKS_PER_BAR;
        uint8_t beat = tickInBar / (TICKS_PER_BAR / BEATS_PER_BAR);
        Display::writeNum("bars.val", bar + 1);
        Display::writeNum("step4.val", beat + 1);

        // --- zoom-aware step counter ---
        float barsOnDisplay = view.barsOnDisplay;
        uint16_t stepsPerBar = (uint16_t)(DISPLAY_STEPS / barsOnDisplay);
        uint32_t ticksPerStep = TICKS_PER_BAR / stepsPerBar;

        uint16_t stepInBar = tickInBar / ticksPerStep;
        if (stepInBar >= stepsPerBar)
            stepInBar = stepsPerBar - 1;

        Display::writeNum("step16.val", stepInBar + 1);
    }

    namespace BarRuler {
        constexpr uint16_t startY   = 100;   // above grid (GRID_Y = 121)
        constexpr uint16_t height   = 12;
        constexpr uint16_t fgColor  = 33840;
    }

    void drawBarRuler() {
        uint32_t viewStartTick = view.startStep * TICKS_PER_STEP;
        uint32_t viewTicks     = TICKS_PER_BAR * view.barsOnDisplay;
        uint32_t viewEndTick   = viewStartTick + viewTicks;

        uint32_t firstBar = viewStartTick / TICKS_PER_BAR;
        uint32_t lastBar  = (viewEndTick - 1) / TICKS_PER_BAR;

        uint32_t maxBar = getSeqLength() - 1;
        if (lastBar > maxBar) lastBar = maxBar;

        // Clear ruler area
        char clearCmd[64];
        sprintf(clearCmd,
            "fill %u,%u,%u,%u,0\xff\xff\xff",
            X_OFFSET, BarRuler::startY,
            DISPLAY_PIXELS, BarRuler::height);
        Display::writeCmd(clearCmd);

        // Draw bar numbers
        for (uint32_t bar = firstBar; bar <= lastBar; bar++) {

            uint32_t barTick = bar * TICKS_PER_BAR;

            float norm = float(barTick - viewStartTick) / float(viewTicks);
            uint16_t x = X_OFFSET + norm * DISPLAY_PIXELS;

            char cmd[64];
            sprintf(cmd,
                "xstr %u,%u,20,%u,1,%u,0,0,1,1,\"%lu\"\xff\xff\xff",
                x,
                BarRuler::startY,
                BarRuler::height,
                BarRuler::fgColor,
                bar + 1);

            Display::writeCmd(cmd);
        }
    }

    // ------------------ GRID UPDATE ------------------
    bool hasTrigInRange(uint8_t note,uint32_t startTick, uint32_t endTick){
        for (uint32_t t = startTick; t < endTick && t < getMaxTicks(); t++) {
            Tick &tick = pattern[t];
            for (uint8_t e = 0; e < tick.count; e++) {
                if (tick.events[e].note == note &&
                    tick.events[e].vel > 0) {
                    return true;  // NOTE-ON found
                }
            }
        }
        return false;
    }

    void processDisplay() {
        if (!viewportRedrawPending) return;
        viewportRedrawPending = false;

        drawPianoRoll();
        drawBarRuler();
        
        uint32_t ticksPerColumn =
            (TICKS_PER_BAR * view.barsOnDisplay) / DISPLAY_STEPS;

        uint32_t viewStartTick = view.startStep * TICKS_PER_STEP;

        for (uint8_t col = 0; col < DISPLAY_STEPS; col++) {

            uint32_t colStart = viewStartTick + col * ticksPerColumn;
            uint32_t colEnd   = colStart + ticksPerColumn;

            for (uint8_t row = 0; row < view.notesOnDisplay; row++) {
                uint8_t note = view.startNote + row;

                bool active = hasTrigInRange(note, colStart, colEnd);

                uint16_t cellIndex = col + row * DISPLAY_STEPS;
                if (lastCellState[cellIndex] != active) {
                    drawGridCellPrebuilt(col, row, active);
                    lastCellState[cellIndex] = active;
                }
            }
        }
    }

    // ------------------ VIEWPORT ALIGNMENT ------------------
    void alignViewportToPlayhead(uint32_t stepIndex) {
        if (transport == PREROLL && prerollActive) return;

        // Steps visible in viewport (supports fractional bars)
        uint16_t stepsInView = (uint16_t)(STEPS_PER_BAR * view.barsOnDisplay);

        // Snap viewport to multiples of stepsInView
        uint32_t newStartStep = (stepIndex / stepsInView) * stepsInView;

        // Clamp to pattern length
        if (newStartStep + stepsInView > getTotalSteps()) {
            newStartStep = getTotalSteps() - stepsInView;
        }

        if (newStartStep != view.startStep) {
            view.startStep = newStartStep;
            viewportRedrawPending = true;
        }
    }

    // ------------------ DISPLAY UPDATE ------------------
    void updateSequencerDisplay(uint32_t playTick) {
        uint16_t stepIndex = playTick / TICKS_PER_STEP;

        alignViewportToPlayhead(stepIndex);
        viewportRedrawPending = true;

        updatePlayhead(stepIndex);
        counter(playTick); 

    }

    // ----------------------------------------------------------------------------------//
    //                                      INIT                                         //
    // ----------------------------------------------------------------------------------//

    void init() {
        initGridXSTR();
        initPlayheadCmds(Grid::stepsVisible, GRID_Y, GRID_H);
        playheadTick = 0;
        lastPhStep = -1;
        viewportRedrawPending = true;
        // CLOCK 
        uClock.setOutputPPQN(uClock.PPQN_96); 
        uClock.setOnOutputPPQN(onTick);
        uClock.setOnStep(onStep);          // for 16th-step display updates
        uClock.setOnClockContinue(handleClockContinue);
        uClock.setTempo(120);
        uClock.init();
    }

    // --------------- TEST PATTERN --------------
    void testPatternGumball(float noteLengthFraction = 0.95f) {

        clearPattern();

        const uint32_t stepLen = TICKS_PER_STEP;
        const uint32_t noteDur = stepLen * 2 * noteLengthFraction; // 8th notes

        // -------- Melody (8 steps per bar) --------
        const uint8_t melody[] = {
            67, 69, 71, 69, 67, 69, 71, 74,
            71, 69, 67, 69, 71, 74, 71, 69
        };

        // -------- Bass --------
        const uint8_t bass[] = {
            43, 50, 43, 50,  // G2 D3
            40, 47, 40, 47,  // E2 B2
            48, 43, 48, 43,  // C3 G2
            50, 45, 50, 45   // D3 A2
        };

        const uint32_t totalSteps = 16 * 8; // 8 bars

        for (uint32_t step = 0; step < totalSteps; step += 2) {
            uint32_t tickOn = step * stepLen;
            if (tickOn >= getMaxTicks()) break;

            uint8_t m = melody[(step / 2) % 16];
            uint8_t b = bass[(step / 2) % 16];

            // NOTE ON
            Tick &tOn = pattern[tickOn];
            if (tOn.count < NUM_VOICES)
                tOn.events[tOn.count++] = { m, defaultVelocity };
            if (tOn.count < NUM_VOICES)
                tOn.events[tOn.count++] = { b, (uint8_t)(defaultVelocity * 0.6f) };

            // NOTE OFF
            uint32_t tickOff = tickOn + noteDur;
            if (tickOff < getMaxTicks()) {
                Tick &tOff = pattern[tickOff];
                if (tOff.count < NUM_VOICES) tOff.events[tOff.count++] = { m, 0 };
                if (tOff.count < NUM_VOICES) tOff.events[tOff.count++] = { b, 0 };
            }
        }
    }



} // namespace Sequencer