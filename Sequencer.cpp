#include "Sequencer.h"
#include "Display.h"

#define ATOMIC(X) noInterrupts(); X; interrupts();

namespace Sequencer {

    Sequence sequences[MAX_SEQUENCES];

    uint8_t currentSequence = 0;
    uint8_t currentTrack    = 0;

    //--- PATTERN STORAGE --- //
    static DMAMEM Event trackEventPool[MAX_PATTERN_SLOTS][MAX_EVENTS_PER_PATTERN];
    static bool patternSlotUsed[MAX_PATTERN_SLOTS];

    bool stepHasEvent[MAX_PATTERN_TICKS]; // true if any tick in the step has an event
    bool stepNoteHasEvent[MAX_PATTERN_STEPS][NOTE_RANGE];  // absolute MIDI notes

    int8_t allocPatternSlot() {
        for (uint8_t i = 0; i < MAX_PATTERN_SLOTS; i++) {
            if (!patternSlotUsed[i]) {
                patternSlotUsed[i] = true;
                memset(trackEventPool[i], 0,
                    sizeof(trackEventPool[i]));
                return i;
            }
        }
        return -1;
    }

    void freePatternSlot(int8_t slot) {
        if (slot < 0 || slot >= MAX_PATTERN_SLOTS) return;
        patternSlotUsed[slot] = false;
    }

    //--- TRACK --- //
    const char* trackTypeToStr(uint8_t type) {
        switch(type) {
            case SYNTH:   return "SYNTH";
            case SAMPLER: return "SAMP";
            //case GRANULAR:return "GRAIN";
            //case PERC:    return "PERCN";
            default:            return "UNKNOWN";
        }
    }

    void setTrackType(TrackType type) {
        uint8_t t = getCurrentTrack();
        if (t >= MAX_TRACKS) return;

        Track &trk = curSeq().tracks[t];

        if (trk.type == type) return;

        trk.type = type;

        AudioEngine::allNotesOff();
        Display::writeStr("ttrack.txt", trackTypeToStr(type));
    }


    void setCurrentTrack(uint8_t t) {
        if (t >= MAX_TRACKS) return;
        currentTrack = t;
        Track& tr = curTrack();

        Display::writeNum("ntrack.val", currentTrack + 1);
        Display::writeStr("ttrack.txt", trackTypeToStr(tr.type));

        // Auto-create track if not active
        if (!tr.active) {
            initTrack(tr, currentTrack); // will allocate pattern slot
        } else if (tr.pattern.events == nullptr) {
            // Track active but no pattern allocated (possible after clear)
            initPattern(tr);
        }

        // Update mute indicator
        uint32_t color = tr.mute ? 65535 : 33808;  
        Display::writeNum("mute.pco", color);

        viewportRedrawPending = true;
    }

    uint8_t getCurrentTrack() {return currentTrack;}

    bool trackHasPatternData(uint8_t trackIndex) {
        if (trackIndex >= MAX_TRACKS) return false;
        const auto &tr = curSeq().tracks[trackIndex];  // use curSeq() to access sequences
        const auto &pat = tr.pattern;
        if (pat.count > 0) return true;   // any events in this pattern
        return false;  // empty track
    }

    void toggleTrackMute(uint8_t track) {
        auto &tr = curSeq().tracks[track];
        tr.mute = !tr.mute;

        if (tr.mute) {
            AudioEngine::muteTrack(track);
        }
        uint32_t color = tr.mute ? 65535 : 33808;
        Display::writeNum("mute.pco", color);
    }

    bool isTrackMuted(uint8_t track) {
        if (track >= MAX_TRACKS) return false;
        return curSeq().tracks[track].mute;
    }

    // SEQ LENGTH
    uint8_t seqLength = 4; // default
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
    static uint8_t pianoStartNote = 48; // C2 default
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
    
    enum class RecordMode {NORMAL, OVERDUB,};
    RecordMode currentRecordMode = RecordMode::NORMAL;

