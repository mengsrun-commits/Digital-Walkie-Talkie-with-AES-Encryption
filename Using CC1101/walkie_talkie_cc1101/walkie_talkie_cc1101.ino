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
#include <ArduinoJson.h>
#include "mbedtls/gcm.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include <Fonts/FreeMono9pt7b.h>

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
#define ENABLE_ACTIVITY_LED 0
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
#define CC1101_GDO0  35
#define CC1101_CS    4

#define RADIO_DEBUG 0

// =================================================================
// 2. GLOBAL VARIABLES & STATES
// =================================================================
typedef struct ADPCMState ADPCMState;
String devicePassword = "";
String deviceName = "Walkie-Talkie";
char message[64] = "Welcome!";
String radioPassword = "";
String radioSaltValue = "";
bool ready = false;
bool sent = false;
volatile bool isProgramMode = false;
volatile bool autoProgramMode = false;
volatile bool usbSelection = true; // true for Yes, false for No
volatile bool usbPromptActive = false;
volatile bool usbRejected = false;
bool hasDisplay = true;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Preferences preferences;
uint8_t savedChannel = 1;
const char *BUILD_ID = __DATE__ " " __TIME__;

enum DisplayState
{
    STATE_IDLE,
    STATE_TRANSMIT,
    STATE_RECEIVE,
    STATE_VOLUME,
    STATE_STARTUP,
    STATE_SCREEN_OFF,
    STATE_CHANNEL,
    STATE_PROGRAM_PROMPT,
    STATE_PROGRAM_MODE,
    STATE_LOGGED_IN,
    STATE_USB_DISCONNECTED
};
volatile DisplayState displayState = STATE_STARTUP;

const char welcomeText[] = "Welcome to TEE!   ";
int16_t scrollX = SCREEN_WIDTH;
const uint8_t scrollSpeed = 5;
uint8_t dotCount = 0;

// Timers
unsigned long lastVolumeChange = 0;
unsigned long lastReceiveTime = 0;
unsigned long lastAnimUpdate = 0;
unsigned long startupTime = 0;
unsigned long lastChannelActivity = 0;
unsigned long disconnectTime = 0;
unsigned long loginSuccessTime = 0;
const unsigned long VOLUME_SHOW_DURATION = 2000;
const unsigned long SCREEN_OFF_DELAY = 1000;
const unsigned long STARTUP_SHOW_DURATION = 1500;
const unsigned long RECEIVE_TIMEOUT = 500;
const unsigned long ANIM_INTERVAL = 300;
const unsigned long CHANNEL_SHOW_DURATION = 1500;
const unsigned long BUTTON_HOLD_DURATION = 700;
const unsigned long RAPID_CHANNEL_SWITCH_TIME = 100;

// Radio & Buffers
uint8_t currentChannel = 1;
volatile bool channelUpdatePending = false;
const uint8_t MAX_CHANNEL_VALUE = 50;
const uint8_t MIN_CHANNEL_VALUE = 1;
const float CHANNEL_SPACING = 1.0; // MHz
#define BASE_FREQUENCY 433 // MHz
volatile bool currentChannelEncrypted = false;

#define SAMPLE_RATE 16000
#define PACKET_SIZE 32
#define UNENC_HEADER_SIZE 4
#define UNENC_ADPCM_DATA_BYTES (PACKET_SIZE - UNENC_HEADER_SIZE)
#define SAMPLES_PER_FRAME (UNENC_ADPCM_DATA_BYTES * 2) // 56 samples per packet

// --- AES-GCM encrypted channels ---
// Encrypted packet layout (32 bytes total):
//   [0]     : Packet type (ENC_PACKET_TYPE)
//   [1-4]   : Packet counter (u32, plaintext) - forms per-packet GCM nonce suffix
//   [5-27]  : AES-GCM ciphertext (23 bytes)
//   [28-31] : Truncated GCM auth tag (4 bytes)
const uint8_t MAX_ENCRYPTED_CHANNELS = MAX_CHANNEL_VALUE - MIN_CHANNEL_VALUE + 1;
uint8_t encryptedChannels[MAX_ENCRYPTED_CHANNELS] = {};
uint8_t encryptedChannelCount = 0;
bool encryptedChannelLookup[MAX_CHANNEL_VALUE + 1] = {};
#define ENC_PACKET_TYPE 0xA5
#define UNENC_PACKET_TYPE 0x5A
#define NONCE_PACKET_TYPE 0xC3
#define ENC_HEADER_SIZE 3
#define ENC_PLAINTEXT_SIZE 23    // 3-byte ADPCM header + 20-byte ADPCM data
#define ENC_ADPCM_DATA_BYTES 20  // 20 bytes = 40 ADPCM samples
#define ENC_SAMPLES_PER_FRAME 40 // samples decoded per encrypted packet
#define ENC_TAG_SIZE 4           // truncated GCM tag
#define ENC_COUNTER_SIZE 4       // packet counter field

static inline bool isEncryptedChannel(uint8_t channel)
{
    return channel >= MIN_CHANNEL_VALUE && channel <= MAX_CHANNEL_VALUE && encryptedChannelLookup[channel];
}

void clearEncryptedChannelConfig()
{
    encryptedChannelCount = 0;
    memset(encryptedChannelLookup, 0, sizeof(encryptedChannelLookup));
}

void addEncryptedChannel(uint8_t channel)
{
    if (channel < MIN_CHANNEL_VALUE || channel > MAX_CHANNEL_VALUE || encryptedChannelLookup[channel])
    {
        return;
    }

    encryptedChannels[encryptedChannelCount++] = channel;
    encryptedChannelLookup[channel] = true;
}

void refreshCurrentChannelEncryption()
{
    currentChannelEncrypted = isEncryptedChannel(currentChannel);
}

void loadEncryptedChannels()
{
    clearEncryptedChannelConfig();
    size_t byteCount = preferences.getBytesLength("enc_channels");
    if (byteCount == 0)
    {
        refreshCurrentChannelEncryption();
        return;
    }

    if (byteCount > MAX_ENCRYPTED_CHANNELS)
    {
        byteCount = MAX_ENCRYPTED_CHANNELS;
    }

    uint8_t loadedChannels[MAX_ENCRYPTED_CHANNELS] = {};
    size_t loadedCount = preferences.getBytes("enc_channels", loadedChannels, byteCount);
    for (size_t i = 0; i < loadedCount; i++)
    {
        addEncryptedChannel(loadedChannels[i]);
    }
    refreshCurrentChannelEncryption();
}

void storeEncryptedChannels(String channelList)
{
    clearEncryptedChannelConfig();
    channelList.trim();

    while (channelList.length() > 0 && encryptedChannelCount < MAX_ENCRYPTED_CHANNELS)
    {
        int separatorIndex = channelList.indexOf(',');
        String token = separatorIndex == -1 ? channelList : channelList.substring(0, separatorIndex);
        token.trim();

        if (token.length() > 0)
        {
            int parsedChannel = token.toInt();
            if (parsedChannel >= MIN_CHANNEL_VALUE && parsedChannel <= MAX_CHANNEL_VALUE)
            {
                addEncryptedChannel((uint8_t)parsedChannel);
            }
        }

        if (separatorIndex == -1)
        {
            break;
        }
        channelList = channelList.substring(separatorIndex + 1);
    }

    if (encryptedChannelCount == 0)
    {
        preferences.remove("enc_channels");
    }
    else
    {
        preferences.putBytes("enc_channels", encryptedChannels, encryptedChannelCount);
    }
    refreshCurrentChannelEncryption();
}

void changeChannel(int8_t direction)
{
    if (direction < 0)
    {
        currentChannel = currentChannel > MIN_CHANNEL_VALUE ? currentChannel - 1 : MAX_CHANNEL_VALUE;
    }
    else if (direction > 0)
    {
        currentChannel = currentChannel < MAX_CHANNEL_VALUE ? currentChannel + 1 : MIN_CHANNEL_VALUE;
    }

    refreshCurrentChannelEncryption();
    channelUpdatePending = true;
    lastChannelActivity = millis();
}

void showChannelSelector()
{
    if (displayState != STATE_CHANNEL && displayState != STATE_SCREEN_OFF && displayState != STATE_STARTUP)
    {
        displayState = STATE_CHANNEL;
        lastChannelActivity = millis();
    }
}

void updateDeviceMessage()
{
    snprintf(message, sizeof(message), "Welcome %s!", deviceName.c_str());
}

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

#define TX_RING_SIZE 6
uint8_t txBuffer[TX_RING_SIZE][PACKET_SIZE];
volatile bool bufferReady[TX_RING_SIZE] = {};
volatile uint8_t fillBuffer = 0;
volatile uint8_t txBufferIndex = 0;
volatile int txFails = 0;
volatile bool isTransmitting = false;

