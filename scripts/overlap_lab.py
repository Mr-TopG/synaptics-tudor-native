# SPDX-License-Identifier: MIT
"""Offline ridge-overlap diagnostic, never an authentication decision.

Fixed rotation/translation search with masked, zero-mean normalized correlation.
Only aggregate scores leave the process. Requires NumPy and SciPy in the lab;
neither is a native driver dependency.
"""
import importlib.util
import json
from pathlib import Path
import sys

import numpy as np
from scipy.ndimage import gaussian_filter, rotate
from scipy.signal import correlate

ANGLES = tuple(range(-30, 31, 3))
MIN_OVERLAP = 0.60


def overlap_score(first, second, angles=ANGLES, min_overlap=MIN_OVERLAP):
    a, b = np.asarray(first, dtype=float), np.asarray(second, dtype=float)
    if (a.ndim != 2 or a.shape != b.shape or min(a.shape) < 8
            or max(a.shape) > 512 or not np.isfinite(a).all()
            or not np.isfinite(b).all() or not 0.5 <= min_overlap <= 1
            or not angles or any(not np.isfinite(x) or abs(x) > 45 for x in angles)):
        raise ValueError('Invalid overlap diagnostic input')
    # Remove slow contrast variation with a fixed filter, independent of labels.
    a = a-gaussian_filter(a, 3, mode='reflect')
    b = b-gaussian_filter(b, 3, mode='reflect')
    ones = np.ones(a.shape)
    best = None
    for angle in angles:
        mask = (rotate(ones, angle, reshape=False, order=1, mode='constant', cval=0,
                       prefilter=False) > 0.999).astype(float)
        rotated = rotate(b, angle, reshape=False, order=1, mode='constant', cval=0,
                         prefilter=False)*mask
        corr = lambda x,y: correlate(x,y,mode='full',method='fft')
        count = np.rint(corr(ones,mask))
        denom_count = np.maximum(count,1)
        sa, sb = corr(a,mask), corr(ones,rotated)
        va = np.maximum(0,corr(a*a,mask)-sa*sa/denom_count)
        vb = np.maximum(0,corr(ones,rotated*rotated)-sb*sb/denom_count)
        valid = ((count >= min_overlap*a.size) & (va > count*1e-6)
                 & (vb > count*1e-6))
        if not valid.any():
            continue
        numerator = corr(a,rotated)-sa*sb/denom_count
        values = numerator[valid]/np.sqrt(va[valid]*vb[valid])
        peak = float(np.clip(values.max(),-1,1))
        best = peak if best is None else max(best,peak)
    return None if best is None else round(best,6)


def main():
    if len(sys.argv) != 2:
        raise SystemExit('Usage: python3 scripts/overlap_lab.py PRIVATE_SAMPLE_DIRECTORY')
    spec = importlib.util.spec_from_file_location('image_lab',Path(__file__).with_name('libfprint-image-lab.py'))
    lab = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lab)
    base = Path(sys.argv[1])
    images = {name:np.frombuffer(lab.read_preview(base/(name+'.bmp')),dtype=np.uint8).reshape(86,104)
              for name in ('same-1','same-2','different-1')}
    pairs = (('exact_replay','same-1','same-1'),('same_finger','same-1','same-2'),
             ('different_vs_first','same-1','different-1'),('different_vs_second','same-2','different-1'))
    scores = {label:overlap_score(images[a],images[b]) for label,a,b in pairs}
    print(json.dumps({'offline_overlap_diagnostic_only':True,'authentication_supported':False,
                      'minimum_overlap_fraction':MIN_OVERLAP,'rotation_range_degrees':[-30,30],
                      'rotation_step_degrees':3,'highpass_sigma_pixels':3,
                      'maximum_normalized_correlations':scores,'files_written':False}))


if __name__ == '__main__':
    try:
        main()
    except (OSError,ValueError) as error:
        raise SystemExit(f'Overlap diagnostic stopped: {error}')
