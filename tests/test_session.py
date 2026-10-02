#!/usr/bin/env python3
"""Independent simulated Tudor peer. Uses synthetic keys, never USB or real state."""
import hashlib
import hmac
import os
from pathlib import Path
import selectors
import struct
import subprocess
import sys
import tempfile

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


def certificate(key, signer, kind):
    p = key.public_key().public_numbers()
    body = struct.pack('<HH68s68sxB', 0x5f3f, 23, p.x.to_bytes(68, 'little'), p.y.to_bytes(68, 'little'), kind)
    signature = signer.sign(body, ec.ECDSA(hashes.SHA256()))
    return body + struct.pack('<H256s', len(signature), signature)


def hs(kind, body):
    return bytes([kind]) + len(body).to_bytes(3, 'big') + body


def record(kind, body):
    return struct.pack('>BHH', kind, 0x0303, len(body)) + body


def records(data):
    out = []
    while data:
        assert len(data) >= 5
        kind, version, n = struct.unpack('>BHH', data[:5])
        assert version == 0x0303 and len(data) >= 5 + n
        out.append((kind, data[5:5+n]))
        data = data[5+n:]
    return out


def handshakes(data):
    while data:
        assert len(data) >= 4
        size = int.from_bytes(data[1:4], 'big')
        assert len(data) >= size + 4
        yield data[0], data[4:4+size], data[:4+size]
        data = data[4+size:]


def prf(secret, label, seed, length):
    seed = label + seed
    a, result = seed, b''
    while len(result) < length:
        a = hmac.digest(secret, a, 'sha384')
        result += hmac.digest(secret, a + seed, 'sha384')
    return result[:length]


class Peer:
    def __init__(self, mode, client, sensor, host_cert):
        self.mode, self.client, self.sensor, self.host_cert = mode, client, sensor, host_cert
        self.phase = 0
        self.tx = self.rx = 0
        self.transcript = b''
        self.last_firmware_record = None

    def seal(self, kind, plain):
        explicit = self.tx.to_bytes(8, 'big')
        aad = explicit + struct.pack('>BHH', kind, 0x303, len(plain))
        result = record(kind, explicit + AESGCM(self.server_key).encrypt(self.server_salt + explicit, plain, aad))
        self.tx += 1
        return result

    def unseal(self, kind, body):
        aad = self.rx.to_bytes(8, 'big') + struct.pack('>BHH', kind, 0x303, len(body) - 24)
        plain = AESGCM(self.client_key).decrypt(self.client_salt + body[:8], body[8:], aad)
        self.rx += 1
        return plain

    def exchange(self, request):
        # Abort is permitted on a failed experiment; never interpret it as success.
        wire = request[4:] if request.startswith(b'\x44\x00\x00\x00') else request
        rs = records(wire)
        if rs and rs[0][0] == 21 and self.mode not in ('valid', 'fragmented', 'dimension-rejected', 'hello-marker'):
            return b''
        if self.phase == 0:
            assert request.startswith(b'\x44\x00\x00\x00') and len(rs) == 1 and rs[0][0] == 22
            msgs = list(handshakes(rs[0][1])); assert len(msgs) == 1 and msgs[0][0] == 1
            _, hello, raw = msgs[0]
            assert hello[:2] == b'\x03\x03' and hello[34:42] == b'\x07' + bytes(7)
            assert hello[42:] == bytes.fromhex('0002c02e00000a000400020017000b00020100')
            self.client_random = hello[2:34]
            self.server_random = bytes(range(32))
            suite = b'\xc0\x05' if self.mode == 'wrong-suite' else b'\xc0\x2e'
            version = b'\x03\x83' if self.mode == 'hello-marker' else b'\x03\x01' if self.mode == 'wrong-version' else b'\x03\x03'
            response = hs(2, version + self.server_random + b'\x07' + bytes(7) + suite + b'\x00')
            response += hs(13, b'\x01\x40\x00\x00') + hs(14, b'')
            self.transcript = raw + response
            self.phase = 1
            if self.mode == 'truncated': return record(22, response)[:-1]
            if self.mode == 'fragmented': return record(22, response[:19]) + record(22, response[19:])
            return record(22, response)
        if self.phase == 1:
            assert request.startswith(b'\x44\x00\x00\x00')
            assert [r[0] for r in rs] == [22, 20, 22] and rs[1][1] == b'\x01'
            messages = list(handshakes(rs[0][1])); assert [m[0] for m in messages] == [11, 16, 15]
            for kind, body, raw in messages:
                if kind == 11:
                    assert body[:8] == bytes.fromhex('0001900001900000') and body[8:] == self.host_cert
                elif kind == 16:
                    assert len(body) == 65
                    ephemeral = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), body)
                    secret = self.sensor.exchange(ec.ECDH(), ephemeral)
                    seed = self.client_random + self.server_random
                    self.master = prf(secret, b'master secret', seed, 48)
                    block = prf(self.master, b'key expansion', seed, 72)
                    self.client_key, self.server_key = block[:32], block[32:64]
                    self.client_salt, self.server_salt = block[64:68], block[68:72]
                else:
                    self.client.public_key().verify(body, self.transcript, ec.ECDSA(hashes.SHA256()))
                self.transcript += raw
            digest = hashlib.sha256(self.transcript).digest()
            expected = hs(20, prf(self.master, b'client finished', digest, 12))
            assert self.unseal(*rs[2]) == expected
            finished = hs(20, prf(self.master, b'server finished', digest, 12))
            if self.mode == 'wrong-finished': finished = finished[:-1] + bytes([finished[-1] ^ 1])
            response = record(20, b'\x01')
            if self.mode == 'fragmented': response += self.seal(22, finished[:7]) + self.seal(22, finished[7:])
            else: response += self.seal(22, finished)
            if self.mode == 'bad-tag': response = response[:-1] + bytes([response[-1] ^ 1])
            if self.mode == 'missing-ccs': response = response[6:]
            self.phase = 2
            return response
        assert len(rs) == 1
        kind, body = rs[0]
        plain = self.unseal(kind, body)
        if kind == 21:
            assert plain == b'\x01\x00' and self.phase == 4
            self.phase = 5
            return self.seal(21, plain)
        assert kind == 23
        if self.phase == 2:
            assert plain == b'\x01'
            response = bytearray(38)
            struct.pack_into('<I', response, 6, 3077710 if self.mode == 'version-mismatch' else 3077709)
            response[10:12] = b'\x0a\x01'; response[13] = 65
            response[24] = 1; response[25] = 0x20; response[37] = 3
            self.phase = 3
            self.last_firmware_record = self.seal(23, bytes(response))
            return self.last_firmware_record
        assert self.phase == 3 and plain == bytes.fromhex('820000000000000207')
        self.phase = 4
        if self.mode == 'replay': return self.last_firmware_record
        if self.mode == 'dimension-rejected': return self.seal(23, b'\x03\x04')
        response = bytes(14) + struct.pack('<9H', 16, 80, 12, 0, 80, 80, 4, 0, 80) + bytes(2)
        return self.seal(23, response)


