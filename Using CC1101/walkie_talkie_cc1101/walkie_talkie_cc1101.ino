#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <RadioLib.h>
#include <driver/i2s.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/adc.h"
#include <Preferences.h>

// =================================================================
// 1. PIN DEFINITIONS
// =================================================================
// OLED
#define SDA_PIN 21
#define SCK_PIN 23
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// Hardware Controls
#define BUTTON 15
#define LED_PIN 5
#define POT_CHANNEL ADC1_CHANNEL_6
#define BTN_LEFT 17
#define BTN_RIGHT 16

// INMP441 I2S Mic
#define I2S_WS 25
#define I2S_SD 33
#define I2S_SCK 32

// MAX98357A Amp
#define MAX98357A_LRCLK 18
#define MAX98357A_BCLK 19
#define MAX98357A_DIN 22

// CC1101 SPI
#define CC1101_SCK   14
#define CC1101_MISO  12
#define CC1101_MOSI  13
#define CC1101_GDO0  2
#define CC1101_CS    4

SPIClass spiCC(HSPI);
class CustomCC1101 : public CC1101 {
  public:
    CustomCC1101(Module* mod) : CC1101(mod) {}
    int16_t SPIsetRegValue(uint8_t reg, uint8_t value, uint8_t msb = 7, uint8_t lsb = 0, uint8_t checkInterval = 2) {
      return CC1101::SPIsetRegValue(reg, value, msb, lsb, checkInterval);
    }
    uint8_t SPIgetRegValue(uint8_t reg, uint8_t msb = 7, uint8_t lsb = 0) {
      return CC1101::SPIgetRegValue(reg, msb, lsb);
    }
    void SPIsendCommand(uint8_t cmd) {
      CC1101::SPIsendCommand(cmd);
    }
};
CustomCC1101 radio = new Module(CC1101_CS, CC1101_GDO0, RADIOLIB_NC, RADIOLIB_NC, spiCC);

// =================================================================
// 2. GLOBAL VARIABLES & STATES
// =================================================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

enum DisplayState { STATE_IDLE, STATE_TRANSMIT, STATE_RECEIVE, STATE_VOLUME, STATE_STARTUP, STATE_SCREEN_OFF, STATE_CHANNEL };
volatile DisplayState displayState = STATE_STARTUP;

String welcomeText = "Welcome to TEE!   ";
int16_t scrollX = SCREEN_WIDTH;
const uint8_t scrollSpeed = 5;
uint8_t dotCount = 0;

// Timers
unsigned long lastVolumeChange = 0;
unsigned long lastReceiveTime = 0;
unsigned long lastAnimUpdate = 0;
unsigned long startupTime = 0;
unsigned long lastChannelActivity = 0;
const unsigned long VOLUME_SHOW_DURATION = 2000;
const unsigned long SCREEN_OFF_DELAY = 1000;
const unsigned long STARTUP_SHOW_DURATION = 1500;
const unsigned long RECEIVE_TIMEOUT = 500;
const unsigned long ANIM_INTERVAL = 300;
const unsigned long CHANNEL_SHOW_DURATION = 1500;
const unsigned long BUTTON_HOLD_DURATION = 700;
const unsigned long RAPID_CHANNEL_SWITCH_TIME = 200;

// Radio & Buffers
uint8_t currentChannel = 1;
volatile bool channelUpdatePending = false;
const uint8_t MAX_CHANNEL_VALUE = 50;
const uint8_t MIN_CHANNEL_VALUE = 1;
const float CHANNEL_SPACING = 1.0; //MHz
Preferences preferences;
uint8_t savedChannel = MIN_CHANNEL_VALUE;

#define BASE_FREQUENCY 433 //MHz
#define SAMPLE_RATE 16000
#define PACKET_SIZE 32
#define ADPCM_HEADER_SIZE 3
#define ADPCM_DATA_BYTES (PACKET_SIZE - ADPCM_HEADER_SIZE) 
#define SAMPLES_PER_FRAME (ADPCM_DATA_BYTES * 2)  

