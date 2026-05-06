#include <stdint.h>
#ifndef SCREEN_DISPLAY_H
#define SCREEN_DISPLAY_H

class ScreenDisplay {
    private:
        const int SDA_PIN = 21;
        const int SCK_PIN = 23;
        const int SCREEN_WIDTH = 128;
        const int SCREEN_HEIGHT = 64;
        int16_t scrollX = SCREEN_WIDTH;
        const uint8_t scrollSpeed = 5;
        uint8_t dotCount = 0;
    public:
        void init();
        void setSDAPin(int);
        void setSCKPin(int);
        void setScreenWidth(int);
        void setScreenHeight(int);
        void drawCenteredText(const char*, uint8_t);
        void drawVolume(float);
        void displayState(int);

};

#endif