   // Time Division
    uint32_t divisionToTicks(TimingDivision rate) {
        switch(rate) {
            case TimingDivision::QUARTER:       return PPQN;
            case TimingDivision::EIGHTH:        return PPQN / 2;
            case TimingDivision::SIXTEENTH:     return PPQN / 4;
            case TimingDivision::SIXTEENTHT:    return PPQN / 6;
            case TimingDivision::THIRTYSECOND:  return PPQN / 8;
            case TimingDivision::THIRTYSECONDT: return PPQN / 12;
            default:                            return PPQN / 4;
        }
    }

    inline const char* divisionToLabel(TimingDivision rate) {
        switch(rate) {
            case TimingDivision::QUARTER:       return "1/4";
            case TimingDivision::EIGHTH:        return "1/8";
            case TimingDivision::SIXTEENTH:     return "1/16";
            case TimingDivision::SIXTEENTHT:    return "1/16T";
            case TimingDivision::THIRTYSECOND:  return "1/32";
            case TimingDivision::THIRTYSECONDT: return "1/32T";
            default:                            return "1/8";
        }
    }

    // QUANTIZE
    static bool quantizeEnabled = false;
    static TimingDivision quantizeDivision = TimingDivision::SIXTEENTH;

    void setQuantizeEnabled(bool on) {
        quantizeEnabled = on;
        Display::writeStr("quant.txt", on ? divisionToLabel(quantizeDivision) : "OFF");
    }

    void setQuantizeDivision(TimingDivision rate) {
        quantizeDivision = rate;
        if (quantizeEnabled) {
            Display::writeStr("quant.txt", divisionToLabel(rate));
        }
    }

    bool isQuantizeEnabled() {
        return quantizeEnabled;
    }

    uint32_t getQuantizeTicks() {
        return quantizeEnabled ? divisionToTicks(quantizeDivision) : 1;
    }

    // NOTE REPEAT
    TimingDivision noteRepeatRate = TimingDivision::EIGHTH;

    void setRepeatDivision(TimingDivision div) {
        noteRepeatRate = div;
        Display::writeStr("rep.txt", divisionToLabel(div));
    }

    static constexpr uint8_t NOTE_REPEAT_GATE_PERCENT = 50;
    uint32_t noteRepeatLastTick = 0;
    bool noteRepeatActive       = false;
    uint8_t noteRepeatNote      = 0;
    uint32_t noteRepeatInterval = 0;
    uint32_t noteRepeatNextTick = 0;
    uint32_t noteRepeatNextTickOff = 0;
    uint32_t noteRepeatDurationTicks = 0;
    bool noteRepeatNoteOn       = false;

    NoteRepeatVoice repeatVoices[MAX_REPEAT_VOICES];

    void startNoteRepeat(uint8_t note) {
        uint8_t curTrackId = Sequencer::getCurrentTrack(); // latch current track

        Sequencer::noteRepeatActive = true;

        for (uint8_t i = 0; i < Sequencer::MAX_REPEAT_VOICES; i++) {
            auto &v = Sequencer::repeatVoices[i];
            if (!v.active) {
                v.active   = true;
                v.noteOn   = false;
                v.note     = note;
                v.nextTick = Sequencer::playheadTick;
                v.offTick  = 0;
                v.trackId  = curTrackId;  // store the track
                return;
            }
        }
    }

    void stopNoteRepeat(uint8_t note) {
        bool anyActive = false;

        for (uint8_t i = 0; i < Sequencer::MAX_REPEAT_VOICES; i++) {
            auto &v = Sequencer::repeatVoices[i];
            if (v.active && v.note == note) {
                if (v.noteOn) {
                    AudioEngine::noteOff(v.trackId, v.note);

                    if (Sequencer::isRecording) {
                        // record the note-off to the correct track
                        Sequencer::recordNoteEvent(v.trackId, v.note, 0);
                    }
                }
                v.active  = false;
                v.noteOn  = false;
            }
            if (v.active) anyActive = true;
        }

        if (!anyActive) {
            Sequencer::noteRepeatActive = false;
        }
    }

