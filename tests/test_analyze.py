#!/usr/bin/env python3
"""Offline analyzer regressions with synthetic pixels and private-file guards."""
import json
import math
import os
from pathlib import Path
import statistics
import struct
import subprocess
import sys
import tempfile

WIDTH, HEIGHT = 104, 86
COUNT = WIDTH * HEIGHT

def invoke(binary, directory):
    return subprocess.run([binary, str(directory)], capture_output=True, text=True, timeout=5)

def snapshot(directory):
    return {p.name: (p.read_bytes(), p.stat().st_mode, p.stat().st_mtime_ns) for p in directory.iterdir()}

def check(binary, root, name, response):
    directory = root / name; directory.mkdir(mode=0o700)
    file = directory / 'frame-response.bin'; file.write_bytes(response); file.chmod(0o600)
    before = snapshot(directory)
    process = invoke(binary, directory)
    assert process.returncode == 0, process.stderr
    result = json.loads(process.stdout, parse_constant=lambda x: (_ for _ in ()).throw(AssertionError(x)))
    assert not result['layout_verified'] and result['response_bytes'] == len(response)
    assert len(result['candidates']) == (8 if len(response) == 17898 else 4)
    for candidate in result['candidates']:
        offset = candidate['pixel_offset_candidate']
        kind = candidate['encoding_candidate']
        code = ('>' if kind.endswith('be') else '<') + str(COUNT) + ('h' if kind.startswith('s') else 'H')
        values = struct.unpack(code, response[offset:offset+COUNT*2])
        assert candidate['min'] == min(values) and candidate['max'] == max(values)
        assert abs(candidate['mean'] - statistics.mean(values)) <= 0.00051
        assert abs(candidate['stddev'] - statistics.pstdev(values)) <= 0.00051
        assert candidate['distinct_samples'] == len(set(values))
        assert candidate['zero_samples'] == values.count(0)
        for stride, label in ((1, 'within_column'), (HEIGHT, 'across_columns')):
            pairs = [(v, values[i+stride]) for i, v in enumerate(values)
                     if (i % HEIGHT != HEIGHT-1 if stride == 1 else i+stride < COUNT)]
            delta = sum(abs(a-b) for a, b in pairs) / len(pairs)
            assert abs(candidate[label+'_mean_abs_delta'] - delta) <= 0.00051
            a, b = zip(*pairs)
            if len(set(a)) == 1 or len(set(b)) == 1:
                assert candidate[label+'_correlation'] is None
            else:
                assert math.isclose(candidate[label+'_correlation'], statistics.correlation(a,b), abs_tol=0.000001)
    assert snapshot(directory) == before, 'offline analysis changed capture files'

def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix='tudor-analysis-') as temp:
        root = Path(temp)
        header = struct.pack('<4H', 0, 1, 0, 2)
        pixels = struct.pack('<'+str(COUNT)+'H', *[(x*273+y*17) % 65536 for x in range(WIDTH) for y in range(HEIGHT)])
        check(binary, root, 'reference', header+pixels)
        check(binary, root, 'extra_header', header+b'\xa5\x5a'+pixels)
        check(binary, root, 'extra_trailer', header+pixels+b'\xa5\x5a')
        check(binary, root, 'constant', header+bytes(COUNT*2))
        signed_pixels = struct.pack('>'+str(COUNT)+'h', *[(-30000+x*300+y*11) for x in range(WIDTH) for y in range(HEIGHT)])
        check(binary, root, 'signed_big_endian', header+signed_pixels)
        directory = root/'guards'; directory.mkdir(mode=0o700)
        file = directory/'frame-response.bin'
        valid = header + pixels + bytes(2)
        modes = ('short', 'long', 'bad-status', 'file-mode', 'directory-mode', 'symlink', 'hardlink', 'fifo', 'directory-symlink')
        for mode in modes:
            file.write_bytes(valid); file.chmod(0o600)
            target = directory
            if mode == 'short': file.write_bytes(valid[:-3])
            elif mode == 'long': file.write_bytes(valid+bytes(1))
            elif mode == 'bad-status': file.write_bytes(b'\xcb\x05'+valid[2:])
            elif mode == 'file-mode': file.chmod(0o644)
            elif mode == 'directory-mode': directory.chmod(0o755)
            elif mode == 'symlink':
                other = root/'link-target'; other.write_bytes(valid); other.chmod(0o600)
                file.unlink(); file.symlink_to(other)
            elif mode == 'hardlink': os.link(file, root/'hardlink-target')
            elif mode == 'fifo': file.unlink(); os.mkfifo(file, 0o600)
            elif mode == 'directory-symlink':
                target = root/'directory-link'; target.symlink_to(directory, target_is_directory=True)
            process = invoke(binary, target)
            assert process.returncode != 0 and not process.stdout, (mode, process.stdout, process.stderr)
            directory.chmod(0o700); file.unlink()
    print('Offline analyzer tests passed: 5 synthetic layouts/statistics fixtures and 9 file guards; captures unchanged.')

if __name__ == '__main__': main()
