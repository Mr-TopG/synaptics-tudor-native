#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ "$#" -ne 1 ] && ! { [ "$#" -eq 2 ] && [ "$1" = --explore ]; }; then
    printf '%s\n' 'Usage: sh scripts/libfprint-match-lab.sh [--explore] PRIVATE_SAMPLE_DIRECTORY' >&2
    exit 2
fi
if [ "$(id -u)" -eq 0 ]; then
    printf '%s\n' 'Run as your normal user; samples must be private and owned by that account.' >&2
    exit 1
fi
lab_typelib_dir="$PWD/build/fprint-lab/root/usr/lib/$(cc -dumpmachine)/girepository-1.0"
if [ -d "$lab_typelib_dir" ]; then
    export GI_TYPELIB_PATH="$lab_typelib_dir${GI_TYPELIB_PATH:+:$GI_TYPELIB_PATH}"
fi
exec timeout 150s /usr/bin/python3 scripts/libfprint-match-lab.py "$@"
