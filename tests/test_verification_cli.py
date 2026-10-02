#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Ensure test labels cannot alter decisions or contaminate enrollment."""
import contextlib
import io
import json
from pathlib import Path
import sys
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'scripts'))
import native_verification_lab as lab

references = {name: bytes([i])*lab.PIXELS for i, name in enumerate(lab.ENROLL)}
fresh = bytes([17])*lab.PIXELS
seen = []


class Verifier:
    def __init__(self):
        self.enrolled = []
        self.probes = []
        self.closed = False
        seen.append(self)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.closed = True

    def enroll(self, image):
        self.enrolled.append(image)
        return {'flags': []}

    def check(self, image):
        self.probes.append(image)
        return {'decision': 'retry', 'authentication_supported': False}


for label in ('same', 'different', 'unspecified'):
    output = io.StringIO()
    with (patch.object(sys, 'argv', ['lab', '/unused', '--probe', '/fresh', '--label', label]),
          patch.object(lab, 'load_samples', return_value=references) as load,
          patch.object(lab.match.lab, 'read_preview', return_value=fresh),
          patch.object(lab, 'NativeVerifier', Verifier), contextlib.redirect_stdout(output)):
        lab.main()
        load.assert_called_once_with(Path('/unused'), lab.ENROLL)
    result = json.loads(output.getvalue())
    assert result['decision'] == 'retry' and result['actual_label'] == label
    assert result['authentication_supported'] is False
    v = seen[-1]
    assert v.closed and v.enrolled == list(references.values()) and v.probes == [fresh]

with (patch.object(sys, 'argv', ['lab', '/unused', '--probe', '/replay']),
      patch.object(lab, 'load_samples', return_value=references),
      patch.object(lab.match.lab, 'read_preview', return_value=references[lab.ENROLL[0]]),
      patch.object(lab, 'NativeVerifier', Verifier)):
    try:
        lab.main()
    except ValueError:
        pass
    else:
        raise AssertionError('Enrollment replay accepted as independent probe')
    assert seen[-1].closed and not seen[-1].probes

with (patch.object(sys, 'argv', ['lab', '/unused', '--validate-enrollment']),
      patch.object(lab, 'load_samples', return_value=references),
      patch.object(lab.match.lab, 'read_preview') as read,
      patch.object(lab, 'NativeVerifier', Verifier), contextlib.redirect_stdout(io.StringIO())):
    lab.main()
    read.assert_not_called()
    assert seen[-1].closed and not seen[-1].probes

# ctypes boundary: invalid sizes and use-after-close stop before entering C.
with lab.NativeVerifier() as v:
    try:
        v.enroll(b'')
    except ValueError:
        pass
    else:
        raise AssertionError('Wrong image length accepted')
    result = v.check(fresh)
    assert result['decision'] == 'retry' and result['retry_reason'] == 'incomplete_enrollment'
try:
    v.check(fresh)
except ValueError:
    pass
else:
    raise AssertionError('Closed verifier accepted')
print('Verification CLI tests passed: label independence, probe separation, replay rejection, cleanup and ctypes boundary.')
