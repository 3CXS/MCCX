#include <Wire.h>

#include "Input.h"
#include "Sequencer.h"
#include "AudioEngine.h"
#include "Display.h"


namespace Input {

    // ---------------- TIMER ----------------
    elapsedMillis trellisTimer;
    elapsedMillis encTimer;
    const uint8_t TRELLIS_INTERVAL = 8;
    const uint8_t ENC_INTERVAL = 20;

    // ----------------------------------------------------------------------------------//
    //                                   BUTTONS & MUX                                   //
    // ----------------------------------------------------------------------------------//
    Mux16 mux(28,29,30,31,32);
    ButtonManager manager;

    uint8_t PLAY_FROM_START, PLAY_PAUSE, STOP, RECORD, ENCODER, RSHIFT, F1, F2, F3, F4, F5, F6;

    bool shiftActive = false;
    bool encActive = false;
    bool f1Active = false;
    bool f2Active = false;
    bool f3Active = false;
    bool f4Active = false;
    bool f5Active = false;
    bool f6Active = false;

    void onPlayFromStart()      { Sequencer::onPlayFromStart(); }
    void onPlayPause()          { Sequencer::onPlayPause(); }
    void onStop()               { Sequencer::onStop(); }

    void onRecord() {
        if (shiftActive) {
            Sequencer::onOverdub();
        } else {
            Sequencer::onRecord();
        }
    }
    void onEncoderButton() { encActive = !encActive; }

    void onShift() {
        shiftActive = !shiftActive;
        if (shiftActive) {
            // SHIFT pressed → show track LEDs
            updateTrackLEDs();
        } else {
            // SHIFT released → LEDs OFF
            clearAllTrackLEDs();
        }
    }
    
    void onF1() {
        f1Active = !f1Active;  // toggle F1 mode
        if (f1Active) {
            showF1PadHints();   // light up top-row hints
        } else {
            clearAllTrackLEDs();  // turn off hints
        }
    }

    void onF2Press() {
        Input::f2Active = true;
        if (Input::shiftActive) {
            // Toggle ARP mode
            if (Sequencer::arpMode == Sequencer::ArpMode::OFF) {
                Sequencer::arpMode = Sequencer::ArpMode::UP_OCTAVE; // or last used
                Sequencer::setArpMode(Sequencer::ArpMode::UP_OCTAVE);
            }
            else {
                Sequencer::arpMode = Sequencer::ArpMode::OFF;
                Sequencer::setArpMode(Sequencer::ArpMode::OFF);
            }
            // Reset ARP state
            Sequencer::arpVoice.active = false;
            Sequencer::arpVoice.noteOn = false;
            Sequencer::numHeldNotes = 0;
            return;
        }
    }

    void onF2Release() {
        Input::f2Active = false;

        // ---------- ARP handling ----------
        if (Sequencer::arpMode != Sequencer::ArpMode::OFF) {
            if (Sequencer::arpVoice.noteOn) {
                AudioEngine::noteOff(Sequencer::arpVoice.trackId, Sequencer::arpVoice.note);
                if (Sequencer::isRecording)
                    Sequencer::recordNoteEvent(Sequencer::arpVoice.trackId, Sequencer::arpVoice.note, 0);
            }
            Sequencer::arpVoice.active = false;
            Sequencer::arpVoice.noteOn = false;
            Sequencer::numHeldNotes = 0;
            return;
        }

        // ---------- Note Repeat handling ----------
        for (uint8_t i = 0; i < Sequencer::MAX_REPEAT_VOICES; i++) {
            auto &v = Sequencer::repeatVoices[i];
            if (v.active) {
                // Stop the note if it's currently on
                if (v.noteOn) {
                    AudioEngine::noteOff(v.trackId, v.note);
                    if (Sequencer::isRecording)
                        Sequencer::recordNoteEvent(v.trackId, v.note, 0);
                }

                // Reset the voice
                v.active = false;
                v.noteOn = false;
            }
        }

        // Clear global note repeat state
        Sequencer::noteRepeatActive = false;
        Sequencer::noteRepeatNote   = 0;
    }

