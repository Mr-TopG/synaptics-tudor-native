#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."
if [ "$#" -ne 1 ] || [ "$(id -u)" -eq 0 ]; then
    printf '%s\n' 'Usage: sh scripts/enrollment-lab.sh PRIVATE_ENROLLMENT_DIRECTORY (normal user)' >&2
    exit 2
fi
lab_typelib_dir="$PWD/build/fprint-lab/root/usr/lib/$(cc -dumpmachine)/girepository-1.0"
if [ -d "$lab_typelib_dir" ]; then
    export GI_TYPELIB_PATH="$lab_typelib_dir${GI_TYPELIB_PATH:+:$GI_TYPELIB_PATH}"
fi
exec timeout 180s /usr/bin/python3 scripts/enrollment_lab.py "$1"
