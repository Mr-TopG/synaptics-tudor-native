#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' 'Run with sudo: this experiment needs USB access and saves a root-only host identity.' >&2
    exit 1
fi
if [ ! -x build/tudor-pair ]; then
    printf '%s\n' 'First build as your normal user: make pairing check-pairing' >&2
    exit 1
fi
# These are data-only protocol keys from the pinned research checkout.
# No code, DLL, or extracted image routine from that checkout is executed.
sha256sum -c scripts/reference-keys.sha256 >&2
printf '%s\n' \
    'Experimental native PAIR: changes host/sensor pairing and may affect Windows Hello pairing.' \
    'Sends no erase, format, reset, or firmware commands. Does not enable fingerprint login.' \
    'New private identity will be stored in /var/lib/tudor-native-pairing-v1.' >&2
exec ./build/tudor-pair /var/lib/tudor-native-pairing-v1 \
    upstream/synaTudor-rev/pydrv/tudor/sensor/sensor_keys/hskey.pem \
    upstream/synaTudor-rev/pydrv/tudor/sensor/sensor_keys/10.1-kf.tsk
