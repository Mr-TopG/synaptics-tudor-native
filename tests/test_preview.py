#!/usr/bin/env python3
"""Validate provisional rendering against independent pixel mapping and bounds."""
import os
from pathlib import Path
import pty
import statistics
import struct
import subprocess
import sys
import tempfile

W, H = 104, 86
COUNT = W*H

def run(binary, directory, output=None):
    return subprocess.run([binary, '--preview', str(directory)], stdout=output or subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=5)

def snapshot(file):
    return file.read_bytes(), file.stat().st_mode, file.stat().st_mtime_ns

def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix='tudor-preview-test-') as tmp:
        directory = Path(tmp); file = directory/'frame-response.bin'
        # Signed gradient with asymmetric axes exposes transposition, byte order,
        # polarity, clipping, and scanline-direction errors.
        values = [x*7-y*3-400 for x in range(W) for y in range(H)]
        header = struct.pack('<5H', 0, 1, 0, 2, COUNT*2)
        good = header + struct.pack('<'+str(COUNT)+'h', *values)
        file.write_bytes(good); file.chmod(0o600)
        before = snapshot(file)
        process = run(binary, directory)
        assert process.returncode == 0, process.stderr
        data = process.stdout
        assert data[:2] == b'BM'
        length, _, _, offset = struct.unpack_from('<IHHI', data, 2)
        dib, width, height, planes, bits, compression, image_length = struct.unpack_from('<IiiHHII', data, 14)
        assert length == len(data) and offset == 54 and dib == 40
        assert (width, height, planes, bits, compression) == (W*8, H*4, 1, 24, 0)
        stride = (width*3+3)//4*4
        assert image_length == stride*height and len(data) == offset+image_length
        ordered = sorted(values); low = ordered[COUNT//100]; high = ordered[-(COUNT//100)-1]
        for y in range(height):
            for x in range(width):
                value = values[((x//4) % W)*H + y//4]
                gray = 0 if value <= low else 255 if value >= high else (value-low)*255//(high-low)
                if x >= W*4: gray = 255-gray
                at = offset + (height-1-y)*stride + x*3
                assert data[at:at+3] == bytes([gray])*3, (x,y)
        assert snapshot(file) == before
        assert b'offset=8 value=17888' in process.stderr
        # Binary biometric data must never be emitted to an interactive terminal.
        master, slave = pty.openpty()
        try:
            p = run(binary, directory, slave)
            assert p.returncode != 0 and b'output is binary' in p.stderr
        finally: os.close(master); os.close(slave)
        fixtures = {
            'wrong-length-field': good[:8] + struct.pack('<H', COUNT*2-2) + good[10:],
            'legacy-no-length': good[:8]+good[10:],
            'short': good[:-1],
            'trailing': good+b'\0',
            'finger-lifted': good[:2]+b'\3\0'+good[4:],
            'incomplete': good[:2]+b'\0\0'+good[4:],
            'constant': header+bytes(COUNT*2),
            'error-status': b'\xcb\x05'+good[2:],
        }
        for name, content in fixtures.items():
            file.write_bytes(content)
            before = snapshot(file)
            p = run(binary, directory)
            assert p.returncode != 0 and not p.stdout, (name, p.stderr)
            assert snapshot(file) == before
        file.write_bytes(good); file.chmod(0o644)
        p = run(binary, directory)
        assert p.returncode != 0 and not p.stdout
    print('Preview tests passed: every BMP pixel verified, 8 malformed/no-contrast fixtures rejected, terminal and permission guards, source unchanged.')

if __name__ == '__main__': main()