uint8_t txBuffer[2][PACKET_SIZE];
volatile bool bufferReady[2] = {false, false};
volatile uint8_t fillBuffer = 0;
volatile uint8_t txBufferIndex = 0;
volatile int txFails = 0;
volatile bool isTransmitting = false;

// Receive-side jitter ring buffer
#define RX_RING_SIZE 18
#define RX_PREFILL 2 //How many packets to fill before sending to amp
uint8_t rxRing[RX_RING_SIZE][PACKET_SIZE];
volatile uint8_t rxHead = 0;
volatile uint8_t rxTail = 0;
volatile uint8_t rxCount = 0;
volatile bool rxPlaying = false;
portMUX_TYPE rxCountMux = portMUX_INITIALIZER_UNLOCKED;

// ADPCM encoder state (persistent across frames during TX)
typedef struct { int16_t predicted; uint8_t index; } ADPCMState;

// Async RX interrupt flag
volatile bool receivedFlag = false;
void IRAM_ATTR setFlag() {
    receivedFlag = true;
}

ADPCMState txAdpcmState = {0, 0};

// Audio Gain Controls
int lastPotValue = -1;
const int VOLUME_HYSTERESIS = 400;

// Display Float vs Fast Integer Math
volatile float GAIN_FOR_UI = 1.0f;               // Kept for drawing the OLED volume bar
volatile int32_t GAIN = 256;        // OPTIMIZED: 1.0 = 256 for fast audio math
const float GAIN_MIN = 0.1f;
const float GAIN_MAX = 3.5f;

// Limiter thresholds 
const int32_t LIMIT_PRE  = 10000; 
const int32_t LIMIT_POST = 28000;

// Raise this value if you still get feedback; lower it if soft speech is cut off.
const int32_t NOISE_GATE_THRESHOLD = 300; // range: 0 (disabled) – ~32767 (full-scale); practical: 50–500
const int32_t FIXED_MIC_GAIN = 179;  // 0.7x gain (256 = 1.0x)

// =================================================================
// 3. AUDIO PROCESSING MATH
// =================================================================

// Fast soft limiter
inline int32_t softLimiter(int32_t s, int32_t thresh) {
    if (s > thresh)  s = thresh + (s - thresh) / 4;
    if (s < -thresh) s = -thresh + (s + thresh) / 4;
    return s;
}

// High-pass filter
int16_t hpFilter(int16_t input) {
    static int32_t prev_input = 0;
    static int32_t prev_output = 0;
    // 65208/65536 represents ~0.995
    int32_t output = input - prev_input + ((65208 * prev_output) >> 16);
    prev_input = input;
    prev_output = output;
    return (int16_t)output;
}

// IMA ADPCM step table
static const int16_t adpcmStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t adpcmIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

// ADPCM encoder: encodes numSamples PCM into packed 4-bit nibbles
void adpcmEncode(int16_t *input, uint8_t *output, int numSamples, ADPCMState *state) {
    for (int i = 0; i < numSamples; i++) {
        int diff = input[i] - state->predicted;
        uint8_t nibble = 0;
        if (diff < 0) { nibble = 8; diff = -diff; }

        int16_t step = adpcmStepTable[state->index];
        int32_t diffq = step >> 3;
        if (diff >= step)  { nibble |= 4; diff -= step; diffq += step; }
        step >>= 1;
        if (diff >= step)  { nibble |= 2; diff -= step; diffq += step; }
        step >>= 1;
        if (diff >= step)  { nibble |= 1; diffq += step; }

        state->predicted += (nibble & 8) ? -diffq : diffq;
        if (state->predicted > 32767)  state->predicted = 32767;
        if (state->predicted < -32768) state->predicted = -32768;

        int idx = state->index + adpcmIndexTable[nibble];
        if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
        state->index = (uint8_t)idx;

        if (i & 1)
            output[i >> 1] |= nibble;         // Low nibble (odd sample)
        else
            output[i >> 1] = nibble << 4;      // High nibble (even sample)
    }
}

