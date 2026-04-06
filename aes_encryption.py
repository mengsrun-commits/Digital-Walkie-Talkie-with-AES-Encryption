from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC
import os

password = b"my_secret_password" # converts the string to raw bytes
salt = os.urandom(16) # generates a random 16 byte value

# Derive a key from the password and salt using PBKDF2
kdf = PBKDF2HMAC(
    algorithm=hashes.SHA256(),
    length=32, # AES-256 key length
    salt=salt,
    iterations=100000
)

key = kdf.derive(password)

# AES-GCM Encryption

plaintext = b"Hello world"
nonce = os.urandom(12)

cipher = Cipher(
    algorithms.AES(key),
    modes.GCM(nonce)
)

encryptor = cipher.encryptor()

ciphertext = encryptor.update(plaintext) + encryptor.finalize()
tag = encryptor.tag # For Authentication

print("Plain Text: ", plaintext)
print("Cipher Text: ", ciphertext)
print("Tag: ", tag)
print("Nonce: ",  nonce)
print("Salt: ", salt)

cipher_decrypt = Cipher(
    algorithms.AES(key),
    modes.GCM(nonce, tag)
)

decryptor = cipher_decrypt.decryptor()
decrypted = decryptor.update(ciphertext) + decryptor.finalize()

print("Decrypted message: ", decrypted)