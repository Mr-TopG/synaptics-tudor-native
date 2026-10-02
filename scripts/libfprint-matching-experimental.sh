#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."
project=$(pwd -P)
automatic=0
if [ "${1:-}" = --automatic ]; then automatic=1; shift; fi
case "${1:-}" in
    enroll) [ "$#" -eq 1 ] || exit 2 ;;
    verify) [ "$#" -eq 2 ] || exit 2 ;;
    *) printf '%s\n' 'Usage: sudo sh scripts/libfprint-matching-experimental.sh [--automatic] enroll | verify /absolute/template-path' >&2; exit 2 ;;
esac
[ "$(id -u)" -eq 0 ] || { echo 'Run with sudo to access the saved pairing and sensor.' >&2; exit 1; }
lab="$project/build/libfprint-matching-build/libfprint/tudor-matching-lab"
[ -x "$lab" ] || { echo 'Build with scripts/build-libfprint-lab.py SOURCE --matching first.' >&2; exit 1; }
sha256sum -c scripts/reference-keys.sha256 >&2
umask 077
ulimit -c 0
export LD_LIBRARY_PATH="$project/build/libfprint-matching-build/libfprint"
export FP_DRIVERS_ALLOWLIST=tudor_native_lab
export TUDOR_NATIVE_EXPERIMENTAL=1 TUDOR_NATIVE_MATCHING_EXPERIMENTAL=1
export TUDOR_NATIVE_AUTOMATIC_CONTACT="$automatic"
if [ "$automatic" -eq 1 ]; then
    echo "Automatic contact experiment: no Enter presses. Lift/place only when prompted; Ctrl+C cancels." >&2
fi
export TUDOR_NATIVE_PAIRING_DIR=/var/lib/tudor-native-pairing-v1
export TUDOR_NATIVE_AUTHORITY_FILE="$project/upstream/synaTudor-rev/pydrv/tudor/sensor/sensor_keys/10.1-kf.tsk"
printf '%s\n' 'Private libfprint enrollment/verification lab. Experimental matching; system login is unchanged.' \
    'Enrollment stores ten grayscale reference scans in a root-only template. Keep the template private.' >&2
if [ "$1" = enroll ]; then
    destination=$(mktemp -d /var/lib/tudor-native-libfprint.XXXXXXXX)
    exec "$lab" enroll "$destination/template.fprint"
fi
exec "$lab" verify "$2"
