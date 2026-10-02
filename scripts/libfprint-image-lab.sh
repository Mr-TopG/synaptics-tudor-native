#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ "$#" -ne 1 ]; then
    printf '%s\n' 'Usage: sh scripts/libfprint-image-lab.sh PRIVATE_PREVIEW_BMP (normal user, no sudo)' >&2
    exit 2
fi
if [ "$(id -u)" -eq 0 ]; then
    printf '%s\n' 'Run as your normal user; use a private preview owned by that account.' >&2
    exit 1
fi
# Optional locally extracted distro introspection data, not bundled vendor code.
lab_typelib_dir="$PWD/build/fprint-lab/root/usr/lib/$(cc -dumpmachine)/girepository-1.0"
if [ -d "$lab_typelib_dir" ]; then
    export GI_TYPELIB_PATH="$lab_typelib_dir${GI_TYPELIB_PATH:+:$GI_TYPELIB_PATH}"
fi
exec timeout 45s /usr/bin/python3 scripts/libfprint-image-lab.py "$1"
