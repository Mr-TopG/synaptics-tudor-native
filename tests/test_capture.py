#!/usr/bin/env python3
"""Independent encrypted capture peer; only synthetic pixels and pairing keys."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from test_session import Peer, certificate, exact, records, ec, serialization

TLS_FAILURES = ('bad-frame-tag', 'truncated-record', 'unexpected-record')
MALFORMED = ('extra-trailer', 'extra-header', 'extra-fragmented', 'extra-unresolved-flags', 'wrong-length', 'legacy-header')
PIXELS = b''.join(struct.pack('<H', i) for i in range(104 * 86))

class CapturePeer(Peer):
    def __init__(self, mode, *args):
        super().__init__('hello-marker', *args)
        self.capture_mode = mode
        self.commands = []
        self.closed = False

    def exchange(self, request):
        if self.phase < 2:
            return super().exchange(request)
        rs = records(request)
        assert len(rs) == 1
        kind, body = rs[0]
        plain = self.unseal(kind, body)
        if kind == 21:
            self.closed = plain == b'\x01\x00'
            assert self.closed or self.capture_mode in TLS_FAILURES
            return self.seal(21, plain) if self.closed else b''
        assert kind == 23 and not self.closed
        self.commands.append(plain[0])
        mode = self.capture_mode
        if plain[0] == 0x82:
            assert plain == bytes.fromhex('820000000000000207')
            response = bytes(14) + struct.pack('<9H', 16, 103 if mode == 'wrong-dimensions' else 104, 0, 0, 104, 86, 0, 0, 86) + bytes(2)
        elif plain[0] == 0x86:
            mask=int.from_bytes(plain[1:5],'little')
            if mode.startswith('contact-') and mask:
                assert mask in (4,6) and plain==b'\x86'+struct.pack('<8I',*[mask]*8)+bytes(4)
                self.contact_mask=mask; self.contact_reads=0
                self.contact_sequence=65535 if mask==4 else 12
                response=bytes(64)+struct.pack('<H',self.contact_sequence)
                if mode=='contact-config-rejected': response=b'\x03\x04'
            else:
                assert plain == b'\x86' + bytes(32) + b'\x04\0\0\0'
                response = bytes(66)
            if mode == 'clear-rejected' and self.commands.count(0x86) == 2:
                response = b'\x03\x04'
        elif plain[0] == 0x87 and mode.startswith('contact-'):
            sequence,requested=struct.unpack('<HH',plain[1:5])
            assert sequence==self.contact_sequence and requested==32
            assert len(plain) in (5,9)
            if len(plain)==9: assert plain[5:]==b'\x01\0\0\0'
            if mode=='contact-legacy' and len(plain)==9:
                return self.seal(23,b'\x05\x04')
            self.contact_reads+=1
            if self.contact_mask==4:
                events=[] if mode=='contact-clear-timeout' else [2]
            else:
                events=[1] if self.contact_reads==1 else []
                if mode=='contact-touch-timeout': events=[]
                if mode=='contact-bounce' and self.contact_reads==2: events=[2,1]
            pending=0
            if mode=='contact-backlog' and self.contact_mask==6 and self.contact_reads==1: pending=1
            if mode=='contact-storm': events=[24]; pending=1
            self.contact_sequence=(sequence+len(events))&65535
            response=struct.pack('<HHH',0,len(events),pending+(len(events) if len(plain)==5 else 0))
            response+=b''.join(bytes([event])+bytes(11) for event in events)
            if mode=='contact-short': response=response[:-1]
            if mode=='contact-count': response=struct.pack('<HHH',0,33,0)
            if mode=='contact-empty-pending': response=struct.pack('<HHH',0,0,1)
            if mode=='contact-read-rejected': response=b'\x03\x04'
        elif plain[0] == 0x80:
            assert plain == bytes.fromhex('8000000000010000000100000801010100')
            response = b'\xcb\x05' if mode == 'acquire-busy' else b'\x03\x04' if mode == 'acquire-rejected' else bytes(2)
        elif plain[0] == 0x7f:
            assert plain == bytes.fromhex('7f00000000ffff0300')
            flags = 3 if mode == 'finger-lifted' else 0 if mode == 'incomplete' else 1
            response = struct.pack('<5H', 0, flags, 0, 2, len(PIXELS)) + PIXELS
            if mode == 'short-frame': response = response[:-2]
            if mode in ('extra-trailer', 'extra-fragmented'): response += b'\xa5\x5a'
            if mode == 'extra-header': response = response[:8] + b'\xa5\x5a' + response[8:]
            if mode == 'extra-unresolved-flags': response = bytes(10) + PIXELS
            if mode == 'oversized-frame': response += bytes(4)
            if mode == 'wrong-length': response = response[:8] + b'\x00\x00' + response[10:]
            if mode == 'legacy-header': response = response[:8] + response[10:]
            self.frame_response = response
            if mode in ('fragmented-frame', 'extra-fragmented'):
                return self.seal(23, response[:10000]) + self.seal(23, response[10000:])
        elif plain[0] == 0x81:
            assert plain == b'\x81'
            response = b'\x03\x04' if mode == 'finish-rejected' else bytes(2)
        else:
            raise AssertionError(('unexpected command', plain.hex()))
        wire = self.seal(23, response)
        if mode == 'bad-frame-tag' and plain[0] == 0x7f:
            wire = wire[:-1] + bytes([wire[-1] ^ 1])
        if mode == 'truncated-record' and plain[0] == 0x7f: wire = wire[:-1]
        if mode == 'unexpected-record' and plain[0] == 0x7f: wire = bytes([22]) + wire[1:]
        return wire


def run(bridge, root, mode, client, sensor, host_cert, memory=False):
    state = root / 'state'
    before = {p.name: (p.read_bytes(), p.stat().st_mode) for p in state.iterdir()}
    output = root / (('memory-' if memory else '') + mode); output.mkdir(mode=0o700)
    peer = CapturePeer(mode, client, sensor, host_cert)
    env = dict(os.environ, TUDOR_TEST_INTERRUPT='cancel' if mode == 'cancel' else 'error' if mode == 'interrupt-error' else '')
    process = subprocess.Popen([bridge] + (['--settled'] if mode.startswith('settled-') else []) + (['--memory'] if memory else []) + [str(state), str(root / 'authority.tsk')] + ([] if memory else [str(output)]),
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0, env=env)
    try:
        while True:
            header = exact(process.stdout, 4)
            if not header: break
            size = int.from_bytes(header, 'big'); assert 0 < size <= 4096
            request = exact(process.stdout, size)
            response = peer.exchange(request)
            confirmation = (b'\n' if mode == 'settled-valid' else b'q') if mode.startswith('settled-') and peer.commands == [0x82] else b''
            process.stdin.write(len(response).to_bytes(4, 'big') + response + confirmation)
        rc = process.wait(timeout=10)
        logs = process.stderr.read().decode()
        success = mode in ('valid', 'fragmented-frame', 'settled-valid')
        assert (rc == 0) == success, (mode, rc, logs)
        marker = '"image_decoded_in_memory":true' if memory else '"raw_frame_received":true'
        assert (marker in logs) == success, (mode, logs)
        if memory and success:
            assert '"image_files_written":false' in logs and '"session_closed":true' in logs
            assert '"contrast_low":89,"contrast_high":8854' in logs
        if mode in TLS_FAILURES:
            stage = {'bad-frame-tag': 'record authentication/bounds', 'truncated-record': 'record framing/version',
                     'unexpected-record': 'unexpected record type', 'oversized-frame': 'application response capacity'}[mode]
            assert f'stage={stage}' in logs and 'FRAME_READ: encrypted response rejected.' in logs, logs
        paths = list(output.iterdir())
        assert len(paths) == int(success and not memory), (mode, paths)
        if success and not memory:
            directory = paths[0]
            assert directory.stat().st_mode & 0o777 == 0o700
            assert {p.name for p in directory.iterdir()} == {'frame.raw', 'frame-response.bin', 'metadata.json'}
            assert all(p.stat().st_mode & 0o777 == 0o600 for p in directory.iterdir())
            assert (directory / 'frame-response.bin').read_bytes() == peer.frame_response
            summary = json.loads((directory / 'metadata.json').read_text())
            assert summary['response_bytes'] == 17898 and summary['pixel_offset'] == 10
            assert summary['length_field_verified'] and summary['frame_index'] == 2
            assert (directory / 'frame.raw').read_bytes() == PIXELS
            assert summary['raw_bytes'] == len(PIXELS) and summary['s16le_max'] == 8943
            assert summary['s16le_mean'] == 4471.5
            assert not summary['image_decoding_validated']
        expected = [0x82]
        if mode not in ('wrong-dimensions', 'settled-cancel'):
            expected += [0x86, 0x80]
            if mode not in ('cancel', 'interrupt-error', 'acquire-rejected', 'acquire-busy'): expected += [0x7f]
            if mode not in TLS_FAILURES: expected += [0x81, 0x86]
        assert peer.commands == expected, (mode, peer.commands)
        if mode not in TLS_FAILURES and mode not in ('wrong-dimensions', 'settled-cancel'):
            assert 'FRAME_FINISH acknowledged' in logs or mode == 'finish-rejected', logs
        assert peer.closed == (mode not in TLS_FAILURES), (mode, logs)
        assert before == {p.name: (p.read_bytes(), p.stat().st_mode) for p in state.iterdir()}
    finally:
        if process.poll() is None: process.kill(); process.wait()
        process.stdin.close(); process.stdout.close(); process.stderr.close()


def main():
    client = ec.derive_private_key(7, ec.SECP256R1())
    sensor = ec.derive_private_key(17, ec.SECP256R1())
    authority = ec.derive_private_key(19, ec.SECP256R1())
    host_cert = certificate(client, authority, 0)
    with tempfile.TemporaryDirectory(prefix='tudor-capture-test-') as tmp:
        root = Path(tmp); state = root / 'state'; state.mkdir(mode=0o700)
        values = {'host-key.pem': client.private_bytes(serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8, serialization.NoEncryption()),
            'host.cert': host_cert, 'sensor.cert': certificate(sensor, authority, 1), 'complete': b'test\n'}
        for name, data in values.items():
            path = state / name; path.write_bytes(data); path.chmod(0o600)
        point = authority.public_key().public_numbers()
        (root / 'authority.tsk').write_bytes(point.x.to_bytes(68, 'little') + point.y.to_bytes(68, 'little') + bytes(120))
        modes = ('valid', 'fragmented-frame', 'wrong-dimensions', 'acquire-rejected',
            'cancel', 'interrupt-error', 'finger-lifted', 'incomplete', 'short-frame', 'bad-frame-tag', 'finish-rejected', 'truncated-record', 'unexpected-record', 'oversized-frame', 'acquire-busy', 'settled-valid', 'settled-cancel') + MALFORMED
        for mode in modes: run(sys.argv[1], root, mode, client, sensor, host_cert)
        memory_modes=('valid','fragmented-frame','settled-valid','settled-cancel','cancel','bad-frame-tag','finish-rejected')
        for mode in memory_modes: run(sys.argv[1],root,mode,client,sensor,host_cert,memory=True)
    print(f'Native capture tests passed: {len(modes)} file-mode and {len(memory_modes)} memory-mode independent peer scenarios (synthetic pixels, no hardware).')

if __name__ == '__main__': main()
