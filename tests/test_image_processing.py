#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import importlib.util
import math
from pathlib import Path
import random
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'scripts'))
from image_processing import local_contrast,enlarge

rng=random.Random(9)
for width,height in ((1,1),(5,4),(16,13)):
    pixels=bytes(rng.randrange(256) for _ in range(width*height))
    for radius in (1,3,7):
        actual=local_contrast(pixels,width,height,radius)
        expected=[]
        for y in range(height):
            for x in range(width):
                values=[pixels[b*width+a] for b in range(max(0,y-radius),min(height,y+radius+1))
                        for a in range(max(0,x-radius),min(width,x+radius+1))]
                mean=sum(values)/len(values)
                variance=sum((v-mean)**2 for v in values)/len(values)
                value=128+48*(pixels[y*width+x]-mean)/max(8,math.sqrt(variance))
                expected.append(max(0,min(255,round(value))))
        assert actual==bytes(expected)
for value in (0,127,255):
    assert local_contrast(bytes([value])*100,10,10)==bytes([128])*100
    for factor in (1,2,3):
        assert enlarge(bytes([value])*12,4,3,factor)==bytes([value])*(12*factor*factor)
assert enlarge(bytes([0,100,200,240]),2,2,2)==bytes([0,25,75,100,50,71,114,135,150,164,191,205,200,210,230,240])
assert enlarge(bytes([3,7]),2,1,1)==bytes([3,7])
for bad in ((b'',1,1),(b'12',1,1),(b'',0,0)):
    for transform in (lambda:local_contrast(*bad),lambda:enlarge(*bad,2)):
        try: transform()
        except ValueError: pass
        else: raise AssertionError('malformed dimensions accepted')
print('Image processing tests passed: independent window statistics, constant-data invariants, bilinear mapping, invalid dimensions.')
