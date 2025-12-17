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
    void writeXString(uint16_t x, uint16_t y,
                      uint16_t w = 16, uint16_t h = 16,
                      uint8_t font = 0,
                      uint32_t color = 65535,
                      uint8_t align = 0,
                      const char* text = "");

    void draw32XRow(uint16_t startX, uint16_t startY, uint8_t spacing = 4);

} // namespace Display

#endif