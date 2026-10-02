#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Independent SciPy controls for native C scoring; synthetic images only."""
import ctypes
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'scripts'))
import numpy as np
from scipy.ndimage import gaussian_filter, rotate
from native_image_lab import NativeScorer,Score,PIXELS
from overlap_lab import overlap_score

scorer=NativeScorer(sys.argv[1])
rng=np.random.default_rng(18317)
a=np.clip(128+gaussian_filter(rng.normal(size=(86,104)),1)*100,0,255).astype(np.uint8)
shifted=np.clip(128+rng.normal(size=a.shape)*20,0,255).astype(np.uint8)
shifted[5:,8:]=a[:-5,:-8]
turned=np.clip(rotate(a.astype(float),12,reshape=False,order=1,mode='constant',cval=128),0,255).astype(np.uint8)
noise=np.clip(128+gaussian_filter(rng.normal(size=a.shape),1)*100,0,255).astype(np.uint8)
for name,b in (('replay',a),('shift',shifted),('rotation',turned),('different',noise)):
    result=scorer.score(a.tobytes(),b.tobytes())
    expected=overlap_score(a,b)
    # Coarse-to-fine native search is approximate; the independent exhaustive
    # reference provides a bound and detects coordinate/normalization mistakes.
    assert result is not None and 0.60<=result['overlap_fraction']<=1
    assert abs(result['correlation']-expected)<0.04,(name,result,expected)
    assert result['correlation']>(0.9 if name!='different' else -1)
    if name=='different': assert result['correlation']<0.3
flat=bytes([128])*PIXELS
assert scorer.score(flat,flat) is None
raw=(ctypes.c_uint8*PIXELS).from_buffer_copy(flat)
out=Score(17,19)
assert scorer.compare(raw,raw,ctypes.byref(out))==1 and (out.correlation,out.overlap_fraction)==(17,19)
assert scorer.compare(None,raw,ctypes.byref(out))==-1
before=a.tobytes()
raw=(ctypes.c_uint8*PIXELS).from_buffer_copy(before)
assert scorer.compare(raw,raw,ctypes.byref(out))==0 and bytes(raw)==before
assert abs(out.correlation-1)<1e-6
try: scorer.score(b'bad',flat)
except ValueError: pass
else: raise AssertionError('Incorrect input dimensions accepted')
# A high score alone is not identity: periodic fields are ambiguous.
x=np.arange(104)[None,:]
stripes=np.broadcast_to(128+40*np.sin(x*2*np.pi/8),a.shape).astype(np.uint8)
other=np.broadcast_to(128+40*np.sin((x+2)*2*np.pi/8),a.shape).astype(np.uint8)
assert scorer.score(stripes.tobytes(),other.tobytes())['correlation']>0.98
print('Native image score passed independent replay/motion/noise controls, flat/NULL/dimension guards, unchanged inputs, periodic ambiguity; no authentication threshold.')
