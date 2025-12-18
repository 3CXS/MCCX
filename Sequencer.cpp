#include "Sequencer.h"
#include "Display.h"

#define ATOMIC(X) noInterrupts(); X; interrupts();

static constexpr uint8_t PREROLL_BEATS = 4;
static constexpr uint32_t PREROLL_TICKS = PREROLL_BEATS * PPQN;
static uint16_t lastPhPos = 0xFFFF;
static uint8_t pianoStartNote = 36; // C2 default

namespace Sequencer {

    static float bpm = 120.0f;
    static constexpr float BPM_MIN = 40.0f;
    static constexpr float BPM_MAX = 300.0f;

    float getBPM() { return bpm; }

    void setBPM(float v) {
        bpm = constrain(v, BPM_MIN, BPM_MAX);
        uClock.setTempo(bpm);
        Display::writeNum("bpm.val", bpm);
    }

    static uint8_t defaultVelocity = 120;

    uint8_t getDefaultVelocity() {
        return defaultVelocity;
    }

    void setDefaultVelocity(uint8_t v) {
        defaultVelocity = constrain(v, 1, 127);
    }

    volatile uint32_t playheadTick = 0;  // current tick within pattern
    volatile uint32_t tickOffset = 0;    // offset to align playhead after preroll

    bool isPlaying = false;
    bool isRecording = false;
    bool scrubMode = false;

    uint32_t prerollTick = 0;
    bool prerollActive = false;

    Tick pattern[TICKS_PER_PATTERN];
    bool stepHasEvent[TICKS_PER_PATTERN]; // true if any tick in the step has an event
    ViewPort view;
    uint8_t lastCellState[MAX_VISIBLE_STEPS];
    char stepObjName[MAX_VISIBLE_STEPS][12];

    EasyNex* display = nullptr;
    TransportState transport = STOPPED;

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
        uint32_t patternTick = (tick - tickOffset) % TICKS_PER_PATTERN;
        playheadTick = patternTick;

