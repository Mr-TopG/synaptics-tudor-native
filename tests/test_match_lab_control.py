#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Optional real-libfprint positive/negative controls using public upstream images.
Requires FPrint GI, virtual_image, Pillow, and local Unix-socket permissions.
"""
import importlib.util
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'scripts'))
from PIL import Image
spec=importlib.util.spec_from_file_location('match_lab',Path(__file__).resolve().parent.parent/'scripts/libfprint-match-lab.py')
match=importlib.util.module_from_spec(spec); spec.loader.exec_module(match)
if len(sys.argv) != 2: raise SystemExit('Usage: test_match_lab_control.py UPSTREAM_PUBLIC_PRINTS_DIRECTORY')
base=Path(sys.argv[1])
a=Image.open(base/'whorl.png').convert('RGBA')
b=Image.open(base/'tented_arch.png').convert('RGBA').resize(a.size)
images={'same-1':a.getchannel('A').tobytes(),'same-2':a.getchannel('A').tobytes(),'different-1':b.getchannel('A').tobytes()}
result=match.compare(images,'normal',match.lab.load_fprint(),width=a.width,height=a.height)
comparisons=result['comparisons']
assert comparisons['same-1']['matched'] is True
assert comparisons['same-2']['matched'] is True
assert comparisons['different-1']['matched'] is False
assert all(value['done'] and not value['timed_out'] for value in comparisons.values())
print('Public libfprint control passed: reference/replay accepted, different print rejected; memory-only templates.')
