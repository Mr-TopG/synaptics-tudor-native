#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' 'Run with sudo to read the existing root-only pairing and access USB.' >&2
    exit 1
fi
if [ ! -x build/tudor-session ]; then
    printf '%s\n' 'First build as your normal user: make session' >&2
    exit 1
fi
sha256sum -c scripts/reference-keys.sha256 >&2
printf '%s\n' \
    'Experimental native TLS: reuse saved pairing, read firmware/frame dimensions, close session.' \
    'No pairing, fingerprint capture, storage writes, resets, or PAM changes.' >&2
exec ./build/tudor-session /var/lib/tudor-native-pairing-v1 \
    upstream/synaTudor-rev/pydrv/tudor/sensor/sensor_keys/10.1-kf.tsk
