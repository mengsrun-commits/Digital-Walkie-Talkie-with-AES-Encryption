#include <ArduinoJson.h>
#include "mbedtls/gcm.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"

const char *PASSWORD = "password2";
const char *DEVICE_NAME = "Device 2";
String message = "Welcome Device 2!";
bool ready = false;
bool sent = false;

String b64encode(const unsigned char* input, size_t len){
    size_t out_len = 0;
    unsigned char out[256];

    mbedtls_base64_encode(out, sizeof(out), &out_len, input, len);
    return String((char*)out).substring(0, out_len);
}

void setup(){
    Serial.begin(115200);
    delay(2000);
    Serial.println("ESP32 waiting for ready...");
}

void loop(){
    delay(10);
    if(Serial.available()){
        String msg = Serial.readStringUntil('\n');
        msg.trim();

        msg.toLowerCase();

        if(msg == "ready"){
            ready = true;
            Serial.println("Python ready!");
            sent = false; // allow resend on each ready handshake
        }
    }
    if (ready && !sent) {
        sent = true;

        unsigned char salt[16];
        unsigned char nonce[12];

        for (int i = 0; i < 16; i++) salt[i] = random(0, 255);
        for (int i = 0; i < 12; i++) nonce[i] = random(0, 255);

        unsigned char key[32];

        mbedtls_pkcs5_pbkdf2_hmac_ext(
            MBEDTLS_MD_SHA256,
            (const unsigned char*)PASSWORD,
            strlen(PASSWORD),
            salt,
            16,
            10000,
            32,
            key
        );

        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);

        unsigned char ciphertext[128];
        unsigned char tag[16];

        mbedtls_gcm_crypt_and_tag(
            &gcm,
            MBEDTLS_GCM_ENCRYPT,
            message.length(),
            nonce,
            12,
            NULL, 0,
            (const unsigned char*)message.c_str(),
            ciphertext,
            16,
            tag
        );

        mbedtls_gcm_free(&gcm);

        StaticJsonDocument<512> out;

        out["device"] = DEVICE_NAME;
        out["salt"] = b64encode(salt, 16);
        out["nonce"] = b64encode(nonce, 12);
        out["ciphertext"] = b64encode(ciphertext, message.length());
        out["tag"] = b64encode(tag, 16);

        serializeJson(out, Serial);
        Serial.println();

    }

}