    void onF3() { 
        f3Active = !f3Active; 
        if (Input::shiftActive) {
            for (uint16_t i = 0; i < MAX_TRACKS; i++) {
                Sequencer::clearPattern(i);
            }
        }
        else {
            Sequencer::clearPattern(Sequencer::getCurrentTrack());
        }
    }

    void onF4() { 
        f4Active = !f4Active;
        if (f4Active) { updateTrackLEDs();} 
        else {clearAllTrackLEDs();}
    }

    void onF5() { f5Active = !f5Active; }
    void onF6() { f6Active = !f6Active; }

    void initButtons() {
        mux.begin();
        manager.begin();

        PLAY_FROM_START = manager.addMuxButton(&mux, 0, onPlayFromStart, nullptr, true);
        PLAY_PAUSE      = manager.addMuxButton(&mux, 1, onPlayPause, nullptr, true);
        STOP            = manager.addMuxButton(&mux, 2, onStop, nullptr, true);
        RECORD          = manager.addMuxButton(&mux, 3, onRecord, nullptr, true);
        ENCODER         = manager.addDirectButton(4, onEncoderButton, nullptr, true);
        RSHIFT          = manager.addMuxButton(&mux, 10, onShift, nullptr, false);
        F1              = manager.addMuxButton(&mux, 9, onF1, nullptr, false);
        F2              = manager.addMuxButton(&mux, 8, onF2Press, onF2Release, true);
        F3              = manager.addMuxButton(&mux, 7, onF3, nullptr, false);
        F4              = manager.addMuxButton(&mux, 6, onF4, nullptr, false);
        F5              = manager.addMuxButton(&mux, 5, onF5, nullptr, false);
        F6              = manager.addMuxButton(&mux, 4, onF6, nullptr, false);

    }

    // ----------------------------------------------------------------------------------//
    //                                   MAIN ENCODER                                    //
    // ----------------------------------------------------------------------------------//
    Encoder MainEnc(2, 3);
    long oldMainEncPos = MainEnc.read();

    void mainEncoder() {
        long newPosition = MainEnc.read();  // raw encoder position
        if (newPosition == oldMainEncPos) return;
        int32_t delta = newPosition - oldMainEncPos;

        // ---------- SHIFT: ZOOM ----------
        if (shiftActive) {
            static int8_t zoomAcc = 0;
            zoomAcc += delta;
            while (zoomAcc >= 4) {
                Sequencer::cycleZoom(+1);
                zoomAcc -= 4;
            }
            while (zoomAcc <= -4) {
                Sequencer::cycleZoom(-1);
                zoomAcc += 4;
            }
        }
        // ---------- F1: SEQ LENGTH ----------
        else if (f1Active) {
            static int8_t seqAcc = 0;
            seqAcc += delta;
            while (seqAcc >= 4) { // 2 steps → +1 bar
                Sequencer::setSeqLength(Sequencer::getSeqLength() - 1);
                seqAcc -= 4;
            }
            while (seqAcc <= -4) { // 2 steps → -1 bar
                Sequencer::setSeqLength(Sequencer::getSeqLength() + 1);
                seqAcc += 4;
            }
        }
        // ---------- F2: BPM ----------
        else if (f2Active) {
            float sign = (delta >= 0) ? -1.0f : 1.0f;
            float absDelta = abs(delta);
            float scaledDelta = sign * pow(absDelta, 1.5f) * 0.5f; // tweak for sensitivity

            // Apply delta to current BPM
            float bpm = Sequencer::getBPM();      // current BPM
            bpm += scaledDelta;                    // apply small increment
            Sequencer::setBPM(bpm);               // update sequencer
        }

        // ---------- ENC PRESS: NOTE SCROLL ----------
        else if (encActive) {
            static int16_t scrollAcc = 0;
            scrollAcc += delta;

            // Only scroll when threshold reached
            const int8_t threshold = 4; // one raster tick
            while (scrollAcc >= threshold) {
                Sequencer::scrollNotes(+1);
                scrollAcc -= threshold;
            }
            while (scrollAcc <= -threshold) {
                Sequencer::scrollNotes(-1);
                scrollAcc += threshold;
            }
            // Clamp accumulator to avoid huge values
            if (scrollAcc > threshold) scrollAcc = threshold;
            if (scrollAcc < -threshold) scrollAcc = -threshold;
        }

        // ---------- DEFAULT: SCRUB ----------
        else {
            Sequencer::scrubMode = true;
            static int8_t scrubAcc = 0;
            scrubAcc += delta;
            while (scrubAcc >= 4) { // 2 steps → +1 bar
                Sequencer::movePlayheadColumns(-1);
                scrubAcc -= 4;
            }
            while (scrubAcc <= -4) { // 2 steps → -1 bar
                Sequencer::movePlayheadColumns(+1);
                scrubAcc += 4;
            }
        }
        oldMainEncPos = newPosition;
    }

