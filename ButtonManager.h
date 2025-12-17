#ifndef BUTTON_MANAGER_H
#define BUTTON_MANAGER_H

#include <Arduino.h>
#include <IntervalTimer.h>

class Mux16 {
public:
    Mux16(uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3, uint8_t sig);

    void begin();
    void select(uint8_t ch);
    uint8_t readChannel(uint8_t ch);

private:
    uint8_t S0, S1, S2, S3, SIG;
};

// Callback type for buttons
typedef void (*BtnCallback)();

class ButtonManager {
public:
    struct ButtonEntry {
        bool isMux;
        Mux16* mux;
        uint8_t channel;
        uint8_t pin;
        BtnCallback callback;
        bool triggerOnPress;      // true = trigger only on press, false = level-sensitive
        uint8_t stableState;      // last stable state
        uint8_t lastState;        // last raw state
        unsigned long lastChange; // last change timestamp
        uint16_t debounceMs;
    };

    ButtonManager(uint16_t debounce = 10);

    uint8_t addMuxButton(Mux16* mux, uint8_t channel, BtnCallback cb, bool pressOnly = true);
    uint8_t addDirectButton(uint8_t pin, BtnCallback cb, bool pressOnly = true);

    void begin();

private:
    static const uint8_t MAX_BUTTONS = 32;
    ButtonEntry buttons[MAX_BUTTONS];
    uint8_t numButtons;
    uint16_t debounceMs;

    static ButtonManager* instance;  // ISR reference
    static IntervalTimer scanTimer;
    static void scanISR();           // ISR runs at 1 kHz

    void scanButtons();              // real scan routine
};

#endif