    void processNoteRepeat(uint32_t tick) {
        if (!noteRepeatActive) return;

        uint32_t interval = divisionToTicks(noteRepeatRate);
        if (interval == 0) return;

        for (uint8_t i = 0; i < MAX_REPEAT_VOICES; i++) {
            auto &v = repeatVoices[i];
            if (!v.active) continue;

            // NOTE ON
            if (!v.noteOn && tick >= v.nextTick) {
                AudioEngine::noteOn(v.trackId, v.note, getDefaultVelocity());
                v.noteOn = true;

                // Set duration to ~80% of interval
                uint32_t dur = interval * 8 / 10;
                v.offTick = v.nextTick + dur;
                v.nextTick += interval;

                if (isRecording)
                    recordNoteEvent(v.trackId, v.note, getDefaultVelocity()); // track-aware
            }

            // NOTE OFF
            if (v.noteOn && tick >= v.offTick) {
                AudioEngine::noteOff(v.trackId, v.note);
                v.noteOn = false;

                if (isRecording)
                    recordNoteEvent(v.trackId, v.note, 0); // track-aware
            }
        }
    }

    // ARPEGGIATOR
    TimingDivision arpRate = TimingDivision::EIGHTH;
    uint8_t numHeldNotes = 0;
    ArpVoice arpVoice = { false, false, 0, 0, 0 };
    ArpMode arpMode = ArpMode::OFF;
    float arpGate = 0.8f;

    void recalcArpTiming() {
        // This function can precompute interval ticks if needed
    }
    static constexpr uint8_t MAX_ARP_VOICES = 1; // monophonic arp output
    uint8_t arpOctaves = 3;       // octave cycle range

