#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
if [ "$#" -ne 1 ]; then
    printf '%s\n' 'Usage: sh scripts/test-fingerprint-round.sh PRIVATE_ENROLLMENT_DIRECTORY (no outer sudo)' >&2
    exit 2
fi
# Preserve the caller's directory so relative enrollment paths also work.
exec /usr/bin/python3 "$(dirname "$0")/test_fingerprint_round.py" "$1"
