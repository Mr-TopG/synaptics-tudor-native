#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Service restoration tests; no service calls, private data or USB access."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import signal
import subprocess
import sys
import unittest

spec=importlib.util.spec_from_file_location('latency',Path(__file__).resolve().parents[1]/'scripts/test-login-latency.py')
lab=importlib.util.module_from_spec(spec); spec.loader.exec_module(lab)


class RunnerTests(unittest.TestCase):
    def trial(self, active, code=0, failure=None):
        calls=[]
        def control(action,check=True):
            calls.append(action)
            if action=='stop' and failure=='stop': raise RuntimeError('stop failed')
            return SimpleNamespace(returncode=0 if active else 3,stderr='')
        def scan(command):
            calls.append('scan')
            self.assertEqual(command[-4:],['--finger','right-index-finger','--settle-ms','500'])
            if failure=='scan': raise RuntimeError('scan failed')
            return code
        if failure:
            with self.assertRaises(RuntimeError): lab.experiment(500,'right-index-finger',control,scan)
        else:
            self.assertEqual(lab.experiment(500,'right-index-finger',control,scan),code)
        self.assertEqual(calls,['is-active','stop']+([] if failure=='stop' else ['scan'])+(['start'] if active else []))

    def test_active_restored_for_match_nonmatch_cancel_and_failures(self):
        for code in (0,1,130): self.trial(True,code)
        for failure in ('stop','scan'): self.trial(True,failure=failure)

    def test_inactive_remains_on_demand(self):
        for code in (0,1,130): self.trial(False,code)
        self.trial(False,failure='scan')

    def test_unknown_state_does_not_stop_service(self):
        calls=[]
        def control(action,check=True):
            calls.append(action); return SimpleNamespace(returncode=4,stderr='unknown')
        with self.assertRaises(RuntimeError): lab.experiment(1000,'right-index-finger',control)
        self.assertEqual(calls,['is-active'])

    def test_signal_waits_for_child_cleanup(self):
        child_code='import signal,sys\ndef stop(*a):\n print("CHILD_CLEANED",flush=True)\n sys.exit(130)\nsignal.signal(signal.SIGINT,stop)\nprint("READY",flush=True)\nsignal.pause()\n'
        parent_code=('import importlib.util,sys\n'
                     f's=importlib.util.spec_from_file_location("lab",{str(Path(lab.__file__))!r})\n'
                     'm=importlib.util.module_from_spec(s);s.loader.exec_module(m)\n'
                     f'sys.exit(m.scan([{sys.executable!r},"-c",{child_code!r}]))\n')
        parent=subprocess.Popen([sys.executable,'-c',parent_code],stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE,text=True)
        try:
            self.assertEqual(parent.stdout.readline().strip(),'READY')
            parent.send_signal(signal.SIGINT)
            output,error=parent.communicate(timeout=10)
            self.assertEqual(parent.returncode,130,error)
            self.assertIn('CHILD_CLEANED',output)
        finally:
            if parent.poll() is None: parent.kill(); parent.wait()


if __name__=='__main__': unittest.main()