// ADPCM decoder: decodes packed 4-bit nibbles into PCM samples
void adpcmDecode(uint8_t *input, int16_t *output, int numSamples, ADPCMState *state) {
    for (int i = 0; i < numSamples; i++) {
        uint8_t nibble = (i & 1) ? (input[i >> 1] & 0x0F)
                                : ((input[i >> 1] >> 4) & 0x0F);

        int16_t step = adpcmStepTable[state->index];
        int32_t diffq = step >> 3;
        if (nibble & 4) diffq += step;
        if (nibble & 2) diffq += step >> 1;
        if (nibble & 1) diffq += step >> 2;

        state->predicted += (nibble & 8) ? -diffq : diffq;
        if (state->predicted > 32767)  state->predicted = 32767;
        if (state->predicted < -32768) state->predicted = -32768;

        int idx = state->index + adpcmIndexTable[nibble];
        if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
        state->index = (uint8_t)idx;

        output[i] = state->predicted;
    }
}

// =================================================================
// 4. I2S INITIALIZATION (Single port, pins swapped on PTT)
// =================================================================
void i2sinit() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = 16,
        .dma_buf_len = SAMPLES_PER_FRAME,
        .use_apll = false
    };
    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
}

void switchToMicPins() {
    i2s_pin_config_t mic_pins = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = -1,
        .data_in_num = I2S_SD
    };
    i2s_set_pin(I2S_NUM_0, &mic_pins);
}

void switchToSpeakerPins() {
    i2s_pin_config_t amp_pins = {
        .bck_io_num = MAX98357A_BCLK,
        .ws_io_num = MAX98357A_LRCLK,
        .data_out_num = MAX98357A_DIN,
        .data_in_num = -1
    };
    i2s_set_pin(I2S_NUM_0, &amp_pins);
}

// =================================================================
// 5. FREERTOS TASKS
// =================================================================

void audioCaptureTask(void *param) {
    size_t bytesRead;
    int32_t rawBuffer[SAMPLES_PER_FRAME];
    int16_t pcmBuffer[SAMPLES_PER_FRAME];

    while (1) {
        if (!isTransmitting) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        while (bufferReady[fillBuffer]) {
            vTaskDelay(1);
        }

        i2s_read(I2S_NUM_0, rawBuffer, SAMPLES_PER_FRAME * sizeof(int32_t), &bytesRead, portMAX_DELAY);

        // --- DC Removal & Noise gate: measure TRUE AC peak ---
        int32_t peak = 0;
        for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
            int32_t sample = rawBuffer[i] >> 14;

            // 1. Remove DC offset FIRST so filter state stays accurate 
            // and noise gate strictly triggers on AC speech peaks
            sample = hpFilter((int16_t)sample);

            int32_t a = (sample < 0) ? -sample : sample;
            if (a > peak) peak = a;
            
            pcmBuffer[i] = (int16_t)sample; 
        }

        if (peak < NOISE_GATE_THRESHOLD) {
            // Silence: header predicted=0, index=0, data bytes=0
            memset(txBuffer[fillBuffer], 0, PACKET_SIZE);
            txAdpcmState.predicted = 0;
            txAdpcmState.index = 0;
        } else {
            for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
                int32_t sample = pcmBuffer[i];

                // 2. Early soft limit
                sample = softLimiter(sample, LIMIT_PRE);

                // 3. Apply fixed mic gain
                sample = (sample * FIXED_MIC_GAIN) >> 8;
                
                // 4. Final limit
                sample = softLimiter(sample, LIMIT_PRE);
                pcmBuffer[i] = (int16_t)sample;
            }

            // Pack ADPCM header (predictor state for independent packet decode)
            txBuffer[fillBuffer][0] = (uint8_t)(txAdpcmState.predicted >> 8);
            txBuffer[fillBuffer][1] = (uint8_t)(txAdpcmState.predicted & 0xFF);
            txBuffer[fillBuffer][2] = txAdpcmState.index;

            // Encode 58 samples → 29 bytes of 4-bit ADPCM
            adpcmEncode(pcmBuffer, &txBuffer[fillBuffer][ADPCM_HEADER_SIZE],
                        SAMPLES_PER_FRAME, &txAdpcmState);
        }

        bufferReady[fillBuffer] = true;
        fillBuffer ^= 1;
        vTaskDelay(1);
    }
}

