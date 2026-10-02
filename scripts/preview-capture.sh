#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ "$#" -ne 1 ]; then
    printf '%s\n' "Usage: sh $0 /var/lib/tudor-native-frame-<name> (run as your normal user)" >&2
    exit 2
fi
if [ "$(id -u)" -eq 0 ]; then
    printf '%s\n' 'Run this script WITHOUT sudo. It calls sudo only to read the protected capture.' >&2
    exit 1
fi
if [ ! -x build/tudor-analyze ]; then
    printf '%s\n' 'First build as your normal user: make analyze' >&2
    exit 1
fi
umask 077
preview_dir=$(mktemp -d /tmp/tudor-native-preview.XXXXXXXX)
# The normal user's shell creates the private file, before sudo runs the reader.
# No privileged process opens or overwrites a user-supplied output pathname.
if ! sudo ./build/tudor-analyze --preview "$1" > "$preview_dir/preview.bmp"; then
    rm -f "$preview_dir/preview.bmp"
    rmdir "$preview_dir"
    exit 1
fi
printf 'Private preview saved: %s/preview.bmp\n' "$preview_dir"
printf '%s\n' 'View it locally. Report whether you see ridges, stripes, or noise; keep the image private.'
if command -v xdg-open >/dev/null 2>&1 && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; then
    xdg-open "$preview_dir/preview.bmp" >/dev/null 2>&1 &
fi
