#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."
if [ "$#" -ne 0 ] || [ "$(id -u)" -eq 0 ]; then
    printf '%s\n' 'Run: sh scripts/collect-enrollment-samples.sh (normal user, no outer sudo)' >&2
    exit 2
fi
if [ ! -x build/tudor-capture ] || [ ! -x build/tudor-analyze ]; then
    printf '%s\n' 'First build: make capture analyze' >&2
    exit 1
fi
umask 077
batch_dir=$(mktemp -d /tmp/tudor-native-enrollment.XXXXXXXX)
printf 'Private enrollment experiment: %s\n' "$batch_dir"
printf '%s\n' 'Ten separate placements of ONE finger build the reference set.' \
    'Then three NEW placements test it, followed by three other fingers.' \
    'All 16 captures use settled contact. This does not change system login.' \
    'Lift fully between captures; place naturally with small position changes, not an extreme roll.'
printf '%s\n' 'settled-contact' > "$batch_dir/capture-timing.txt"
labels='enroll-01 enroll-02 enroll-03 enroll-04 enroll-05 enroll-06 enroll-07 enroll-08 enroll-09 enroll-10 same-01 same-02 same-03 different-01 different-02 different-03'
for label in $labels; do
    case "$label" in
        enroll-*) instruction='Use your chosen enrollment finger; lift and place it anew.' ;;
        same-*) instruction='Use the SAME finger for a fresh verification scan. This scan will NOT train the reference.' ;;
        different-*) instruction='Use a finger OTHER than the enrolled one; choose a different test finger each time.' ;;
    esac
    printf '\n%s: %s\n' "$label" "$instruction"
    printf '%s\n' 'Clear the sensor and press Enter. Then follow the settled-contact prompt.'
    IFS= read -r ready || exit 1
    if ! sudo sh scripts/capture-experimental.sh --settled > "$batch_dir/$label.json"; then
        printf 'Capture failed; stopped without retry. Keep this directory: %s\n' "$batch_dir" >&2
        exit 1
    fi
    capture_name=$(/usr/bin/python3 - "$batch_dir/$label.json" <<'PY'
import json, re, sys
with open(sys.argv[1]) as f: data=json.load(f)
name=data.get('capture_directory_name','')
if not re.fullmatch(r'tudor-native-frame-[0-9a-f]{16}',name): raise SystemExit('Invalid capture directory')
if data.get('raw_frame_received') is not True or data.get('session_closed') is not True:
    raise SystemExit('Capture incomplete or TLS not closed')
print(name)
PY
    )
    if ! sudo ./build/tudor-analyze --preview "/var/lib/$capture_name" > "$batch_dir/$label.bmp"; then
        rm -f "$batch_dir/$label.bmp"
        printf '%s\n' 'Preview failed; collection stopped. Original capture remains private.' >&2
        exit 1
    fi
    printf 'Saved %s. Remove your finger.\n' "$label"
done
printf '%s\n' 'ten-independent-scans-v1' > "$batch_dir/collection-complete.txt"
printf '\nCollection complete: %s\n' "$batch_dir"
printf '%s\n' 'Paste the directory path and any errors; keep images and pairing files private.'