    // ---------------- UNIFIED EVENT QUEUE ----------------
    #define INPUT_EVENT_BUF 32

    enum class InputEventType {
        ENC_TURN,
        ENC_PRESS,
        ENC_RELEASE,
        PAD_PRESS,
        PAD_RELEASE
    };

    struct InputEvent {
        InputEventType type;
        uint8_t id;       // encoder index or pad key
        int32_t delta;    // only used for encoder turns
        uint32_t dt;         // time since last tick (ms)
    };

    // Ring buffer
    InputEvent inputEvents[INPUT_EVENT_BUF];
    volatile uint8_t evtW = 0, evtR = 0;

    bool popInputEvent(InputEvent &e) {
        noInterrupts();
        if (evtR == evtW) {
            interrupts();
            return false;
        }
        e = inputEvents[evtR];
        evtR = (evtR + 1) % INPUT_EVENT_BUF;
        interrupts();
        return true;
    }

    void pushInputEvent(const InputEvent &e) {
        uint8_t next = (evtW + 1) % INPUT_EVENT_BUF;
        if (next == evtR) return; // overflow, drop event
        inputEvents[evtW] = e;
        evtW = next;
    }

    // ----------------------------------------------------------------------------------//
    //                                 QUAD ENCODERS                                     //
    // ----------------------------------------------------------------------------------//
    Adafruit_seesaw encA(&Wire1); // 0x49
    #define NUM_ENCODERS 4
    int32_t encPos[NUM_ENCODERS] = {0};
    bool encButtonPrev[NUM_ENCODERS] = {HIGH, HIGH, HIGH, HIGH};
    const uint8_t encPins[NUM_ENCODERS] = {12, 14, 17, 9};
    Adafruit_seesaw* encBoards[NUM_ENCODERS] = {&encA, &encA, &encA, &encA };
    const char* encNames[NUM_ENCODERS] = { "enc5.val", "enc6.val", "enc7.val", "enc8.val" };

    #define ENC_EVENT_BUF 16
    enum class EncoderEventType {
        TURN,
        PRESS,
        RELEASE
    };
    struct EncoderEvent {
        uint8_t encoder;   // which encoder
        EncoderEventType type;
        int32_t delta;     // + / - steps (TURN only)
    };

    float computeAccel(uint32_t dt, float maxAccel) {
        if (dt == 0) dt = 1;  // avoid divide by zero

        // normalized speed: smaller dt = faster turn
        float speed = 100.0f / float(dt);   // tweak 50.0f for sensitivity
        speed = constrain(speed, 0.0f, maxAccel);

        // exponential curve: 1.0 → maxAccel
        float accel = 1.0f + pow(speed, 2.0f);
        accel = constrain(accel, 1.0f, maxAccel);
        return accel;
    }

    struct EncoderMapping {
        EncParam param;
        float minVal;
        float maxVal;
        float step;
        float *currentValue;
        float displayScale;   // e.g. 100 for %, 1 for raw
        uint8_t decimals;     // number of decimals to show
        float maxAccel;
    };

    EncoderEvent encEvents[ENC_EVENT_BUF];
    volatile uint8_t encEvtW = 0, encEvtR = 0;

    #define NUM_ENCODER_PAGES 4
    EncoderMapping* currentMap = nullptr;
    //EncoderMapping* currentMap = encMapSynth;

