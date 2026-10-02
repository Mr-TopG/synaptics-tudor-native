#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ "$#" -ne 1 ]; then
    printf '%s\n' "Usage: sudo sh $0 /var/lib/tudor-native-frame-<name>" >&2
    exit 2
fi
if [ ! -x build/tudor-analyze ]; then
    printf '%s\n' 'First build as your normal user: make analyze' >&2
    exit 1
fi
exec ./build/tudor-analyze "$1"
