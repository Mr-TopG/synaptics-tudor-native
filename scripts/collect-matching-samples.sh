#!/bin/sh
# Development data only. No enrollment, authentication, or login changes.
set -eu
cd "$(dirname "$0")/.."
if [ "$(id -u)" -eq 0 ]; then
    printf '%s\n' 'Run WITHOUT an outer sudo; the script invokes sudo for protected sensor access.' >&2
    exit 1
fi
if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != --settled ]; }; then
    printf '%s\n' 'Usage: sh scripts/collect-matching-samples.sh [--settled]' >&2
    exit 2
fi
if [ ! -x build/tudor-capture ] || [ ! -x build/tudor-analyze ]; then
    printf '%s\n' 'First build as your normal user: make capture analyze' >&2
    exit 1
fi
umask 077
batch_dir=$(mktemp -d /tmp/tudor-native-matching.XXXXXXXX)
capture_timing=before-contact
if [ "$#" -eq 1 ]; then capture_timing=settled-contact; fi
printf '%s\n' "$capture_timing" > "$batch_dir/capture-timing.txt"
printf 'Private sample directory: %s\n' "$batch_dir"
printf 'Capture timing: %s\n' "$capture_timing"
printf '%s\n' 'This collects two independent placements of the SAME finger, then one DIFFERENT finger.' \
    'It saves local development images only. It does not enroll fingerprints or change login.'
for label in same-1 same-2 different-1; do
    case "$label" in
        same-1) instruction='Choose one finger for the first sample.' ;;
        same-2) instruction='Use that SAME finger again, lifting and placing it naturally for a new sample.' ;;
        different-1) instruction='Use a DIFFERENT finger for the final sample.' ;;
    esac
    printf '\n%s: %s\n' "$label" "$instruction"
    printf '%s\n' 'Keep the sensor clear, press Enter when ready, then touch ONLY at the capture prompt.'
    IFS= read -r ready || exit 1
    if ! sudo sh scripts/capture-experimental.sh "$@" > "$batch_dir/$label.json"; then
        printf '%s\n' 'Capture failed; collection stopped without retry. Paste the terminal error.' >&2
        exit 1
    fi
    # Validate the generated directory name before constructing a privileged read path.
    capture_name=$(/usr/bin/python3 - "$batch_dir/$label.json" <<'PY'
import json, re, sys
with open(sys.argv[1]) as f: data=json.load(f)
name=data.get('capture_directory_name', '')
assert re.fullmatch(r'tudor-native-frame-[0-9a-f]{16}', name)
assert data.get('raw_frame_received') is True and data.get('session_closed') is True
print(name)
PY
    )
    if ! sudo ./build/tudor-analyze --preview "/var/lib/$capture_name" > "$batch_dir/$label.bmp"; then
        rm -f "$batch_dir/$label.bmp"
        printf '%s\n' 'Preview validation failed; collection stopped. The original private capture remains intact.' >&2
        exit 1
    fi
    printf 'Saved %s; remove your finger now.\n' "$label"
done
printf '\nCollection complete: %s\n' "$batch_dir"
printf '%s\n' 'Paste this final directory path and any errors. Keep the image files private.'
