#include "Sequencer.h"
#include "Display.h"

#define ATOMIC(X) noInterrupts(); X; interrupts();

static constexpr uint8_t PREROLL_BEATS = 4;
static constexpr uint32_t PREROLL_TICKS = PREROLL_BEATS * PPQN;
static uint16_t lastPhPos = 0xFFFF;
static uint8_t pianoStartNote = 48; // C2 default

namespace Sequencer {

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


    bool viewportRedrawPending = true;

    Tick pattern[TICKS_PER_PATTERN];
    bool stepHasEvent[TICKS_PER_PATTERN]; // true if any tick in the step has an event
    bool stepNoteHasEvent[TOTAL_STEPS][NOTE_RANGE];  // absolute MIDI notes
    ViewPort view;
    uint8_t lastCellState[MAX_VISIBLE_STEPS * MAX_NOTES_DISPLAY];

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
        ATOMIC(tick = playheadTick);

        Tick &t = pattern[tick % TICKS_PER_PATTERN];

        // Add event if room
        if (t.count < NUM_VOICES) {
            t.events[t.count++] = { note, vel };
        }

        // Mark for display
        markStepEvent(tick, note, vel);

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

    // ------------------ VIEWPORT ------------------
    namespace Grid {
        constexpr uint16_t startX       = X_OFFSET+2;
        constexpr uint16_t startY       = 121;
        constexpr uint16_t rowHeight    = 18;
        constexpr uint8_t  spacingY     = 2;
        constexpr uint8_t  spacingX     = 4;
        constexpr uint8_t  stepsVisible = MAX_VISIBLE_STEPS;
        constexpr uint16_t stepW        = DISPLAY_PIXELS / stepsVisible - spacingX;
        constexpr uint16_t fgOn         = 65535;
        constexpr uint16_t fgOff        = 0;
    }

    // Precompute commands for all visible cells
    char xstrCellOn[MAX_NOTES_DISPLAY][Grid::stepsVisible][64];
    char xstrCellOff[MAX_NOTES_DISPLAY][Grid::stepsVisible][64];

