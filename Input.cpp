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

    uint8_t PLAY_FROM_START, PLAY_PAUSE, STOP, RECORD, ENCODER, RSHIFT, F1, F2;
    bool shiftActive = false;
    bool encActive = false;
    bool f1Active = false;
    bool f2Active = false;

    void onPlayFromStart()      { Sequencer::onPlayFromStart(); }
    void onPlayPause()          { Sequencer::onPlayPause(); }
    void onStop()               { Sequencer::onStop(); }
    void onShift()              { shiftActive = !shiftActive; }
    
    void onRecord() {
        // Determine mode locally in Input.cpp
        if (shiftActive) {
            // Shift + Record → Overdub
            Sequencer::onOverdub();  // call a dedicated overdub function in Sequencer
        } else {
            // Normal Record
            Sequencer::onRecord();
        }
    }

    void onEncoderButton() { encActive = !encActive; }

    void onF1() {
        f1Active = !f1Active;  // toggle F1 mode
        if (f1Active) {
            showF1PadHints();   // light up top-row hints
        } else {
            clearF1PadHints();  // turn off hints
        }
    }

    void onF2Press() {
        Input::f2Active = true;
        if (Input::shiftActive) {
            // Toggle ARP mode
            if (Sequencer::arpMode == Sequencer::ArpMode::OFF) {
                Sequencer::arpMode = Sequencer::ArpMode::UP_OCTAVE; // or last used
                Sequencer::updateArpLabel(Sequencer::ArpMode::UP_OCTAVE);
            }
            else {
                Sequencer::arpMode = Sequencer::ArpMode::OFF;
                Sequencer::updateArpLabel(Sequencer::ArpMode::OFF);
            }
            // Reset ARP state
            Sequencer::arpVoice.active = false;
            Sequencer::arpVoice.noteOn = false;
            Sequencer::numHeldNotes = 0;
            return;
        }

        // Existing drum note repeat logic here (if not in shift mode)
    }

    void onF2Release() {
        Input::f2Active = false;

        if (Sequencer::arpMode != Sequencer::ArpMode::OFF) {
            // Stop all arp notes immediately
            if (Sequencer::arpVoice.noteOn) {
                AudioEngine::noteOff(Sequencer::arpVoice.note);
                Sequencer::arpVoice.noteOn = false;
                if (Sequencer::isRecording)
                    Sequencer::recordNoteEvent(Sequencer::arpVoice.note, 0);
            }
            Sequencer::arpVoice.active = false;
            Sequencer::numHeldNotes = 0;
        } else {
            // Stop drum note repeat if active
            if (Sequencer::noteRepeatActive) {
                Sequencer::noteRepeatActive = false;
                AudioEngine::noteOff(Sequencer::noteRepeatNote);
                if (Sequencer::isRecording)
                    Sequencer::recordNoteEvent(Sequencer::noteRepeatNote, 0);
            }
        }
    }

    void initButtons() {
        mux.begin();
        manager.begin();

        PLAY_FROM_START = manager.addMuxButton(&mux, 0, onPlayFromStart, nullptr, true);
        PLAY_PAUSE      = manager.addMuxButton(&mux, 1, onPlayPause, nullptr, true);
        STOP            = manager.addMuxButton(&mux, 2, onStop, nullptr, true);
        RECORD          = manager.addMuxButton(&mux, 3, onRecord, nullptr, true);
        RSHIFT          = manager.addMuxButton(&mux, 10, onShift, nullptr, false);
        ENCODER         = manager.addDirectButton(4, onEncoderButton, nullptr, true);
        F1              = manager.addMuxButton(&mux, 9, onF1, nullptr, false);
        F2              = manager.addMuxButton(&mux, 8, onF2Press, onF2Release, true);
    }

    inline int32_t clampInt32(int32_t v, int32_t lo, int32_t hi) {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }

    inline uint8_t clampU8(int32_t v, uint8_t lo, uint8_t hi) {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return (uint8_t)v;
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

    struct EncoderMapping {
        SynthParam param;
        float minVal;
        float maxVal;
        float step;
        float *currentValue;
    };

    EncoderEvent encEvents[ENC_EVENT_BUF];
    volatile uint8_t encEvtW = 0, encEvtR = 0;

    #define NUM_ENCODER_PAGES 3
    EncoderMapping* currentMap = nullptr;
    //EncoderMapping* currentMap = encMapSynth;

    static const char* encoderPageNames[] = {
        "SYNTH",
        "ADSR",
        "ARP",
        "GLOBAL"
    };

    // ---------- ARP encoder values ----------
    float arpRateVal    = 2;   // index into enum
    float arpOctaveVal  = 2;
    float arpModeVal    = 1;
    float arpGateVal    = 0.8f;

    EncoderMapping encMapArp[NUM_ENCODERS] = {
        { SynthParam::ARP_RATE,    0, 3, 1,   &arpRateVal   }, // enum index
        { SynthParam::ARP_OCTAVES, 1, 4, 1,   &arpOctaveVal },
        { SynthParam::ARP_MODE,    0, 3, 1,   &arpModeVal   },
        { SynthParam::ARP_GATE,    0.1, 1.0, 0.05, &arpGateVal }
    };

    // ---------- SYNTH encoder values ----------
    float cutoff = 2000;
    float resonance = 0.7;
    float crushBits = 8;
    float osc1pulse = 0.5;

    EncoderMapping encMapSynth[NUM_ENCODERS] = {
        { SynthParam::FILTER_CUTOFF,    0, 8000, 100, &cutoff },
        { SynthParam::FILTER_RESONANCE, 0, 4,  0.05, &resonance },
        { SynthParam::BITCRUSH_BITS,    4,   16,   1, &crushBits },
        { SynthParam::OSC1_PULSE,       0,   3,   1, &osc1pulse }
    };

    // ---------- ADSR encoder values ----------
    float attack = 0;
    float decay = 10;
    float sustain = 10;
    float release = 20;

    EncoderMapping encMapADSR[NUM_ENCODERS] = {
        { SynthParam::ENV_ATT, 0, 200, 10, &attack },
        { SynthParam::ENV_DEC, 0, 200, 10, &decay },
        { SynthParam::ENV_SUS, 0, 200, 10, &sustain },
        { SynthParam::ENV_REL, 0, 200, 10, &release }
    };

    void readEncoders() {
        if (encTimer <= ENC_INTERVAL) return;

        for (int i = 0; i < NUM_ENCODERS; i++) {

            // --- ROTATION ---
            int32_t pos = encBoards[i]->getEncoderPosition(i);
            int32_t delta = pos - encPos[i];
            if (delta != 0) {
                encPos[i] = pos;

                InputEvent e;
                e.type  = InputEventType::ENC_TURN;
                e.id    = i;      // encoder index
                e.delta = delta;  // rotation delta
                pushInputEvent(e);
            }

            // --- BUTTON ---
            bool btn = encBoards[i]->digitalRead(encPins[i]);
            if (btn != encButtonPrev[i]) {
                InputEvent e;
                e.type  = (btn == LOW) ? InputEventType::ENC_PRESS : InputEventType::ENC_RELEASE;
                e.id    = i;
                e.delta = 0;
                pushInputEvent(e);

                encButtonPrev[i] = btn;
            }
        }

        encTimer = 0;
    }

    void handleEncoderEvent(const InputEvent &e) {
        if (currentMap == nullptr) return;

        auto &m = currentMap[e.id];   // <-- use currentMap, not always encMap

        *m.currentValue += e.delta * m.step;
        *m.currentValue = constrain(*m.currentValue, m.minVal, m.maxVal);

        // ARP page
        if (currentMap == encMapArp) {
            switch(e.id) {
                case 0: Sequencer::arpRate   = (Sequencer::ArpRate)(int)*m.currentValue; break;
                case 1: Sequencer::arpOctaves= (uint8_t)*m.currentValue; break;
                case 2: Sequencer::arpMode   = (Sequencer::ArpMode)(int)*m.currentValue; break;
                case 3: Sequencer::arpGate   = *m.currentValue; break;
            }
            Sequencer::recalcArpTiming();
        } else {
            // normal synth parameters
            AudioEngine::setSynthParam(m.param, *m.currentValue);
        }

        Display::writeNum(encNames[e.id], *m.currentValue);
    }

    static uint8_t currentEncoderPage = 0;

    void setEncoderPageInternal(uint8_t page) {
        switch (page) {
            case 0: currentMap = encMapSynth; break;
            case 1: currentMap = encMapADSR;   break;
            case 2: currentMap = encMapArp;   break;
            //case 3: currentMap = encMapGlobal;break;
        }
    }

    void setEncoderPage(uint8_t page) {
        currentEncoderPage = constrain(page, 0, NUM_ENCODER_PAGES - 1);
        setEncoderPageInternal(currentEncoderPage); // your existing or new logic

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

    // map each key to MIDI note 
    uint8_t keyToNote(uint8_t keyIndex) { 
      uint8_t row = keyIndex / X_DIM; // 0 = top row 
      uint8_t col = keyIndex % X_DIM; // 0 = left column 
      uint8_t bottomLeftRow = Y_DIM - 1 - row; // flip vertically 
      return 48 + bottomLeftRow * X_DIM + col; // 48 = starting MIDI note (C3) 
    }

    static const QuantizeType topRowQuantizeMap[4] = {
        QuantizeType::OFF,          // button 0
        QuantizeType::QUARTER,      // button 1
        QuantizeType::SIXTEENTH,    // button 2
        QuantizeType::THIRTYSECOND, // button 3
    };

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

    constexpr uint32_t COLOR_PINK_DIM  = 0x200020; // dim magenta (R + B)
    constexpr uint32_t COLOR_BLUE_DIM  = 0x000020; // dim blue

    void showF1PadHints() {
        // Quantize buttons (0–3)
        for (uint8_t k = 0; k < 4; k++) {
            trellis.setPixelColor(k, COLOR_PINK_DIM);
        }

        // Zoom buttons (4–7)
        for (uint8_t k = 4; k < 8; k++) {
            trellis.setPixelColor(k, COLOR_BLUE_DIM);
        }

        trellisDirty = true;
    }

    void clearF1PadHints() {
        for (uint8_t k = 0; k < 8; k++) {
            trellis.setPixelColor(k, 0x000000);
        }
        trellisDirty = true;
    }

    void handlePadEvent(const InputEvent &e) {
        const uint8_t key   = e.id;
        const bool pressed  = (e.type == InputEventType::PAD_PRESS);
        const uint8_t note  = keyToNote(key);

        // --- F1 MODE: top-row pads ---
        if (f1Active && key < 8) {
            if (!pressed) return;

            if (key < 4) {
                Sequencer::setQuantizeMode(topRowQuantizeMap[key]);
            } 

        else { // Encoder pages
            uint8_t page = key - 4;   // pads 4–7 → pages 0–3
            Input::setEncoderPage(page);
                    }
                    return;
        }

        // --- F1: note-repeat rate ---
        if (f1Active && key >= 8 && key < 14 && pressed) {
            static const Sequencer::NoteRepeatRate repeatMap[6] = {
                Sequencer::NoteRepeatRate::QUARTER,
                Sequencer::NoteRepeatRate::EIGHTH,
                Sequencer::NoteRepeatRate::SIXTEENTH,
                Sequencer::NoteRepeatRate::SIXTEENTHT,
                Sequencer::NoteRepeatRate::THIRTYSECOND,
                Sequencer::NoteRepeatRate::THIRTYSECONDT
            };

            Sequencer::noteRepeatRate = repeatMap[key - 8];
            Sequencer::noteRepeatInterval =
                Sequencer::getNoteRepeatIntervalTicks(Sequencer::noteRepeatRate);

            Sequencer::updateNoteRepeatLabel(Sequencer::noteRepeatRate);
            return;
        }

        // --- NORMAL PAD / NOTE REPEAT LOGIC ---
        if (Sequencer::arpMode != Sequencer::ArpMode::OFF) {
            if (pressed)
                Sequencer::startArp(note);
            else
                Sequencer::stopArp(note);
        } else if (Input::f2Active) {
            // Existing drum note repeat logic
            if (pressed) {
                Sequencer::startNoteRepeat(note);
            } else {
                Sequencer::stopNoteRepeat(note);
            }
        } else {
            // normal pad behavior
            if (pressed)
                AudioEngine::noteOn(note, Sequencer::getDefaultVelocity());
            else
                AudioEngine::noteOff(note);

            if (Sequencer::isRecording)
                Sequencer::recordNoteEvent(note, pressed ? Sequencer::getDefaultVelocity() : 0);
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