        // Play scheduled notes for this tick
        Tick* t = &pattern[patternTick];
        for (uint8_t i = 0; i < t->count; i++) {
            AudioEngine::pushPending(t->events[i].note, t->events[i].vel);
        }
        // METRO WHILE PLAY
        if (isRecording) {
            AudioEngine::Metro(patternTick);
        }
    }

    void onStep(uint32_t stepIndex) {
        updateSequencerDisplay(playheadTick);
    }

    void handleClockContinue() { 
        scrubMode = false; 
    }

    // ------------------ RECORD ------------------
    void recordNoteEvent(uint8_t note, uint8_t vel) {
        uint32_t tick;
        ATOMIC ( tick = playheadTick );

        Tick &t = pattern[tick % TICKS_PER_PATTERN];

        // Only add if we haven't exceeded NUM_VOICES events
        if (t.count < NUM_VOICES) {
            t.events[t.count++] = { note, vel };
        } 
        // Update stepHasEvent
        markStepEvent(tick, vel);
        // Refresh display
        updateSequencerDisplay(playheadTick);
    }

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

    void onRecord() {
        isRecording = true;
        playheadTick = 0;
        //allNotesOff();
        clearPattern();
        transport = STOPPED;
        updateSequencerDisplay(playheadTick);
        Display::writeStr("rec.txt", "REC");
    }

    // ------------------ PATTERN ------------------
    void clearPattern() { 
        for (uint32_t i = 0; i < TICKS_PER_PATTERN; i++) { 
            pattern[i].count = 0; 
            for (uint8_t e = 0; e < NUM_VOICES; e++) {
                pattern[i].events[e].note = 0; 
                pattern[i].events[e].vel  = 0; 
            }
        }
        // Reset step event flags
        for (uint16_t i = 0; i < TOTAL_STEPS; i++) {
            stepHasEvent[i] = false;
        }
    }

    void clearpatterndisplay() { 
      for(uint16_t i=0;i<32;++i){ 
        char objName[12]; 
        int objIndex=i; 
        if(objIndex>9999)objIndex=9999; 
        snprintf(objName,sizeof(objName),"p%04d.txt",objIndex); 
        Display::writeStr(objName," "); } }

    // ------------------ VIEWPORT ------------------
    void initView(uint16_t zoomBars){
        if(zoomBars<1) zoomBars=1; if(zoomBars>NUMBER_OF_BARS) zoomBars=NUMBER_OF_BARS;
        view.barsOnDisplay=zoomBars;
        view.steps=DISPLAY_STEPS_PER_BAR*zoomBars;
        view.startStep=0;
        for(uint16_t i=0;i<view.steps;i++) lastCellState[i]=0xFF;
        lastPhPos=0xFFFF;
    }

    void alignViewportToPlayhead(uint32_t stepIndex){
        // Don't scroll viewport during preroll
        if(transport == PREROLL && prerollActive) return;

        uint16_t stepsPerPage = DISPLAY_STEPS_PER_BAR * view.barsOnDisplay;
        uint32_t newStartStep = (stepIndex / stepsPerPage) * stepsPerPage;
        if(newStartStep + stepsPerPage > TOTAL_STEPS) newStartStep = TOTAL_STEPS - stepsPerPage;

        if(newStartStep != view.startStep){ 
            view.startStep = newStartStep; 
            for(uint16_t i=0;i<view.steps;i++) lastCellState[i]=0xFF; 
            lastPhPos=0xFFFF; 
        }
    }

    void markStepEvent(uint32_t tick, uint8_t vel) {
        uint32_t step = tick / TICKS_PER_STEP;
        if(step < TOTAL_STEPS) stepHasEvent[step] = (vel > 0); 
    }

    static inline bool tickHasNote(const Tick& t, uint8_t note) {
        for (uint8_t i = 0; i < t.count; i++) {
            if (t.events[i].note == note && t.events[i].vel > 0) {
                return true;
            }
        }
        return false;
    }

    // ---------------------- DISPLAY UPDATE ----------------------

    #define PLAYHEAD_COLOR 48631  // light gray (RGB565)
    #define GRID_Y 122             // top of your grid
    #define GRID_H 236            // total height of 12 rows, adjust as needed
    #define STEP_W 12 

    void drawPlayhead(uint16_t columnIndex) {
        uint16_t x = columnIndex;  // horizontal position

        String cmd = "draw ";
        cmd += x;
        cmd += ",";
        cmd += GRID_Y;
        cmd += ",";
        cmd += x + STEP_W;
        cmd += ",";
        cmd += GRID_Y + GRID_H;
        cmd += ",";
        cmd += PLAYHEAD_COLOR;

        Display::writeCmd(cmd.c_str());
    }

    void erasePlayhead(uint16_t columnIndex) {
        uint16_t x = columnIndex;

        String cmd = "draw ";
        cmd += x;
        cmd += ",";
        cmd += GRID_Y;
        cmd += ",";
        cmd += x + STEP_W;
        cmd += ",";
        cmd += GRID_Y + GRID_H;
        cmd += ",";
        cmd += 0;  // background color

        Display::writeCmd(cmd.c_str());

        // redraw notes under this column if needed
        redrawStepColumn(columnIndex);
    }

    void redrawStepColumn(uint16_t columnIndex) {
        const uint8_t numRows = 12;
        const uint16_t startX = X_OFFSET;
        const uint16_t startY = 121;
        const uint16_t rowHeight = 20;
        const uint8_t spacingY = 2;
        const uint16_t stepW = DISPLAY_PIXELS / MAX_VISIBLE_STEPS;

        for (uint8_t row = 0; row < numRows; row++) {
            uint8_t note = pianoStartNote + row;
            if (note >= 128) break;

            uint16_t y = startY + row * (rowHeight + spacingY);
            uint16_t step = view.startStep + columnIndex;
            if (step >= TOTAL_STEPS) continue;

            uint32_t tickIndex = (step * TICKS_PER_STEP) % TICKS_PER_PATTERN;

            if (tickHasNote(pattern[tickIndex], note)) {
                uint16_t x = startX + columnIndex * stepW;
                Display::writeXString(
                    x, y,
                    stepW, rowHeight,
                    0, 65535, 0, 0, 0, 3,
                    "X"
                );
            }
        }
    }

    int16_t lastPhStep = -1;        // step index of previous playhead
    uint16_t lastPhStartNote = 0;   // top note of previous viewport
    uint16_t lastViewStartStep = 0; 
    
    void drawPianoPattern(uint8_t firstNote, uint8_t numNotes) {
        const uint8_t stepsVisible = MAX_VISIBLE_STEPS;
        const uint16_t startX = X_OFFSET;
        const uint16_t startY = 121;
        const uint16_t rowHeight = 20;
        const uint8_t spacingY = 2;
        const uint16_t stepW = DISPLAY_PIXELS / stepsVisible;

        for (uint8_t row = 0; row < numNotes; row++) {
            uint8_t note = firstNote + row;
            if (note >= 128) break;

            uint16_t y = startY + row * (rowHeight + spacingY);

            for (uint8_t s = 0; s < stepsVisible; s++) {
                uint16_t step = view.startStep + s;
                if (step >= TOTAL_STEPS) break;

                uint32_t tickIndex = (step * TICKS_PER_STEP) % TICKS_PER_PATTERN;

                if (tickHasNote(pattern[tickIndex], note)) {
                    uint16_t x = startX + s * stepW;
                    Display::writeXString(
                        x, y,
                        stepW, rowHeight,
                        0,          // font
                        65535,      // white
                        0,          // bg color (ignored)
                        0, 0, 3,    // align left/top, no fill (transparent)
                        "X"
                    );
                }
            }
        }
    }

    /*
    void updatePianoPlayhead(uint16_t stepIndex, uint8_t startNote) {
        const uint16_t numRows = 12;
        const uint16_t startX = 143;
        const uint16_t rowHeight = 24;
        const uint8_t spacingY = 2;

        // Erase old playhead if visible in previous viewport
        if (lastPhStep >= 0) {
            int16_t oldStepIndex = lastPhStep;
            int16_t oldPhIndex = lastPhStep  - lastViewStartStep;
            uint16_t oldPhX = X_OFFSET + (DISPLAY_PIXELS / view.steps) * oldPhIndex;
            for (uint8_t i = 0; i < numRows; i++) {
                uint16_t oldPhY = 341 + i * (rowHeight + spacingY);
                erasePlayhead(oldPhX);  // implement erase for rectangle
            }
        }

        // Draw new playhead across all visible rows
        int phIndex = stepIndex - view.startStep;

        for (uint8_t i = 0; i < numRows; i++) {
            uint16_t phX = X_OFFSET + (DISPLAY_PIXELS / view.steps) * phIndex;
            uint16_t phY = 341 + i * (rowHeight + spacingY);
            drawPlayhead(phX);           // implement draw rectangle
        }

        lastPhStep = stepIndex;
        lastPhStartNote = startNote;
    }
    */

    void updatePlayhead(uint16_t stepIndex) {
        // erase old playhead if it was visible in the previous viewport
        if (lastPhStep  >= 0) {
            int16_t oldPhIndex = lastPhStep  - lastViewStartStep;
            if (oldPhIndex >= 0 && oldPhIndex < view.steps) {
                uint16_t oldPhX = X_OFFSET + (DISPLAY_PIXELS / view.steps) * oldPhIndex;
                erasePlayhead(oldPhX);
            }
        }

        // draw new playhead if within current viewport
        int phIndex = stepIndex - view.startStep;
        if (phIndex >= 0 && phIndex < view.steps) {
            uint16_t phX = X_OFFSET + (DISPLAY_PIXELS / view.steps) * phIndex;
            drawPlayhead(phX);
            lastPhStep = stepIndex;             // store absolute step
            lastViewStartStep = view.startStep; // store viewport start
        } else {
            lastPhStep = -1;                    // not visible
        }
    }

    void updateSequencerDisplay(uint32_t playTick) {
        uint16_t stepIndex = playTick / TICKS_PER_STEP;

        // 1️⃣ Update horizontal viewport if needed
        alignViewportToPlayhead(stepIndex);

        // 2️⃣ Draw piano pattern (triggers) for visible rows
        drawPianoPattern(pianoStartNote, 12);

        // 3️⃣ Update playhead
        int phIndex = stepIndex - view.startStep;

        // Erase old playhead if visible in previous viewport
        if (lastPhStep >= 0) {
            int16_t oldPhIndex = lastPhStep - lastViewStartStep;
            if (oldPhIndex >= 0 && oldPhIndex < view.steps) {
                uint16_t oldPhX = X_OFFSET + oldPhIndex * STEP_W;
                erasePlayhead(oldPhX);
            }
        }

        // Draw new playhead if within current viewport
        if (phIndex >= 0 && phIndex < view.steps) {
            uint16_t phX = X_OFFSET + phIndex * STEP_W;
            drawPlayhead(phX);
            lastPhStep = stepIndex;
            lastViewStartStep = view.startStep;
        } else {
            lastPhStep = -1;
        }

        // 4️⃣ Update step/bar counters
        uint32_t bar = stepIndex / STEPS_PER_BAR;
        uint8_t beat = (stepIndex / (STEPS_PER_BAR / BEATS_PER_BAR)) % BEATS_PER_BAR;
        uint16_t stepInBar = stepIndex % STEPS_PER_BAR;

        Display::writeNum("bars.val", bar + 1);
        Display::writeNum("step4.val", beat + 1);
        Display::writeNum("step16.val", stepInBar + 1);
    }



    // ------------------ INIT ------------------
    void init( uint16_t zoomBars) {

        initView(zoomBars);
        //initDisplayObjectNames(MAX_VISIBLE_STEPS);
        clearPattern();
        clearpatterndisplay();

        // CLOCK 
        uClock.setOutputPPQN(uClock.PPQN_96); 
        uClock.setOnOutputPPQN(onTick);
        uClock.setOnStep(onStep);          // for 16th-step display updates
        uClock.setOnClockContinue(handleClockContinue);
        uClock.setTempo(120);
        uClock.init();

    }

    // --------------- TEST PATTERN --------------
    void testPattern(float noteLengthFraction = 0.5f) {
        clearPattern();

        uint32_t noteDuration = (uint32_t)(PPQN * noteLengthFraction);

        for (uint32_t bar = 0; bar < NUMBER_OF_BARS; bar++) {
            for (uint8_t beat = 0; beat < BEATS_PER_BAR; beat++) {

                uint32_t tickOn =
                    bar * BEATS_PER_BAR * PPQN +
                    beat * PPQN;

                if (tickOn >= TICKS_PER_PATTERN) continue;

                // --- NOTE ON ---
                Tick &tOn = pattern[tickOn];
                if (tOn.count < NUM_VOICES) {
                    uint8_t note = 48 + beat;
                    tOn.events[tOn.count++] = { note, defaultVelocity };

                    // mark step directly
                    uint32_t step = tickOn / TICKS_PER_STEP;
                    if (step < TOTAL_STEPS)
                        stepHasEvent[step] = true;

                    // --- NOTE OFF ---
                    uint32_t tickOff = tickOn + noteDuration;
                    if (tickOff < TICKS_PER_PATTERN) {
                        Tick &tOff = pattern[tickOff];
                        if (tOff.count < NUM_VOICES) {
                            tOff.events[tOff.count++] = { note, 0 };
                        }
                    }
                }
            }
        }

        playheadTick = 0;
        alignViewportToPlayhead(0);
        updateSequencerDisplay(playheadTick);
    }

    void testPattern16th(float noteLengthFraction = 0.8f)
    {
        clearPattern();

        uint32_t noteDuration =
            (uint32_t)(TICKS_PER_STEP * noteLengthFraction);

        for (uint32_t step = 0; step < TOTAL_STEPS; step++) {

            uint32_t tickOn = step * TICKS_PER_STEP;
            if (tickOn >= TICKS_PER_PATTERN) break;

            uint8_t note = 48 + (step % 16);   // cycle notes per bar

            // --- NOTE ON ---
            Tick &tOn = pattern[tickOn];
            if (tOn.count < NUM_VOICES) {
                tOn.events[tOn.count++] = { note, defaultVelocity };
                stepHasEvent[step] = true;
            }

            // --- NOTE OFF ---
            uint32_t tickOff = tickOn + noteDuration;
            if (tickOff < TICKS_PER_PATTERN) {
                Tick &tOff = pattern[tickOff];
                if (tOff.count < NUM_VOICES) {
                    tOff.events[tOff.count++] = { note, 0 };
                }
            }
        }

        playheadTick = 0;
        alignViewportToPlayhead(0);
        updateSequencerDisplay(playheadTick);
    }

} // namespace Sequencer