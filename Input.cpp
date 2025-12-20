#include <Wire.h>

#include "Input.h"
#include "Sequencer.h"
#include "AudioEngine.h"
#include "Display.h"

namespace Input {

    // ---------------- TIMER ----------------
    elapsedMillis trellisTimer;
    elapsedMillis encTimer;
    const uint8_t TRELLIS_INTERVAL = 12;
    const uint8_t ENC_INTERVAL = 15;

    // ---------------- BUTTONS & MUX ----------------
    Mux16 mux(28,29,30,31,32);
    ButtonManager manager;

    uint8_t PLAY_FROM_START, PLAY_PAUSE, STOP, RECORD, ENCODER, RSHIFT;
    bool shiftActive = false;
    bool scrollActive = false;

    void onPlayFromStart()      { Sequencer::onPlayFromStart(); }
    void onPlayPause()          { Sequencer::onPlayPause(); }
    void onStop()               { Sequencer::onStop(); }
    void onRecord()             { Sequencer::onRecord(); }
    void onShift()              { shiftActive = !shiftActive; }
    void onEncoderButton() { 
        scrollActive = !scrollActive; 
        Serial.println("ENCODER BTN"); 
        }

    void initButtons() {
        mux.begin();
        manager.begin();

        PLAY_FROM_START = manager.addMuxButton(&mux, 0, onPlayFromStart, true);
        PLAY_PAUSE      = manager.addMuxButton(&mux, 1, onPlayPause, true);
        STOP            = manager.addMuxButton(&mux, 2, onStop, true);
        RECORD          = manager.addMuxButton(&mux, 3, onRecord, true);
        RSHIFT          = manager.addMuxButton(&mux, 10, onShift, false);
        ENCODER         = manager.addDirectButton(4, onEncoderButton, true);
    }

    // ---------------- MAIN ENCODER ----------------
    Encoder MainEnc(2, 3);
    long oldMainEncPos = -999;
    static int16_t scrollAccumulator = 0;

    void mainEncoder() {
        long newPosition = MainEnc.read();  // raw encoder position
        if (newPosition != oldMainEncPos) {
            int32_t delta = newPosition - oldMainEncPos;

            if (shiftActive) {  // BPM
                float sign = (delta >= 0) ? -1.0f : 1.0f;
                float absDelta = abs(delta);
                float scaledDelta = sign * pow(absDelta, 1.5f) * 0.5f; // tweak for sensitivity

                // Apply delta to current BPM
                float bpm = Sequencer::getBPM();      // current BPM
                bpm += scaledDelta;                    // apply small increment
                Sequencer::setBPM(bpm);               // update sequencer

            }

            else if (scrollActive) {  // NOTE SCROLL MODE
                scrollAccumulator += delta;

                // Only scroll when threshold reached
                const int8_t threshold = 4; // one raster tick
                while (scrollAccumulator >= threshold) {
                    Sequencer::scrollNotes(+1);
                    scrollAccumulator -= threshold;
                }
                while (scrollAccumulator <= -threshold) {
                    Sequencer::scrollNotes(-1);
                    scrollAccumulator += threshold;
                }

            }

            else if (!Sequencer::isPlaying) {  // SCRUB MODE
                Sequencer::scrubMode = true;
                float sign = (delta >= 0) ? -1.0f : 1.0f;
                float absDelta = abs(delta);
                float scaledDelta = sign * pow(absDelta, 1.5f) * 0.25f; // tweak for sensitivity

                int32_t newTick = Sequencer::playheadTick + scaledDelta * TICKS_PER_STEP;
                newTick = constrain(newTick, 0, TICKS_PER_PATTERN - 1);
                Sequencer::playheadTick = (uint32_t)newTick;

                AudioEngine::allNotesOff();
                Sequencer::updateSequencerDisplay(Sequencer::playheadTick);
            }

        oldMainEncPos = newPosition;

        }
    }

    // ---------------- QUAD ENCODERS ----------------
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

    EncoderEvent encEvents[ENC_EVENT_BUF];
    volatile uint8_t encEvtW = 0, encEvtR = 0;

    float cutoff = 2000;
    float resonance = 0.7;
    float crushBits = 8;
    float osc1pulse = 0.5;

    struct EncoderMapping {
        SynthParam param;
        float minVal;
        float maxVal;
        float step;
        float *currentValue;
    };

