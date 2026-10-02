#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify training/test separation and failure handling without hardware or GI."""
from pathlib import Path
import sys
from unittest.mock import patch
import json
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'scripts'))
import enrollment_lab as lab

images = {name:bytes([i])*8944 for i,name in enumerate(lab.ENROLL+lab.PROBES)}
instances = []

class Device:
    def get_nr_enroll_stages(self): return 5

class Session:
    device = Device()
    fail_enrollment = False
    fail_verify = False
    def __init__(self,*args,**kwargs):
        self.training = []; self.probes = []; self.closed = False
        instances.append(self)
    def operate(self,pixels,reference=None,enrollment_images=None):
        if enrollment_images is not None:
            assert pixels is None and reference is None
            self.training.extend(enrollment_images)
            result = {'done':True,'timed_out':False,'images_sent':5}
            if not self.fail_enrollment: result['reference'] = len(self.training)
            return result
        self.probes.append(pixels)
        if self.fail_verify: return {'done':True,'timed_out':False,'error_code':1}
        return {'done':True,'timed_out':False,'matched':reference == 10}
    def close(self): self.closed = True

result = lab.compare_bank(images,None,'native-normal',Session)
session = instances[-1]
assert session.training == [images[n] for n in lab.ENROLL]
assert session.probes == [images[n] for n in lab.PROBES for _ in range(2)]
assert session.closed and result['complete']
assert all(c['bank_matched'] is True for c in result['comparisons'].values())
assert all('reference' not in state for state in result['reference_creation'])
Session.fail_verify = True
result = lab.compare_bank(images,None,'native-normal',Session)
assert not result['complete'] and all(c['bank_matched'] is None for c in result['comparisons'].values())
assert instances[-1].closed
Session.fail_verify = False; Session.fail_enrollment = True
result = lab.compare_bank(images,None,'native-normal',Session)
assert not result['complete'] and not result['comparisons'] and instances[-1].closed
with patch.object(lab.match.lab,'read_preview',return_value=bytes(8944)):
    try: lab.load_samples(Path('/unused'))
    except ValueError: pass
    else: raise AssertionError('Replayed captures accepted')
with tempfile.TemporaryDirectory(prefix='tudor-label-test-') as tmp:
    directory=Path(tmp)
    assert lab.load_probe_labels(directory)['different-01']=='different'
    path=directory/'probe-labels.json'
    path.write_text(json.dumps({'version':1,'overrides':{'different-01':'same'}})); path.chmod(0o600)
    labels=lab.load_probe_labels(directory)
    assert labels['different-01']=='same' and labels['different-02']=='different'
    for invalid in ({'version':1,'overrides':{'enroll-01':'different'}},
                    {'version':1,'overrides':{'same-01':'enroll'}}, []):
        path.write_text(json.dumps(invalid))
        try: lab.load_probe_labels(directory)
        except ValueError: pass
        else: raise AssertionError('Unsafe label correction accepted')
    path.chmod(0o644)
    try: lab.load_probe_labels(directory)
    except ValueError: pass
    else: raise AssertionError('Public label file accepted')
    path.unlink(); path.symlink_to(directory/'missing')
    try: lab.load_probe_labels(directory)
    except OSError: pass
    else: raise AssertionError('Symlinked correction accepted')
print('Enrollment separation tests passed: ten distinct inputs, held-out probes, both references, failures, duplicate rejection, cleanup.')
