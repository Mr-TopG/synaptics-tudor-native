#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Shell collection integration using fake sudo, synthetic metadata, no USB."""
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile

source=Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='tudor-collection-test-') as tmp:
    root=Path(tmp)
    for name in ('scripts','build','fakebin','captures'): (root/name).mkdir()
    shutil.copyfile(source/'scripts/collect-enrollment-samples.sh',root/'scripts/collect-enrollment-samples.sh')
    for name in ('tudor-capture','tudor-analyze'):
        p=root/'build'/name; p.write_text('#!/bin/sh\nexit 99\n'); p.chmod(0o700)
    stubs={
        'id':'#!/bin/sh\nprintf "1000\\n"\n',
        'mktemp':'#!/bin/sh\nexec /usr/bin/mktemp -d "$TEST_ROOT/captures/batch.XXXXXXXX"\n',
        'sudo':'''#!/usr/bin/python3
import json, os, pathlib, sys
root=pathlib.Path(os.environ['TEST_ROOT'])
counter=root/'count'
if sys.argv[1:]==['sh','scripts/capture-experimental.sh','--settled']:
    n=int(counter.read_text())+1 if counter.exists() else 1
    counter.write_text(str(n))
    if n==int(os.environ.get('FAIL_AT','0')): raise SystemExit(1)
    print(json.dumps({'capture_directory_name':f'tudor-native-frame-{n:016x}',
                      'raw_frame_received':True,'session_closed':True}))
elif len(sys.argv)==4 and sys.argv[1:3]==['./build/tudor-analyze','--preview']:
    assert sys.argv[3].startswith('/var/lib/tudor-native-frame-')
    sys.stdout.buffer.write(b'Synthetic preview; never a biometric image')
else:
    raise SystemExit('Unexpected privileged command')
'''}
    for name,code in stubs.items():
        p=root/'fakebin'/name; p.write_text(code); p.chmod(0o700)
    env=dict(os.environ,TEST_ROOT=str(root),PATH=str(root/'fakebin')+':'+os.environ['PATH'])
    for fail_at in (0,4):
        (root/'count').unlink(missing_ok=True)
        before=set((root/'captures').iterdir())
        result=subprocess.run(['sh',str(root/'scripts/collect-enrollment-samples.sh')],
                              input='\n'*16,text=True,capture_output=True,env=dict(env,FAIL_AT=str(fail_at)),timeout=20)
        created=set((root/'captures').iterdir())-before
        assert len(created)==1
        batch=created.pop()
        assert stat.S_IMODE(batch.stat().st_mode)==0o700
        assert all(stat.S_IMODE(p.stat().st_mode)==0o600 for p in batch.iterdir())
        assert (batch/'capture-timing.txt').read_text().strip()=='settled-contact'
        assert int((root/'count').read_text())==(16 if not fail_at else 4)
        if not fail_at:
            assert result.returncode==0, result.stderr
            assert len(list(batch.glob('enroll-*.bmp')))==10
            assert len(list(batch.glob('same-*.bmp')))==3
            assert len(list(batch.glob('different-*.bmp')))==3
            assert (batch/'collection-complete.txt').is_file()
            dirs=[json.loads(p.read_text())['capture_directory_name'] for p in batch.glob('*.json')]
            assert len(set(dirs))==16
        else:
            assert result.returncode!=0 and len(list(batch.glob('*.bmp')))==3
            assert not (batch/'collection-complete.txt').exists()
print('Collection tests passed: 16 distinct captures, settled commands only, private output modes, stop on failure without retry; no USB or real sudo.')
