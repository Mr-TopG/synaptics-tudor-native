#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Six fresh probes against a fixed native bank; no threshold or login decision."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from enrollment_lab import ENROLL, load_samples, match
from native_image_lab import NativeBank

ROOT=Path(__file__).resolve().parent.parent
PLAN=(
    ('same-01','same','Use the enrolled finger in your usual position.'),
    ('same-02','same','Lift fully and place the SAME enrolled finger naturally again.'),
    ('same-03','same','Use the SAME enrolled finger with a small natural position change; keep good contact.'),
    ('different-01','different','Use a finger OTHER than the enrolled finger.'),
    ('different-02','different','Use ANOTHER different finger, not the enrolled finger or the previous test finger.'),
    ('different-03','different','Use a THIRD different finger, not the enrolled finger or either previous test finger.'),
)


def summarize(results):
    summary={'completed_scans':len(results),'planned_scans':len(PLAN),
             'authentication_supported':False,'acceptance_threshold':None}
    all_usable=len(results)==len(PLAN)
    for label in ('same','different'):
        group=[r for r in results if r['label']==label]
        values=[r['best_correlation'] for r in group if r['best_correlation'] is not None]
        all_usable &= len(group)==3 and len(values)==3
        summary[label]={'scans':len(group),'usable':len(values),'scores':values,
                        'minimum':min(values) if values else None,'maximum':max(values) if values else None}
    summary['all_comparisons_usable']=bool(all_usable)
    summary['observed_score_gap']=(round(summary['same']['minimum']-summary['different']['maximum'],6)
                                   if all_usable else None)
    summary['observed_ranges_separated']=(summary['observed_score_gap']>0 if all_usable else None)
    return summary


def snapshot(images):
    return {name:hashlib.sha256(images[name]).digest() for name in ENROLL}


def capture(directory,name):
    metadata=directory/(name+'.json')
    with metadata.open('xb') as stream:
        subprocess.run(['sudo','sh','scripts/capture-experimental.sh','--settled'],cwd=ROOT,stdout=stream,check=True)
    data=json.loads(metadata.read_text())
    capture_name=data.get('capture_directory_name','')
    if (not re.fullmatch(r'tudor-native-frame-[0-9a-f]{16}',capture_name)
            or data.get('raw_frame_received') is not True or data.get('session_closed') is not True):
        raise ValueError('Capture metadata invalid or capture/session incomplete')
    preview=directory/(name+'.bmp')
    try:
        with preview.open('xb') as stream:
            subprocess.run(['sudo','./build/tudor-analyze','--preview','/var/lib/'+capture_name],
                           cwd=ROOT,stdout=stream,check=True)
    except Exception:
        preview.unlink(missing_ok=True)
        raise
    return match.lab.read_preview(preview)


def run_round(enrollment):
    if os.geteuid()==0:
        raise ValueError('Run as your normal user, without an outer sudo')
    os.umask(0o077)
    images=load_samples(enrollment,ENROLL)
    original=snapshot(images)
    library=ROOT/'build/libtudor-image-score.so'
    library_hash=hashlib.sha256(library.read_bytes()).hexdigest()
    for executable in ('tudor-capture','tudor-analyze'):
        if not os.access(ROOT/'build'/executable,os.X_OK):
            raise ValueError('First build: make capture analyze image-score')
    results=[]
    seen=set(original.values())
    with NativeBank(library) as bank:
        for name in ENROLL: bank.add(images[name])
        directory=Path(tempfile.mkdtemp(prefix='tudor-native-round.'))
        print(f'Private test directory: {directory}',flush=True)
        print('Six fresh scans: three of the enrolled finger, then three other fingers.\n'
              'Lift fully between scans. The ten-image bank and C comparison stay fixed.\n'
              'No threshold is chosen and no test scan updates enrollment.',flush=True)
        report={'format_version':1,'scorer_library_sha256':library_hash,'enrollment_unchanged':None,
                'scorer_unchanged':None,'complete':False,'results':results,'authentication_supported':False}
        try:
            for name,label,instruction in PLAN:
                print(f'\n{name}: {instruction}',flush=True)
                expected='s' if label=='same' else 'd'
                answer=input(f'Confirm this is a {label.upper()} finger: type {expected}, then Enter (q cancels): ').strip().lower()
                if answer!=expected: raise ValueError('Cancelled before this capture: finger confirmation did not match')
                input('Keep the sensor clear and press Enter; then follow the settled-contact prompt: ')
                pixels=capture(directory,name)
                digest=hashlib.sha256(pixels).digest()
                if digest in seen: raise ValueError('Exact reused image detected; test stopped')
                seen.add(digest)
                scores=bank.score(pixels)
                values=[s['correlation'] for s in scores if s is not None]
                if not all(math.isfinite(v) and -1<=v<=1 for v in values):
                    raise ValueError('Invalid native comparison score')
                results.append({'sample':name,'label':label,'scores':scores,
                                'best_correlation':max(values) if values else None})
                print(f'Saved {name}. Remove your finger. Scores will be shown after all six scans.',flush=True)
            report['enrollment_unchanged']=snapshot(load_samples(enrollment,ENROLL))==original
            report['scorer_unchanged']=hashlib.sha256(library.read_bytes()).hexdigest()==library_hash
            if not report['enrollment_unchanged'] or not report['scorer_unchanged']:
                raise ValueError('Enrollment or scorer changed during testing; round is invalid')
            report['complete']=True
        except (ValueError,RuntimeError,OSError,subprocess.SubprocessError,EOFError,KeyboardInterrupt) as error:
            report['stopped_reason']=type(error).__name__
            raise
        finally:
            report['summary']=summarize(results)
            report['summary']['round_valid']=report['complete']
            with (directory/'report.json').open('x') as stream:
                json.dump(report,stream,indent=2); stream.write('\n')
    print('\n'+json.dumps(report['summary']),flush=True)
    print(f'Test round complete: {directory}',flush=True)
    print('Paste the summary and directory path; keep previews private. These are diagnostic scores, not login decisions.',flush=True)
    if shutil.which('xdg-open') and (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        try:
            subprocess.Popen(['xdg-open',str(directory/'same-01.bmp')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        except OSError:
            print(f'Open the preview locally: {directory / "same-01.bmp"}',flush=True)
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('enrollment',type=Path)
    args=parser.parse_args()
    try: run_round(args.enrollment.resolve())
    except (ValueError,RuntimeError,OSError,subprocess.SubprocessError,EOFError,KeyboardInterrupt) as error:
        raise SystemExit(f'Test round stopped without retry: {error}')


if __name__=='__main__': main()
