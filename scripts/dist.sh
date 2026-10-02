#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p dist
# Explicit allowlist: no reference binaries, hardware reports, or build products.
tar --exclude='__pycache__' --exclude='*.pyc' --transform='s,^,tudor-native-0.20.0/,' \
    -czf dist/tudor-native-0.20.0.tar.gz \
    Makefile README.md INSTALL.md RESEARCH.md LICENSE references.lock src tests scripts integration
printf '%s\n' 'Created dist/tudor-native-0.20.0.tar.gz (experimental system-service deployment; biometric accuracy unvalidated)'
