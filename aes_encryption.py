from cryptography.hazmat.primitives.kdf.scrypt import Scrypt
import os

password = b"mysecretkeyword"   # your keyword
salt = os.urandom(16)          # must be saved for later!

kdf = Scrypt(
    salt=salt,
    length=32,     # 32 bytes = AES-256
    n=2**14,
    r=8,
    p=1,
)

key = kdf.derive(password)

print(key)