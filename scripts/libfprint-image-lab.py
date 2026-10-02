#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Development-only, isolated libfprint feature extraction from our private BMP.

Uses the installed open-source virtual_image driver, never fprintd or real USB
capture. No minutia coordinates, images, templates, or enrollment state are saved.
"""
import json
import os
from pathlib import Path
import socket
import stat
import struct
import sys
import tempfile
import time

INPUT_WIDTH, INPUT_HEIGHT, SCALE = 104, 86, 4
WIDTH, HEIGHT = INPUT_WIDTH, INPUT_HEIGHT
BMP_WIDTH, BMP_HEIGHT = WIDTH*2*SCALE, HEIGHT*SCALE
BMP_SIZE = 54 + BMP_WIDTH*BMP_HEIGHT*3


def read_preview(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode) or st.st_uid != os.geteuid() or st.st_mode & 0o077 or st.st_nlink != 1 or st.st_size != BMP_SIZE:
            raise ValueError('Expected a private, owned BMP produced by preview-capture.sh')
        parts = bytearray()
        while len(parts) < BMP_SIZE:
            data = os.read(fd, BMP_SIZE-len(parts))
            if not data: raise ValueError('Truncated preview')
            parts.extend(data)
        if os.read(fd, 1): raise ValueError('Growing preview')
    finally:
        os.close(fd)
    data = parts
    if data[:2] != b'BM' or struct.unpack_from('<I',data,2)[0] != BMP_SIZE or struct.unpack_from('<I',data,10)[0] != 54:
        raise ValueError('Unexpected BMP framing')
    if struct.unpack_from('<IiiHHII',data,14) != (40,BMP_WIDTH,BMP_HEIGHT,1,24,0,BMP_SIZE-54):
        raise ValueError('Unexpected preview dimensions or format')
    pixels = bytearray(INPUT_WIDTH*INPUT_HEIGHT)
    stride = BMP_WIDTH*3
    for y in range(INPUT_HEIGHT):
        for x in range(INPUT_WIDTH):
            at = 54+(BMP_HEIGHT-1-y*SCALE)*stride+x*SCALE*3
            value = data[at]
            if data[at:at+3] != bytes([value])*3: raise ValueError('Preview is not grayscale')
            inverse = at + INPUT_WIDTH*SCALE*3
            if data[inverse:inverse+3] != bytes([255-value])*3: raise ValueError('Unexpected inverted panel')
            pixels[y*INPUT_WIDTH+x] = value
    return bytes(pixels)


def load_fprint():
    # Do not discover/open a physical driver through this process.
    os.environ['FP_DRIVERS_WHITELIST'] = 'virtual_image'
    import gi
    gi.require_version('FPrint', '2.0')
    from gi.repository import FPrint, Gio, GLib
    return FPrint, Gio, GLib


def measure(pixels, polarity, modules):
    FPrint, Gio, GLib = modules
    with tempfile.TemporaryDirectory(prefix='tudor-image-lab-') as directory:
        os.environ['FP_VIRTUAL_IMAGE'] = str(Path(directory)/'image.socket')
        context = FPrint.Context()
        devices = list(context.get_devices())
        if len(devices) != 1 or devices[0].get_driver() != 'virtual_image':
            raise RuntimeError('An isolated libfprint virtual_image driver is required')
        device = devices[0]
        device.open_sync(None)
        loop = GLib.MainLoop()
        cancel = Gio.Cancellable()
        result = {'polarity':polarity, 'width':WIDTH, 'height':HEIGHT,
                  'minutiae_detected':False, 'minutiae_count':0}
        done = False
        started = False
        def complete(dev, async_result, user_data):
            nonlocal done
            try:
                image = dev.capture_finish(async_result)
                count = len(image.get_minutiae() or [])
                result.update(minutiae_detected=count > 0, minutiae_count=count,
                              assumed_pixels_per_mm=image.get_ppmm())
            except GLib.Error as error:
                # Error codes are sufficient: never log templates or coordinates.
                result.update(error_domain=str(error.domain), error_code=error.code)
            finally:
                done = True
                loop.quit()
        def timeout():
            result['timed_out'] = True
            cancel.cancel()
            loop.quit()
            return GLib.SOURCE_REMOVE
        timer = GLib.timeout_add_seconds(15, timeout)
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(3)
                connection.connect(os.environ['FP_VIRTUAL_IMAGE'])
                device.capture(True, cancel, complete, None)
                started = True
                connection.sendall(struct.pack('=ii',WIDTH,HEIGHT)+pixels)
                loop.run()
        finally:
            if not result.get('timed_out'): GLib.source_remove(timer)
            if started and not done:
                cancel.cancel()
                deadline = time.monotonic() + 2
                while not done and time.monotonic() < deadline:
                    GLib.MainContext.default().iteration(False)
                    time.sleep(0.01)
                if not done: raise RuntimeError('Virtual capture cancellation did not finish')
            device.close_sync(None)
        return result


def main():
    if len(sys.argv) != 2:
        raise SystemExit('Usage: libfprint-image-lab.py PRIVATE_PREVIEW_BMP (normal user, no sudo)')
    os.umask(0o077)
    pixels = read_preview(sys.argv[1])
    modules = load_fprint()
    results = [measure(pixels, 'normal', modules),
               measure(bytes(255-value for value in pixels), 'inverted', modules)]
    print(json.dumps({'libfprint_image_lab':True, 'source':'private provisional preview',
          'physical_device_opened':False, 'templates_saved':False,
          'matching_tested':False, 'authentication_supported':False, 'results':results}))


if __name__ == '__main__':
    try: main()
    except (ImportError, ValueError, OSError, RuntimeError) as error:
        print(f'Image lab stopped: {error}', file=sys.stderr)
        raise SystemExit(1)