    void initGridXSTR()
    {
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

    inline void drawGridCellPrebuilt(uint8_t col, uint8_t row, bool noteActive)
    {
        if (noteActive) {
            Display::writeCmd(xstrCellOn[row][col]);
        } else {
            Display::writeCmd(xstrCellOff[row][col]);
        }
    }

    void initView(uint16_t zoomBars){
        if(zoomBars<1) zoomBars=1; if(zoomBars>NUMBER_OF_BARS) zoomBars=NUMBER_OF_BARS;
        view.barsOnDisplay  = zoomBars;
        view.steps          = DISPLAY_STEPS_PER_BAR*zoomBars;
        view.startStep      = 1;
        view.startNote      = pianoStartNote;
        view.notesOnDisplay = MAX_NOTES_DISPLAY;
        for(uint16_t i=0;i<view.steps;i++) lastCellState[i]=0xFF;
        lastPhPos=0xFFFF;
    }

    void markStepEvent(uint32_t tick, uint8_t note, uint8_t vel)
    {
        if (vel == 0) return;

        uint32_t step = tick / TICKS_PER_STEP;
        if (step >= TOTAL_STEPS) return;

        stepHasEvent[step] = true;
        stepNoteHasEvent[step][note] = true;  // 🔥 ABSOLUTE NOTE
    }

    // ------------------ PATTERN ------------------
    void clearPattern()
    {
        // 1️⃣ Clear internal pattern data
        for (uint32_t i = 0; i < TICKS_PER_PATTERN; i++) {
            pattern[i].count = 0;
            for (uint8_t e = 0; e < NUM_VOICES; e++) {
                pattern[i].events[e].note = 0;
                pattern[i].events[e].vel  = 0;
            }
        }

        // 2️⃣ Reset step events
        for (uint16_t i = 0; i < TOTAL_STEPS; i++) {
            stepHasEvent[i] = false;
            for (uint8_t n = 0; n < NOTE_RANGE; n++)
                stepNoteHasEvent[i][n] = false;
        }

        // 3️⃣ Clear visible cells (only previously active)
        for (uint8_t row = 0; row < MAX_NOTES_DISPLAY; row++) {
            for (uint8_t col = 0; col < Grid::stepsVisible; col++) {
                uint16_t cellIndex = row * Grid::stepsVisible + col;
                if (lastCellState[cellIndex]) {
                    drawGridCellPrebuilt(col, row, false);
                    lastCellState[cellIndex] = false;
                }
            }
        }

        // 4️⃣ Reset playhead
        lastPhStep = -1;
        lastViewStartStep = view.startStep;
        updatePlayhead(view.startStep);
    }

    // ---------------------- DISPLAY UPDATE ----------------------

    #define PLAYHEAD_COLOR 33840	 
    #define GRID_Y 121             
    #define GRID_H 236            
    #define STEP_W 16 

    char playheadCmd[MAX_VISIBLE_STEPS][64];
    char playheadEraseCmd[MAX_VISIBLE_STEPS][64];

    void initPlayheadCmds(uint16_t stepsVisible, uint16_t gridY, uint16_t gridH)
    {
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

    int16_t lastPhStep = -1;        // step index of previous playhead
    uint16_t lastPhStartNote = 0;   // top note of previous viewport
    uint16_t lastViewStartStep = 0; 
    
    void updatePlayhead(uint16_t stepIndex)
    {
        int16_t oldPhIndex = lastPhStep - lastViewStartStep;
        int16_t newPhIndex = stepIndex - view.startStep;

        // Erase old playhead if visible
        if (oldPhIndex >= 0 && oldPhIndex < view.steps) {
            Display::writeCmd(playheadEraseCmd[oldPhIndex]);
        }

        // Draw new playhead if visible
        if (newPhIndex >= 0 && newPhIndex < view.steps) {
            Display::writeCmd(playheadCmd[newPhIndex]);
            lastPhStep = stepIndex;
            lastViewStartStep = view.startStep;
        } else {
            lastPhStep = -1;
        }
    }

    void scrollNotes(int8_t delta)
    {
        int16_t newStart = (int16_t)view.startNote + delta;

        // Clamp to MIDI range
        if (newStart < 0) newStart = 0;
        if (newStart > 127 - view.notesOnDisplay)
            newStart = 127 - view.notesOnDisplay;

        if (newStart != view.startNote) {
            view.startNote = newStart;
            viewportRedrawPending = true;   
            //lastPhStep = -1;  // --> redraw Playhead
        }
    }

    void ensureNoteVisible(uint8_t note)
    {
        if (note < view.startNote)
            scrollNotes(note - view.startNote);
        else if (note >= view.startNote + view.notesOnDisplay)
            scrollNotes(note - (view.startNote + view.notesOnDisplay - 1));
    }

    void alignViewportToPlayhead(uint32_t stepIndex)
    {
        if (transport == PREROLL && prerollActive) return;

        uint16_t stepsPerPage = DISPLAY_STEPS_PER_BAR * view.barsOnDisplay;
        uint32_t newStartStep = (stepIndex / stepsPerPage) * stepsPerPage;

        if (newStartStep + stepsPerPage > TOTAL_STEPS)
            newStartStep = TOTAL_STEPS - stepsPerPage;

        if (newStartStep != view.startStep) {

            view.startStep = newStartStep;
            viewportRedrawPending = true;
        }
    }

    void counter(uint32_t stepIndex) {
        uint32_t bar       = stepIndex / STEPS_PER_BAR;
        uint8_t  beat      = (stepIndex / (STEPS_PER_BAR / BEATS_PER_BAR)) % BEATS_PER_BAR;
        uint16_t stepInBar = stepIndex % STEPS_PER_BAR;

        Display::writeNum("bars.val", bar + 1);
        Display::writeNum("step4.val", beat + 1);
        Display::writeNum("step16.val", stepInBar + 1);
    }

    void processDisplay() {

        if (!viewportRedrawPending)
            return;

        viewportRedrawPending = false;

        const uint8_t stepsVisible = view.steps;

        for (uint8_t row = 0; row < MAX_NOTES_DISPLAY; row++) {

            for (uint8_t col = 0; col < stepsVisible; col++) {

                uint16_t step = view.startStep + col;
                if (step >= TOTAL_STEPS) break;

                uint8_t note = view.startNote + row;
                bool noteActive = stepNoteHasEvent[step][note];

                uint16_t cellIndex = col + row * stepsVisible;

                if (lastCellState[cellIndex] != noteActive) {
                    drawGridCellPrebuilt(col, row, noteActive);
                    lastCellState[cellIndex] = noteActive;
                }
            }
        }
        
    }

    void updateSequencerDisplay(uint32_t playTick) {

        uint16_t stepIndex = playTick / TICKS_PER_STEP;

        alignViewportToPlayhead(stepIndex);
        viewportRedrawPending = true;
        updatePlayhead(stepIndex);
        counter(stepIndex);

    }

    // ------------------ INIT ------------------
    void init( uint16_t zoomBars) {
        initGridXSTR();
        initPlayheadCmds(Grid::stepsVisible, GRID_Y, GRID_H);
        
        initView(zoomBars);

        // CLOCK 
        uClock.setOutputPPQN(uClock.PPQN_96); 
        uClock.setOnOutputPPQN(onTick);
        uClock.setOnStep(onStep);          // for 16th-step display updates
        uClock.setOnClockContinue(handleClockContinue);
        uClock.setTempo(120);
        uClock.init();

    }

    // --------------- TEST PATTERN --------------

    void testPattern16th(float noteLengthFraction = 0.8f) {
        clearPattern();  // clear old pattern + display

        uint32_t noteDuration = (uint32_t)(TICKS_PER_STEP * noteLengthFraction);

        for (uint32_t step = 0; step < TOTAL_STEPS; step++) {
            uint32_t tickOn = step * TICKS_PER_STEP;
            if (tickOn >= TICKS_PER_PATTERN) break;

            uint8_t note = pianoStartNote + (step % MAX_NOTES_DISPLAY);

            // NOTE ON
            Tick &tOn = pattern[tickOn];
            if (tOn.count < NUM_VOICES) {
                tOn.events[tOn.count++] = { note, defaultVelocity };
                markStepEvent(tickOn, note, defaultVelocity);  // display flag
            }

            // NOTE OFF (audio only)
            uint32_t tickOff = tickOn + noteDuration;
            if (tickOff < TICKS_PER_PATTERN) {
                Tick &tOff = pattern[tickOff];
                if (tOff.count < NUM_VOICES) {
                    tOff.events[tOff.count++] = { note, 0 };
                }
            }
        }
    }

} // namespace Sequencer