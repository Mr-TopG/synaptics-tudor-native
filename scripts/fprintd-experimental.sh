#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."
exec /usr/bin/python3 -I scripts/fprintd-lab.py "$@"
