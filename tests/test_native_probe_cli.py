#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Single-probe CLI separation; no hardware, images or native library needed."""
import contextlib
import io
import json
from pathlib import Path
import sys
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'scripts'))
import native_image_lab as lab

references={name:bytes([i])*lab.PIXELS for i,name in enumerate(lab.ENROLL)}
fresh=bytes([17])*lab.PIXELS
seen=[]
class Bank:
    def __init__(self,path): self.training=[]; self.probes=[]; seen.append(self)
    def __enter__(self): return self
    def __exit__(self,*args): self.closed=True
    def add(self,image): self.training.append(image)
    def score(self,image):
        self.probes.append(image)
        return [{'correlation':0.7,'overlap_fraction':0.8}]*10

for label in ('same','different'):
    output=io.StringIO()
    with (patch.object(sys,'argv',['native_image_lab.py','/unused','--probe','/fresh.bmp','--label',label]),
          patch.object(lab,'load_samples',return_value=references) as load,
          patch.object(lab.match.lab,'read_preview',return_value=fresh),
          patch.object(lab,'NativeBank',Bank),contextlib.redirect_stdout(output)):
        lab.main()
        load.assert_called_once_with(Path('/unused'),lab.ENROLL)
    bank=seen[-1]
    assert bank.closed and bank.training==list(references.values()) and bank.probes==[fresh]
    result=json.loads(output.getvalue())
    assert result['actual_label']==label and result['best_correlation']==0.7
    assert not result['authentication_supported'] and not result['templates_saved']
with (patch.object(sys,'argv',['native_image_lab.py','/unused','--probe','/replayed.bmp']),
      patch.object(lab,'load_samples',return_value=references),
      patch.object(lab.match.lab,'read_preview',return_value=references[lab.ENROLL[0]]),
      patch.object(lab,'NativeBank') as constructor):
    try: lab.main()
    except ValueError: pass
    else: raise AssertionError('Enrollment-image replay accepted as fresh probe')
    constructor.assert_not_called()
print('Single-probe tests passed: ten enrollment images only, probe excluded from training, same/different label handling, duplicate rejection, bank cleanup.')
