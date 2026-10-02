#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."
if [ "$#" -ne 0 ]; then
    printf '%s\n' 'Usage: sudo sh scripts/memory-capture-experimental.sh' >&2
    exit 2
fi
if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' 'Run with sudo to use the existing private pairing and USB sensor.' >&2
    exit 1
fi
if [ ! -x build/tudor-capture ]; then
    printf '%s\n' 'First build as your normal user: make capture' >&2
    exit 1
fi
sha256sum -c scripts/reference-keys.sha256 >&2
printf '%s\n' \
    'Experimental in-memory capture: one settled scan, stop capture, close TLS, print summary.' \
    'Keep your finger OFF the sensor until prompted. Images are decoded in memory and discarded.' \
    'No image files, re-pairing, sensor storage changes, enrollment, or login changes.' >&2
exec ./build/tudor-capture --memory --settled /var/lib/tudor-native-pairing-v1 \
    upstream/synaTudor-rev/pydrv/tudor/sensor/sensor_keys/10.1-kf.tsk
