#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
case "${1:-}" in
    --install-only|--activate|--rollback|--status) [ "$#" -eq 1 ] || exit 2 ;;
    *) echo 'Usage: sudo sh scripts/install-fprintd-experimental.sh --install-only | --activate | --rollback | --status' >&2; exit 2 ;;
esac
[ "$(id -u)" -eq 0 ] || { echo 'Run with sudo for installation and service control.' >&2; exit 1; }
cd "$(dirname "$0")/.."
project=$(pwd -P)
installed=/usr/local/lib/tudor-native/0.20.0/service.py
case "$1" in
    --rollback) exec /usr/bin/python3 -I "$installed" rollback ;;
    --status) exec /usr/bin/python3 -I "$installed" status ;;
esac
bundle="$project/dist/tudor-native-service-0.20.0"
[ -f "$bundle/payload/manifest.json" ] || { echo 'Build scripts/build-service-bundle.py as your normal user first.' >&2; exit 1; }
umask 077
ulimit -c 0
/usr/bin/python3 -I "$bundle/install.py" install --bundle "$bundle/payload" \
    --authority "$project/upstream/synaTudor-rev/pydrv/tudor/sensor/sensor_keys/10.1-kf.tsk"
if [ "$1" = --activate ]; then
    echo 'Switching system fprintd to the experimental native driver. Finish any other fingerprint operation first.'
    echo 'Reuses the ten-scan fprintd enrollment. Normal system Polkit applies; no PAM files are edited.'
    echo 'Rollback from 0.20 restores any previous managed 0.19 installation.'
    exec /usr/bin/python3 -I "$installed" activate
fi