// Radio-only task: handles TX and RX without ever blocking on I2S
void radioTask(void *param) {
    static unsigned long lastTxPrint = 0;
    static unsigned long lastRxPrint = 0;
    static int txSuccessCount = 0;
    static int txFailCount = 0;
    
    while (1) {
        if (isTransmitting) {
            // --- TRANSMIT MODE ---
            if (bufferReady[txBufferIndex]) {
                // Use non-blocking startTransmit to bypass RadioLib's pin wait timeouts
                int state = radio.startTransmit(txBuffer[txBufferIndex], PACKET_SIZE);
                
                if (state == RADIOLIB_ERR_NONE) {
                    // Poll MARCSTATE until IDLE (0x01)
                    unsigned long startTx = millis();
                    while (radio.SPIgetRegValue(0x35, 4, 0) != 0x01) { // 0x35 is MARCSTATE
                        taskYIELD();
                        if (millis() - startTx > 15) { // 15ms safety timeout
                            state = RADIOLIB_ERR_TX_TIMEOUT;
                            break;
                        }
                    }
                }
                
                radio.standby();
                radio.SPIsendCommand(0x3B); // CMD_FLUSH_TX
                
                bufferReady[txBufferIndex] = false;
                
                if (state == RADIOLIB_ERR_NONE) {
                    txFails = 0;
                    txSuccessCount++;
                } else {
                    txFails++;
                }
                txBufferIndex ^= 1;

                // Print TX stats every 2 seconds
                if (millis() - lastTxPrint > 2000) {
                    Serial.printf("TX: %d ok, %d fail | Freq: %.2f MHz\n", txSuccessCount, txFailCount, BASE_FREQUENCY + currentChannel * CHANNEL_SPACING);
                    lastTxPrint = millis();
                    txSuccessCount = 0;
                    txFailCount = 0;
                }
            }
            taskYIELD();
        } else {
            // --- RECEIVE MODE (interrupt-driven map to GDO0) ---
            if (receivedFlag) {
                receivedFlag = false;
                
                uint8_t tempBuf[PACKET_SIZE];
                int state = radio.readData(tempBuf, PACKET_SIZE);
                
                if (state == RADIOLIB_ERR_NONE) {
                    if (rxCount < RX_RING_SIZE) {
                        Serial.printf("RX: packet received! RSSI: %.1f dBm | ring: %d/%d\n", radio.getRSSI(), rxCount + 1, RX_RING_SIZE);
                        memcpy(rxRing[rxHead], tempBuf, PACKET_SIZE);
                        rxHead = (rxHead + 1) % RX_RING_SIZE;
                        portENTER_CRITICAL(&rxCountMux);
                        rxCount++;
                        portEXIT_CRITICAL(&rxCountMux);
                        lastReceiveTime = millis();

                        if (displayState == STATE_IDLE) {
                            displayState = STATE_RECEIVE;
                            dotCount = 0;
                            lastAnimUpdate = millis();
                        }
                    }
                }
                // Prep for next interrupt
                radio.startReceive();
            }

            if (channelUpdatePending) {
                radio.standby();
                radio.setFrequency(BASE_FREQUENCY + currentChannel * CHANNEL_SPACING);
                radio.startReceive();
                channelUpdatePending = false;
                receivedFlag = false;
            }
            vTaskDelay(1);
        }
    }
}