    EncoderMapping encMap[NUM_ENCODERS] = {
        { SynthParam::FILTER_CUTOFF,    100, 8000, 50, &cutoff },
        { SynthParam::FILTER_RESONANCE, 0.1, 1.5,  0.05, &resonance },
        { SynthParam::BITCRUSH_BITS,    4,   12,   1, &crushBits },
        { SynthParam::OSC1_PULSE,    0,   3,   1, &osc1pulse},
    };

    bool popEncoderEvent(EncoderEvent &e) {
        noInterrupts();
        if (encEvtR == encEvtW) {
            interrupts();
            return false;
        }
        e = encEvents[encEvtR];
        encEvtR = (encEvtR + 1) % ENC_EVENT_BUF;
        interrupts();
        return true;
    }

    void pushEncoderEvent(const EncoderEvent &e) {
        uint8_t next = (encEvtW + 1) % ENC_EVENT_BUF;
        if (next == encEvtR) return; // overflow
        encEvents[encEvtW] = e;
        encEvtW = next;
    }

    void encoders() {
        if (encTimer <= ENC_INTERVAL) return;

        for (int i = 0; i < NUM_ENCODERS; i++) {

            // --- ROTATION ---
            int32_t pos = encBoards[i]->getEncoderPosition(i < 4 ? i : i - 4);
            int32_t delta = pos - encPos[i];
            if (delta != 0) {
                encPos[i] = pos;
                EncoderEvent e;
                e.encoder = i;
                e.type = EncoderEventType::TURN;
                e.delta = delta;
                pushEncoderEvent(e);
            }

            // --- BUTTON ---
            bool btn = encBoards[i]->digitalRead(encPins[i]);
            if (btn != encButtonPrev[i]) {
                EncoderEvent e;
                e.encoder = i;
                e.type = (btn == LOW)
                    ? EncoderEventType::PRESS
                    : EncoderEventType::RELEASE;
                e.delta = 0;
                pushEncoderEvent(e);
                encButtonPrev[i] = btn;
            }
        }

        encTimer = 0;
    }

    void processEncoderEvents() {
        EncoderEvent e;

        while (popEncoderEvent(e)) {
            if (e.type == EncoderEventType::TURN) {
                auto &m = encMap[e.encoder];

                *m.currentValue += e.delta * m.step;
                *m.currentValue = constrain(
                    *m.currentValue, m.minVal, m.maxVal
                );

                AudioEngine::setSynthParam(m.param, *m.currentValue);
                Display::writeNum(encNames[e.encoder], *m.currentValue);
            }

            if (e.type == EncoderEventType::PRESS) {
                // example: change encoder mode, page, etc.
            }
        }
    }

    /*
    void setEncoderPage(uint8_t page) {
        switch (page) {
        case 0: currentMap = encMapSynth; break;
        case 1: currentMap = encMapFX;    break;
        case 2: currentMap = encMapMenu;  break;
        }
    }
    */

    // ---------------- BUTTONPAD / TRELLIS ----------------
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

    TrellisCallback keyPress(keyEvent evt) {
        uint8_t note = keyToNote(evt.bit.NUM);

        if (evt.bit.EDGE == SEESAW_KEYPAD_EDGE_RISING) {
            // Pressed: light up blue
            trellis.setPixelColor(evt.bit.NUM, 0x0000FF);
            AudioEngine::noteOn(note, Sequencer::getDefaultVelocity());

            // Record note if in recording mode
            if (Sequencer::isRecording && Sequencer::isPlaying) {
                Sequencer::recordNoteEvent(note, Sequencer::getDefaultVelocity());
                //Sequencer::ensureNoteVisible(note);
            }

        } else { // RELEASE
            // Turn LED off
            trellis.setPixelColor(evt.bit.NUM, 0x000000);
            AudioEngine::noteOff(note);

            // Record note-off (velocity 0)
            if (Sequencer::isRecording && Sequencer::isPlaying) {
                Sequencer::recordNoteEvent(note, 0);
            }
        }

        trellis.show();
        return 0;
    }

    void buttonpad() {
        if(trellisTimer > TRELLIS_INTERVAL){
        trellis.read();   // <--- no parameters!
        trellisTimer = 0;
        } 
    }

    // ---------------- INIT  ----------------
    void init() {
        Wire1.begin();            // WIRE1 - IC2
        Wire1.setClock(400000);      
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



