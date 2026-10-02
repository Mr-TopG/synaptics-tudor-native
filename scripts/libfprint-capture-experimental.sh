#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."
project=$(pwd -P)
lab="$project/build/libfprint-native-build/libfprint/tudor-capture-lab"
if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != --list ]; }; then
    printf '%s\n' 'Usage: sudo sh scripts/libfprint-capture-experimental.sh (or --list without sudo)' >&2
    exit 2
fi
if [ ! -x "$lab" ]; then
    printf '%s\n' 'First build the local adapter with scripts/build-libfprint-lab.py; see README.' >&2
    exit 1
fi
export LD_LIBRARY_PATH="$project/build/libfprint-native-build/libfprint"
export FP_DRIVERS_ALLOWLIST=tudor_native
if [ "${1:-}" = --list ]; then exec "$lab" --list; fi
if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' 'Run with sudo to reuse the private pairing and access the sensor.' >&2
    exit 1
fi
sha256sum -c scripts/reference-keys.sha256 >&2
export TUDOR_NATIVE_EXPERIMENTAL=1
export TUDOR_NATIVE_PAIRING_DIR=/var/lib/tudor-native-pairing-v1
export TUDOR_NATIVE_AUTHORITY_FILE="$project/upstream/synaTudor-rev/pydrv/tudor/sensor/sensor_keys/10.1-kf.tsk"
printf '%s\n' \
    'Experimental libfprint adapter: one capture through the private local library.' \
    'Reuse saved pairing; no image files, re-pairing, enrollment, or system login changes.' \
    'Keep your finger clear until prompted. Ctrl+C cancels and waits for cleanup.' >&2
exec "$lab" --capture
