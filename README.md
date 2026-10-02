# Synaptics Tudor Native

Experimental open-source Linux fingerprint driver for **Synaptics 06cb:00be**,
with **libfprint** and **fprintd** integration. The current development target is
the ThinkPad L14 Gen 1 AMD running Linux Mint 22.3.

**Start with the [installation guide](INSTALL.md)** for fresh installation,
upgrading, enrollment, Fingwit management, troubleshooting and rollback.

## Status

The implementation supports native pairing, authenticated encrypted capture,
automatic finger-contact handling, ten-scan enrollment, verification and
cancellation. It uses native C and OpenSSL cryptographic operations, without
loading a Windows driver DLL.

This is an **experimental driver**, not an upstream libfprint release or a
production-certified biometric implementation. Authentication error rates and
spoof resistance have not been validated. Other sensor IDs are not supported.
Keep password authentication available when trying fingerprint login.

## Installation

```sh
git clone https://github.com/Mr-TopG/synaptics-tudor-native.git
cd synaptics-tudor-native
```

Follow [INSTALL.md](INSTALL.md) for dependencies, pinned reference inputs,
building, pairing and service activation. Build on the target machine; a binary
built for Mint is not a universal Linux package. The service installer currently
requires systemd and a compatible root-run fprintd daemon.

Pairing is a separate, explicit step and may affect Windows Hello pairing on a
dual-boot machine. The service installer reuses existing pairing and does not
pair again or edit PAM. Any fingerprint-enabled login clients can use the
experimental matcher once the service is activated.

`sudo make install` installs only the diagnostic `tudor-native` command. Use the
service-installation instructions to enable the libfprint driver for fprintd.

## Normal use

After installation, run these as your normal desktop user:

```sh
fprintd-enroll -f right-index-finger
fprintd-list "$USER"
fprintd-verify -f right-index-finger
```

Enrollment requires ten accepted placements. Start with the sensor clear,
place your finger and hold still, then lift between accepted scans or retries.
Automatic capture includes a one-second settling interval.

On Mint, **Fingwit / Fingerprints** manages the same enrollments. Close it before
using a separate command-line scan, because it claims the device while open.
See the installation guide for the distinction between managing fingerprints
and enabling fingerprint login.

## Development

The diagnostic probe builds with a C11 compiler, Make and Linux userspace
headers. Pairing and encrypted capture additionally need OpenSSL 3 development
files and pkg-config. The libfprint adapter needs the dependencies listed in
the installation guide and pinned upstream libfprint 1.94.7 source.

Core checks use generated data and do not scan the sensor:

```sh
make all check
make pairing check-pairing session
make check-image-score check-verification
```

Build the optional matching adapter and its synthetic API, storage and
transport checks after obtaining the pinned source:

```sh
python3 scripts/build-libfprint-lab.py build/libfprint-v1.94.7 --matching
make service-bundle
make check-service-package
```

The service-package checks use a temporary staging root and mocked service
control. Upgrade-specific cases require the archived 0.19 bundle. These checks
do not replace hardware or biometric validation.

Version 0.20 reduces redundant reference validation and skips impossible
correlation-overlap work. It retains the matching policy, template format and
settling interval. `scripts/benchmark-verification.py` compares a frozen baseline
and candidate verification library using synthetic inputs only.

## Project layout

| Path | Purpose |
| --- | --- |
| `src/` | Native protocol, TLS, capture, decoding and experimental matching |
| `integration/libfprint/` | libfprint adapter and synthetic integration tests |
| `integration/system/` | Versioned service installation, activation and rollback |
| `scripts/` | Build tools and explicitly invoked development utilities |
| `tests/` | Core tests and optional integration checks |
| `references.lock` | Pinned upstream sources and archive hashes |
| [RESEARCH.md](RESEARCH.md) | Public implementation and protocol notes |
| [INSTALL.md](INSTALL.md) | Step-by-step installation and management |

Build products, downloaded upstream trees, local reports, fingerprint images,
private pairing keys and enrolled templates are excluded from the repository.
Do not attach biometric data or private keys to issues; diagnostic errors and
sensor/OS versions are enough to begin troubleshooting.

## License and upstream references

Project code is licensed under [MIT](LICENSE). Upstream libfprint is LGPL-2.1-or-later;
generated service bundles retain its license and corresponding source. This
repository does not redistribute the downloaded upstream source trees.

Protocol research references are pinned in [references.lock](references.lock),
including [synaTudor](https://github.com/Popax21/synaTudor) and
[libfprint](https://gitlab.freedesktop.org/libfprint/libfprint). The pairing tools
obtain checksum-verified protocol reference data separately. Private host keys
are generated locally and are never supplied by this repository.