// Playback task: decodes ring buffer and writes to I2S
void playbackTask(void *param) {
    size_t bytes_written;
    int16_t stereoBuf[SAMPLES_PER_FRAME * 2];
    uint8_t underrunCount = 0;

    while (1) {
        if (isTransmitting) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // Play from ring buffer once prefill threshold is reached
        if ((rxPlaying && rxCount > 0) || rxCount >= RX_PREFILL) {
            rxPlaying = true;
            uint8_t *pkt = rxRing[rxTail];
            int32_t currentGain = GAIN;

            // Decode ADPCM header
            ADPCMState rxState;
            rxState.predicted = (int16_t)((pkt[0] << 8) | pkt[1]);
            rxState.index = pkt[2];
            if (rxState.index > 88) rxState.index = 88;

            // Decode 29 bytes of 4-bit ADPCM → 58 PCM samples
            int16_t pcmOut[SAMPLES_PER_FRAME];
            adpcmDecode(&pkt[ADPCM_HEADER_SIZE], pcmOut, SAMPLES_PER_FRAME, &rxState);

            for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
                int32_t sample = pcmOut[i];
                sample = (sample * currentGain) >> 8;
                sample = softLimiter(sample, LIMIT_POST);

                int16_t out = (int16_t)sample;
                stereoBuf[i * 2]     = out;
                stereoBuf[i * 2 + 1] = out;
            }
            rxTail = (rxTail + 1) % RX_RING_SIZE;
            portENTER_CRITICAL(&rxCountMux);
            rxCount--;
            portEXIT_CRITICAL(&rxCountMux);
            underrunCount = 0;
            digitalWrite(LED_PIN, HIGH);
        } else {
            // Underrun — output silence but tolerate brief gaps
            memset(stereoBuf, 0, sizeof(stereoBuf));
            if (rxPlaying) {
                underrunCount++;
                if (underrunCount >= 3) {
                    rxPlaying = false;
                    underrunCount = 0;
                }
            }
            digitalWrite(LED_PIN, LOW);
        }
        // i2s_write paces playback naturally — blocking here does NOT affect radio reads
        i2s_write(I2S_NUM_0, stereoBuf, sizeof(stereoBuf), &bytes_written, portMAX_DELAY);
    }
}

// Monitors the physical PTT button and Channel Buttons
void buttonTask(void *param) {
    bool lastLeft = HIGH;
    bool lastRight = HIGH;

    while (1) {
        bool pressed = !digitalRead(BUTTON);
        bool leftState = digitalRead(BTN_LEFT);
        bool rightState = digitalRead(BTN_RIGHT);
        
        if (pressed && !isTransmitting) {
            isTransmitting = true;
            radio.standby();
            switchToMicPins();
            txAdpcmState.predicted = 0;
            txAdpcmState.index = 0;
            vTaskDelay(pdMS_TO_TICKS(10));

            if (displayState != STATE_SCREEN_OFF)
                displayState = STATE_TRANSMIT;
            dotCount = 0;
            lastAnimUpdate = millis();
            Serial.println("TX mode");
            
        } else if (!pressed && isTransmitting) {
            isTransmitting = false;
            radio.standby();
            switchToSpeakerPins();
            vTaskDelay(pdMS_TO_TICKS(10));

            // Reset jitter buffer for clean playback
            rxHead = 0;
            rxTail = 0;
            rxCount = 0;
            rxPlaying = false;

            if (displayState != STATE_SCREEN_OFF) {
                displayState = STATE_IDLE;
                scrollX = SCREEN_WIDTH;
            }
            Serial.println("RX mode");
            radio.startReceive(); // Re-arm RX!
        }

        unsigned long now = millis();
        static unsigned long lastLeftPressTime = 0;
        static unsigned long lastRightPressTime = 0;
        static unsigned long leftHoldStartTime = 0;
        static unsigned long rightHoldStartTime = 0;
        static bool leftRepeating = false;
        static bool rightRepeating = false;

        if (leftState == LOW && !isTransmitting) {
            if (lastLeft == HIGH) { // Initial press
                if (now - lastLeftPressTime > 200) { // 200ms deep software debounce
                    lastLeftPressTime = now;
                    leftHoldStartTime = now;
                    leftRepeating = false;
                    
                    if (displayState != STATE_CHANNEL) {
                        if (displayState != STATE_SCREEN_OFF && displayState != STATE_STARTUP) {
                            displayState = STATE_CHANNEL;
                            lastChannelActivity = millis();
                        }
                    } else {
                        if (currentChannel > MIN_CHANNEL_VALUE) currentChannel--;
                        else currentChannel = MAX_CHANNEL_VALUE;
                        channelUpdatePending = true;
                        lastChannelActivity = millis();
                    }
                }
            } else { // Being held
                if (!leftRepeating && (now - leftHoldStartTime > BUTTON_HOLD_DURATION)) {
                    leftRepeating = true;
                    leftHoldStartTime = now; // Reset timer for the 500ms autorepeat intervals
                }
                if (leftRepeating && (now - leftHoldStartTime > RAPID_CHANNEL_SWITCH_TIME)) {
                    leftHoldStartTime = now;
                    // Directly change channel since UI is already awake
                    if (currentChannel > MIN_CHANNEL_VALUE) currentChannel--;
                    else currentChannel = MAX_CHANNEL_VALUE;
                    channelUpdatePending = true;
                    lastChannelActivity = millis();
                }
            }
        } else {
            leftRepeating = false;
        }

        if (rightState == LOW && !isTransmitting) {
            if (lastRight == HIGH) { // Initial press
                if (now - lastRightPressTime > 200) { // 200ms deep software debounce
                    lastRightPressTime = now;
                    rightHoldStartTime = now;
                    rightRepeating = false;

                    if (displayState != STATE_CHANNEL) {
                        if (displayState != STATE_SCREEN_OFF && displayState != STATE_STARTUP) {
                            displayState = STATE_CHANNEL;
                            lastChannelActivity = millis();
                        }
                    } else {
                        if (currentChannel < MAX_CHANNEL_VALUE) currentChannel++;
                        else currentChannel = MIN_CHANNEL_VALUE;
                        channelUpdatePending = true;
                        lastChannelActivity = millis();
                    }
                }
            } else { // Being held
                if (!rightRepeating && (now - rightHoldStartTime > BUTTON_HOLD_DURATION)) {
                    rightRepeating = true;
                    rightHoldStartTime = now; // Reset timer for the 500ms autorepeat intervals
                }
                if (rightRepeating && (now - rightHoldStartTime > RAPID_CHANNEL_SWITCH_TIME)) {
                    rightHoldStartTime = now;
                    // Directly change channel since UI is already awake
                    if (currentChannel < MAX_CHANNEL_VALUE) currentChannel++;
                    else currentChannel = MIN_CHANNEL_VALUE;
                    channelUpdatePending = true;
                    lastChannelActivity = millis();
                }
            }
        } else {
            rightRepeating = false;
        }

        lastLeft = leftState;
        lastRight = rightState;

        vTaskDelay(pdMS_TO_TICKS(20)); // Debounce
    }
}

