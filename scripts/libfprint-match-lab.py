#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Isolated development comparison. Templates exist in memory only.

The reference image is repeated to satisfy the virtual driver's enrollment
stages. This is a comparison harness, not a real enrollment procedure or a
security evaluation. Matching thresholds are never modified.
"""
import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import time
from image_processing import local_contrast, enlarge

spec = importlib.util.spec_from_file_location('image_lab', Path(__file__).with_name('libfprint-image-lab.py'))
lab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lab)


class Session:
    def __init__(self, modules, width=104, height=86):
        self.width, self.height = width, height
        self.FPrint, self.Gio, self.GLib = modules
        self.directory = tempfile.TemporaryDirectory(prefix='tudor-match-lab-')
        self.device = self.connection = None
        try:
            os.environ['FP_VIRTUAL_IMAGE'] = str(Path(self.directory.name)/'image.socket')
            self.context = self.FPrint.Context()
            devices = list(self.context.get_devices())
            if len(devices) != 1 or devices[0].get_driver() != 'virtual_image':
                raise RuntimeError('Exactly one isolated virtual_image device is required')
            self.device = devices[0]
            self.device.open_sync(None)
            self.connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.connection.settimeout(3)
            self.connection.connect(os.environ['FP_VIRTUAL_IMAGE'])
        except Exception:
            self.close()
            raise

    def close(self):
        if self.connection:
            self.connection.close(); self.connection = None
        if self.device and self.device.is_open(): self.device.close_sync(None)
        self.device = None
        self.directory.cleanup()

    def operate(self, pixels, reference=None, enrollment_images=None):
        GLib, Gio, FPrint = self.GLib, self.Gio, self.FPrint
        loop, cancel = GLib.MainLoop(), Gio.Cancellable()
        state = {'done':False, 'timed_out':False, 'images_sent':0, 'minutiae_counts':[]}
        expected = self.device.get_nr_enroll_stages() if reference is None else 1
        if not 1 <= expected <= 20: raise RuntimeError('Unexpected virtual enrollment stage count')
        if enrollment_images is not None:
            if reference is not None or len(enrollment_images) != expected:
                raise ValueError('Independent enrollment must supply exactly one image per stage')
            if any(len(data) != self.width*self.height for data in enrollment_images):
                raise ValueError('Invalid enrollment image dimensions')
        elif len(pixels) != self.width*self.height:
            raise ValueError('Invalid image dimensions')
        def send():
            if state['done'] or cancel.is_cancelled(): return GLib.SOURCE_REMOVE
            try:
                if state['images_sent'] >= expected: raise RuntimeError('Unexpected extra image request')
                current = enrollment_images[state['images_sent']] if enrollment_images is not None else pixels
                self.connection.sendall(struct.pack('=ii',self.width,self.height)+current)
                state['images_sent'] += 1
            except (OSError, RuntimeError) as error:
                state['harness_error'] = str(error); cancel.cancel()
            return GLib.SOURCE_REMOVE
        def progress(dev, stage, print_, *rest):
            error = next((value for value in rest if isinstance(value, GLib.Error)), None)
            if error is not None:
                state['retry_domain'], state['retry_code'] = str(error.domain), error.code
                cancel.cancel(); return
            if print_ and print_.get_image():
                state['minutiae_counts'].append(len(print_.get_image().get_minutiae() or []))
            if stage < expected: GLib.idle_add(send)
        def complete(dev, result, *user_data):
            try:
                if reference is None:
                    state['reference'] = dev.enroll_finish(result)
                else:
                    matched, print_ = dev.verify_finish(result)
                    state['matched'] = bool(matched)
                    if print_ and print_.get_image():
                        state['minutiae_counts'].append(len(print_.get_image().get_minutiae() or []))
            except GLib.Error as error:
                state['error_domain'], state['error_code'] = str(error.domain), error.code
            finally:
                state['done'] = True; loop.quit()
        def timeout():
            state['timed_out'] = True; cancel.cancel(); loop.quit()
            return GLib.SOURCE_REMOVE
        timer = GLib.timeout_add_seconds(20, timeout)
        try:
            if reference is None:
                # The notified progress callback takes an explicit userdata tuple in PyGI.
                self.device.enroll(FPrint.Print.new(self.device), cancel, progress, (), complete)
            else:
                self.device.verify(reference, cancel, None, (), complete)
            GLib.idle_add(send)
            loop.run()
        finally:
            if not state['timed_out']: GLib.source_remove(timer)
            if not state['done']:
                cancel.cancel()
                deadline = time.monotonic()+2
                while not state['done'] and time.monotonic() < deadline:
                    GLib.MainContext.default().iteration(False); time.sleep(0.01)
                if not state['done']: raise RuntimeError('Virtual operation did not cancel')
        return state


def compare(images, polarity, modules, width=104, height=86):
    if not 1 <= width <= 1024 or not 1 <= height <= 1024 or any(len(data) != width*height for data in images.values()):
        raise ValueError('Invalid comparison image dimensions')
    session = Session(modules, width, height)
    try:
        convert = lambda data: data if polarity == 'normal' else bytes(255-v for v in data)
        result = {'polarity':polarity, 'reference_images_are_replays':True,
                  'matching_threshold':'unmodified virtual-driver default'}
        enrollment = session.operate(convert(images['same-1']))
        reference = enrollment.pop('reference', None)
        result['reference_creation'] = enrollment
        if reference is None: return result
        result['comparisons'] = {}
        for label in ('same-1', 'same-2', 'different-1'):
            result['comparisons'][label] = session.operate(convert(images[label]), reference)
        reference = None
        return result
    finally:
        session.close()


def main():
    explore = len(sys.argv) == 3 and sys.argv[1] == '--explore'
    if len(sys.argv) != 2 and not explore:
        raise SystemExit('Usage: libfprint-match-lab.py [--explore] PRIVATE_SAMPLE_DIRECTORY')
    os.umask(0o077)
    directory = Path(sys.argv[2 if explore else 1])
    images = {name:lab.read_preview(directory/(name+'.bmp')) for name in ('same-1','same-2','different-1')}
    modules = lab.load_fprint()
    presets = ((0,1),(0,2),(0,3),(7,1),(7,2),(15,1),(15,2)) if explore else ((0,1),)
    for radius, scale in presets:
        transformed = {name:enlarge(local_contrast(pixels,104,86,radius) if radius else pixels,104,86,scale)
                       for name,pixels in images.items()}
        result = {'development_comparison_only':True, 'physical_device_opened':False,
                  'templates_saved':False, 'system_enrollment':False,
                  'authentication_supported':False, 'contrast_radius':radius, 'bilinear_scale':scale,
                  'results':[compare(transformed, polarity, modules, width=104*scale, height=86*scale)
                             for polarity in ('normal','inverted')]}
        print(json.dumps(result), flush=True)


if __name__ == '__main__':
    try: main()
    except (ImportError, ValueError, RuntimeError, OSError) as error:
        print(f'Match lab stopped: {error}', file=sys.stderr)
        raise SystemExit(1)
