#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Ten independent enrollment scans, six held-out probes; no system enrollment."""
import hashlib
import importlib.util
import json
import os
import stat
from pathlib import Path
import sys

from image_processing import local_contrast, enlarge

spec = importlib.util.spec_from_file_location('match_lab',Path(__file__).with_name('libfprint-match-lab.py'))
match = importlib.util.module_from_spec(spec)
spec.loader.exec_module(match)
ENROLL = tuple(f'enroll-{i:02}' for i in range(1,11))
PROBES = tuple(f'{kind}-{i:02}' for kind in ('same','different') for i in range(1,4))


def load_probe_labels(directory):
    labels = {name:name.split('-')[0] for name in PROBES}
    try:
        fd = os.open(directory/'probe-labels.json',os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK)
    except FileNotFoundError:
        return labels
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid()
                or info.st_mode & 0o077 or info.st_nlink != 1 or info.st_size > 4096):
            raise ValueError('Expected a private, owned probe-label correction file')
        correction = json.loads(os.read(fd,4097))
    finally:
        os.close(fd)
    if (not isinstance(correction,dict) or correction.get('version') != 1
            or not isinstance(correction.get('overrides'),dict)):
        raise ValueError('Invalid probe-label correction format')
    for name,label in correction['overrides'].items():
        if name not in PROBES or label not in ('same','different'):
            raise ValueError('Only held-out probe labels may be corrected')
        labels[name] = label
    return labels


def load_samples(directory, names=ENROLL+PROBES):
    images = {name:match.lab.read_preview(directory/(name+'.bmp')) for name in names}
    digests = [hashlib.sha256(data).digest() for data in images.values()]
    if len(set(digests)) != len(digests):
        raise ValueError('Duplicate images: collect independent placements and fresh test scans')
    return images


def compare_bank(images, modules, preset, session_factory=match.Session, image_width=104, image_height=86):
    # Presets are fixed before collection of this new batch. No selection using
    # held-out results, image pooling, threshold adjustment or feature merging.
    if preset == 'native-normal':
        convert = lambda data:data
        width,height = image_width,image_height
    elif preset == 'local15-scale2-inverted':
        convert = lambda data:bytes(255-v for v in enlarge(local_contrast(data,image_width,image_height,15),image_width,image_height,2))
        width,height = image_width*2,image_height*2
    else:
        raise ValueError('Unknown fixed preset')
    if (not 1 <= width <= 1024 or not 1 <= height <= 1024 or set(images) != set(ENROLL+PROBES)
            or any(len(v) != image_width*image_height for v in images.values())):
        raise ValueError('Expected ten enrollment images and six held-out images')
    session = session_factory(modules,width=width,height=height)
    references = []
    result = {'preset':preset,'independent_enrollment_images':10,
              'reference_images_are_replays':False,'matching_threshold':'unmodified virtual-driver default',
              'bank_rule':'any reference accepts; each reference tested separately',
              'reference_creation':[],'comparisons':{}}
    try:
        stages = session.device.get_nr_enroll_stages()
        if stages != 5:
            raise RuntimeError('This experiment requires the virtual driver with five enrollment stages')
        # The installed virtual driver takes five scans per template. Two
        # independent five-scan templates cover all ten; no private API mutation.
        for start in (0,5):
            scans = [convert(images[name]) for name in ENROLL[start:start+5]]
            state = session.operate(None,enrollment_images=scans)
            reference = state.pop('reference',None)
            result['reference_creation'].append(state)
            if reference is None or not state['done'] or state['timed_out'] or state['images_sent'] != stages:
                result['complete'] = False
                return result
            references.append(reference)
        for name in PROBES:
            checks = [session.operate(convert(images[name]),reference) for reference in references]
            complete = all(c['done'] and not c['timed_out'] and 'matched' in c for c in checks)
            result['comparisons'][name] = {'references':checks,
                'complete':complete,'bank_matched':any(c.get('matched') is True for c in checks) if complete else None}
        result['complete'] = all(c['complete'] for c in result['comparisons'].values())
        return result
    finally:
        references.clear()
        session.close()


def overlap_bank(images):
    import numpy as np
    from overlap_lab import overlap_score
    arrays = {name:np.frombuffer(data,dtype=np.uint8).reshape(86,104) for name,data in images.items()}
    return {probe:[overlap_score(arrays[name],arrays[probe]) for name in ENROLL] for probe in PROBES}


def main():
    if len(sys.argv) != 2:
        raise SystemExit('Usage: enrollment_lab.py PRIVATE_ENROLLMENT_DIRECTORY')
    os.umask(0o077)
    images = load_samples(Path(sys.argv[1]))
    labels = load_probe_labels(Path(sys.argv[1]))
    modules = match.lab.load_fprint()
    for preset in ('native-normal','local15-scale2-inverted'):
        print(json.dumps({'development_only':True,'authentication_supported':False,
                          'system_enrollment':False,'templates_saved':False,
                          'probe_labels':labels,
                          'result':compare_bank(images,modules,preset)}),flush=True)
    # Informational correlations only; no new login matcher or threshold.
    try:
        scores = overlap_bank(images)
    except ImportError:
        print(json.dumps({'overlap_diagnostic':'unavailable; requires NumPy and SciPy'}),flush=True)
    else:
        print(json.dumps({'overlap_diagnostic_only':True,'authentication_supported':False,
                          'probe_labels':labels,
                          'enrollment_order':ENROLL,'probe_correlations':scores}),flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError,ValueError,RuntimeError,ImportError) as error:
        raise SystemExit(f'Enrollment lab stopped: {error}')
