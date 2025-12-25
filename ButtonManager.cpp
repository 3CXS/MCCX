#include "ButtonManager.h"

//-------------------- Mux16 --------------------

Mux16::Mux16(uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3, uint8_t sig)
    : S0(s0), S1(s1), S2(s2), S3(s3), SIG(sig) {}

void Mux16::begin() {
    pinMode(S0, OUTPUT);
    pinMode(S1, OUTPUT);
    pinMode(S2, OUTPUT);
    pinMode(S3, OUTPUT);
    pinMode(SIG, INPUT_PULLUP);
}

void Mux16::select(uint8_t ch) {
    digitalWrite(S0, ch & 1);
    digitalWrite(S1, (ch >> 1) & 1);
    digitalWrite(S2, (ch >> 2) & 1);
    digitalWrite(S3, (ch >> 3) & 1);
}

uint8_t Mux16::readChannel(uint8_t ch) {
    select(ch);
    delayMicroseconds(5); // tiny settling time
    return digitalRead(SIG);
}

//-------------------- ButtonManager --------------------

ButtonManager* ButtonManager::instance = nullptr;
IntervalTimer ButtonManager::scanTimer;

ButtonManager::ButtonManager(uint16_t debounce)
    : numButtons(0), debounceMs(debounce) {}

void ButtonManager::begin() {
    for (uint8_t i = 0; i < numButtons; i++) {
        ButtonEntry &b = buttons[i];
        b.stableState = b.lastState = b.isMux ? b.mux->readChannel(b.channel) : digitalRead(b.pin);
        b.lastChange = millis();
    }

    instance = this;
    scanTimer.begin(scanISR, 1000); // 1 kHz
}


uint8_t ButtonManager::addMuxButton(Mux16* mux, uint8_t channel, 
                                    BtnCallback cbPress, BtnCallback cbRelease, 
                                    bool pressOnly) {
    if (numButtons >= MAX_BUTTONS) return 255;

    buttons[numButtons++] = {
        true,         // isMux
        mux,
        channel,
        0,            // pin unused
        cbPress,      // press callback
        cbRelease,    // release callback
        pressOnly,
        1, 1,
        millis(),
        debounceMs
    };

    return numButtons - 1;
}

uint8_t ButtonManager::addDirectButton(uint8_t pin, 
                                       BtnCallback cbPress, BtnCallback cbRelease, 
                                       bool pressOnly) {
    if (numButtons >= MAX_BUTTONS) return 255;

    pinMode(pin, INPUT_PULLUP);

    buttons[numButtons++] = {
        false,        // isMux
        nullptr,
        0,
        pin,
        cbPress,      // press callback
        cbRelease,    // release callback
        pressOnly,
        1, 1,
        millis(),
        debounceMs
    };

    return numButtons - 1;
}

//-------------------- ISR --------------------

void ButtonManager::scanISR() {
    if (!instance) return;
    instance->scanButtons();
}

//-------------------- Scan Routine --------------------

void ButtonManager::scanButtons() {
    unsigned long now = millis();

    for (uint8_t i = 0; i < numButtons; i++) {
        ButtonEntry* b = &buttons[i];
        uint8_t raw = b->isMux ? b->mux->readChannel(b->channel) : digitalRead(b->pin);

        // Debounce
        if (raw != b->lastState) {
            b->lastChange = now;
            b->lastState = raw;
        }
        if ((now - b->lastChange) >= b->debounceMs) {
            if (raw != b->stableState) {
                b->stableState = raw;

                // Trigger press/release callbacks
                if (b->triggerOnPress) {
                    if (!b->stableState && b->callbackPress) {   // pressed (active-low)
                        b->callbackPress();
                    } else if (b->stableState && b->callbackRelease) { // released
                        b->callbackRelease();
                    }
                } else {
                    // level-sensitive
                    if (b->callbackPress) b->callbackPress();
                }
            }
        }
    }
}