    static const char* encoderPageNames[] = {
        "SYNTH",
        "ADSR",
        "ARP",
        "GLOBAL"
    };
    // ---------- SYNTH encoder values ----------
    float cutoff = 2000;
    float resonance = 0.7;
    float crushBits = 8;
    float osc1pulse = 0.5;

    EncoderMapping encMapSynth[NUM_ENCODERS] = {
        { EncParam::FILTER_CUTOFF,    0, 8000, 50, &cutoff, 100.0f / 8000.0f, 0, 50.0f },
        { EncParam::FILTER_RESONANCE, 0, 4,  0.05, &resonance, 100.0f / 4, 0, 50.0f},
        { EncParam::BITCRUSH_BITS,    4,   16,   1, &crushBits, 1.0f, 0, 50.0f},
        { EncParam::OSC1_PULSE,       0,   3,   1, &osc1pulse, 100.0f / 4, 0, 50.0f}
    };

    // ---------- ADSR encoder values ----------
    float attack = 0;
    float decay = 10;
    float sustain = 10;
    float release = 20;

    EncoderMapping encMapADSR[NUM_ENCODERS] = {
        { EncParam::ENV_ATT, 0, 1000, 10, &attack, 100.0f / 1000, 0, 10.0f},
        { EncParam::ENV_DEC, 0, 1000, 10, &decay, 100.0f / 1000, 0, 10.0f},
        { EncParam::ENV_SUS, 0, 1000, 10, &sustain, 100.0f / 1000, 0, 10.0f},
        { EncParam::ENV_REL, 0, 2000, 10, &release, 100.0f / 2000, 0, 10.0f}
    };

    // ---------- ARP encoder values ----------
    float arpRateVal    = 2;   // index into enum
    float arpOctaveVal  = 2;
    float arpModeVal    = 1;
    float arpGateVal    = 0.8f;

    EncoderMapping encMapArp[NUM_ENCODERS] = {
        { EncParam::ARP_RATE,    0, 5, 1,   &arpRateVal, 1.0f, 0, 10.0f  }, // enum index
        { EncParam::ARP_OCTAVES, 1, 4, 1,   &arpOctaveVal, 1.0f, 0, 10.0f },
        { EncParam::ARP_MODE,    0, 3, 1,   &arpModeVal, 1.0f, 0, 10.0f  },
        { EncParam::ARP_GATE,    0.1, 1.0, 0.05, &arpGateVal, 100.0f, 0, 10.0f }
    };

    // ---------- GLOBAL encoder values ----------
    float volume = 0.5;
    float main2 = 0;
    float main3 = 0;
    float main4 = 0;

    EncoderMapping encMapMain[NUM_ENCODERS] = {
         //         param   min  max  step  value  scale  decimals accel
        { EncParam::MAIN_VOL, 0.0, 1.0, 0.05, &volume, 100.0f, 0, 10.0f },
        { EncParam::MAIN_2,   0,   100, 1,   &main2,   1.0f, 0, 10.0f },
        { EncParam::MAIN_3,   0,   100, 1,   &main3,   1.0f, 0, 10.0f },
        { EncParam::MAIN_4,   0,   100, 1,   &main4,   1.0f, 0, 10.0f }
    };

    uint32_t lastEncTick[NUM_ENCODERS] = {0};

    void readEncoders() {
        if (encTimer <= ENC_INTERVAL) return;

        uint32_t now = millis();

        for (int i = 0; i < NUM_ENCODERS; i++) {

            // --- ROTATION ---
            int32_t pos = encBoards[i]->getEncoderPosition(i);
            int32_t delta = pos - encPos[i];
            if (delta != 0) {
                uint32_t dt = now - lastEncTick[i];  // time since last move
                if (dt > 200) dt = 200;              // clamp for slow rotations
                lastEncTick[i] = now;

                encPos[i] = pos;

                InputEvent e;
                e.type  = InputEventType::ENC_TURN;
                e.id    = i;
                e.delta = delta;
                e.dt    = dt;           // send real delta time
                pushInputEvent(e);
            }

            // --- BUTTON ---
            bool btn = encBoards[i]->digitalRead(encPins[i]);
            if (btn != encButtonPrev[i]) {
                InputEvent e;
                e.type  = (btn == LOW) ? InputEventType::ENC_PRESS : InputEventType::ENC_RELEASE;
                e.id    = i;
                e.delta = 0;
                e.dt    = 0;
                pushInputEvent(e);

                encButtonPrev[i] = btn;
            }
        }

        encTimer = 0;
    }

