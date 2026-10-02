#!/usr/bin/env python3
"""Optional independent wire-format check using Python cryptography, no USB."""
import struct
import subprocess
import sys

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

# Fixed, public TEST-ONLY scalar, never used by the driver or against hardware.
key = ec.derive_private_key(7, ec.SECP256R1())
point = key.public_key().public_numbers()
prefix = struct.pack("<HH68s68sxB", 0x5F3F, 23,
                     point.x.to_bytes(68, "little"), point.y.to_bytes(68, "little"), 0)
signature = key.sign(prefix, ec.ECDSA(hashes.SHA256()))
certificate = prefix + struct.pack("<H256s", len(signature), signature)
assert len(prefix) == 142 and len(certificate) == 400
pem = key.private_bytes(serialization.Encoding.PEM,
                        serialization.PrivateFormat.PKCS8,
                        serialization.NoEncryption())
bridge = sys.argv[1]
result = subprocess.run([bridge], input=pem + certificate, capture_output=True, check=True)
native = result.stdout
assert len(native) == 400 and native[:142] == prefix
length = struct.unpack_from("<H", native, 142)[0]
key.public_key().verify(native[144:144 + length], prefix, ec.ECDSA(hashes.SHA256()))
tampered = bytearray(certificate)
tampered[141] ^= 1
result = subprocess.run([bridge], input=pem + tampered, capture_output=True)
assert result.returncode == 1 and not result.stdout
try:
    key.public_key().verify(native[144:144 + length], bytes(tampered[:142]), ec.ECDSA(hashes.SHA256()))
except InvalidSignature:
    pass
else:
    raise AssertionError("independent verifier accepted tampered certificate")
print("Certificate interoperability passed in both directions (OpenSSL EVP / Python cryptography).")
