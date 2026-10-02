#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Optional NumPy/SciPy test with synthetic data only."""
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'scripts'))
import numpy as np
from scipy.ndimage import gaussian_filter, rotate
from overlap_lab import overlap_score

rng = np.random.default_rng(1927)
a = gaussian_filter(rng.normal(size=(86,104)),1)*50+128
assert overlap_score(a,a) == 1
assert overlap_score(a,2*a+19) == 1
# Independent shifted field, with uncorrelated fill at newly uncovered edges.
b = rng.normal(size=a.shape)*10+128
b[5:,8:] = a[:-5,:-8]
assert overlap_score(a,b) > 0.95
b = rotate(a-128,12,reshape=False,order=1,mode='constant',cval=0)+128
assert overlap_score(a,b) > 0.90
unrelated = gaussian_filter(rng.normal(size=a.shape),1)*50+128
assert overlap_score(a,unrelated) < 0.3
assert overlap_score(np.ones(a.shape),np.ones(a.shape)) is None
# Periodic patterns can also correlate perfectly: this must not become a login
# threshold. A phase shift is enough to align two synthetic stripe fields.
x = np.arange(104)[None,:]
stripes = np.broadcast_to(128+40*np.sin(x*2*np.pi/8),a.shape)
phase_shifted = np.broadcast_to(128+40*np.sin((x+2)*2*np.pi/8),a.shape)
assert overlap_score(stripes,phase_shifted) > 0.98
for bad in (np.zeros((2,2)),np.full(a.shape,np.nan)):
    try:
        overlap_score(a,bad)
    except ValueError:
        pass
    else:
        raise AssertionError('Invalid input accepted')
print('Overlap diagnostic controls passed: replay, brightness, translation/rotation, unrelated noise, flat/invalid inputs; periodic-pattern ambiguity demonstrated.')