    void handleEncoderEvent(const InputEvent &e) {
        if (currentMap == nullptr) return;

        auto &m = currentMap[e.id];   // current mapping

        float baseStep = m.step;
        float effectiveStep = baseStep;

        // --- ACCELERATION ---
        float speed = (e.dt > 0) ? 1.0f / float(e.dt) : 0.0f;   // faster turn → smaller dt → larger speed
        float accel = computeAccel(e.dt, m.maxAccel);                    // tune factor

        // --- Fine scaling for small ranges ---
        if ((m.maxVal - m.minVal) < 10.0f) {
            accel = 1.0f + speed;   // less aggressive
        }

        effectiveStep = baseStep * accel;

        // --- APPLY ---
        *m.currentValue += e.delta * effectiveStep;
        *m.currentValue = constrain(*m.currentValue, m.minVal, m.maxVal);

        int displayValue = roundf(*m.currentValue * m.displayScale);

        // --- UPDATE ENGINE ---
        if (currentMap == encMapSynth || currentMap == encMapADSR) {
            AudioEngine::setSynthParam(m.param, *m.currentValue);
        }
        else if (currentMap == encMapArp) {
            switch(e.id) {
                case 0: Sequencer::arpRate  = (Sequencer::TimingDivision)(int)*m.currentValue; break;
                case 1: Sequencer::arpOctaves = (uint8_t)*m.currentValue; break;
                case 2: Sequencer::arpMode    = (Sequencer::ArpMode)(int)*m.currentValue; break;
                case 3: Sequencer::arpGate    = *m.currentValue; break;
            }
            Sequencer::recalcArpTiming();
        }
        else if (currentMap == encMapMain) {
            AudioEngine::setMainParam(m.param, *m.currentValue);
        }

        Display::writeNum(encNames[e.id], displayValue);
    }

    static uint8_t currentEncoderPage = 0;

    void setEncoderPageInternal(uint8_t page) {
        switch (page) {
            case 0: currentMap = encMapSynth; break;
            case 1: currentMap = encMapADSR;  break;
            case 2: currentMap = encMapArp;   break;
            case 3: currentMap = encMapMain;  break;
        }
    }

    void setEncoderPage(uint8_t page) {
        currentEncoderPage = constrain(page, 0, NUM_ENCODER_PAGES - 1);
        setEncoderPageInternal(currentEncoderPage);

        // Optional UI feedback
        Display::writeStr("encPage.txt", encoderPageNames[currentEncoderPage]);
    }

    // ----------------------------------------------------------------------------------//
    //                                 BUTTON PAD                                        //
    // ----------------------------------------------------------------------------------//
    Adafruit_NeoTrellis t_array[Y_DIM/4][X_DIM/4] = {
        { Adafruit_NeoTrellis(0x2E), Adafruit_NeoTrellis(0x2F) }
    };
    Adafruit_MultiTrellis trellis((Adafruit_NeoTrellis *)t_array, Y_DIM/4, X_DIM/4);


    //------------------------------------------//
    uint8_t activeTrack = 0;
    // LED color for active track selection


    // map each key to MIDI note 
    uint8_t keyToNote(uint8_t keyIndex) { 
      uint8_t row = keyIndex / X_DIM; // 0 = top row 
      uint8_t col = keyIndex % X_DIM; // 0 = left column 
      uint8_t bottomLeftRow = Y_DIM - 1 - row; // flip vertically 
      return 48 + bottomLeftRow * X_DIM + col; // 48 = starting MIDI note (C3) 
    }
    inline uint8_t padToTrack(uint8_t padIndex) {
        uint8_t row = padIndex / X_DIM;
        uint8_t col = padIndex % X_DIM;
        uint8_t bottomRow = Y_DIM - 1 - row;   // flip vertically
        return bottomRow * X_DIM + col;        // track numbering from bottom-left
    }

