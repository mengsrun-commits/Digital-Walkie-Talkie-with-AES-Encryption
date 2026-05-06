import serial
import time
import json
import base64
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import hashes

ser = serial.Serial('COM3', 115200, timeout=1)
time.sleep(2)

print("Waiting for ESP32 encrypted message...")
ser.write(b"READY\n")

while True:
    if ser.in_waiting:
        line = ser.readline().decode().strip()

        try:
            msg = json.loads(line)
            print("\nEncrypted packet received ✔")
            print("Ciphertext:", base64.b64decode(msg["ciphertext"]))
            break
        except:
            continue


salt = base64.b64decode(msg["salt"])
nonce = base64.b64decode(msg["nonce"])
ciphertext = base64.b64decode(msg["ciphertext"])
tag = base64.b64decode(msg["tag"])


while True:
    password = input("\nEnter password: ").encode()

    # derive key (same as ESP32)
    kdf = PBKDF2HMAC(
        algorithm=hashes.SHA256(),
        length=32,
        salt=salt,
        iterations=100000
    )

    key = kdf.derive(password)

    # AES-GCM decrypt
    try:
        cipher = Cipher(
            algorithms.AES(key),
            modes.GCM(nonce, tag)
        )

        decryptor = cipher.decryptor()
        plaintext = decryptor.update(ciphertext) + decryptor.finalize()

        print("\n✅ Correct password!")
        print("Decrypted message:", plaintext.decode())

        ser.close()
        break

    except Exception:
        print("❌ Wrong password, try again.")