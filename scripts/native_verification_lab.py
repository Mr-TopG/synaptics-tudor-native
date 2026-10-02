#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline experimental C quality/decision policy; never authorizes login."""
import argparse
import ctypes
import json
from pathlib import Path

from enrollment_lab import ENROLL, load_samples, match

PIXELS = 104 * 86
LIBRARY = Path(__file__).resolve().parent.parent / 'build/libtudor-verification.so'
DECISIONS = ('retry', 'candidate_match', 'candidate_nonmatch')
REASONS = ('none', 'invalid_input', 'incomplete_enrollment', 'image_quality',
           'unusable_comparison', 'uncertain_score')
FLAGS = ((1, 'low_contrast'), (2, 'low_coverage'), (4, 'low_structure'),
         (8, 'directional'), (16, 'clipped'), (32, 'invalid'))


class Quality(ctypes.Structure):
    _fields_ = [('flags', ctypes.c_uint)] + [(name, ctypes.c_double) for name in
        ('standard_deviation', 'active_fraction', 'local_coherence',
         'global_coherence', 'clipped_fraction')]

    def summary(self):
        return {'flags': [name for flag, name in FLAGS if self.flags & flag],
                **{name: round(getattr(self, name), 6) for name, _ in self._fields_[1:]}}


class Result(ctypes.Structure):
    _fields_ = [('policy_version', ctypes.c_uint), ('decision', ctypes.c_int),
                ('retry_reason', ctypes.c_int), ('best_correlation', ctypes.c_double),
                ('quality', Quality)]


class NativeVerifier:
    def __init__(self, path=LIBRARY):
        self.library = lib = ctypes.CDLL(str(Path(path).resolve()))
        image = ctypes.POINTER(ctypes.c_uint8)
        lib.tudor_verifier_create.argtypes = []
        lib.tudor_verifier_create.restype = ctypes.c_void_p
        lib.tudor_verifier_free.argtypes = [ctypes.c_void_p]
        lib.tudor_verifier_free.restype = None
        lib.tudor_verifier_enroll.argtypes = [ctypes.c_void_p, image, ctypes.c_size_t,
                                             ctypes.POINTER(Quality)]
        lib.tudor_verifier_enroll.restype = ctypes.c_int
        lib.tudor_verifier_check.argtypes = [ctypes.c_void_p, image, ctypes.c_size_t,
                                            ctypes.POINTER(Result)]
        lib.tudor_verifier_check.restype = ctypes.c_int
        self.handle = lib.tudor_verifier_create()
        if not self.handle:
            raise RuntimeError('Native verifier allocation failed')

    def close(self):
        if self.handle:
            self.library.tudor_verifier_free(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def _call(self, function, image, result):
        if not self.handle or len(image) != PIXELS:
            raise ValueError('Expected a 104x86 grayscale image and open verifier')
        data = (ctypes.c_uint8 * PIXELS).from_buffer_copy(image)
        try:
            return function(self.handle, data, PIXELS, ctypes.byref(result))
        finally:
            ctypes.memset(data, 0, PIXELS)

    def enroll(self, image):
        quality = Quality()
        status = self._call(self.library.tudor_verifier_enroll, image, quality)
        if status:
            raise ValueError(f'Enrollment rejected: status={status}, quality={quality.summary()}')
        return quality.summary()

    def check(self, image):
        result = Result()
        status = self._call(self.library.tudor_verifier_check, image, result)
        if status:
            raise ValueError('Native verification rejected invalid input')
        if result.policy_version != 1 or result.decision not in range(3) or result.retry_reason not in range(6):
            raise RuntimeError('Unexpected native verification result')
        return {'experimental_policy_version': result.policy_version,
                'authentication_supported': False, 'decision': DECISIONS[result.decision],
                'retry_reason': REASONS[result.retry_reason],
                'best_correlation': round(result.best_correlation, 6) if result.best_correlation >= -1 else None,
                'quality': result.quality.summary()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument('--probe', type=Path)
    operation.add_argument('--validate-enrollment', action='store_true')
    parser.add_argument('--label', choices=('same', 'different', 'unspecified'), default='unspecified')
    args = parser.parse_args()
    if args.label != 'unspecified' and args.probe is None:
        parser.error('--label requires --probe')
    images = load_samples(args.directory, ENROLL)
    with NativeVerifier() as verifier:
        quality = []
        for name in ENROLL:
            try:
                quality.append(verifier.enroll(images[name]))
            except ValueError as error:
                raise ValueError(f'{name}: {error}') from error
        if args.validate_enrollment:
            output = {'experimental_policy_version': 1, 'authentication_supported': False,
                      'accepted_enrollment_samples': 10, 'quality': quality}
        else:
            probe = match.lab.read_preview(args.probe)
            if probe in images.values():
                raise ValueError('Exact enrollment replay; use an independent probe capture')
            output = verifier.check(probe)
            output['actual_label'] = args.label
        print(json.dumps(output, allow_nan=False), flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        raise SystemExit(f'Experimental verifier stopped: {error}')