    inline uint8_t trackToPad(uint8_t trackNumber) {
        uint8_t row = trackNumber / X_DIM;
        uint8_t col = trackNumber % X_DIM;
        uint8_t bottomRow = Y_DIM - 1 - row;   // flip vertically back
        return bottomRow * X_DIM + col;
    }


    static const uint8_t topRowZoomMap[4] = { 1, 2, 3, 4 };

    volatile bool trellisDirty = false;

    void readPads() {
        if(trellisTimer > TRELLIS_INTERVAL){
        trellis.read();   // <--- no parameters!
        trellisTimer = 0;
        } 
    }

    TrellisCallback keyPress(keyEvent evt) {
        InputEvent e;
        e.id    = evt.bit.NUM;  // pad key number
        e.delta = 0;            // not used for pads

        if (evt.bit.EDGE == SEESAW_KEYPAD_EDGE_RISING) {
            e.type = InputEventType::PAD_PRESS;
        } else {
            e.type = InputEventType::PAD_RELEASE;
        }

        // Push into the unified event queue
        pushInputEvent(e);

        // Return immediately, ISR-safe
        return 0;
    }


    volatile bool shiftModeActive = false;
    volatile bool f4ModeActive = false;

    void handlePadEvent(const InputEvent &e) {
        const uint8_t key   = e.id;
        const bool pressed  = (e.type == InputEventType::PAD_PRESS);
        const uint8_t note  = keyToNote(key);
        uint8_t trk = Sequencer::getCurrentTrack();

        // ---------- SHIFT + PAD: TRACK SELECTION ----------
        if (shiftActive && key < MAX_TRACKS) {
            if (pressed) {
                activeTrack = padToTrack(key);                     // select track
                Sequencer::setCurrentTrack(activeTrack); 
                updateTrackLEDs();                      // light only the selected track
                shiftModeActive = true;                 // enable shift LED mode
            }
            return; // skip normal pad behavior when Shift is active
        }

        // Handle shift release
        if (!shiftActive && shiftModeActive) shiftModeActive = false;
        if (!f4Active && f4ModeActive) f4ModeActive = false;

        // ---------- F4 + PAD: TRACK MUTE ----------
        if (f4Active && key < MAX_TRACKS) {
            if (pressed) {
                f4ModeActive = true;
                uint8_t track = padToTrack(key);
                Sequencer::toggleTrackMute(track);
                updateTrackLEDs();  // new function to update F4 LED feedback
                
            }
            return;
        }

        // ---------- F1 pad behavior ----------
        // F1 QUANTIZE
        if (f1Active && key < 4 && pressed) {
            static const Sequencer::TimingDivision quantizeMap[4] = {
                Sequencer::TimingDivision::QUARTER,
                Sequencer::TimingDivision::EIGHTH,
                Sequencer::TimingDivision::SIXTEENTH,
                Sequencer::TimingDivision::THIRTYSECOND,
            };
            if (key == 0) {
                Sequencer::setQuantizeEnabled(false);
            } else {
                Sequencer::setQuantizeEnabled(true);
                Sequencer::setQuantizeDivision(quantizeMap[key]);
            }
            return;
        }
        // F1 ENCODER PAGE
        if (f1Active && key >= 4 && key < 8) {
            Input::setEncoderPage(key - 4);
        }
        // F1 TIME DIVISIONS
        if (f1Active && key >= 8 && key < 14 && pressed) {
            static const Sequencer::TimingDivision repeatMap[6] = {
                Sequencer::TimingDivision::QUARTER,
                Sequencer::TimingDivision::EIGHTH,
                Sequencer::TimingDivision::SIXTEENTH,
                Sequencer::TimingDivision::SIXTEENTHT,
                Sequencer::TimingDivision::THIRTYSECOND,
                Sequencer::TimingDivision::THIRTYSECONDT
            };
            Sequencer::noteRepeatRate = repeatMap[key-8];
            Sequencer::noteRepeatInterval =
                Sequencer::divisionToTicks(Sequencer::noteRepeatRate);
            Sequencer::setRepeatDivision(repeatMap[key-8]);
            return;
        }
        // F1 TRACK TYPE
        if (f1Active && key >= 16 && key < 20 && pressed) {
            if (!pressed) return;
            Sequencer::setTrackType(Sequencer::TrackType(key-16));
            return;
        }
        // F1 ENGINE ID
        if (f1Active && key >= 20 && key < 24 && pressed) {
            if (!pressed) return;
            Sequencer::assignTrackToEngine(key-20);
            return;
        }
        // ---------- Normal pad behavior ----------
        // ARP
        if (Sequencer::arpMode != Sequencer::ArpMode::OFF) {
            if (pressed) Sequencer::startArp(note);
            else         Sequencer::stopArp(note);
            return;
        }

        // F2 note repeat
        if (Input::f2Active) {
            if (pressed) Sequencer::startNoteRepeat(note);
            else         Sequencer::stopNoteRepeat(note);
            return;
        }

        // normal note-on/off
        if (pressed) AudioEngine::noteOn(trk,note, Sequencer::getDefaultVelocity());
        else         AudioEngine::noteOff(trk, note);

        if (Sequencer::isRecording)
            Sequencer::recordNoteEvent(trk, note, pressed ? Sequencer::getDefaultVelocity() : 0);

        trellisDirty = true;
    }