// Monitors Potentiometer and scales the gain
void volumeReadTask(void *param) {
    while (1) { 
        int potValue = adc1_get_raw(POT_CHANNEL);

        // Detect pot at absolute bottom (switch is about to cut power)
        // Immediately blank OLED before ESP32 loses power
        if (potValue < 30 && displayState != STATE_SCREEN_OFF) {
            display.clearDisplay();
            display.display();
            display.ssd1306_command(SSD1306_DISPLAYOFF);
            displayState = STATE_SCREEN_OFF;
            GAIN_FOR_UI = GAIN_MIN;
            GAIN = (int32_t)(GAIN_MIN * 256); // Fast integer for audio processing
            lastPotValue = potValue;
        }
        else if (abs(potValue - lastPotValue) > VOLUME_HYSTERESIS) {
            lastPotValue = potValue;

            float newGain = 0.1f + (float)potValue * ((GAIN_MAX-0.1f) / 4095.0f);
            newGain = constrain(newGain, GAIN_MIN, GAIN_MAX);

            GAIN_FOR_UI = newGain; // Float kept for drawing the UI
            GAIN = (int32_t)(newGain * 256); 

            lastVolumeChange = millis();
            if (displayState == STATE_SCREEN_OFF && newGain <= GAIN_MIN) {
                // Stay off — don't flicker the screen for pot noise at zero
            } else {
                if (displayState == STATE_SCREEN_OFF) {
                    display.ssd1306_command(SSD1306_DISPLAYON); // Wake the OLED hardware back up
                }
                displayState = STATE_VOLUME;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// =================================================================
// 6. OLED UI HELPERS & TASK
// =================================================================
void drawCenteredText(const char* text, uint8_t textSize = 3) {
    uint16_t w = strlen(text) * 6 * textSize;
    uint16_t h = 8 * textSize;
    display.setTextSize(textSize);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor((SCREEN_WIDTH - w) / 2, (SCREEN_HEIGHT - h) / 2);
    display.print(text);
}

void drawVolume(float gain) {
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);

    const char* volText = "Volume:";
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(volText, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((display.width() - w) / 2, 0);
    display.print(volText);

    int bars = constrain((int)((gain - 0.1f) / ((GAIN_MAX-0.1f)/10.0f)), 0, 10);
    int barWidth = 8, spacing = 2;
    int totalBarWidth = bars > 0 ? (bars * barWidth) + ((bars - 1) * spacing) : 0;
    int startX = (display.width() - totalBarWidth) / 2;
    int barY = display.height() / 2 - 5;

    for (int i = 0; i < bars; i++) {
        display.fillRect(startX + i * (barWidth + spacing), barY, barWidth, 20, SSD1306_WHITE);
    }
}

void OLEDTask(void* parameter) {
    const uint16_t CHAR_WIDTH = 6 * 3; // For scrolling text math
    const int16_t textY = (SCREEN_HEIGHT - 24) / 2; 

    while (true) {
        unsigned long now = millis();
        display.clearDisplay();

        switch (displayState) {
            case STATE_TRANSMIT: {
                if (now - lastAnimUpdate >= ANIM_INTERVAL) {
                    lastAnimUpdate = now;
                    dotCount = (dotCount + 1) % 4;
                }
                char txText[12] = "SENDING";
                for (int i = 0; i < dotCount; i++) strcat(txText, ".");
                drawCenteredText(txText, 2);
                break;
            }
            case STATE_RECEIVE: {
                if (now - lastAnimUpdate >= ANIM_INTERVAL) {
                    lastAnimUpdate = now;
                    dotCount = (dotCount + 1) % 4;
                }
                char rxText[13] = "RECEIVING";
                for (int i = 0; i < dotCount; i++) strcat(rxText, ".");
                drawCenteredText(rxText, 2);
                break;
            }
            case STATE_VOLUME: {
                if(displayState != STATE_STARTUP){
                    bool volIsZero = (GAIN_FOR_UI <= GAIN_MIN);
                    unsigned long timeout = volIsZero ? SCREEN_OFF_DELAY : VOLUME_SHOW_DURATION;
                    if (now - lastVolumeChange > timeout) {
                        if (volIsZero) {
                            display.clearDisplay();
                            display.display();
                            display.ssd1306_command(SSD1306_DISPLAYOFF); // HW off so OLED is blank when ESP32 loses power
                            displayState = STATE_SCREEN_OFF;
                        } else {
                            displayState = STATE_IDLE;
                            scrollX = SCREEN_WIDTH;
                        }
                    } else {
                        drawVolume(GAIN_FOR_UI);
                    }
                }
                break;
            }
            case STATE_SCREEN_OFF: {
                // Screen stays blank — clearDisplay already called above
                break;
            }
            case STATE_STARTUP: {
                if (now - startupTime > STARTUP_SHOW_DURATION) {
                    displayState = STATE_IDLE;
                    scrollX = SCREEN_WIDTH;
                } else {
                    drawCenteredText("Starting..", 2);
                }
                break;
            }
            case STATE_CHANNEL: {
                if (now - lastChannelActivity > CHANNEL_SHOW_DURATION) {
                    displayState = STATE_IDLE;
                    scrollX = SCREEN_WIDTH;
                    if (currentChannel != savedChannel) {
                        preferences.putUChar("channel", currentChannel);
                        savedChannel = currentChannel;
                    }
                } else {
                    char chText[16];
                    snprintf(chText, sizeof(chText), "CH %d", currentChannel);
                    drawCenteredText(chText, 3);
                }
                break;
            }
            case STATE_IDLE:
            default: {
                display.setTextSize(3);
                display.setTextColor(SSD1306_WHITE);
                display.setTextWrap(false);
                display.setCursor(scrollX, textY);
                display.print(welcomeText);
                
                scrollX -= scrollSpeed;
                if (scrollX + (int16_t)(welcomeText.length() * CHAR_WIDTH) < 0) {
                    scrollX = SCREEN_WIDTH;
                }
                break;
            }
        }

        // Return to idle if RX times out
        if (displayState == STATE_RECEIVE && (now - lastReceiveTime > RECEIVE_TIMEOUT)) {
            displayState = STATE_IDLE;
            scrollX = SCREEN_WIDTH;
            dotCount = 0;
        }

        display.display();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// =================================================================
// 7.  CC1101 CONFIGURATION (RadioLib)
// =================================================================

void cc1101_init(){
    const float bitrate = 250.0; //kbps
    const float frequency_deviation = 127.0; //kHz 
    const float rxBandwidth = 540.0; //kHz 
    const int8_t radio_power = 10; // dBm
    
    int state = radio.begin(BASE_FREQUENCY + currentChannel * CHANNEL_SPACING, bitrate, frequency_deviation, rxBandwidth, radio_power, 32);
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("CC1101 init success!"));
        
        radio.setCrcFiltering(true);
        radio.fixedPacketLengthMode(PACKET_SIZE);
        
        uint8_t syncWord[2] = {0x53, 0x72};
        radio.setSyncWord(syncWord, 2);
        
        // Hard-set GFSK modulation via inherited SPIsetRegValue wrapper
        radio.SPIsetRegValue(0x12, 0x10, 6, 4);

        // Map interrupt dynamically back to RadioLib scope
        radio.setPacketReceivedAction(setFlag);
        radio.startReceive();
    } else {
        Serial.print(F("CC1101 init failed, code "));
        Serial.println(state);
    }
}
// =================================================================
// 8. MAIN HARDWARE SETUP
// =================================================================

void setup() {
    Serial.begin(115200);
    
    // --- GPIO Setup ---
    pinMode(BUTTON, INPUT_PULLUP);
    pinMode(BTN_LEFT, INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    
    // --- OLED Setup ---
    Wire.begin(SDA_PIN, SCK_PIN);
    Wire.setClock(400000); // OPTIMIZATION: High-speed I2C
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("SSD1306 allocation failed");
        for (;;);
    }
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(0xCF);
    display.clearDisplay();
    display.display();
    startupTime = millis();
    
    // --- Preferences Setup ---
    preferences.begin("walkie", false);
    currentChannel = preferences.getUChar("channel", savedChannel);
    if (currentChannel < MIN_CHANNEL_VALUE || currentChannel > MAX_CHANNEL_VALUE){
        currentChannel = MIN_CHANNEL_VALUE;
    }
    savedChannel = currentChannel;
    
    // --- SPI Radio Setup ---
    spiCC.begin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);
    cc1101_init();

    // --- I2S Initialization (single port, starts in RX/speaker mode) ---
    i2sinit();
    switchToSpeakerPins();

    // --- ADC Potentiometer Setup ---
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(POT_CHANNEL, ADC_ATTEN_DB_11);

    // Init Volume to prevent pop-up on boot
    lastPotValue = adc1_get_raw(POT_CHANNEL);
    float bootGain = constrain(0.1f + (float)lastPotValue * ((GAIN_MAX-0.1f) / 4095.0f), GAIN_MIN, GAIN_MAX);
    GAIN_FOR_UI = bootGain;
    GAIN = (int32_t)(bootGain * 256);
    
    lastVolumeChange = millis();
    scrollX = SCREEN_WIDTH;

    // --- Boot Tasks on Dual Cores ---
    xTaskCreatePinnedToCore(audioCaptureTask, "AudioCapture", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(radioTask, "RadioTask", 4096, NULL, 3, NULL, 1);       // Highest priority on core 1
    xTaskCreatePinnedToCore(playbackTask, "Playback", 4096, NULL, 2, NULL, 1);     // Below radio, paced by I2S DMA
    xTaskCreatePinnedToCore(buttonTask, "ButtonTask", 2048, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(volumeReadTask, "VolumeRead", 2048, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(OLEDTask, "OLED Task", 4096, NULL, 1, NULL, 0);

    Serial.println("ESP32 Walkie-Talkie Ready!");
}

void loop() {
    // FreeRTOS handles all operations.
}