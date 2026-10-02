#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare two verification libraries with synthetic inputs, never hardware/data files.

The baseline must be a frozen pre-optimization build. Timings exclude image
generation, loading, device capture, TLS, contact settling, and service overhead.
"""
import argparse
import ctypes as C
import hashlib
import json
import math
from pathlib import Path
import platform
import random
import statistics
import struct
import sys
import time

WIDTH, HEIGHT, SCANS = 104, 86, 10
PIXELS = WIDTH * HEIGHT
Image = C.c_uint8 * PIXELS


class Score(C.Structure):
    _fields_ = [('correlation', C.c_double), ('overlap_fraction', C.c_double)]


class Quality(C.Structure):
    _fields_ = [('flags', C.c_uint)] + [(name, C.c_double) for name in
        ('standard_deviation', 'active_fraction', 'local_coherence',
         'global_coherence', 'clipped_fraction')]


class Result(C.Structure):
    _fields_ = [('policy_version', C.c_uint), ('decision', C.c_int),
               ('retry_reason', C.c_int), ('best_correlation', C.c_double),
               ('quality', Quality)]


def snapshot(value):
    return {name: snapshot(getattr(value, name)) if kind is Quality
            else getattr(value, name) for name, kind in value._fields_}


def exact(value):
    """Compare actual double bits, including signed zero; ignore C padding."""
    if isinstance(value, float):
        return ('double', struct.pack('=d', value).hex())
    if isinstance(value, dict):
        return {key: exact(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [exact(item) for item in value]
    return value


def require_equal(label, baseline, candidate):
    if exact(baseline) != exact(candidate):
        raise RuntimeError(f'Semantic difference in {label}: '
                           f'baseline={baseline!r}; candidate={candidate!r}')


class Library:
    def __init__(self, path):
        self.path = path.resolve(strict=True)
        self.lib = C.CDLL(str(self.path))
        image = C.POINTER(C.c_uint8)
        signatures = {
            'tudor_image_compare': ([image, image, C.POINTER(Score)], C.c_int),
            'tudor_image_assess': ([image, C.c_size_t, C.POINTER(Quality)], C.c_int),
            'tudor_image_bank_create': ([], C.c_void_p),
            'tudor_image_bank_free': ([C.c_void_p], None),
            'tudor_image_bank_add': ([C.c_void_p, image, C.c_size_t], C.c_int),
            'tudor_image_bank_count': ([C.c_void_p], C.c_uint),
            'tudor_verifier_create': ([], C.c_void_p),
            'tudor_verifier_free': ([C.c_void_p], None),
            'tudor_verifier_count': ([C.c_void_p], C.c_uint),
            'tudor_verifier_enroll': ([C.c_void_p, image, C.c_size_t,
                                      C.POINTER(Quality)], C.c_int),
            'tudor_verifier_check': ([C.c_void_p, image, C.c_size_t,
                                     C.POINTER(Result)], C.c_int),
        }
        self.has_self_check = hasattr(self.lib, 'tudor_image_self_check')
        if self.has_self_check:
            signatures['tudor_image_self_check'] = ([image], C.c_int)
        for name, (arguments, result) in signatures.items():
            function = getattr(self.lib, name)
            function.argtypes, function.restype = arguments, result

    def compare(self, first, second):
        result = Score(17, 19)
        status = self.lib.tudor_image_compare(first, second, C.byref(result))
        return {'status': status, **snapshot(result)}

    def assess(self, image):
        quality = Quality()
        status = self.lib.tudor_image_assess(image, PIXELS, C.byref(quality))
        return {'status': status, **snapshot(quality)}

    def acceptance(self, image):
        bank = self.lib.tudor_image_bank_create()
        if not bank:
            raise MemoryError('Synthetic bank allocation failed')
        try:
            first = self.lib.tudor_image_bank_add(bank, image, PIXELS)
            second = self.lib.tudor_image_bank_add(bank, image, PIXELS)
            return {'first_status': first, 'second_status': second,
                    'count': self.lib.tudor_image_bank_count(bank)}
        finally:
            self.lib.tudor_image_bank_free(bank)


def image_from(function):
    return Image(*(max(0, min(255, int(function(x, y))))
                   for y in range(HEIGHT) for x in range(WIDTH)))


def ridge(phase=0, different=False, amplitude=60):
    return image_from(lambda x, y: 128 + amplitude * math.sin(
        (0.91 * x + 0.011 * y * y if different else 0.55 * x + 0.006 * y * y)
        + phase))


def corpus():
    images = {f'constant-{value}': Image(*([value] * PIXELS))
              for value in (0, 1, 127, 128, 254, 255)}
    coordinates = ((0, 0), (1, 1), (103, 0), (0, 85), (103, 85),
                   (52, 43), (51, 42), (52, 0), (0, 43))
    for x, y in coordinates:
        for amplitude in (1, 127):
            image = Image(*([128] * PIXELS))
            image[y * WIDTH + x] += amplitude
            images[f'impulse-{x}-{y}-{amplitude}'] = image
    for axis in ('x', 'y'):
        images[f'ramp-{axis}'] = image_from(
            lambda x, y: (x * 2 if axis == 'x' else y * 2))
        images[f'line-{axis}'] = image_from(
            lambda x, y: 255 if (x == 51 if axis == 'x' else y == 42) else 128)
    images['checkerboard'] = image_from(lambda x, y: 255 * ((x + y) % 2))
    images['checkerboard-low'] = image_from(lambda x, y: 127 + (x + y) % 2)
    images['ridge'] = ridge()
    images['ridge-low'] = ridge(amplitude=1)
    images['ridge-other'] = ridge(different=True)
    images['straight-ridge'] = image_from(lambda x, y: 128 + 60 * math.sin(0.55 * x))
    generator = random.Random(18317)
    images['noise'] = Image(*(generator.randrange(256) for _ in range(PIXELS)))
    images['noise-low'] = Image(*(127 + generator.randrange(2) for _ in range(PIXELS)))
    for count in (2, 16, 128):
        image = Image(*([128] * PIXELS))
        for index in generator.sample(range(PIXELS), count):
            image[index] = generator.choice((0, 127, 129, 255))
        images[f'sparse-{count}'] = image
    source = images['ridge']
    images['ridge-moved'] = image_from(
        lambda x, y: source[(y - 5) * WIDTH + x - 8] if x >= 8 and y >= 5 else 128)
    images['ridge-inverted'] = Image(*(255 - value for value in source))
    return images


def equivalent(baseline, candidate):
    images = corpus()
    report = []
    print(f'Checking {len(images)} synthetic images against the frozen baseline.',
          file=sys.stderr, flush=True)
    for name, image in images.items():
        unchanged = bytes(image)
        old = baseline.compare(image, image)
        new = candidate.compare(image, image)
        require_equal(f'{name} full self score', old, new)
        if candidate.has_self_check:
            status = candidate.lib.tudor_image_self_check(image)
            require_equal(f'{name} self-check return', old['status'], status)
        old_quality = baseline.assess(image)
        require_equal(f'{name} quality', old_quality, candidate.assess(image))
        old_acceptance = baseline.acceptance(image)
        require_equal(f'{name} bank acceptance', old_acceptance, candidate.acceptance(image))
        require_equal(f'{name} input immutability', unchanged, bytes(image))
        report.append({'case': name, 'self_status': old['status'],
                       'quality_flags': old_quality['flags'], **old_acceptance})
    pairs = [('ridge', 'ridge-moved'), ('ridge-moved', 'ridge'),
             ('ridge', 'ridge-inverted'), ('ridge', 'ridge-other'),
             ('noise', 'ridge'), ('noise-low', 'noise'),
             ('constant-128', 'ridge'), ('ridge', 'constant-128'),
             ('impulse-0-0-1', 'impulse-103-85-127'),
             ('checkerboard', 'checkerboard-low')]
    pair_report = []
    for first, second in pairs:
        before = (bytes(images[first]), bytes(images[second]))
        old = baseline.compare(images[first], images[second])
        require_equal(f'{first}/{second} score', old,
                      candidate.compare(images[first], images[second]))
        require_equal(f'{first}/{second} input immutability', before,
                      (bytes(images[first]), bytes(images[second])))
        pair_report.append({'first': first, 'second': second, **old})
    require_equal('NULL compare first', baseline.compare(None, images['ridge']),
                  candidate.compare(None, images['ridge']))
    require_equal('NULL compare second', baseline.compare(images['ridge'], None),
                  candidate.compare(images['ridge'], None))
    if candidate.has_self_check:
        require_equal('NULL self-check', -1, candidate.lib.tudor_image_self_check(None))
    return {'exact_float_bits': True, 'self_check_symbol_tested': candidate.has_self_check,
            'self_images': len(report), 'additional_pairs': len(pair_report),
            'images': report, 'pairs': pair_report}


def measure(library, enrollment, probes):
    verifier = library.lib.tudor_verifier_create()
    if not verifier:
        raise MemoryError('Synthetic verifier allocation failed')
    try:
        accepted = []
        start = time.perf_counter_ns()
        for image in enrollment:
            quality = Quality()
            status = library.lib.tudor_verifier_enroll(verifier, image, PIXELS, C.byref(quality))
            accepted.append({'status': status, 'quality': snapshot(quality)})
        prepare_seconds = (time.perf_counter_ns() - start) / 1e9
        if any(item['status'] for item in accepted):
            raise RuntimeError('Generated curved-ridge enrollment unexpectedly rejected')
        count = library.lib.tudor_verifier_count(verifier)
        if count != SCANS:
            raise RuntimeError(f'Expected ten accepted synthetic references, found {count}')
        results, times = {}, {}
        for name, image in probes.items():
            result = Result()
            start = time.perf_counter_ns()
            status = library.lib.tudor_verifier_check(verifier, image, PIXELS, C.byref(result))
            times[name] = (time.perf_counter_ns() - start) / 1e9
            results[name] = {'status': status, **snapshot(result)}
        return {'bank_preparation_seconds': prepare_seconds, 'verification_seconds': times,
                'accepted': accepted, 'count': count, 'results': results}
    finally:
        library.lib.tudor_verifier_free(verifier)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--runs', type=int, default=3, help='Runs per library, minimum 3')
    parser.add_argument('--output', type=Path, help='Write aggregate JSON, never image data')
    args = parser.parse_args()
    if args.runs < 3:
        parser.error('--runs must be at least 3')
    libraries = {name: Library(getattr(args, name)) for name in ('baseline', 'candidate')}
    if libraries['baseline'].path.samefile(libraries['candidate'].path):
        parser.error('Use a frozen baseline library distinct from the candidate')
    correctness = equivalent(libraries['baseline'], libraries['candidate'])
    enrollment = [ridge(0.2 * (index + 1)) for index in range(SCANS)]
    probes = {'curved_same': ridge(0.45), 'curved_different': ridge(different=True),
              'flat_retry': Image(*([128] * PIXELS))}
    samples = {'baseline': [], 'candidate': []}
    orders = []
    reference = None
    for iteration in range(args.runs):
        order = ('baseline', 'candidate') if iteration % 2 == 0 else ('candidate', 'baseline')
        orders.append(list(order))
        for name in order:
            print(f'Timing run {iteration + 1}/{args.runs}: {name}', file=sys.stderr, flush=True)
            result = measure(libraries[name], enrollment, probes)
            semantics = {key: result[key] for key in ('accepted', 'count', 'results')}
            if reference is None:
                reference = semantics
            else:
                require_equal(f'{name} run {iteration + 1} enrollment/decisions', reference, semantics)
            samples[name].append(result)
    medians = {}
    for name, runs in samples.items():
        preparation = statistics.median(run['bank_preparation_seconds'] for run in runs)
        matching = {probe: statistics.median(run['verification_seconds'][probe] for run in runs)
                    for probe in probes}
        medians[name] = {'bank_preparation_seconds': preparation, 'verification_seconds': matching,
                         'prepare_plus_same_verification_seconds': statistics.median(
                             run['bank_preparation_seconds'] + run['verification_seconds']['curved_same']
                             for run in runs)}
    speedups = {'bank_preparation': medians['baseline']['bank_preparation_seconds'] /
                                  medians['candidate']['bank_preparation_seconds'],
                'same_verification': medians['baseline']['verification_seconds']['curved_same'] /
                                     medians['candidate']['verification_seconds']['curved_same'],
                'prepare_plus_same_verification':
                    medians['baseline']['prepare_plus_same_verification_seconds'] /
                    medians['candidate']['prepare_plus_same_verification_seconds']}
    report = {'benchmark_version': 1, 'synthetic_only': True, 'semantic_equivalence_passed': True,
              'platform': platform.platform(), 'python': platform.python_version(),
              'libraries': {name: {'path': str(library.path),
                                  'sha256': hashlib.sha256(library.path.read_bytes()).hexdigest()}
                            for name, library in libraries.items()},
              'runs_per_library': args.runs, 'alternating_order': orders,
              'equivalence': correctness, 'medians': medians, 'speedups': speedups,
              'timing_samples': {name: [{key: run[key] for key in
                                ('bank_preparation_seconds', 'verification_seconds')} for run in runs]
                                 for name, runs in samples.items()},
              'synthetic_enrollment_and_decisions': reference,
              'assumptions': ['Generated grayscale inputs only; no fingerprint files or hardware access.',
                              'Ten distinct curved-ridge references pass the unchanged quality gate.',
                              'Timings include ctypes calls; exclude loading and synthetic image generation.',
                              'CPU-only measurements exclude USB, TLS, contact settling, D-Bus, and GUI time.',
                              'Alternating runs reduce order bias; machine load and CPU frequency still affect timing.',
                              'Exact equivalence on this bounded corpus does not prove biometric reliability.']}
    encoded = json.dumps(report, indent=2, allow_nan=False) + '\n'
    if args.output:
        args.output.write_text(encoded, encoding='utf-8')
        print(f'Wrote synthetic benchmark report: {args.output}', file=sys.stderr)
    print(encoded, end='')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, MemoryError, AttributeError) as error:
        raise SystemExit(f'Synthetic benchmark failed: {error}')
