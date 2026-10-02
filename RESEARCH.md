# Implementation notes

This document describes the implementation and public protocol references.
Local hardware transcripts, fingerprint comparison results and development
reports are not part of the repository.

## Scope and source references

The driver accepts USB ID `06cb:00be` and the researched firmware 10.1,
product `0x41`, provision-state 3, advanced-security/key-flag profile. Other
profiles are rejected rather than guessing an alternate pairing or protocol.
See `src/protocol.c`, `src/pair.c` and the libfprint backend for these checks.

`references.lock` pins research checkouts and the upstream libfprint 1.94.7
archive. The builder adds the native adapter to a generated copy of that
upstream tree. It does not modify the downloaded original tree or install a
Windows driver. The pairing wrapper verifies the two reference-data hashes in
`scripts/reference-keys.sha256` before use.

## Pairing and encrypted sessions

Pairing creates a fresh P-256 host identity and persists it before issuing the
sensor PAIR command. The response must agree with the generated identity and
validate against the selected sensor authority before state is marked complete.
An existing state directory prevents automatic re-pairing.

The native session implements the sensor's TLS framing with P-256,
AES-256-GCM and SHA-384. It accepts the Tudor ServerHello version marker while
validating record framing, certificates, Finished proofs and authenticated
application records. Malformed or unverified responses are rejected.

No sensor erase, storage-format or firmware-update path is provided by the
installation flow. Pairing, enrollment and service activation are separate
operations. Cancellation waits for capture and encrypted-session cleanup.

## Capture and decoding

Automatic contact requires a clear sensor, a fresh touch and a configured interval of
settling without a removal event. Contact polling and acquisition have bounded
waits. Contact failures and cancellations use the same cleanup path as normal
capture.

The 0.21 service explicitly selects 500 ms; the native API accepts 500 or
1000 ms, while zero/default selects 1000 ms. Other explicit values are rejected before any
device command. A removal/re-touch restarts the full selected interval; changing
the interval does not bypass the clear-sensor requirement or error cleanup.

The supported frame layout has 104 by 86 signed little-endian 16-bit samples
in column-major storage. The response includes a ten-byte header with a pixel
length field at offset eight. The decoder checks lengths and dimensions before
converting the samples to an eight-bit grayscale image.

The libfprint path decodes in memory. Separate development utilities can save
captures only when explicitly invoked. Those files are local biometric data
and must not be committed or uploaded.

## Experimental matching

Enrollment requires ten quality-checked, byte-distinct grayscale reference
images. Matching applies high-pass filtering and searches relative rotations
and translations using normalized correlation and a minimum overlap. The
quality and decision policy is implemented in `src/image_quality.c` and
`src/verification.c`; it is not a calibrated population-level accuracy model.

Version 0.20 uses an early-exit self-check when reference insertion needs only
a usability result. Full probe comparisons retain maximum-score selection.
Correlation loops clip to the stride-aligned intersection and skip translations
that cannot reach the minimum overlap, preserving the remaining arithmetic order.

Version 0.21 shares filtered/rotated probe data between reference
comparisons during one operation. Up to four workers process disjoint reference
slots; the caller joins all workers before reducing scores in reference order.
All ten references must still be usable under the decision policy. Resource
failures fall back to serial computation. The prepared probe is read-only during
comparison and wiped after the join; no persistent image cache is added.

Prepared comparison results are checked against a frozen 0.20 library, including
exact floating-point bits and unchanged failure outputs. Synthetic tests also
exercise partial/all worker-creation failure, preprocessing allocation failure
and simultaneous readers. These checks do not establish population-level
biometric accuracy or guarantee capture quality for every placement.

Synthetic checks exercise constants, sparse inputs, periodic fields, translated
images, noise, malformed sizes, duplicate enrollment and cleanup. Synthetic
positive/negative controls cannot establish biometric security or spoof resistance.

## libfprint and fprintd

The adapter runs bounded GUsb operations in a worker context and reports
completion and contact status to libfprint's main context. Enrollment and
verification use an experimental serialized ten-image template. Such templates
contain biometric data and remain in the local private store.

The system-service bundle installs a versioned library and launch helper,
retaining the distribution library. A managed systemd drop-in selects the
private library for fprintd. Normal system D-Bus and Polkit authorization apply.
The installer checks the daemon's file-storage override support before activation.

Activation records the previous managed configuration. Upgrade rollback restores
the supported earlier version; fresh-install rollback restores the distribution
configuration. Pairing and templates are retained. Rollback does not undo sensor
pairing or login settings enabled separately through a desktop application.

See [INSTALL.md](INSTALL.md) for commands, prerequisites and recovery instructions.