    // ---------- Update LEDs ----------
    constexpr uint32_t COLOR_OFF       = 0x000000;
    constexpr uint32_t COLOR_PINK_DIM  = 0x200020; 
    constexpr uint32_t COLOR_BLUE_DIM  = 0x000020;
    constexpr uint32_t COLOR_CYAN_DIM  = 0x002020;
    constexpr uint32_t COLOR_RED_DIM   = 0x200000;
    constexpr uint32_t COLOR_WHITE_DIM  = 0x101010; // dim grey/blue
    constexpr uint32_t COLOR_BLUE      = 0x0040FF; // bright blue
    constexpr uint32_t COLOR_MUTED_SELECTED = 0xFF2020;

    void updateTrackLEDs() {
        for (uint8_t t = 0; t < MAX_TRACKS; t++) {
            uint8_t pad = trackToPad(t);

            bool muted   = Sequencer::isTrackMuted(t);
            bool active  = (t == activeTrack);
            bool hasData = Sequencer::trackHasPatternData(t);

            uint32_t color = COLOR_OFF;

            if (active && muted) {
                color = COLOR_MUTED_SELECTED;
            }
            else if (active) {
                color = COLOR_BLUE;
            }
            else if (muted) {
                color = COLOR_RED_DIM;
            }
            else if (hasData) {
                color = COLOR_WHITE_DIM;
            }

            trellis.setPixelColor(pad, color);
        }

        trellisDirty = true;
    }

    void showF1PadHints() {
        // Quantize
        for (uint8_t k = 0; k < 4; k++) {
            trellis.setPixelColor(k, COLOR_PINK_DIM);
        }
        // Encoder Page
        for (uint8_t k = 4; k < 8; k++) {
            trellis.setPixelColor(k, COLOR_BLUE_DIM);
        }
        // Note Repeat
        for (uint8_t k = 8; k < 14; k++) {
            trellis.setPixelColor(k, COLOR_RED_DIM);
        }
        // Track Type
        for (uint8_t k = 16; k < 20; k++) {
            trellis.setPixelColor(k, COLOR_CYAN_DIM);
        }
        // Engine ID
        for (uint8_t k = 20; k < 24; k++) {
            trellis.setPixelColor(k, COLOR_WHITE_DIM);
        }
        trellisDirty = true;
    }

    void clearAllTrackLEDs() {
        for (uint8_t i = 0; i < MAX_TRACKS; i++) {
            trellis.setPixelColor(i, COLOR_OFF);
        }
        trellisDirty = true;
    }

