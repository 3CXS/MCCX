#include "Display.h"

namespace Display {

    static EasyNex* displayInstance = nullptr;

    void init(HardwareSerial &serialPort, uint32_t baud, uint8_t brightness) {
        if(displayInstance) delete displayInstance;
        displayInstance = new EasyNex(serialPort);
        displayInstance->begin(baud);
        delay(500);
        setBrightness(brightness);
    }
  
    void setBrightness(uint8_t bright) {
        if(displayInstance) {
            char cmd[16];
            snprintf(cmd, sizeof(cmd), "dim=%d", bright);
            displayInstance->writeStr(cmd, ""); // empty value, just send command
        }
    }

    void writeStr(const char* obj, const char* value) {
        if(displayInstance) displayInstance->writeStr(obj, value);
    }

    void writeCmd(const char* value) {
        if(displayInstance) displayInstance->writeStr(value);
    }

    void writeNum(const char* obj, int32_t value) {
        if(displayInstance) displayInstance->writeNum(obj, value);
    }

    EasyNex* getInstance() {
        return displayInstance;
    }

    void writeXString(uint16_t x, uint16_t y,
                    uint16_t w, uint16_t h,
                    uint8_t font,
                    uint16_t fgColor,
                    uint16_t bgColor,
                    uint8_t alignH,
                    uint8_t alignV,
                    uint8_t fill,
                    const char* text) {

        char cmd[160];
        snprintf(cmd, sizeof(cmd),
                "xstr %d,%d,%d,%d,%d,%d,%d,%d,%d,%d,\"%s\"",
                x, y, w, h,
                font,
                fgColor,
                bgColor,
                alignH,
                alignV,
                fill,
                text);

        // IMPORTANT: send as RAW command
        displayInstance->writeStr(cmd);
    }

    void draw32XRow(uint16_t startX, uint16_t startY, uint8_t spacing) {
        const uint8_t count = 32;
        const uint16_t charW = 14;
        const uint16_t charH = 18;

        for (uint8_t i = 0; i < count; i++) {
            uint16_t x = startX + i * (charW + spacing);
            writeXString(
                x,
                startY,
                charW,
                charH,
                0,        // font
                65535,    // white
                0,        // background transparent
                1,        // horizontal center
                1,        // vertical center
                3,        // fill = none (important!)
                "X"
            );
        }
    }

} // namespace Display
