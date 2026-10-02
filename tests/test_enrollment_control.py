#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Optional real virtual-driver control with PUBLIC upstream fingerprint images.
Ten transformed images exercise input sequencing, not physical enrollment.
"""
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'scripts'))
from PIL import Image, ImageChops
import enrollment_lab as lab

if len(sys.argv) != 2: raise SystemExit('Usage: test_enrollment_control.py PUBLIC_PRINTS_DIRECTORY')
base=Path(sys.argv[1])
a=Image.open(base/'whorl.png').convert('RGBA').getchannel('A')
b=Image.open(base/'tented_arch.png').convert('RGBA').getchannel('A').resize(a.size)
images={name:ImageChops.offset(a,i-5,0).tobytes() for i,name in enumerate(lab.ENROLL)}
images.update({f'same-{i:02}':ImageChops.offset(a,5+i,0).tobytes() for i in range(1,4)})
images.update({f'different-{i:02}':ImageChops.offset(b,i,0).tobytes() for i in range(1,4)})
result=lab.compare_bank(images,lab.match.lab.load_fprint(),'native-normal',image_width=a.width,image_height=a.height)
assert result['complete'], 'Public control did not complete'
assert [s['images_sent'] for s in result['reference_creation']] == [5,5]
for name,state in result['comparisons'].items():
    assert state['bank_matched'] is name.startswith('same-'), name
print('Public ten-scan control passed: two five-image references; three transformed same-print probes accepted; three different-print probes rejected. Memory-only templates.')