// Receive-side jitter ring buffer
#define RX_RING_SIZE 32
#define RX_PREFILL 2 // How many packets to fill before sending to amp
uint8_t rxRing[RX_RING_SIZE][PACKET_SIZE];
volatile uint8_t rxHead = 0;
volatile uint8_t rxTail = 0;
volatile uint8_t rxCount = 0;
volatile bool rxPlaying = false;
portMUX_TYPE rxCountMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool cc1101PacketInterrupt = false;
TaskHandle_t radioTaskHandle = NULL;
volatile uint32_t playbackDecodedFrames = 0;
volatile uint32_t playbackSilentFrames = 0;
volatile uint32_t playbackUnderrunFrames = 0;
volatile uint32_t playbackShortWrites = 0;

// ADPCM encoder state (persistent across frames during TX)
struct ADPCMState
{
    int16_t predicted;
    uint8_t index;
};
ADPCMState txAdpcmState = {0, 0};

// Audio Gain Controls
int lastPotValue = -1;
const int VOLUME_HYSTERESIS = 400;

// Display Float vs Fast Integer Math
volatile float GAIN_FOR_UI = 1.0f; // Kept for drawing the OLED volume bar
volatile int32_t GAIN = 256;       // OPTIMIZED: 1.0 = 256 for fast audio math
const float GAIN_MIN = 0.1f;
const float GAIN_MAX = 3.5f;
const int POT_AUTO_PROGRAM_THRESHOLD = 5;
const int POT_OFF_THRESHOLD = 30;
const int POT_ON_THRESHOLD = 150;

// Limiter thresholds
const int32_t LIMIT_PRE = 10000;
const int32_t LIMIT_POST = 28000;

// Raise this value if you still get feedback; lower it if soft speech is cut off.
const int32_t NOISE_GATE_THRESHOLD = 300; // range: 0 (disabled) to ~32767 (full-scale); practical: 50-500
const int32_t FIXED_MIC_GAIN = 179;       // 0.7x gain (256 = 1.0x)

String b64encode(const unsigned char *input, size_t len)
{
    size_t out_len = 0;
    unsigned char out[256];

    mbedtls_base64_encode(out, sizeof(out), &out_len, input, len);
    return String((char *)out).substring(0, out_len);
}

bool decodeB64ToString(const String &encoded, String &decoded, size_t maxDecodedLength = 128)
{
    if (encoded.length() == 0)
    {
        decoded = "";
        return true;
    }

    unsigned char buffer[128];
    size_t decodedLength = 0;
    if (maxDecodedLength > sizeof(buffer) - 1)
    {
        maxDecodedLength = sizeof(buffer) - 1;
    }

    int decodeResult = mbedtls_base64_decode(
        buffer,
        maxDecodedLength,
        &decodedLength,
        (const unsigned char *)encoded.c_str(),
        encoded.length());

    if (decodeResult != 0)
    {
        return false;
    }

    buffer[decodedLength] = '\0';
    decoded = String((char *)buffer);
    return true;
}

// =================================================================
// RADIO AES-GCM ENCRYPTION
// =================================================================
uint8_t radioKey[32];                  // AES-256 key (derived from Preferences radio password)
uint8_t radioBaseNonce[8];             // 8-byte base; combined with counter to make 12-byte GCM nonce
volatile uint32_t txPacketCounter = 0; // monotonically increasing TX sequence number
uint8_t radioSessionNonce[8];          // per-PTT session nonce base
volatile bool sessionNonceValid = false;
uint8_t txNoncePacket[PACKET_SIZE];
volatile bool txNoncePending = false;

// Derive radioKey and radioBaseNonce deterministically from the stored radio password.
// This keeps radio encryption consistent across devices with different login PASSWORD values.
void initRadioEncryption()
{
#if RADIO_DEBUG
    Serial.println("[Radio] Deriving channel encryption key from passwords...");
#endif
    const char *storedRadioPassword = radioPassword.c_str();

    // 1. Derive Radio Key: Key=storedRadioPassword, Salt=custom Preferences salt or SHA256(storedRadioPassword + suffix)
    uint8_t autoRadioKeySalt[32];
    const unsigned char *radioKeySalt = nullptr;
    size_t radioKeySaltLength = 0;
    if (radioSaltValue.length() > 0)
    {
        radioKeySalt = (const unsigned char *)radioSaltValue.c_str();
        radioKeySaltLength = radioSaltValue.length();
    }
    else
    {
        char radioSaltInput[64];
        snprintf(radioSaltInput, sizeof(radioSaltInput), "%s|radio_salt", storedRadioPassword);
        mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                   (const unsigned char *)radioSaltInput, strlen(radioSaltInput),
                   autoRadioKeySalt);
        radioKeySalt = autoRadioKeySalt;
        radioKeySaltLength = sizeof(autoRadioKeySalt);
    }
    mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256,
        (const unsigned char *)storedRadioPassword, strlen(storedRadioPassword),
        radioKeySalt, radioKeySaltLength,
        5000, 32, radioKey);

    // 2. Derive Nonce Base: Key=storedRadioPassword, Salt=SHA256(storedRadioPassword + suffix)
    uint8_t nonceSalt[32];
    char nonceSaltInput[64];
    snprintf(nonceSaltInput, sizeof(nonceSaltInput), "%s|radio_salt_nonce", storedRadioPassword);
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
               (const unsigned char *)nonceSaltInput, strlen(nonceSaltInput),
               nonceSalt);

    uint8_t nonceBuf[32];
    mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256,
        (const unsigned char *)storedRadioPassword, strlen(storedRadioPassword),
        nonceSalt, sizeof(nonceSalt),
        1000, 32, nonceBuf);
    memcpy(radioBaseNonce, nonceBuf, 8);

#if RADIO_DEBUG
    Serial.println("[Radio] Channel encryption ready (Password-derived).");
#endif
}

// Encrypt ENC_PLAINTEXT_SIZE bytes into a 32-byte radio packet.
// Packet: [type(1)] [counter(4)] [ciphertext(23)] [tag_truncated(4)]
bool encryptRadioPacket(const uint8_t *plaintext, uint8_t *outPkt, uint32_t counter)
{
    if (!sessionNonceValid)
        return false;
    // 12-byte GCM nonce = radioBaseNonce[8] + counter[4]
    uint8_t nonce[12];
    memcpy(nonce, radioSessionNonce, 8);
    nonce[8] = (counter >> 24) & 0xFF;
    nonce[9] = (counter >> 16) & 0xFF;
    nonce[10] = (counter >> 8) & 0xFF;
    nonce[11] = counter & 0xFF;

    outPkt[0] = ENC_PACKET_TYPE;
    // Write counter into packet bytes [1-4]
    outPkt[1] = nonce[8];
    outPkt[2] = nonce[9];
    outPkt[3] = nonce[10];
    outPkt[4] = nonce[11];

    uint8_t fullTag[16];
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, radioKey, 256) != 0)
    {
        mbedtls_gcm_free(&gcm);
        return false;
    }
    int ret = mbedtls_gcm_crypt_and_tag(
        &gcm, MBEDTLS_GCM_ENCRYPT, ENC_PLAINTEXT_SIZE,
        nonce, 12, NULL, 0,
        plaintext,
        outPkt + 1 + ENC_COUNTER_SIZE, // ciphertext to bytes [5-27]
        16, fullTag);
    mbedtls_gcm_free(&gcm);
    if (ret != 0)
        return false;
    // Copy first 4 bytes of tag to bytes [28-31]
    memcpy(outPkt + 1 + ENC_COUNTER_SIZE + ENC_PLAINTEXT_SIZE, fullTag, ENC_TAG_SIZE);
    return true;
}

// Decrypt a 32-byte radio packet. Returns false if auth tag mismatch.
bool decryptRadioPacket(const uint8_t *inPkt, uint8_t *outPlaintext)
{
    if (inPkt[0] != ENC_PACKET_TYPE)
        return false;
    if (!sessionNonceValid)
        return false;
    // Reconstruct 12-byte nonce from packet bytes [1-4]
    uint8_t nonce[12];
    memcpy(nonce, radioSessionNonce, 8);
    nonce[8] = inPkt[1];
    nonce[9] = inPkt[2];
    nonce[10] = inPkt[3];
    nonce[11] = inPkt[4];

    uint8_t fullTag[16];
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, radioKey, 256) != 0)
    {
        mbedtls_gcm_free(&gcm);
        return false;
    }
    int ret = mbedtls_gcm_crypt_and_tag(
        &gcm, MBEDTLS_GCM_DECRYPT, ENC_PLAINTEXT_SIZE,
        nonce, 12, NULL, 0,
        inPkt + 1 + ENC_COUNTER_SIZE, // ciphertext at bytes [5-27]
        outPlaintext,
        16, fullTag);
    mbedtls_gcm_free(&gcm);
    if (ret != 0)
        return false;
    // Verify truncated 4-byte tag
    return (memcmp(fullTag, inPkt + 1 + ENC_COUNTER_SIZE + ENC_PLAINTEXT_SIZE, ENC_TAG_SIZE) == 0);
}

// =================================================================
// 3. AUDIO PROCESSING MATH
// =================================================================

// Fast soft limiter
inline int32_t softLimiter(int32_t s, int32_t thresh)
{
    if (s > thresh)
        s = thresh + (s - thresh) / 4;
    if (s < -thresh)
        s = -thresh + (s + thresh) / 4;
    return s;
}

// High-pass filter
int16_t hpFilter(int16_t input)
{
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
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static const int8_t adpcmIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8};