    void setArpMode(ArpMode mode) {
        arpMode = mode;
        const char* txt = "OFF";
        switch(mode) {
            case ArpMode::UP_OCTAVE:  txt = "UP"; break;
            case ArpMode::HELD_NOTES: txt = "HLD"; break;
            case ArpMode::OFF:         txt = "OFF"; break;
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
        if (arpMode == ArpMode::OFF)
            return;
        uint8_t curTrackId = Sequencer::getCurrentTrack(); // latch current track

        addHeldNote(note);

        if (arpVoice.active)
            return;

        arpVoice.active    = true;
        arpVoice.noteOn    = false;
        arpVoice.stepIndex = 0;
        arpVoice.nextTick  = playheadTick;  // start immediately
        arpVoice.offTick   = 0;
        arpVoice.trackId  = curTrackId; 
    }

    void stopArp(uint8_t note) {
        removeHeldNote(note);

        // If the currently playing note is related to this released note, stop it
        if (arpVoice.noteOn) {
            bool stillValid = false;

            for (uint8_t i = 0; i < numHeldNotes; i++) {
                if (arpMode == ArpMode::UP_OCTAVE) {
                    uint8_t base = heldNotes[0];
                    for (uint8_t o = 0; o < arpOctaves; o++) {
                        if (arpVoice.note == base + o * 12) {
                            stillValid = true;
                            break;
                        }
                    }
                } else { // HELD_NOTES
                    if (arpVoice.note == heldNotes[i]) {
                        stillValid = true;
                    }
                }
                if (stillValid) break;
            }

            if (!stillValid) {
                AudioEngine::noteOff(arpVoice.trackId, arpVoice.note);
                arpVoice.noteOn = false;

                if (isRecording)
                    recordNoteEvent(arpVoice.trackId, arpVoice.note, 0); // track-aware
            }
        }
    }

    // --- Get next arp note based on mode ---
    uint8_t getNextArpNote(uint8_t step) {
        if (numHeldNotes == 0) return 0; // sanity

        switch (arpMode) {
            case ArpMode::UP_OCTAVE: {
                uint8_t baseNote = heldNotes[0];
                uint8_t octave    = step % arpOctaves;
                return baseNote + octave * 12;
            }
            case ArpMode::HELD_NOTES:
                return heldNotes[step % numHeldNotes];
            default:
                return 0;
        }
    }

    void processArp(uint32_t tick) {
        if (!arpVoice.active || numHeldNotes == 0 || arpMode == ArpMode::OFF)
            return;

        uint32_t interval = divisionToTicks(arpRate);
        if (interval == 0) return;

        // --- check currently playing note ---
        if (arpVoice.noteOn) {
            bool stillHeld = false;
            for (uint8_t i = 0; i < numHeldNotes; i++) {
                uint8_t expectedNote = (arpMode == ArpMode::UP_OCTAVE) ? getNextArpNote(arpVoice.stepIndex-1) 
                                                                    : heldNotes[i];
                if (arpVoice.note == expectedNote) {
                    stillHeld = true;
                    break;
                }
            }
            if (!stillHeld) {
                AudioEngine::noteOff(arpVoice.trackId, arpVoice.note);
                arpVoice.noteOn = false;
            }
        }

        // --- NOTE ON ---
        if (!arpVoice.noteOn && tick >= arpVoice.nextTick) {
            if (numHeldNotes == 0) {
                arpVoice.active = false;
                return;
            }

            uint8_t noteToPlay = getNextArpNote(arpVoice.stepIndex);

            AudioEngine::noteOn(arpVoice.trackId, noteToPlay, getDefaultVelocity());
            arpVoice.note = noteToPlay;
            arpVoice.noteOn = true;

            arpVoice.offTick = tick + (uint32_t)(interval * arpGate);
            arpVoice.nextTick = tick + interval;
            arpVoice.stepIndex++;

            if (isRecording)
                recordNoteEvent(arpVoice.trackId, noteToPlay, getDefaultVelocity());
        }

        // --- NOTE OFF ---
        if (arpVoice.noteOn && tick >= arpVoice.offTick) {
            AudioEngine::noteOff(arpVoice.trackId, arpVoice.note);
            arpVoice.noteOn = false;

            if (isRecording)
                recordNoteEvent(arpVoice.trackId, arpVoice.note, 0);
        }
    }

    void onLoopWrap() {

        // ---------- ARP ----------
        if (arpVoice.active) {
            if (arpVoice.noteOn) {
                AudioEngine::noteOff(arpVoice.trackId, arpVoice.note);
                if (isRecording)
                    recordNoteEvent(arpVoice.trackId, arpVoice.note, 0);
            }
            arpVoice.noteOn   = false;
            arpVoice.nextTick = 0;      // relative to playhead
            arpVoice.offTick  = 0;
        }

        // ---------- NOTE REPEAT ----------
        for (uint8_t i = 0; i < MAX_REPEAT_VOICES; i++) {
            auto &v = repeatVoices[i];
            if (!v.active) continue;

            if (v.noteOn) {
                AudioEngine::noteOff(arpVoice.trackId, v.note);
                if (isRecording)
                    recordNoteEvent(arpVoice.trackId, v.note, 0);
            }

            v.noteOn   = false;
            v.nextTick = 0;
            v.offTick  = 0;
        }
    }
    
    // ----------------------------------------------------------------------------------//
    //                                      SEQUENCER                                    //
    // ----------------------------------------------------------------------------------//

    // ------------------ CLOCK ------------------
    void onTick(uint32_t tick) {
        // PREROLL handling
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
        if (patternTick < playheadTick) {
            onLoopWrap();
        }

        playheadTick = patternTick;

        for (uint8_t tr = 0; tr < MAX_TRACKS; tr++) {
            Track& track = curSeq().tracks[tr];
            if (!track.active || track.mute) continue;

            // Sparse playback
            for (uint8_t e = 0; e < track.pattern.count; e++) {
                auto& ev = track.pattern.events[e];
                if (ev.tick == patternTick) {  // only trigger events at this tick
                    AudioEngine::pushPending(tr, ev.note, ev.value);
                }
            }
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
        clearPattern(getCurrentTrack());
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

    void recordNoteEvent(uint8_t trackId, uint8_t note, uint8_t vel) {
        if (trackId >= MAX_TRACKS) return;

        uint32_t tick;
        ATOMIC(tick = playheadTick);

        // Quantize ONLY note-on
        if (vel > 0 && quantizeEnabled) {
            uint32_t qTicks = divisionToTicks(quantizeDivision);
            tick = ((tick + qTicks / 2) / qTicks) * qTicks;

            if (tick >= getMaxTicks())
                tick = getMaxTicks() - 1;
        }

        Track& track = curSeq().tracks[trackId];

        // ----------------- CRITICAL GUARD -----------------
        if (track.pattern.events == nullptr) {
            initPattern(track); // ensure slot allocated
            if (track.pattern.events == nullptr) return; // still no memory
        }

        // prevent overflow
        if (track.pattern.count >= track.pattern.maxEvents) return;

        // add event
        track.pattern.events[track.pattern.count++] =
            makeEvent(tick, note, vel);

        if (vel > 0) {
            markStepEvent(tick, note, vel);
        }

        updateSequencerDisplay(playheadTick);
    }

    void clearPattern(uint8_t track) {
        Track& tr = curSeq().tracks[track];

        // Release pattern slot if allocated
        if (tr.pattern.slotIndex >= 0) {
            freePatternSlot(tr.pattern.slotIndex);
        }
        tr.pattern = {};  // reset struct safely
        // Reset pattern event count
        tr.pattern.count = 0;

        // Reset step events
        for (uint16_t i = 0; i < getTotalSteps(); i++) {
            stepHasEvent[i] = false;
            for (uint8_t n = 0; n < NOTE_RANGE; n++)
                stepNoteHasEvent[i][n] = false;
        }

        // Clear visible cells
        for (uint8_t row = 0; row < MAX_NOTES_DISPLAY; row++) {
            for (uint8_t col = 0; col < Grid::stepsVisible; col++) {
                uint16_t cellIndex = row * Grid::stepsVisible + col;
                if (lastCellState[cellIndex]) {
                    drawGridCellPrebuilt(col, row, false);
                    lastCellState[cellIndex] = false;
                }
            }
        }

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
    bool hasTrigInRange(uint8_t note, uint32_t startTick, uint32_t endTick) {
        auto& pat = curTrack().pattern;
        for (uint16_t i = 0; i < pat.count; i++) {
            const auto& e = pat.events[i];
            if (e.tick >= endTick) break;  // sorted -> done
            if (e.tick >= startTick && e.note == note && e.value > 0) return true;
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

    void initTimingControls() {
        // Quantize OFF
        setQuantizeEnabled(false);
        setQuantizeDivision(TimingDivision::SIXTEENTH); // safe default
        // Note Repeat 1/16
        setRepeatDivision(TimingDivision::SIXTEENTH);
        // Arpeggiator OFF
        setArpMode(ArpMode::OFF);
    }

    void initTrack(Track& tr, uint8_t trackIndex) {
        tr.active   = true;
        tr.mute     = false;
        //tr.type     = SAMPLER;
        tr.midiCh   = 1;

        // Allocate a pattern slot for this track
        initPattern(tr);
    }

    void initPattern(Track& tr) {
        if (tr.pattern.slotIndex >= 0)
            return;

        int8_t slot = allocPatternSlot();
        if (slot < 0) {
            tr.pattern = {};
            return;
        }

        tr.pattern.slotIndex = slot;
        tr.pattern.events    = trackEventPool[slot];
        tr.pattern.count     = 0;
        tr.pattern.maxEvents = MAX_EVENTS_PER_PATTERN;
    }

    void init() {
        // Init sequence 0
        Sequence& seq = sequences[0];
        seq.lengthBars = seqLength;
        seq.bpm        = bpm;
        
        memset(trackEventPool, 0, sizeof(trackEventPool));
        memset(patternSlotUsed, 0, sizeof(patternSlotUsed));
        // Initialize all tracks in the sequence
        for (uint8_t t = 0; t < MAX_TRACKS; t++) {
            initTrack(curSeq().tracks[t], 0);
        }
        setCurrentTrack(0);
        // Display & playhead
        initGridXSTR();
        initPlayheadCmds(Grid::stepsVisible, GRID_Y, GRID_H);
        playheadTick = 0;
        lastPhStep = -1;
        viewportRedrawPending = true;

        // CLOCK setup
        uClock.setOutputPPQN(uClock.PPQN_96); 
        uClock.setOnOutputPPQN(onTick);
        uClock.setOnStep(onStep);
        uClock.setOnClockContinue(handleClockContinue);
        uClock.setTempo(bpm);
        uClock.init();
    }

    // --------------- TEST PATTERN --------------

    void testPatternGumball(float noteLengthFraction = 0.75f) {

        const uint32_t stepTicks = TICKS_PER_STEP;
        const uint32_t leadDur   = stepTicks * noteLengthFraction;
        const uint32_t bassDur   = stepTicks * 6 * noteLengthFraction; // dotted-ish bass

        const uint8_t leadVel = getDefaultVelocity();
        const uint8_t bassVel = getDefaultVelocity() / 2;

        const uint8_t totalBars = 4;
        const uint32_t maxTicks = getMaxTicks();

        // ---------- TRACK SETUP ----------
        Track& lead = curSeq().tracks[0];
        Track& bass = curSeq().tracks[1];

        initTrack(lead, 0);
        initTrack(bass, 1);

        lead.mute = false;
        bass.mute = false;

        clearPattern(0);
        clearPattern(1);

        // IMPORTANT: clearPattern frees memory
        initPattern(lead);
        initPattern(bass);

        // ---------- LEAD (Hybris-style pulse riff) ----------
        // D minor scale, tight repeating motif
        // D  F  G  A  C  A  G  F
        const uint8_t leadNotes[8] = {
            74, 77, 79, 81,
            84, 81, 79, 77
        };

        for (uint8_t bar = 0; bar < totalBars; bar++) {
            for (uint8_t step = 0; step < 16; step++) {

                // Leave small gaps for groove
                if (step % 4 == 3) continue;

                uint32_t globalStep = bar * STEPS_PER_BAR + step;
                uint32_t tickOn  = globalStep * stepTicks;
                uint32_t tickOff = tickOn + leadDur;

                if (tickOn >= maxTicks) continue;

                uint8_t note = leadNotes[(step + bar * 2) % 8];

                // NOTE ON
                lead.pattern.events[lead.pattern.count++] =
                    makeEvent(tickOn, note, leadVel);
                markStepEvent(tickOn, note, leadVel);

                // NOTE OFF
                if (tickOff < maxTicks) {
                    lead.pattern.events[lead.pattern.count++] =
                        makeEvent(tickOff, note, 0);
                }
            }
        }

        // ---------- BASS (classic Amiga pulse) ----------
        // Root + octave movement
        const uint8_t bassRoot = 38; // D2

        for (uint8_t bar = 0; bar < totalBars; bar++) {

            // Beat 1: root
            uint32_t step1 = bar * STEPS_PER_BAR;
            uint32_t tick1 = step1 * stepTicks;

            // Beat 3: octave
            uint32_t step2 = step1 + 8;
            uint32_t tick2 = step2 * stepTicks;

            if (tick1 < maxTicks) {
                bass.pattern.events[bass.pattern.count++] =
                    makeEvent(tick1, bassRoot, bassVel);
                markStepEvent(tick1, bassRoot, bassVel);

                uint32_t off = tick1 + bassDur;
                if (off < maxTicks)
                    bass.pattern.events[bass.pattern.count++] =
                        makeEvent(off, bassRoot, 0);
            }

            if (tick2 < maxTicks) {
                bass.pattern.events[bass.pattern.count++] =
                    makeEvent(tick2, bassRoot + 12, bassVel);
                markStepEvent(tick2, bassRoot + 12, bassVel);

                uint32_t off = tick2 + bassDur;
                if (off < maxTicks)
                    bass.pattern.events[bass.pattern.count++] =
                        makeEvent(off, bassRoot + 12, 0);
            }
        }

        viewportRedrawPending = true;
    }





} // namespace Sequencer