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

} // namespace Display