// ADPCM encoder: encodes numSamples PCM into packed 4-bit nibbles
void adpcmEncode(int16_t *input, uint8_t *output, int numSamples, ADPCMState *state)
{
    for (int i = 0; i < numSamples; i++)
    {
        int diff = input[i] - state->predicted;
        uint8_t nibble = 0;
        if (diff < 0)
        {
            nibble = 8;
            diff = -diff;
        }

        int16_t step = adpcmStepTable[state->index];
        int32_t diffq = step >> 3;
        if (diff >= step)
        {
            nibble |= 4;
            diff -= step;
            diffq += step;
        }
        step >>= 1;
        if (diff >= step)
        {
            nibble |= 2;
            diff -= step;
            diffq += step;
        }
        step >>= 1;
        if (diff >= step)
        {
            nibble |= 1;
            diffq += step;
        }

        state->predicted += (nibble & 8) ? -diffq : diffq;
        if (state->predicted > 32767)
            state->predicted = 32767;
        if (state->predicted < -32768)
            state->predicted = -32768;

        int idx = state->index + adpcmIndexTable[nibble];
        if (idx < 0)
            idx = 0;
        else if (idx > 88)
            idx = 88;
        state->index = (uint8_t)idx;

        if (i & 1)
            output[i >> 1] |= nibble; // Low nibble (odd sample)
        else
            output[i >> 1] = nibble << 4; // High nibble (even sample)
    }
}

// ADPCM decoder: decodes packed 4-bit nibbles into PCM samples
void adpcmDecode(uint8_t *input, int16_t *output, int numSamples, ADPCMState *state)
{
    for (int i = 0; i < numSamples; i++)
    {
        uint8_t nibble = (i & 1) ? (input[i >> 1] & 0x0F)
                                 : ((input[i >> 1] >> 4) & 0x0F);

        int16_t step = adpcmStepTable[state->index];
        int32_t diffq = step >> 3;
        if (nibble & 4)
            diffq += step;
        if (nibble & 2)
            diffq += step >> 1;
        if (nibble & 1)
            diffq += step >> 2;

        state->predicted += (nibble & 8) ? -diffq : diffq;
        if (state->predicted > 32767)
            state->predicted = 32767;
        if (state->predicted < -32768)
            state->predicted = -32768;

        int idx = state->index + adpcmIndexTable[nibble];
        if (idx < 0)
            idx = 0;
        else if (idx > 88)
            idx = 88;
        state->index = (uint8_t)idx;

        output[i] = state->predicted;
    }
}

// =================================================================
// 4. I2S INITIALIZATION (Single port, pins swapped on PTT)
// =================================================================
void i2sinit()
{
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = 16,
        .dma_buf_len = SAMPLES_PER_FRAME,
        .use_apll = false};
    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
}

void switchToMicPins()
{
    i2s_pin_config_t mic_pins = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = -1,
        .data_in_num = I2S_SD};
    i2s_set_pin(I2S_NUM_0, &mic_pins);
}

void switchToSpeakerPins()
{
    i2s_pin_config_t amp_pins = {
        .bck_io_num = MAX98357A_BCLK,
        .ws_io_num = MAX98357A_LRCLK,
        .data_out_num = MAX98357A_DIN,
        .data_in_num = -1};
    i2s_set_pin(I2S_NUM_0, &amp_pins);
}

inline void setActivityLed(bool on)
{
#if ENABLE_ACTIVITY_LED
    digitalWrite(LED_PIN, on ? HIGH : LOW);
#else
    (void)on;
#endif
}

// =================================================================
// 5. FREERTOS TASKS
// =================================================================

void IRAM_ATTR cc1101PacketISR()
{
    cc1101PacketInterrupt = true;
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    if (radioTaskHandle != NULL)
    {
        vTaskNotifyGiveFromISR(radioTaskHandle, &higherPriorityTaskWoken);
    }
    if (higherPriorityTaskWoken)
    {
        portYIELD_FROM_ISR();
    }
}

// Helpers for CC1101 fixed-length packets.
bool cc1101_available()
{
    uint8_t marc = radio.SPIgetRegValue(0x35, 4, 0);
    if (marc == 0x11)
    {
        radio.standby();
        radio.SPIsendCommand(0x3A); // SFRX
        radio.startReceive();
        return false;
    }

    uint8_t rxBytes = radio.SPIgetRegValue(0x3B, 6, 0);
    if (rxBytes >= PACKET_SIZE)
    {
        uint8_t rxBytes2 = radio.SPIgetRegValue(0x3B, 6, 0);
        return rxBytes2 >= PACKET_SIZE;
    }
    return false;
}

bool cc1101_read(uint8_t *buf, uint8_t len)
{
    int state = radio.readData(buf, len);
    if (state != RADIOLIB_ERR_NONE)
    {
        radio.standby();
        radio.SPIsendCommand(0x3A); // SFRX
        radio.startReceive();
        return false;
    }
    return true;
}

bool cc1101_write(const uint8_t *buf, uint8_t len)
{
    // Clear any stale notifications so we only wait for the *new* TX interrupt
    cc1101PacketInterrupt = false;
    ulTaskNotifyTake(pdTRUE, 0);

    int state = radio.startTransmit((uint8_t *)buf, len);
    if (state == RADIOLIB_ERR_NONE)
    {
        // Block this task until the GDO0 pin triggers the ISR (end of transmission).
        // This yields the CPU entirely to other tasks, exactly like nRF's radio.write!
        // At 250kbps, a 32-byte packet takes ~1.5ms. We allow up to 10ms.
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)) == pdFALSE && !cc1101PacketInterrupt)
        {
            // Fallback: poll the GPIO pin directly (much faster than SPI), then check MARCSTATE
            unsigned long startTx = millis();
            while (digitalRead(CC1101_GDO0) || radio.SPIgetRegValue(0x35, 4, 0) != 0x01)
            {
                taskYIELD();
                if (millis() - startTx > 10)
                {
                    state = RADIOLIB_ERR_TX_TIMEOUT;
                    break;
                }
            }
        }
    }

    cc1101PacketInterrupt = false;
    radio.standby();
    radio.SPIsendCommand(0x3B); // SFTX
    return state == RADIOLIB_ERR_NONE;
}

// Manual frequency calibration. With FS_AUTOCAL disabled (see cc1101_init) the radio no
// longer recalibrates on every IDLE->TX/RX transition. That ~720us-per-packet calibration
// was the main reason the CC1101 could not keep up with the ~300-400 packet/sec audio rate.
// Instead we calibrate once, here, whenever we (re)enter RX or start a transmission.
void cc1101_calibrate()
{
    radio.standby();            // SIDLE -> IDLE
    radio.SPIsendCommand(0x33); // SCAL: calibrate, auto-returns to IDLE when finished
    unsigned long start = millis();
    while (radio.SPIgetRegValue(0x35, 4, 0) != 0x01) // wait for MARCSTATE = IDLE
    {
        if (millis() - start > 5)
            break;
    }
}

void cc1101_start_rx()
{
    cc1101PacketInterrupt = false;
    radio.standby();
    radio.SPIsendCommand(0x3A); // SFRX
    radio.setFrequency(BASE_FREQUENCY + currentChannel * CHANNEL_SPACING);
    cc1101_calibrate(); // calibrate for this frequency (FS_AUTOCAL is disabled)
    radio.startReceive();
}


