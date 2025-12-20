#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include "EasyNextionLibrary.h"

namespace Display {

    // --- Exposed functions ---
    void init(HardwareSerial &serialPort, uint32_t baud = 921600, uint8_t brightness = 10);
    void writeStr(const char* obj, const char* value);
    void writeCmd(const char* value);
    void writeNum(const char* obj, int32_t value);
    void setBrightness(uint8_t bright);
    EasyNex* getInstance(); // optional, if you need direct access

} // namespace Display

#endif

