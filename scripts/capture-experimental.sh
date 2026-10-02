#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != --settled ]; }; then
    printf '%s\n' 'Usage: sudo sh scripts/capture-experimental.sh [--settled]' >&2
    exit 2
fi
if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' 'Run with sudo to read the existing root-only pairing and access USB.' >&2
    exit 1
fi
if [ ! -x build/tudor-capture ]; then
    printf '%s\n' 'First build as your normal user: make capture' >&2
    exit 1
fi
sha256sum -c scripts/reference-keys.sha256 >&2
printf '%s\n' \
    'Experimental native capture: reuse pairing, attempt one raw frame, stop capture, close TLS.' \
    'Keep your finger OFF the sensor until prompted, then place it and hold still.' \
    'Raw fingerprint data stays local in a fresh root-only directory under /var/lib.' \
    'Share only terminal output, not frame-response.bin, frame.raw, or pairing keys.' \
    'No re-pairing, sensor storage writes, resets, enrollment, or PAM changes.' >&2
exec ./build/tudor-capture "$@" /var/lib/tudor-native-pairing-v1 \
    upstream/synaTudor-rev/pydrv/tudor/sensor/sensor_keys/10.1-kf.tsk /var/lib
