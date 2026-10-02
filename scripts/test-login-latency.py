#!/usr/bin/python3
# SPDX-License-Identifier: MIT
"""Try the private optimized driver, then restore the system fprintd service."""
import argparse
from pathlib import Path
import os
import signal
import subprocess
import sys

ROOT=Path(__file__).resolve().parent.parent
FINGERS=[side+'-'+finger for side in ('left','right') for finger in
         ('thumb','index-finger','middle-finger','ring-finger','little-finger')]


def control(action, check=True):
    return subprocess.run(['/usr/bin/systemctl',action,'fprintd.service'],
                          capture_output=True,text=True,check=check,timeout=60)


def scan(command):
    child=None
    interrupted=False

    def cancel(signum, frame):
        nonlocal interrupted
        if not interrupted:
            interrupted=True
            print('Cancellation requested; waiting for private capture cleanup.',flush=True)
            if child is not None and child.poll() is None:
                child.send_signal(signal.SIGINT)

    old={sig:signal.signal(sig,cancel) for sig in (signal.SIGINT,signal.SIGTERM)}
    try:
        child=subprocess.Popen(command,start_new_session=True)
        if interrupted:
            child.send_signal(signal.SIGINT)
        return child.wait()
    finally:
        for sig,handler in old.items():
            signal.signal(sig,handler)


def experiment(settle_ms, finger, run_control=control, run_scan=scan):
    status=run_control('is-active',check=False)
    if status.returncode not in (0,3):
        raise RuntimeError('Cannot determine system fprintd state: '+status.stderr.strip())
    was_active=status.returncode==0
    try:
        run_control('stop')
        print(f'Testing private driver with {settle_ms} ms settling. Use the selected enrolled finger.',flush=True)
        command=['/usr/bin/python3','-I',str(ROOT/'scripts/fprintd-lab.py'),
                 'verify','--finger',finger,'--settle-ms',str(settle_ms)]
        return run_scan(command)
    finally:
        if was_active:
            run_control('start')
            print('System fprintd restarted with its installed driver.',flush=True)
        else:
            print('System driver configuration retained; fprintd can start on demand.',flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--settle-ms',type=int,choices=(500,1000),default=500)
    parser.add_argument('--finger',choices=FINGERS,default='right-index-finger')
    args=parser.parse_args()
    if os.geteuid()!=0:
        parser.error('Run with sudo after closing Fingwit and other fingerprint operations')
    if not (ROOT/'build/libfprint-matching-build/libfprint/libfprint-2.so.2').is_file():
        parser.error('Build the private matching adapter first')
    try:
        return experiment(args.settle_ms,args.finger)
    except (OSError,RuntimeError,subprocess.SubprocessError) as error:
        print('Latency experiment failed: '+str(error),file=sys.stderr)
        print('If system fprintd could not restart, run: sudo systemctl start fprintd',file=sys.stderr)
        return 1


if __name__=='__main__':
    sys.exit(main())