    void processTrellisLEDs() {
        static elapsedMillis ledTimer;

        if (trellisDirty && ledTimer >= 5) {
            trellis.show();      // must call show() to update hardware
            trellisDirty = false;
            ledTimer = 0;
        }
    }

    // ---------------- PROCESS  ----------------
    void processInputEvents() {
        InputEvent e;
        while (popInputEvent(e)) {
            switch (e.type) {
                case InputEventType::PAD_PRESS:
                case InputEventType::PAD_RELEASE:
                    handlePadEvent(e);
                    break;

                case InputEventType::ENC_TURN:
                case InputEventType::ENC_PRESS:
                case InputEventType::ENC_RELEASE:
                    handleEncoderEvent(e);
                    break;
            }
        }
    }

    // ----------------------------------------------------------------------------------//
    //                                        INIT                                       //
    // ----------------------------------------------------------------------------------//
    void init() {
        Wire1.begin();            // WIRE1 - IC2
        Wire1.setClock(800000);      
        //Wire1.setTimeout(2000);

        initButtons();

        // BUTTONPAD
        if(!trellis.begin()){ Serial.println("NeoTrellis fail"); while(1); }

        for (int by = 0; by < 1; by++) {
            for (int bx = 0; bx < 2; bx++) {
                for (int y = 0; y < 4; y++) {
                    for (int x = 0; x < 4; x++) {
                        int globalX = bx*4 + x;
                        int globalY = by*4 + y;
                        trellis.activateKey(globalX, globalY, SEESAW_KEYPAD_EDGE_RISING, true);
                        trellis.activateKey(globalX, globalY, SEESAW_KEYPAD_EDGE_FALLING, true);
                        trellis.registerCallback(globalX, globalY, keyPress);
                        trellis.setPixelColor(globalX, globalY, 0x000000);
                    }
                }
            }
        }

        // ENCODERS
        if(!encA.begin(0x49)){ Serial.println("Encoder A fail"); while(1); }
        //if(!encB.begin(0x4A)){ Serial.println("Encoder B fail"); while(1); }

        encA.pinMode(12, INPUT_PULLUP); encA.pinMode(14, INPUT_PULLUP);
        encA.pinMode(17, INPUT_PULLUP); encA.pinMode(9, INPUT_PULLUP);
        //encB.pinMode(12, INPUT_PULLUP); encB.pinMode(14, INPUT_PULLUP);
        //encB.pinMode(17, INPUT_PULLUP); encB.pinMode(9, INPUT_PULLUP);

        encPos[0]=encA.getEncoderPosition(0); encPos[1]=encA.getEncoderPosition(1);
        encPos[2]=encA.getEncoderPosition(2); encPos[3]=encA.getEncoderPosition(3);
        //encPos[4]=encB.getEncoderPosition(0); encPos[5]=encB.getEncoderPosition(1);
        //encPos[6]=encB.getEncoderPosition(1); encPos[7]=encB.getEncoderPosition(3);

        encA.enableEncoderInterrupt(0); encA.enableEncoderInterrupt(1);
        encA.enableEncoderInterrupt(2); encA.enableEncoderInterrupt(3);
        //encB.enableEncoderInterrupt(0); encB.enableEncoderInterrupt(1);
        //encB.enableEncoderInterrupt(2); encB.enableEncoderInterrupt(3);

        //pixels.begin(); pixels.setBrightness(255); pixels.show();

        setEncoderPage(0);

    }
    
    // Input a value 0 to 255 to get a color value.
    // The colors are a transition r - g - b - back to r.
    uint32_t Wheel(byte WheelPos) {
      if(WheelPos < 85) {
      return seesaw_NeoPixel::Color(WheelPos * 3, 255 - WheelPos * 3, 0);
      } else if(WheelPos < 170) {
      WheelPos -= 85;
      return seesaw_NeoPixel::Color(255 - WheelPos * 3, 0, WheelPos * 3);
      } else {
      WheelPos -= 170;
      return seesaw_NeoPixel::Color(0, WheelPos * 3, 255 - WheelPos * 3);
      }
      return 0;
    }

} // namespace InputModule



