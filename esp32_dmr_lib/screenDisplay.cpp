#include "screenDisplay.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

void ScreenDisplay::init() {
    Wire.begin(SDA_PIN, SCK_PIN);
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println(F("SSD1306 allocation failed"));
        for(;;);
    }
    display.clearDisplay();
    display.display();
}

void ScreenDisplay::setSDAPin(int pin) {
    SDA_PIN = pin;
}

void ScreenDisplay::setSCKPin(int pin) {
    SCK_PIN = pin;
}

void ScreenDisplay::setScreenWidth(int width) {
    SCREEN_WIDTH = width;
}

void ScreenDisplay::setScreenHeight(int height) {
    SCREEN_HEIGHT = height;
}
