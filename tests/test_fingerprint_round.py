#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Round lifecycle/separation with mocked sensor and native bank; no USB."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'scripts'))
spec=importlib.util.spec_from_file_location('round_workflow',Path(__file__).resolve().parent.parent/'scripts/test_fingerprint_round.py')
round_=importlib.util.module_from_spec(spec); spec.loader.exec_module(round_)

references={name:bytes([i])*8944 for i,name in enumerate(round_.ENROLL)}
class Bank:
    instances=[]
    def __init__(self,path): self.training=[]; self.probes=[]; self.closed=False; self.instances.append(self)
    def __enter__(self): return self
    def __exit__(self,*args): self.closed=True
    def add(self,data): self.training.append(data)
    def score(self,data):
        assert len(self.training)==10 and data not in self.training
        self.probes.append(data)
        best=(0.78,0.71,0.69,0.32,0.39,0.44)[data[0]-20]
        return [{'correlation':best,'overlap_fraction':0.8}]*10

original_mkdtemp=tempfile.mkdtemp
with tempfile.TemporaryDirectory(prefix='tudor-round-test-') as tmp:
    root=Path(tmp); (root/'build').mkdir()
    library=root/'build/libtudor-image-score.so'; library.write_bytes(b'fixed test library')
    for name in ('tudor-capture','tudor-analyze'):
        p=root/'build'/name; p.write_text('unused'); p.chmod(0o700)
    for mode in ('valid','capture-failure','wrong-label','changed-library','duplicate'):
        library.write_bytes(b'fixed test library')
        calls=[]
        answers=[value for _,label,_ in round_.PLAN for value in ('s' if label=='same' else 'd','')]
        if mode=='wrong-label': answers[0]='d'
        def capture(directory,name):
            calls.append(name)
            if mode=='capture-failure' and len(calls)==4:
                raise subprocess.CalledProcessError(1,['mock-capture'])
            if mode=='changed-library': library.write_bytes(b'changed')
            if mode=='duplicate': return references[round_.ENROLL[0]]
            return bytes([19+len(calls)])*8944
        before=set(root.glob('tudor-native-round.*'))
        with (patch.object(round_,'ROOT',root),patch.object(round_,'NativeBank',Bank),
              patch.object(round_,'load_samples',return_value=references),patch.object(round_,'capture',side_effect=capture),
              patch.object(round_.os,'geteuid',return_value=1000),patch('builtins.input',side_effect=answers),
              patch.object(round_.tempfile,'mkdtemp',side_effect=lambda prefix:original_mkdtemp(prefix=prefix,dir=root)),
              patch.object(round_.shutil,'which',return_value=None),contextlib.redirect_stdout(io.StringIO())):
            try: round_.run_round(Path('/unused'))
            except (ValueError,subprocess.CalledProcessError): assert mode!='valid'
            else: assert mode=='valid'
        bank=Bank.instances[-1]
        assert bank.closed and bank.training==list(references.values())
        directory=(set(root.glob('tudor-native-round.*'))-before).pop()
        report=json.loads((directory/'report.json').read_text())
        assert directory.stat().st_mode & 0o777 == 0o700
        assert (directory/'report.json').stat().st_mode & 0o777 == 0o600
        assert report['complete']==(mode=='valid')
        assert report['summary']['round_valid']==(mode=='valid')
        if mode=='valid':
            assert calls==[name for name,_,_ in round_.PLAN]
            assert len(bank.probes)==6 and report['summary']['observed_score_gap']==0.25
            assert report['enrollment_unchanged'] and report['scorer_unchanged']
        elif mode=='capture-failure': assert len(calls)==4 and len(report['results'])==3
        elif mode=='wrong-label': assert not calls and not bank.probes
        elif mode=='duplicate': assert len(calls)==1 and not bank.probes
        elif mode=='changed-library': assert not report['scorer_unchanged']
missing=[{'label':label,'best_correlation':None} for _,label,_ in round_.PLAN]
assert round_.summarize(missing)['observed_ranges_separated'] is None
overlap=[{'label':label,'best_correlation':0.5} for _,label,_ in round_.PLAN]
assert round_.summarize(overlap)['observed_ranges_separated'] is False
print('Six-scan round tests passed: frozen references/scorer, six fresh probes, label confirmation, privacy, cancellation/failure without retry, duplicate rejection, unusable/overlapping scores.')
