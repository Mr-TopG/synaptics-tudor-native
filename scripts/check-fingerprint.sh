#!/bin/sh
# SPDX-License-Identifier: MIT
# One fresh capture, local preview and native C comparison. No system enrollment.
set -eu
cd "$(dirname "$0")/.."
policy=0
if [ "${1:-}" = '--experimental-policy' ]; then
    policy=1
    shift
fi
if [ "$#" -ne 1 ] || [ "$(id -u)" -eq 0 ]; then
    printf '%s\n' 'Usage: sh scripts/check-fingerprint.sh [--experimental-policy] PRIVATE_ENROLLMENT_DIRECTORY (no outer sudo)' >&2
    exit 2
fi
if [ ! -x build/tudor-capture ] || [ ! -x build/tudor-analyze ] || [ ! -f build/libtudor-image-score.so ]; then
    printf '%s\n' 'First build: make capture analyze image-score' >&2
    exit 1
fi
# Validate all ten reference files before requesting any sensor operation.
if [ "$policy" -eq 1 ]; then
    if [ ! -f build/libtudor-verification.so ]; then
        printf '%s\n' 'First build: make verification' >&2
        exit 1
    fi
    /usr/bin/python3 scripts/native_verification_lab.py "$1" --validate-enrollment
    printf '%s\n' 'Ten samples passed the experimental C quality gate. Login remains unsupported.'
else
/usr/bin/python3 - "$1" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0,'scripts')
from enrollment_lab import load_samples,ENROLL
load_samples(Path(sys.argv[1]),ENROLL)
print('Validated ten existing enrollment samples. They will remain unchanged.')
PY
fi
printf '%s\n' 'Which finger will you test?' \
    '  s = the SAME finger you enrolled' \
    '  d = a DIFFERENT finger' \
    'Enter s or d, then press Enter:'
IFS= read -r choice || exit 1
case "$choice" in
    s|S) label=same ;;
    d|D) label=different ;;
    *) printf '%s\n' 'Cancelled before sensor access: enter s or d next time.' >&2; exit 2 ;;
esac
printf 'Selected test: %s finger.\n' "$label"
printf '%s\n' 'Clear the sensor and press Enter. Then follow the settled-contact prompt.'
IFS= read -r ready || exit 1
umask 077
probe_dir=$(mktemp -d /tmp/tudor-native-check.XXXXXXXX)
printf '%s\n' "$label" > "$probe_dir/label.txt"
printf 'Private check directory: %s\n' "$probe_dir"
if ! sudo sh scripts/capture-experimental.sh --settled > "$probe_dir/capture.json"; then
    printf '%s\n' 'Capture failed; stopped without retry. Paste the terminal error.' >&2
    exit 1
fi
capture_name=$(/usr/bin/python3 - "$probe_dir/capture.json" <<'PY'
import json,re,sys
with open(sys.argv[1]) as f: data=json.load(f)
name=data.get('capture_directory_name','')
if not re.fullmatch(r'tudor-native-frame-[0-9a-f]{16}',name): raise SystemExit('Invalid capture directory')
if data.get('raw_frame_received') is not True or data.get('session_closed') is not True:
    raise SystemExit('Incomplete capture or TLS not closed')
print(name)
PY
)
if ! sudo ./build/tudor-analyze --preview "/var/lib/$capture_name" > "$probe_dir/preview.bmp"; then
    rm -f "$probe_dir/preview.bmp"
    printf '%s\n' 'Preview failed; stopped. Original capture remains private.' >&2
    exit 1
fi
printf '\nPrivate preview: %s/preview.bmp\n' "$probe_dir"
printf '%s\n' 'Left: normal contrast. Right: inverted. Both are enlarged 4x.'
if command -v xdg-open >/dev/null 2>&1 && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; then
    xdg-open "$probe_dir/preview.bmp" >/dev/null 2>&1 &
fi
if [ "$policy" -eq 1 ]; then
    printf '%s\n' 'Experimental C decision: candidate match, candidate nonmatch, or retry. Login is unchanged.'
    timeout 30s /usr/bin/python3 scripts/native_verification_lab.py "$1" --probe "$probe_dir/preview.bmp" --label "$label"
else
    printf '%s\n' 'Comparing with the ten-image C bank. Scores are experimental; login is unchanged.'
    timeout 30s /usr/bin/python3 scripts/native_image_lab.py "$1" --probe "$probe_dir/preview.bmp" --label "$label"
fi
printf '\nCheck complete: %s\n' "$probe_dir"
printf '%s\n' 'Paste the comparison output and directory path. View the image locally; keep it private.'
