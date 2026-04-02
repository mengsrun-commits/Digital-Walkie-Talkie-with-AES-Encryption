#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RadioLib.h>

// =================================================================
// USER CONFIGURATION
// =================================================================
// Set to 'true' for the transmitting walkie-talkie, 'false' for receiving
#define IS_TX_NODE true

const float START_FREQ = 387.0;
const float END_FREQ = 464.0;
const float FREQ_STEP = 1.0;
const int PACKETS_PER_FREQ = 100;
const int PACKET_DELAY_MS = 20; // 20ms between packets

// =================================================================
// PIN DEFINITIONS (Matched to walkie_talkie_cc1101.ino)
// =================================================================
#define CC1101_SCK   14
#define CC1101_MISO  12
#define CC1101_MOSI  13
#define CC1101_CS    4
#define CC1101_GDO0  2
#define BUTTON       15  // Press to start test

#define SDA_PIN 21
#define SCK_PIN 23
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// =================================================================
// GLOBALS
// =================================================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
SPIClass spiCC(HSPI);
CC1101 radio = new Module(CC1101_CS, CC1101_GDO0, RADIOLIB_NC, RADIOLIB_NC, spiCC);

volatile bool rxFlag = false;
void IRAM_ATTR setRxFlag() {
    rxFlag = true;
}

struct TestPacket {
    float frequency;
    uint32_t packet_num;
    uint8_t padding[24]; // Pad to 32 bytes
};

void updateDisplay(String line1, String line2, String line3 = "") {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(line1);
    display.println(line2);
    display.println(line3);
    display.display();
}

void setup() {
    Serial.begin(115200);
    pinMode(BUTTON, INPUT_PULLUP);

    Wire.begin(SDA_PIN, SCK_PIN);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("OLED failed");
        for (;;);
    }
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    updateDisplay("CC1101 Test", IS_TX_NODE ? "Mode: TX" : "Mode: RX", "Init radio...");

    spiCC.begin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);
    int state = radio.begin(START_FREQ);
    if (state != RADIOLIB_ERR_NONE) {
        updateDisplay("Radio Error", String(state));
        while (true);
    }

    // Identical optimal config from walkie_talkie.ino
    radio.setBitRate(100.0);
    radio.setFrequencyDeviation(50.0);
    radio.setRxBandwidth(210.0); // 2*50 + 100 + 10
    radio.setOutputPower(12);
    radio.setSyncWord(0x53, 0x72);
    radio.setCrcFiltering(true);
    radio.fixedPacketLengthMode(sizeof(TestPacket));
    radio.setOOK(false);
    radio.SPIsetRegValue(0x12, 0x1B, 6, 4); // GFSK

    if (!IS_TX_NODE) {
        radio.setPacketReceivedAction(setRxFlag);
    }

    updateDisplay(IS_TX_NODE ? "TX Ready" : "RX Ready", "Press physical", "button to start!");
    Serial.println(IS_TX_NODE ? "TX Node Ready" : "RX Node Ready");
    
    // Wait for button press to synchronize start
    while (digitalRead(BUTTON) == HIGH) {
        delay(10);
    }
    updateDisplay("Starting sweep...", "387 -> 464 MHz");
    delay(1000); // Give user 1s to release button
}

void runTxSweep() {
    TestPacket pkt;
    for (float freq = START_FREQ; freq <= END_FREQ; freq += FREQ_STEP) {
        radio.standby();
        radio.setFrequency(freq);
        delay(50); // Let PLL settle

        updateDisplay("TX Sweeping...", "Freq: " + String(freq, 1) + " MHz");
        Serial.print("TX Freq: "); Serial.print(freq, 1); Serial.println(" MHz");

        pkt.frequency = freq;
        for (int i = 0; i < PACKETS_PER_FREQ; i++) {
            pkt.packet_num = i;
            radio.transmit((uint8_t*)&pkt, sizeof(TestPacket));
            delay(PACKET_DELAY_MS);
        }
    }
    updateDisplay("TX Complete!", "Check RX monitor");
    Serial.println("TX Sweep Complete");
    while (true) delay(1000);
}

void runRxSweep() {
    TestPacket pkt;
    Serial.println("Freq(MHz)\tReceived\tSuccess(%)\tAvg_RSSI");
    
    for (float freq = START_FREQ; freq <= END_FREQ; freq += FREQ_STEP) {
        radio.standby();
        radio.setFrequency(freq);
        delay(50); // Let PLL settle
        
        // Clear flag and start listening
        rxFlag = false;
        radio.startReceive();
        
        int receivedCount = 0;
        float totalRssi = 0;
        
        updateDisplay("RX Listening...", "Freq: " + String(freq, 1) + " MHz");

        bool heardFirstPacket = false;
        unsigned long lastPacketTime = millis();

        // Loop infinitely until the 100-packet burst finishes (detected by 300ms of silence)
        while (true) {
            if (rxFlag) {
                rxFlag = false;
                if (radio.readData((uint8_t*)&pkt, sizeof(TestPacket)) == RADIOLIB_ERR_NONE) {
                    if (abs(pkt.frequency - freq) < 0.1) {
                        heardFirstPacket = true;
                        receivedCount++;
                        totalRssi += radio.getRSSI();
                        lastPacketTime = millis(); // Reset the silence timer
                    }
                }
                radio.startReceive();
            }
            
            // If we started receiving, but the stream stops for 300ms, TX is done with this frequency.
            if (heardFirstPacket && (millis() - lastPacketTime > 300)) {
                break;
            }
            delay(1);
        }

        float successRate = (float)receivedCount / PACKETS_PER_FREQ * 100.0;
        float avgRssi = receivedCount > 0 ? (totalRssi / receivedCount) : -120.0;

        Serial.print(freq, 1);
        Serial.print("\t\t");
        Serial.print(receivedCount);
        Serial.print("\t\t");
        Serial.print(successRate, 1);
        Serial.print("\t\t");
        Serial.println(avgRssi, 1);
    }
    
    updateDisplay("RX Complete!", "Check serial log");
    Serial.println("RX Sweep Complete");
    while (true) delay(1000);
}

void loop() {
    if (IS_TX_NODE) {
        runTxSweep();
    } else {
        runRxSweep();
    }
}