void audioCaptureTask(void *param)
{
    size_t bytesRead;
    int32_t rawBuffer[SAMPLES_PER_FRAME];
    int16_t pcmBuffer[SAMPLES_PER_FRAME];

    while (1)
    {
        if (!isTransmitting || isProgramMode)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        while (bufferReady[fillBuffer])
        {
            vTaskDelay(1);
        }

        bool useEncryption = currentChannelEncrypted;
        if (useEncryption && txNoncePending)
        {
            vTaskDelay(1);
            continue;
        }

        int frameSamples = useEncryption ? ENC_SAMPLES_PER_FRAME : SAMPLES_PER_FRAME;

        i2s_read(I2S_NUM_0, rawBuffer, frameSamples * sizeof(int32_t), &bytesRead, pdMS_TO_TICKS(20));
        if (!isTransmitting || isProgramMode)
        {
            continue;
        }
        if (bytesRead < frameSamples * sizeof(int32_t))
        {
            vTaskDelay(1);
            continue;
        }

        // --- DC Removal & Noise gate ---
        int32_t peak = 0;
        for (int i = 0; i < frameSamples; i++)
        {
            int32_t sample = rawBuffer[i] >> 14;
            sample = hpFilter((int16_t)sample);
            int32_t a = (sample < 0) ? -sample : sample;
            if (a > peak)
                peak = a;
            pcmBuffer[i] = (int16_t)sample;
        }

        if (useEncryption)
        {
            // --- ENCRYPTED PATH (Channel 90) ---
            // Plaintext: [pred_hi][pred_lo][index][adpcm_data x20] = 23 bytes
            uint8_t plaintext[ENC_PLAINTEXT_SIZE];
            if (peak < NOISE_GATE_THRESHOLD)
            {
                memset(plaintext, 0, ENC_PLAINTEXT_SIZE);
                txAdpcmState.predicted = 0;
                txAdpcmState.index = 0;
            }
            else
            {
                for (int i = 0; i < ENC_SAMPLES_PER_FRAME; i++)
                {
                    int32_t sample = pcmBuffer[i];
                    sample = softLimiter(sample, LIMIT_PRE);
                    sample = (sample * FIXED_MIC_GAIN) >> 8;
                    sample = softLimiter(sample, LIMIT_PRE);
                    pcmBuffer[i] = (int16_t)sample;
                }
                plaintext[0] = (uint8_t)(txAdpcmState.predicted >> 8);
                plaintext[1] = (uint8_t)(txAdpcmState.predicted & 0xFF);
                plaintext[2] = txAdpcmState.index;
                // Encode 40 samples to 20 bytes ADPCM
                adpcmEncode(pcmBuffer, &plaintext[ENC_HEADER_SIZE],
                            ENC_SAMPLES_PER_FRAME, &txAdpcmState);
            }
            uint32_t counter = txPacketCounter++;
            encryptRadioPacket(plaintext, txBuffer[fillBuffer], counter);
        }
        else
        {
            // --- UNENCRYPTED PATH (all other channels) ---
            if (peak < NOISE_GATE_THRESHOLD)
            {
                memset(txBuffer[fillBuffer], 0, PACKET_SIZE);
                txBuffer[fillBuffer][0] = UNENC_PACKET_TYPE;
                txBuffer[fillBuffer][1] = 0;
                txBuffer[fillBuffer][2] = 0;
                txBuffer[fillBuffer][3] = 0;
                txAdpcmState.predicted = 0;
                txAdpcmState.index = 0;
            }
            else
            {
                for (int i = 0; i < frameSamples; i++)
                {
                    int32_t sample = pcmBuffer[i];
                    sample = softLimiter(sample, LIMIT_PRE);
                    sample = (sample * FIXED_MIC_GAIN) >> 8;
                    sample = softLimiter(sample, LIMIT_PRE);
                    pcmBuffer[i] = (int16_t)sample;
                }
                txBuffer[fillBuffer][0] = UNENC_PACKET_TYPE;
                txBuffer[fillBuffer][1] = (uint8_t)(txAdpcmState.predicted >> 8);
                txBuffer[fillBuffer][2] = (uint8_t)(txAdpcmState.predicted & 0xFF);
                txBuffer[fillBuffer][3] = txAdpcmState.index;
                // Encode current frame to ADPCM data
                adpcmEncode(pcmBuffer, &txBuffer[fillBuffer][UNENC_HEADER_SIZE],
                            frameSamples, &txAdpcmState);
            }
        }

        bufferReady[fillBuffer] = true;
        fillBuffer = (fillBuffer + 1) % TX_RING_SIZE;
        vTaskDelay(1);
    }
}