def exact(stream, count):
    result = b''
    while len(result) < count:
        # Timeout protects the test suite against a stuck experimental client.
        with selectors.DefaultSelector() as selector:
            selector.register(stream, selectors.EVENT_READ)
            if not selector.select(10): raise TimeoutError('session bridge stopped responding')
        data = os.read(stream.fileno(), count - len(result))
        if not data:
            if not result: return b''
            raise EOFError('partial bridge frame')
        result += data
    return result


def run(bridge, root, mode, client, sensor, host_cert):
    before = {p.name: (p.read_bytes(), p.stat().st_mode) for p in (root / 'state').iterdir()}
    peer = Peer(mode, client, sensor, host_cert)
    process = subprocess.Popen([bridge, str(root / 'state'), str(root / 'authority.tsk')],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    try:
        while True:
            header = exact(process.stdout, 4)
            if not header: break
            size = int.from_bytes(header, 'big'); assert 0 < size <= 4096
            request = exact(process.stdout, size)
            response = peer.exchange(request)
            process.stdin.write(len(response).to_bytes(4, 'big') + response)
        rc = process.wait(timeout=10)
        logs = process.stderr.read().decode()
        success = mode in ('valid', 'fragmented', 'dimension-rejected', 'hello-marker')
        assert (rc == 0) == success, (mode, rc, logs)
        assert ('"tls_established_and_verified": true' in logs) == success, (mode, logs)
        if success: assert peer.phase == 5
        after = {p.name: (p.read_bytes(), p.stat().st_mode) for p in (root / 'state').iterdir()}
        assert before == after, 'session modified pairing state'
    finally:
        if process.poll() is None: process.kill(); process.wait()
        process.stdin.close(); process.stdout.close(); process.stderr.close()


def main():
    client = ec.derive_private_key(7, ec.SECP256R1())
    sensor = ec.derive_private_key(17, ec.SECP256R1())
    authority = ec.derive_private_key(19, ec.SECP256R1())
    host_cert = certificate(client, authority, 0)
    with tempfile.TemporaryDirectory(prefix='tudor-session-test-') as tmp:
        root = Path(tmp); state = root / 'state'; state.mkdir(mode=0o700)
        values = {'host-key.pem': client.private_bytes(serialization.Encoding.PEM,
                   serialization.PrivateFormat.PKCS8, serialization.NoEncryption()),
                  'host.cert': host_cert, 'sensor.cert': certificate(sensor, authority, 1),
                  'complete': b'synthetic completed pairing\n'}
        for name, data in values.items():
            path = state / name; path.write_bytes(data); path.chmod(0o600)
        point = authority.public_key().public_numbers()
        (root / 'authority.tsk').write_bytes(point.x.to_bytes(68, 'little') + point.y.to_bytes(68, 'little') + bytes(120))
        for mode in ('valid', 'fragmented', 'wrong-suite', 'truncated', 'wrong-finished',
                     'bad-tag', 'missing-ccs', 'version-mismatch', 'replay', 'dimension-rejected',
                     'hello-marker', 'wrong-version'):
            run(sys.argv[1], root, mode, client, sensor, host_cert)
        # Unsafe or mismatched local state must fail before communicating with USB.
        for mode in ('permissions', 'missing-marker', 'wrong-certificate', 'symlink'):
            if mode == 'permissions': (state / 'host-key.pem').chmod(0o644)
            elif mode == 'missing-marker': (state / 'complete').unlink()
            elif mode == 'wrong-certificate': (state / 'sensor.cert').write_bytes(host_cert[:-1])
            else:
                (state / 'host-key.pem').unlink()
                (state / 'host-key.pem').symlink_to(root / 'authority.tsk')
            p = subprocess.run([sys.argv[1], str(state), str(root / 'authority.tsk')], capture_output=True, timeout=10)
            assert p.returncode != 0 and not p.stdout, mode
            for name, data in values.items():
                path = state / name
                if path.is_symlink(): path.unlink()
                path.write_bytes(data); path.chmod(0o600)
    print('Native TLS/session tests passed: 12 independent peer scenarios and 4 pairing-state guards (no hardware).')


if __name__ == '__main__': main()
