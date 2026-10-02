#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the C scoring core on a private ten-scan bank; no login decisions."""
import ctypes
import argparse
import json
from pathlib import Path
import sys
from enrollment_lab import ENROLL, PROBES, load_samples, load_probe_labels, match

PIXELS=104*86

class Score(ctypes.Structure):
    _fields_=[('correlation',ctypes.c_double),('overlap_fraction',ctypes.c_double)]


class BankScores(ctypes.Structure):
    _fields_=[('valid_references',ctypes.c_uint),('best_correlation',ctypes.c_double),
              ('scores',Score*10),('valid',ctypes.c_uint8*10)]


class NativeScorer:
    def __init__(self,path):
        self.library=ctypes.CDLL(str(Path(path).resolve()))
        self.compare=self.library.tudor_image_compare
        self.compare.argtypes=[ctypes.POINTER(ctypes.c_uint8),ctypes.POINTER(ctypes.c_uint8),ctypes.POINTER(Score)]
        self.compare.restype=ctypes.c_int

    def score(self,first,second):
        if len(first)!=PIXELS or len(second)!=PIXELS:
            raise ValueError('Expected two 104x86 grayscale images')
        a=(ctypes.c_uint8*PIXELS).from_buffer_copy(first)
        b=(ctypes.c_uint8*PIXELS).from_buffer_copy(second)
        result=Score()
        status=self.compare(a,b,ctypes.byref(result))
        if status==1: return None
        if status: raise RuntimeError('Native image comparison rejected input')
        return {'correlation':round(result.correlation,6),'overlap_fraction':round(result.overlap_fraction,6)}


class NativeBank:
    def __init__(self,path):
        self.library=ctypes.CDLL(str(Path(path).resolve()))
        lib=self.library
        lib.tudor_image_bank_create.argtypes=[]
        lib.tudor_image_bank_create.restype=ctypes.c_void_p
        lib.tudor_image_bank_free.argtypes=[ctypes.c_void_p]
        lib.tudor_image_bank_free.restype=None
        lib.tudor_image_bank_add.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_uint8),ctypes.c_size_t]
        lib.tudor_image_bank_add.restype=ctypes.c_int
        lib.tudor_image_bank_compare.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_uint8),ctypes.c_size_t,ctypes.POINTER(BankScores)]
        lib.tudor_image_bank_compare.restype=ctypes.c_int
        self.handle=lib.tudor_image_bank_create()
        if not self.handle: raise RuntimeError('Native enrollment allocation failed')

    def close(self):
        if self.handle:
            self.library.tudor_image_bank_free(self.handle)
            self.handle=None

    def __enter__(self): return self

    def __exit__(self,*args): self.close()

    def add(self,image):
        if not self.handle or len(image)!=PIXELS: raise ValueError('Invalid enrollment input or closed bank')
        data=(ctypes.c_uint8*PIXELS).from_buffer_copy(image)
        try:
            status=self.library.tudor_image_bank_add(self.handle,data,len(image))
        finally:
            ctypes.memset(data,0,PIXELS)
        if status: raise ValueError(f'Native enrollment rejected sample (status {status})')

    def score(self,image):
        if not self.handle or len(image)!=PIXELS: raise ValueError('Invalid probe or closed bank')
        data=(ctypes.c_uint8*PIXELS).from_buffer_copy(image)
        result=BankScores()
        try:
            status=self.library.tudor_image_bank_compare(self.handle,data,len(image),ctypes.byref(result))
        finally:
            ctypes.memset(data,0,PIXELS)
        if status==4: return [None]*10
        if status: raise RuntimeError(f'Native bank comparison failed (status {status})')
        return [{'correlation':round(result.scores[i].correlation,6),
                 'overlap_fraction':round(result.scores[i].overlap_fraction,6)}
                if result.valid[i] else None for i in range(10)]


def main():
    parser=argparse.ArgumentParser(description='Offline C reference-bank scores; no login decisions.')
    parser.add_argument('directory',type=Path)
    parser.add_argument('--probe',type=Path,help='One private preview to compare against the ten enrollment images')
    parser.add_argument('--label',choices=('same','different','unspecified'),default='unspecified')
    args=parser.parse_args()
    if args.label!='unspecified' and args.probe is None:
        parser.error('--label requires --probe')
    if args.probe is not None:
        images=load_samples(args.directory,ENROLL)
        probe=match.lab.read_preview(args.probe)
        if probe in images.values():
            raise ValueError('Probe is an exact copy of an enrollment image; use a separate capture')
        probe_images={'probe':probe}
        probes=('probe',)
        labels={'probe':args.label}
    else:
        images=load_samples(args.directory)
        labels=load_probe_labels(args.directory)
        probes=PROBES
        probe_images={name:images[name] for name in probes}
    with NativeBank(Path(__file__).resolve().parent.parent/'build/libtudor-image-score.so') as bank:
        for name in ENROLL: bank.add(images[name])
        for probe in probes:
            scores=bank.score(probe_images[probe])
            valid=[score['correlation'] for score in scores if score is not None]
            print(json.dumps({'native_image_diagnostic_only':True,'authentication_supported':False,
                              'reference_bank_implemented_in_c':True,'templates_saved':False,
                              'probe':probe,'actual_label':labels[probe],
                              'enrollment_order':ENROLL,'scores':scores,
                              'best_correlation':max(valid) if valid else None}),flush=True)


if __name__=='__main__':
    try: main()
    except (OSError,ValueError,RuntimeError) as error:
        raise SystemExit(f'Native diagnostic stopped: {error}')