// Radio-only task: handles TX and RX without ever blocking on I2S
void radioTask(void *param)
{
    radioTaskHandle = xTaskGetCurrentTaskHandle();
    static unsigned long lastTxPrint = 0;
    static unsigned long lastRxPrint = 0;
    static int txSuccessCount = 0;
    static int txFailCount = 0;
    static int rxPacketCount = 0;
    static int rxUnencCount = 0;
    static int rxEncCount = 0;
    static int rxUnknownCount = 0;

    while (1)
    {
        if (channelUpdatePending)
        {
            cc1101_start_rx();
            channelUpdatePending = false;
        }

        if (isTransmitting && !isProgramMode)
        {
            if (txNoncePending)
            {
                if (cc1101_write(txNoncePacket, PACKET_SIZE))
                {
                    txNoncePending = false;
                    txSuccessCount++;
                }
                else
                {
                    txFailCount++;
                }
                taskYIELD();
                continue;
            }

            if (bufferReady[txBufferIndex])
            {
                if (cc1101_write(txBuffer[txBufferIndex], PACKET_SIZE))
                {
                    bufferReady[txBufferIndex] = false;
                    txFails = 0;
                    txSuccessCount++;
                    txBufferIndex = (txBufferIndex + 1) % TX_RING_SIZE;
                }
                else
                {
                    txFails++;
                    txFailCount++;
                    if (txFails > 10)
                    {
                        radio.standby();
                        radio.SPIsendCommand(0x3B); // SFTX
                        txFails = 0;
                    }
                }

                if (millis() - lastTxPrint > 2000)
                {
                    lastTxPrint = millis();
#if RADIO_DEBUG
                    Serial.printf("TX: %d ok, %d fail | CH=%d F=%.2f MHz encrypted=%d\n",
                                  txSuccessCount, txFailCount, currentChannel,
                                  (float)(BASE_FREQUENCY + currentChannel * CHANNEL_SPACING),
                                  currentChannelEncrypted);
#endif
                    txSuccessCount = 0;
                    txFailCount = 0;
                }
            }
            taskYIELD();
        }
        else if (!isProgramMode)
        {
            if (!cc1101PacketInterrupt)
            {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
            }

            if (cc1101PacketInterrupt || cc1101_available())
            {
                cc1101PacketInterrupt = false;
                while (cc1101_available())
                {
                    uint8_t pkt[PACKET_SIZE];
                    if (!cc1101_read(pkt, PACKET_SIZE))
                    {
                        break;
                    }
                    bool enqueuePacket = false;

                    if (pkt[0] == NONCE_PACKET_TYPE)
                    {
                        if (currentChannelEncrypted)
                        {
                            memcpy(radioSessionNonce, &pkt[1], 8);
                            sessionNonceValid = true;
                            lastReceiveTime = millis();
                        }
                        continue;
                    }

                    if (pkt[0] == ENC_PACKET_TYPE && !currentChannelEncrypted)
                    {
                        continue;
                    }

                    if (pkt[0] == UNENC_PACKET_TYPE)
                    {
                        rxUnencCount++;
                        enqueuePacket = true;
                    }
                    else if (pkt[0] == ENC_PACKET_TYPE)
                    {
                        rxEncCount++;
                        enqueuePacket = currentChannelEncrypted;
                    }
                    else
                    {
                        rxUnknownCount++;
                        // A bad first byte means the FIFO is not aligned to packet starts.
                        // Flush once so the next sync word starts a clean 32-byte packet.
                        radio.standby();
                        radio.SPIsendCommand(0x3A); // SFRX
                        radio.startReceive();
                        break;
                    }

                    if (!enqueuePacket)
                    {
                        continue;
                    }

                    if (rxCount >= RX_RING_SIZE)
                    {
                        rxTail = (rxTail + 1) % RX_RING_SIZE;
                        portENTER_CRITICAL(&rxCountMux);
                        rxCount--;
                        portEXIT_CRITICAL(&rxCountMux);
                    }

                    memcpy(rxRing[rxHead], pkt, PACKET_SIZE);
                    rxHead = (rxHead + 1) % RX_RING_SIZE;
                    portENTER_CRITICAL(&rxCountMux);
                    rxCount++;
                    portEXIT_CRITICAL(&rxCountMux);
                    lastReceiveTime = millis();
                    rxPacketCount++;

                    if (displayState == STATE_IDLE)
                    {
                        displayState = STATE_RECEIVE;
                        dotCount = 0;
                        lastAnimUpdate = millis();
                    }
                }
            }

            if (!isTransmitting && rxCount == 0 && (millis() - lastReceiveTime > RECEIVE_TIMEOUT))
            {
                sessionNonceValid = false;
            }

            if (millis() - lastRxPrint > 2000)
            {
                lastRxPrint = millis();
                uint8_t marc = radio.SPIgetRegValue(0x35, 4, 0);
#if RADIO_DEBUG
                uint8_t rxb = radio.SPIgetRegValue(0x3B, 6, 0);
                uint32_t decodedFrames = playbackDecodedFrames;
                uint32_t silentFrames = playbackSilentFrames;
                uint32_t underrunFrames = playbackUnderrunFrames;
                uint32_t shortWrites = playbackShortWrites;
                playbackDecodedFrames = 0;
                playbackSilentFrames = 0;
                playbackUnderrunFrames = 0;
                playbackShortWrites = 0;

                Serial.printf("RX: %d pkts u=%d e=%d ?=%d | play=%lu silent=%lu under=%lu short=%lu | MARC=0x%02X RXBYTES=%d ring=%d/%d CH=%d F=%.2f encrypted=%d\n",
                              rxPacketCount, rxUnencCount, rxEncCount, rxUnknownCount,
                              decodedFrames, silentFrames, underrunFrames, shortWrites,
                              marc, rxb, rxCount, RX_RING_SIZE, currentChannel,
                              (float)(BASE_FREQUENCY + currentChannel * CHANNEL_SPACING),
                              currentChannelEncrypted);
#endif
                rxPacketCount = 0;
                rxUnencCount = 0;
                rxEncCount = 0;
                rxUnknownCount = 0;

                // Cheap health check kept in the hot path: recover if the radio fell into
                // RX FIFO overflow (0x11) or a stuck state (0x16).
                if (marc == 0x11 || marc == 0x16)
                {
#if RADIO_DEBUG
                    Serial.printf("RX: error state 0x%02X, recovering\n", marc);
#endif
                    cc1101_start_rx();
                }
            }

            vTaskDelay(1);
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

// Playback task: decodes ring buffer and writes to I2S
void playbackTask(void *param)
{
    size_t bytes_written;
    int16_t stereoBuf[SAMPLES_PER_FRAME * 2];
    uint8_t underrunCount = 0;
    uint8_t lastPacketType = 0;

    while (1)
    {
        if (isTransmitting || isProgramMode)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int samplesOut = SAMPLES_PER_FRAME;
        uint8_t packetType = 0;

        // Play from ring buffer once prefill threshold is reached
        if ((rxPlaying && rxCount > 0) || rxCount >= RX_PREFILL)
        {
            rxPlaying = true;
            uint8_t *pkt = rxRing[rxTail];
            int32_t currentGain = GAIN;
            int16_t pcmOut[SAMPLES_PER_FRAME];
            bool decoded = false;
            bool isEncPkt = (pkt[0] == ENC_PACKET_TYPE);
            bool isUnencPkt = (pkt[0] == UNENC_PACKET_TYPE);
            bool channelEncrypted = currentChannelEncrypted;
            packetType = isEncPkt ? ENC_PACKET_TYPE : (isUnencPkt ? UNENC_PACKET_TYPE : 0);

            if (isEncPkt && channelEncrypted)
            {
                // --- DECRYPTION PATH (encrypted channel only) ---
                uint8_t plaintext[ENC_PLAINTEXT_SIZE];
                bool ok = decryptRadioPacket(pkt, plaintext);
                if (ok)
                {
                    ADPCMState rxState;
                    rxState.predicted = (int16_t)((plaintext[0] << 8) | plaintext[1]);
                    rxState.index = plaintext[2];
                    if (rxState.index > 88)
                        rxState.index = 88;
                    // Decode encrypted frame samples
                    adpcmDecode(&plaintext[ENC_HEADER_SIZE], pcmOut, ENC_SAMPLES_PER_FRAME, &rxState);
                    samplesOut = ENC_SAMPLES_PER_FRAME;
                    decoded = true;
                }
            }
            else if (isEncPkt && !channelEncrypted)
            {
                // Encrypted packet on an unencrypted channel -> decode as raw ADPCM (noise)
                ADPCMState rxState;
                rxState.predicted = (int16_t)((pkt[1] << 8) | pkt[2]);
                rxState.index = pkt[3];
                if (rxState.index > 88)
                    rxState.index = 88;
                adpcmDecode(&pkt[UNENC_HEADER_SIZE], pcmOut, SAMPLES_PER_FRAME, &rxState);
                samplesOut = SAMPLES_PER_FRAME;
                decoded = true;
            }
            else if (isUnencPkt)
            {
                // --- UNENCRYPTED PATH (all channels) ---
                ADPCMState rxState;
                rxState.predicted = (int16_t)((pkt[1] << 8) | pkt[2]);
                rxState.index = pkt[3];
                if (rxState.index > 88)
                    rxState.index = 88;
                adpcmDecode(&pkt[UNENC_HEADER_SIZE], pcmOut, SAMPLES_PER_FRAME, &rxState);
                samplesOut = SAMPLES_PER_FRAME;
                decoded = true;
            }

            if (!decoded)
            {
                // Unknown packet type or decrypt failure — output silence
                memset(pcmOut, 0, SAMPLES_PER_FRAME * sizeof(int16_t));
            }

            if (packetType != lastPacketType)
            {
                // Reset DMA when switching packet types to avoid I2S lockups
                i2s_zero_dma_buffer(I2S_NUM_0);
                lastPacketType = packetType;
            }

            for (int i = 0; i < samplesOut; i++)
            {
                int32_t sample = pcmOut[i];
                sample = (sample * currentGain) >> 8;
                sample = softLimiter(sample, LIMIT_POST);
                int16_t out = (int16_t)sample;
                stereoBuf[i * 2] = out;
                stereoBuf[i * 2 + 1] = out;
            }
            rxTail = (rxTail + 1) % RX_RING_SIZE;
            portENTER_CRITICAL(&rxCountMux);
            rxCount--;
            portEXIT_CRITICAL(&rxCountMux);
            underrunCount = 0;
            digitalWrite(LED_PIN, HIGH);
        }
        else
        {
            // Underrun — output silence but tolerate brief gaps
            memset(stereoBuf, 0, sizeof(stereoBuf));
            if (rxPlaying)
            {
                underrunCount++;
                if (underrunCount >= 8)
                {
                    rxPlaying = false;
                    underrunCount = 0;
                }
            }
            digitalWrite(LED_PIN, LOW);
        }
        // i2s_write paces playback naturally
        i2s_write(I2S_NUM_0, stereoBuf, samplesOut * 2 * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(20));
    }
}

// Monitors the physical PTT button and Channel Buttons
void buttonTask(void *param)
{
    bool lastLeft = HIGH;
    bool lastRight = HIGH;
    unsigned long lastPttChange = 0;
    const unsigned long PTT_DEBOUNCE_MS = 50;

    while (1)
    {
        bool pressed = !digitalRead(BUTTON);
        bool leftState = digitalRead(BTN_LEFT);
        bool rightState = digitalRead(BTN_RIGHT);
        unsigned long now = millis();

        if (displayState == STATE_PROGRAM_PROMPT)
        {
            if (leftState == LOW && lastLeft == HIGH)
            {
                usbSelection = false;
                lastAnimUpdate = millis(); // Refresh UI
            }
            if (rightState == LOW && lastRight == HIGH)
            {
                usbSelection = true;
                lastAnimUpdate = millis(); // Refresh UI
            }
            if (pressed)
            {
                if (usbSelection)
                {
                    isProgramMode = true;
                    autoProgramMode = false;
                    displayState = STATE_PROGRAM_MODE;
                    preferences.putBool("in_prog", true);
                    ready = true; // Start handshake immediately
                    sent = false;
                    Serial.println("Entering Program Mode");
                }
                else
                {
                    isProgramMode = false;
                    autoProgramMode = false;
                    usbRejected = true;
                    displayState = STATE_IDLE;
                    preferences.putBool("in_prog", false);
                    Serial.println("Stayed in Walkie-Talkie Mode");
                }
                usbPromptActive = false;
                vTaskDelay(pdMS_TO_TICKS(300)); // Debounce confirm
            }
            lastLeft = leftState;
            lastRight = rightState;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (pressed && !isTransmitting && !isProgramMode && (now - lastPttChange >= PTT_DEBOUNCE_MS))
        {
            lastPttChange = now;
            radio.standby();
            radio.SPIsendCommand(0x3B); // SFTX
            radio.SPIsendCommand(0x3A); // SFRX
            cc1101_calibrate(); // one-time calibration for this transmission (FS_AUTOCAL disabled)

            switchToMicPins();

            if (currentChannelEncrypted)
            {
                for (int i = 0; i < 8; i++)
                {
                    radioSessionNonce[i] = (uint8_t)random(0, 256);
                }
                sessionNonceValid = true;
                txPacketCounter = 0;

                memset(txNoncePacket, 0, PACKET_SIZE);
                txNoncePacket[0] = NONCE_PACKET_TYPE;
                memcpy(&txNoncePacket[1], radioSessionNonce, 8);
                txNoncePending = true;
            }
#if RADIO_DEBUG
            Serial.print("[Radio] New session nonce: ");
            for (int i = 0; i < 8; i++)
            {
                if (radioSessionNonce[i] < 16)
                    Serial.print('0');
                Serial.print(radioSessionNonce[i], HEX);
            }
            Serial.println();
#endif
            isTransmitting = true;
            txAdpcmState.predicted = 0;
            txAdpcmState.index = 0;
            fillBuffer = 0;
            txBufferIndex = 0;
            for (int i = 0; i < TX_RING_SIZE; i++) bufferReady[i] = false;
            vTaskDelay(pdMS_TO_TICKS(10));

            if (displayState != STATE_SCREEN_OFF)
                displayState = STATE_TRANSMIT;
            dotCount = 0;
            lastAnimUpdate = millis();
#if RADIO_DEBUG
            Serial.println("TX mode");
#endif
        }
        else if (!pressed && isTransmitting && (now - lastPttChange >= PTT_DEBOUNCE_MS))
        {
            lastPttChange = now;

            i2s_stop(I2S_NUM_0);
            switchToSpeakerPins();
            i2s_zero_dma_buffer(I2S_NUM_0);
            i2s_start(I2S_NUM_0);

            isTransmitting = false;
            radio.SPIsendCommand(0x3B); // SFTX
            cc1101_start_rx();
            vTaskDelay(pdMS_TO_TICKS(10));

            sessionNonceValid = false;
            txNoncePending = false;

            // Reset jitter buffer for clean playback
            rxHead = 0;
            rxTail = 0;
            rxCount = 0;
            rxPlaying = false;

            if (displayState != STATE_SCREEN_OFF)
            {
                displayState = STATE_IDLE;
                scrollX = SCREEN_WIDTH;
            }
#if RADIO_DEBUG
            Serial.println("RX mode");
#endif
        }

        static unsigned long lastLeftPressTime = 0;
        static unsigned long lastRightPressTime = 0;
        static unsigned long leftHoldStartTime = 0;
        static unsigned long rightHoldStartTime = 0;
        static bool leftRepeating = false;
        static bool rightRepeating = false;

        if (leftState == LOW && !isTransmitting && !isProgramMode)
        {
            if (lastLeft == HIGH)
            { // Initial press
                if (now - lastLeftPressTime > 200)
                { // 200ms deep software debounce
                    lastLeftPressTime = now;
                    leftHoldStartTime = now;
                    leftRepeating = false;

                    if (displayState != STATE_CHANNEL)
                    {
                        showChannelSelector();
                    }
                    else
                    {
                        changeChannel(-1);
                    }
                }
            }
            else
            { // Being held
                if (!leftRepeating && (now - leftHoldStartTime > BUTTON_HOLD_DURATION))
                {
                    leftRepeating = true;
                    leftHoldStartTime = now; // Reset timer for the 500ms autorepeat intervals
                }
                if (leftRepeating && (now - leftHoldStartTime > RAPID_CHANNEL_SWITCH_TIME))
                {
                    leftHoldStartTime = now;
                    changeChannel(-1);
                }
            }
        }
        else
        {
            leftRepeating = false;
        }

        if (rightState == LOW && !isTransmitting && !isProgramMode)
        {
            if (lastRight == HIGH)
            { // Initial press
                if (now - lastRightPressTime > 200)
                { // 200ms deep software debounce
                    lastRightPressTime = now;
                    rightHoldStartTime = now;
                    rightRepeating = false;

                    if (displayState != STATE_CHANNEL)
                    {
                        showChannelSelector();
                    }
                    else
                    {
                        changeChannel(1);
                    }
                }
            }
            else
            { // Being held
                if (!rightRepeating && (now - rightHoldStartTime > BUTTON_HOLD_DURATION))
                {
                    rightRepeating = true;
                    rightHoldStartTime = now; // Reset timer for the 500ms autorepeat intervals
                }
                if (rightRepeating && (now - rightHoldStartTime > RAPID_CHANNEL_SWITCH_TIME))
                {
                    rightHoldStartTime = now;
                    changeChannel(1);
                }
            }
        }
        else
        {
            rightRepeating = false;
        }

        lastLeft = leftState;
        lastRight = rightState;

        vTaskDelay(pdMS_TO_TICKS(20)); // Debounce
    }
}

// Monitors Potentiometer and scales the gain
void volumeReadTask(void *param)
{
    while (1)
    {
        int potValue = adc1_get_raw(POT_CHANNEL);

        // Detect pot at absolute bottom (switch is about to cut power)
        // Immediately blank OLED before ESP32 loses power (or OLED loses battery power)
        if (potValue < POT_OFF_THRESHOLD && displayState != STATE_SCREEN_OFF && displayState != STATE_STARTUP)
        {
            if (hasDisplay)
            {
                display.clearDisplay();
                display.display();
                display.ssd1306_command(SSD1306_DISPLAYOFF);
            }
            displayState = STATE_SCREEN_OFF;
            lastPotValue = potValue;
        }
        else if (abs(potValue - lastPotValue) > VOLUME_HYSTERESIS)
        {
            lastPotValue = potValue;

            if (potValue >= POT_ON_THRESHOLD && displayState == STATE_SCREEN_OFF)
            {
                bool wasAutoProgramMode = autoProgramMode;
                if (wasAutoProgramMode)
                {
                    isProgramMode = false;
                    autoProgramMode = false;
                    usbPromptActive = false;
                    ready = false;
                    sent = false;
                    preferences.putBool("in_prog", false);
                }

                // If the OLED hardware lost power via the switch, it needs full re-initialization
                // since the ESP32 stayed alive on USB power.
                if (hasDisplay)
                {
                    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
                    display.ssd1306_command(SSD1306_SETCONTRAST);
                    display.ssd1306_command(0xCF);
                    display.clearDisplay();
                    display.display();
                    display.ssd1306_command(SSD1306_DISPLAYON);
                }

                // Restore the correct state
                if (wasAutoProgramMode)
                {
                    displayState = STATE_VOLUME;
                }
                else if (isProgramMode)
                {
                    displayState = STATE_PROGRAM_MODE;
                }
                else if (usbPromptActive)
                {
                    displayState = STATE_PROGRAM_PROMPT;
                }
                else
                {
                    displayState = STATE_VOLUME;
                }
            }

            // Only change the actual volume if we are in normal walkie-talkie mode
            if (!isProgramMode && !usbPromptActive && displayState != STATE_USB_DISCONNECTED)
            {
                float newGain = 0.1f + (float)potValue * ((GAIN_MAX - 0.1f) / 4095.0f);
                newGain = constrain(newGain, GAIN_MIN, GAIN_MAX);

                GAIN_FOR_UI = newGain; // Float kept for drawing the UI
                GAIN = (int32_t)(newGain * 256);

                lastVolumeChange = millis();
                if (displayState != STATE_SCREEN_OFF && displayState != STATE_PROGRAM_MODE && displayState != STATE_PROGRAM_PROMPT)
                {
                    displayState = STATE_VOLUME;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// =================================================================
// 6. OLED UI HELPERS & TASK
// =================================================================
void drawCenteredText(const char *text, uint8_t textSize = 3)
{
    uint16_t w = strlen(text) * 6 * textSize;
    uint16_t h = 8 * textSize;
    display.setTextSize(textSize);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor((SCREEN_WIDTH - w) / 2, (SCREEN_HEIGHT - h) / 2);
    display.print(text);
}

void drawVolume(float gain)
{
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);

    const char *volText = "Volume:";
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(volText, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((display.width() - w) / 2, 0);
    display.print(volText);

    int bars = constrain((int)((gain - 0.1f) / ((GAIN_MAX - 0.1f) / 10.0f)), 0, 10);
    int barWidth = 8, spacing = 2;
    int totalBarWidth = bars > 0 ? (bars * barWidth) + ((bars - 1) * spacing) : 0;
    int startX = (display.width() - totalBarWidth) / 2;
    int barY = display.height() / 2 - 5;

    for (int i = 0; i < bars; i++)
    {
        display.fillRect(startX + i * (barWidth + spacing), barY, barWidth, 20, SSD1306_WHITE);
    }
}

void OLEDTask(void *parameter)
{
    if (!hasDisplay)
    {
        vTaskDelete(NULL);
        return;
    }
    const uint16_t CHAR_WIDTH = 6 * 3; // For scrolling text math
    const int16_t textY = (SCREEN_HEIGHT - 24) / 2;

    while (true)
    {
        unsigned long now = millis();
        display.clearDisplay();

        switch (displayState)
        {
        case STATE_STARTUP:
        {
            if (now - startupTime > STARTUP_SHOW_DURATION)
            {
                displayState = STATE_IDLE;
                scrollX = SCREEN_WIDTH;
            }
            else
            {
                drawCenteredText("Starting..", 2);
            }
            break;
        }
        case STATE_TRANSMIT:
        {
            if (now - lastAnimUpdate >= ANIM_INTERVAL)
            {
                lastAnimUpdate = now;
                dotCount = (dotCount + 1) % 4;
            }
            char txText[12] = "SENDING";
            for (int i = 0; i < dotCount; i++)
                strcat(txText, ".");
            drawCenteredText(txText, 2);
            break;
        }
        case STATE_RECEIVE:
        {
            if (now - lastAnimUpdate >= ANIM_INTERVAL)
            {
                lastAnimUpdate = now;
                dotCount = (dotCount + 1) % 4;
            }
            char rxText[13] = "RECEIVING";
            for (int i = 0; i < dotCount; i++)
                strcat(rxText, ".");
            drawCenteredText(rxText, 2);
            break;
        }
        case STATE_VOLUME:
        {
            bool volIsZero = (GAIN_FOR_UI <= GAIN_MIN);
            unsigned long timeout = volIsZero ? SCREEN_OFF_DELAY : VOLUME_SHOW_DURATION;
            if (now - lastVolumeChange > timeout)
            {
                if (volIsZero)
                {
                    display.clearDisplay();
                    display.display();
                    display.ssd1306_command(SSD1306_DISPLAYOFF); // HW off so OLED is blank when ESP32 loses power
                    displayState = STATE_SCREEN_OFF;
                }
                else
                {
                    displayState = STATE_IDLE;
                    scrollX = SCREEN_WIDTH;
                }
            }
            else
            {
                drawVolume(GAIN_FOR_UI);
            }
            break;
        }
        case STATE_SCREEN_OFF:
        {
            // Screen stays blank; clearDisplay already called above
            break;
        }
        case STATE_CHANNEL:
        {
            if (now - lastChannelActivity > CHANNEL_SHOW_DURATION)
            {
                displayState = STATE_IDLE;
                scrollX = SCREEN_WIDTH;
                if (currentChannel != savedChannel)
                {
                    preferences.putUChar("channel", currentChannel);
                    savedChannel = currentChannel;
                }
            }
            else
            {
                char chText[16];
                snprintf(chText, sizeof(chText), "CH %d", currentChannel);

                // Draw channel text centered with size 2
                drawCenteredText(chText, 2);

                if (currentChannelEncrypted)
                {
                    // Draw "(Encrypted)" centered at the bottom
                    const char *encText = "(Encrypted)";
                    uint16_t encW = strlen(encText) * 6 * 1;
                    display.setTextSize(1);
                    display.setTextColor(SSD1306_WHITE);
                    display.setCursor((SCREEN_WIDTH - encW) / 2, SCREEN_HEIGHT - 12);
                    display.print(encText);
                }
            }
            break;
        }
        case STATE_PROGRAM_PROMPT:
        {
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);
            display.setCursor(0, 5);
            display.println("CONNECTED!");
            display.setTextSize(1);
            display.setCursor(0, 20);
            display.println("Enter Program mode?");

            // Draw No
            if (!usbSelection)
            {
                display.fillRoundRect(0, 45, 40, 15, 3, SSD1306_WHITE);
                display.setTextColor(SSD1306_BLACK);
            }
            else
            {
                display.drawRoundRect(0, 45, 40, 15, 3, SSD1306_WHITE);
                display.setTextColor(SSD1306_WHITE);
            }
            display.setCursor(12, 49);
            display.print("No");

            // Draw Yes
            if (usbSelection)
            {
                display.fillRoundRect(88, 45, 40, 15, 3, SSD1306_WHITE);
                display.setTextColor(SSD1306_BLACK);
            }
            else
            {
                display.drawRoundRect(88, 45, 40, 15, 3, SSD1306_WHITE);
                display.setTextColor(SSD1306_WHITE);
            }
            display.setCursor(97, 49);
            display.print("Yes");

            display.setTextColor(SSD1306_WHITE);
            break;
        }
        case STATE_PROGRAM_MODE:
        {
            if (now - lastAnimUpdate >= ANIM_INTERVAL)
            {
                lastAnimUpdate = now;
                dotCount = (dotCount + 1) % 4;
            }
            display.setFont(&FreeMono9pt7b);
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);

            const char *line1 = "Please";
            char line2[15] = "log in";
            for (int i = 0; i < dotCount; i++)
                strcat(line2, ".");

            int16_t x1, y1;
            uint16_t w, h;

            // "Please"
            display.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
            display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 25);
            display.print(line1);

            // "log in..."
            display.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
            display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 45);
            display.print(line2);

            display.setFont();
            break;
        }
        case STATE_LOGGED_IN:
        {
            display.setFont(&FreeMono9pt7b);
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);

            if (now - loginSuccessTime < 2000)
            {
                const char *msg = "Connected!";
                int16_t x1, y1;
                uint16_t w, h;
                display.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
                display.setCursor((SCREEN_WIDTH - w) / 2 - x1, (SCREEN_HEIGHT - h) / 2 - y1);
                display.print(msg);
            }
            else
            {
                const char *line1 = "In";
                const char *line2 = "Program Mode";
                int16_t x1, y1;
                uint16_t w, h;

                // Line 1: "In"
                display.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
                display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 25);
                display.print(line1);

                // Line 2: "Program Mode"
                display.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
                display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 45);
                display.print(line2);
            }

            display.setFont();
            break;
        }
        case STATE_USB_DISCONNECTED:
        {
            if (now - disconnectTime > 2000)
            {
                displayState = STATE_IDLE;
                scrollX = SCREEN_WIDTH;
            }
            else
            {
                display.setFont(&FreeMono9pt7b);
                display.setTextSize(1);
                display.setTextColor(SSD1306_WHITE);
                const char *msg = "Exiting...";
                int16_t x1, y1;
                uint16_t w, h;
                display.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
                display.setCursor((SCREEN_WIDTH - w) / 2 - x1, (SCREEN_HEIGHT - h) / 2 - y1);
                display.print(msg);
                display.setFont();
            }
            break;
        }
        case STATE_IDLE:
        default:
        {
            display.setTextSize(3);
            display.setTextColor(SSD1306_WHITE);
            display.setTextWrap(false);
            display.setCursor(scrollX, textY);
            display.print(welcomeText);

            scrollX -= scrollSpeed;
            if (scrollX + (int16_t)(strlen(welcomeText) * CHAR_WIDTH) < 0)
            {
                scrollX = SCREEN_WIDTH;
            }
            break;
        }
        }

        // Return to idle if RX times out
        if (displayState == STATE_RECEIVE && (now - lastReceiveTime > RECEIVE_TIMEOUT))
        {
            displayState = STATE_IDLE;
            scrollX = SCREEN_WIDTH;
            dotCount = 0;
        }

        display.display();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Handles the USB handshake and password verification with the PC app
void usbHandshakeTask(void *param)
{
    unsigned long lastHeartbeatTime = 0;
    bool heartbeatActive = false;
    while (1)
    {
        // USB Detection: On most ESP32s, Serial will be true if the USB bridge is powered.
        // We wait for the "ready" command from the Python program as the activation signal.
        if (Serial)
        {
            if (!isProgramMode && !usbPromptActive && !usbRejected)
            {
                int potValue = adc1_get_raw(POT_CHANNEL);
                if (potValue <= POT_AUTO_PROGRAM_THRESHOLD)
                {
                    // Device is off. Auto-enter program mode to allow headless programming
                    // while keeping the OLED off until the knob is turned back up.
                    isProgramMode = true;
                    autoProgramMode = true;
                    displayState = STATE_SCREEN_OFF;
                    Serial.println("Auto-entered Program Mode (Device OFF)");
                }
                else
                {
                    // Only show prompt if NVS says we were in program mode
                    bool wasInProgram = preferences.getBool("in_prog", false);
                    if (wasInProgram)
                    {
                        usbPromptActive = true;
                        displayState = STATE_PROGRAM_PROMPT;
                        usbSelection = true; // Default Yes
                    }
                }
            }

            // Always check for Serial data to keep heartbeat alive
            Serial.setTimeout(100);
            if (Serial.available())
            {
                String msg = Serial.readStringUntil('\n');
                msg.trim();
                if (msg.length() > 0 && heartbeatActive)
                {
                    lastHeartbeatTime = millis();
                }
                if (msg.indexOf("heartbeat") != -1)
                {
                    heartbeatActive = true;
                    lastHeartbeatTime = millis();
                }
                else if (msg.indexOf("ready") != -1)
                {
                    heartbeatActive = true;
                    lastHeartbeatTime = millis();
                    if (!isProgramMode && !usbPromptActive)
                    {
                        // Show prompt when PC connects, instead of auto-entering
                        usbPromptActive = true;
                        displayState = STATE_PROGRAM_PROMPT;
                        usbSelection = true;
                    }

                    if (isProgramMode)
                    {
                        ready = true;
                        sent = false;
                        Serial.println("Python ready received!");
                        vTaskDelay(pdMS_TO_TICKS(500));
                    }
                }
                else if (msg.indexOf("login_ok") != -1)
                {
                    heartbeatActive = true;
                    lastHeartbeatTime = millis();
                    loginSuccessTime = millis();
                    displayState = STATE_LOGGED_IN;
                    Serial.println("Login confirmation received.");
                }
                else if (msg.startsWith("device_password_b64:"))
                {
                    String encodedPassword = msg.substring(strlen("device_password_b64:"));
                    String decodedPassword = "";
                    if (decodeB64ToString(encodedPassword, decodedPassword) && decodedPassword.length() > 0)
                    {
                        devicePassword = decodedPassword;
                        preferences.putString("device_pwd", devicePassword);
                        Serial.println("Device password stored.");
                    }
                    else
                    {
                        Serial.println("Failed to store device password.");
                    }
                }
                else if (msg.startsWith("device_name_b64:"))
                {
                    String encodedName = msg.substring(strlen("device_name_b64:"));
                    String decodedName = "";
                    if (decodeB64ToString(encodedName, decodedName) && decodedName.length() > 0)
                    {
                        deviceName = decodedName;
                        updateDeviceMessage();
                        preferences.putString("device_name", deviceName);
                        Serial.println("Device name stored.");
                    }
                    else
                    {
                        Serial.println("Failed to store device name.");
                    }
                }
                else if (msg.startsWith("radio_password_b64:"))
                {
                    String encodedPassword = msg.substring(strlen("radio_password_b64:"));
                    String decodedPassword = "";
                    if (decodeB64ToString(encodedPassword, decodedPassword) && decodedPassword.length() > 0)
                    {
                        radioPassword = decodedPassword;
                        preferences.putString("radio_pwd", radioPassword);
                        initRadioEncryption();
                        Serial.println("Radio password stored.");
                    }
                    else
                    {
                        Serial.println("Failed to store radio password.");
                    }
                }
                else if (msg.startsWith("radio_salt_b64:"))
                {
                    String encodedSalt = msg.substring(strlen("radio_salt_b64:"));
                    String decodedSalt = "";
                    if (decodeB64ToString(encodedSalt, decodedSalt))
                    {
                        radioSaltValue = decodedSalt;
                        if (radioSaltValue.length() > 0)
                        {
                            preferences.putString("radio_salt", radioSaltValue);
                        }
                        else
                        {
                            preferences.remove("radio_salt");
                        }
                        initRadioEncryption();
                        Serial.println("Radio salt stored.");
                    }
                    else
                    {
                        Serial.println("Failed to store radio salt.");
                    }
                }
                else if (msg.startsWith("encrypted_channels:"))
                {
                    String channelList = msg.substring(strlen("encrypted_channels:"));
                    storeEncryptedChannels(channelList);
                    Serial.println("Encrypted channels stored.");
                }
            }

            // Heartbeat Timeout Logic
            if (heartbeatActive)
            {
                if (millis() - lastHeartbeatTime > 3000)
                {
                    heartbeatActive = false;
                    isProgramMode = false;
                    autoProgramMode = false;
                    usbPromptActive = false;
                    usbRejected = false;
                    ready = false;
                    sent = false;
                    if (displayState == STATE_PROGRAM_PROMPT || displayState == STATE_PROGRAM_MODE || displayState == STATE_LOGGED_IN)
                    {
                        displayState = STATE_USB_DISCONNECTED;
                        disconnectTime = millis();
                    }
                    preferences.putBool("in_prog", false);
                    Serial.println("USB disconnected (Heartbeat Timeout).");
                }
            }

            if (ready && !sent)
            {
                sent = true;
                Serial.println("Handshake started...");

                unsigned char salt[16];
                unsigned char nonce[12];

                for (int i = 0; i < 16; i++)
                    salt[i] = random(0, 255);
                for (int i = 0; i < 12; i++)
                    nonce[i] = random(0, 255);

                unsigned char key[32];

                Serial.println("Computing PBKDF2...");
                mbedtls_pkcs5_pbkdf2_hmac_ext(
                    MBEDTLS_MD_SHA256,
                    (const unsigned char *)devicePassword.c_str(),
                    devicePassword.length(),
                    salt,
                    16,
                    10000,
                    32,
                    key);

                mbedtls_gcm_context gcm;
                mbedtls_gcm_init(&gcm);
                mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);

                unsigned char ciphertext[128];
                unsigned char tag[16];

                mbedtls_gcm_crypt_and_tag(
                    &gcm,
                    MBEDTLS_GCM_ENCRYPT,
                    strlen(message),
                    nonce,
                    12,
                    NULL, 0,
                    (const unsigned char *)message,
                    ciphertext,
                    16,
                    tag);

                mbedtls_gcm_free(&gcm);

                StaticJsonDocument<1024> out;
                bool passwordConfigured = devicePassword.length() > 0;

                out["device"] = deviceName;
                out["password_enabled"] = passwordConfigured;
                out["radio_salt"] = radioSaltValue;
                JsonArray encryptedChannelJson = out.createNestedArray("encrypted_channels");
                for (uint8_t i = 0; i < encryptedChannelCount; i++)
                {
                    encryptedChannelJson.add(encryptedChannels[i]);
                }
                out["salt"] = b64encode(salt, 16);
                out["nonce"] = b64encode(nonce, 12);
                out["ciphertext"] = b64encode(ciphertext, strlen(message));
                out["tag"] = b64encode(tag, 16);

                Serial.println("Sending Handshake JSON...");
                serializeJson(out, Serial);
                Serial.println();
                Serial.println("Handshake JSON sent!");
            }
        }
        else
        {
            // Reset state if Serial is lost
            if (isProgramMode || usbPromptActive || usbRejected)
            {
                isProgramMode = false;
                autoProgramMode = false;
                usbPromptActive = false;
                usbRejected = false;
                ready = false;
                sent = false;
                if (displayState == STATE_PROGRAM_PROMPT || displayState == STATE_PROGRAM_MODE)
                {
                    displayState = STATE_USB_DISCONNECTED;
                    disconnectTime = millis();
                }
                preferences.putBool("in_prog", false);
                Serial.println("USB disconnected.");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void cc1101_init()
{
    const float bitrate = 250.0; // kbps
    const float frequency_deviation = 127.0; // kHz
    const float rxBandwidth = 540.0; // kHz
    const int8_t radio_power = 10; // dBm

    int state = radio.begin(BASE_FREQUENCY + currentChannel * CHANNEL_SPACING, bitrate, frequency_deviation, rxBandwidth, radio_power, 32);
    if (state == RADIOLIB_ERR_NONE)
    {
        Serial.println(F("CC1101 init success!"));

        radio.setCrcFiltering(true);
        radio.fixedPacketLengthMode(PACKET_SIZE);

        uint8_t syncWord[2] = {0x53, 0x72};
        radio.setSyncWord(syncWord, 2);

        // Keep the existing CC1101 modulation and packet behavior.
        radio.SPIsetRegValue(0x12, 0x10, 6, 4); // MDMCFG2.MOD_FORMAT = GFSK
        radio.SPIsetRegValue(0x07, 0x00, 2, 2); // PKTCTRL1.APPEND_STATUS = off; keep RX FIFO aligned to 32-byte packets
        radio.SPIsetRegValue(0x07, 0x01, 3, 3); // PKTCTRL1.CRC_AUTOFLUSH = on; HW drops bad-CRC packets so a single bit error can't desync the 32-byte byte stream
        radio.SPIsetRegValue(0x17, 0x0C, 3, 2); // MCSM1.RXOFF_MODE = stay in RX
        radio.SPIsetRegValue(0x18, 0x00, 5, 4); // MCSM0.FS_AUTOCAL = never; we calibrate manually (cc1101_calibrate) to avoid recalibrating on every packet

        radio.setPacketReceivedAction(cc1101PacketISR);
        cc1101_start_rx();
    }
    else
    {
        Serial.print(F("CC1101 init failed, code "));
        Serial.println(state);
    }
}

// =================================================================
// 7. MAIN HARDWARE SETUP
// =================================================================

void setup()
{
    Serial.begin(115200);

    // --- OLED Setup ---
    Wire.begin(SDA_PIN, SCK_PIN);
    Wire.setClock(100000); // Use standard speed for detection

    // Check if anything is responding at the OLED address
    Wire.beginTransmission(0x3C);
    if (Wire.endTransmission() == 0)
    {
        hasDisplay = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    }
    else
    {
        hasDisplay = false;
    }

    if (!hasDisplay)
    {
        Serial.println("OLED not found at 0x3C or unpowered.");
    }
    else
    {
        Wire.setClock(400000); // Back to high speed
        display.ssd1306_command(SSD1306_SETCONTRAST);
        display.ssd1306_command(0xCF);
        display.clearDisplay();
        display.display();
        Serial.println("OLED Initialized.");
        display.clearDisplay();
        display.setTextSize(2);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(4, (SCREEN_HEIGHT - 16) / 2);
        display.print("Starting..");
        display.display();
    }

    startupTime = millis();
    preferences.begin("walkie", false);
    // Clear stored settings once after each new upload.
    String storedBuildId = preferences.getString("build_id", "");
    if (storedBuildId != BUILD_ID)
    {
        preferences.clear();
        preferences.putString("build_id", BUILD_ID);
    }
    deviceName = preferences.getString("device_name", "Walkie-Talkie");
    updateDeviceMessage();
    devicePassword = preferences.getString("device_pwd", "");
    radioPassword = preferences.getString("radio_pwd", "");
    radioSaltValue = preferences.getString("radio_salt", "");
    loadEncryptedChannels();
    initRadioEncryption(); // Derive radio key & nonce from the stored radio password.
    // --- GPIO Setup ---
    pinMode(BUTTON, INPUT_PULLUP);
    pinMode(BTN_LEFT, INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);
    pinMode(CC1101_GDO0, INPUT);
    pinMode(LED_PIN, OUTPUT);
    setActivityLed(false);

    // --- Preferences Setup ---
    currentChannel = preferences.getUChar("channel", MIN_CHANNEL_VALUE);
    if (currentChannel < MIN_CHANNEL_VALUE || currentChannel > MAX_CHANNEL_VALUE)
        currentChannel = MIN_CHANNEL_VALUE;
    savedChannel = currentChannel;
    refreshCurrentChannelEncryption();

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
    float bootGain = constrain(0.1f + (float)lastPotValue * ((GAIN_MAX - 0.1f) / 4095.0f), GAIN_MIN, GAIN_MAX);
    GAIN_FOR_UI = bootGain;
    GAIN = (int32_t)(bootGain * 256);

    lastVolumeChange = millis();
    scrollX = SCREEN_WIDTH;

    // --- Boot Tasks on Dual Cores ---
    xTaskCreatePinnedToCore(audioCaptureTask, "AudioCapture", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(radioTask, "RadioTask", 4096, NULL, 4, NULL, 1);   // Highest priority on core 1
    xTaskCreatePinnedToCore(playbackTask, "Playback", 4096, NULL, 2, NULL, 1); // Below radio, paced by I2S DMA
    xTaskCreatePinnedToCore(buttonTask, "ButtonTask", 2048, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(volumeReadTask, "VolumeRead", 2048, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(OLEDTask, "OLED Task", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(usbHandshakeTask, "USB Handshake", 8192, NULL, 1, NULL, 0);

    Serial.println("ESP32 Walkie-Talkie Ready!");
}

void loop()
{
    // Handled by FreeRTOS tasks
}
