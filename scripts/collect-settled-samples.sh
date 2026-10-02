#!/bin/sh
# Dedicated entry point: makes the capture-timing experiment explicit.
set -eu
cd "$(dirname "$0")/.."
if [ "$#" -ne 0 ]; then
    printf '%s\n' 'Usage: sh scripts/collect-settled-samples.sh (normal user, no sudo)' >&2
    exit 2
fi
printf '%s\n' 'SETTLED-CONTACT EXPERIMENT: acquisition waits for finger placement and a second Enter confirmation.'
exec sh scripts/collect-matching-samples.sh --settled
