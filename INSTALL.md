# Install the native ThinkPad fingerprint driver

This guide installs **tudor-native 0.20** for the Synaptics **06cb:00be** sensor.
It connects the sensor to **libfprint → fprintd → your fingerprint settings app**.
You can enroll, verify and manage fingerprints using normal Linux tools.

Tested on a ThinkPad L14 Gen 1 AMD running **Linux Mint 22.3**. The driver remains
experimental: successful scans do not establish production authentication accuracy.

## Choose your starting point

| Your situation | Start here |
| --- | --- |
| Version 0.20 already works on your computer | [Everyday use](#everyday-use). Nothing to reinstall. |
| You have the working 0.19 driver and the prepared 0.20 bundle | [Upgrade from 0.19](#upgrade-from-019). Keep your enrollment. |
| This computer has never used this native driver | [Fresh installation](#fresh-installation). |
| You use a different Linux distribution | Read [Other distributions](#other-distributions) first. |

**Use a normal terminal. Run only commands beginning with `sudo` as root.**
Run each block separately and continue only when it succeeds. Do not paste the
terminal prompt (`alex@...$`) along with a command.

## Upgrade from 0.19

Close Fingwit/Fingerprints and finish any fingerprint scan. Open your existing
project folder; on the development computer it is:

```sh
cd "$HOME/Documents/thinkpad_l14g1"
sudo sh scripts/install-fprintd-experimental.sh --activate
```

This requires the prepared `dist/tudor-native-service-0.20.0` bundle. It keeps
your pairing and enrolled fingers, and retains 0.19 for rollback. Do not repeat
pairing or enrollment. If the installer reports that 0.20 is already activated,
use its status command below; do not reinstall just to clear that message.

```sh
fprintd-list "$USER"
fprintd-verify -f right-index-finger
```

Use the finger name shown by `fprintd-list` if it differs. Expect
`verify-match (done)` with your enrolled finger.

## Fresh installation

### 1. Get the project source

Clone the [source repository](https://github.com/Mr-TopG/synaptics-tudor-native).
On Mint, install Git first if needed:

```sh
sudo apt update
sudo apt install git
mkdir -p "$HOME/Documents"
git clone https://github.com/Mr-TopG/synaptics-tudor-native.git "$HOME/Documents/synaptics-tudor-native"
cd "$HOME/Documents/synaptics-tudor-native"
```

If you already have the project folder, open it instead. You can also use
GitHub's **Code → Download ZIP**, extract it and open a terminal in that folder.
A supplied `tudor-native-0.20.0.tar.gz` source archive works too. The separate
service bundle alone does not contain the ready-to-run fresh-install workspace.

**All remaining commands in this section run from the source folder.** Keep it
until installation is complete. The repository contains source; step 5 builds
the service bundle for your system.

### 2. Install the build tools and fprintd

On Linux Mint 22.x / Ubuntu 24.04 package bases:

```sh
sudo apt update
sudo apt install build-essential pkg-config python3 git curl ca-certificates \
    usbutils binutils meson ninja-build libglib2.0-dev libgusb-dev \
    libjson-glib-dev libssl-dev fprintd
```

The package recipe uses the Mint development host's dependencies. Hardware
validation is on Mint 22.3; it is not a claim that every Ubuntu installation has
been tested.

### 3. Check your sensor

```sh
lsusb -d 06cb:00be
make all check
sudo ./build/tudor-native probe
```

The first command must show a device. The probe should report `usb_id` as
`06cb:00be`. The supported profile is firmware **10.1**, `product_id: 65`,
`provision_state: 3`, `advanced_security: true`, and `key_flag: true`. The tested
full firmware version is **10.1.3077709**. Other firmware builds are unvalidated.

If the ID/profile differs or the probe fails, stop here and keep its output for
diagnosis. Laptops with the same model name can have different sensors.
`authentication_supported: false` in this diagnostic tool is expected; the
libfprint integration supplies enrollment and verification later.

### 4. Get the pinned build inputs

These commands download upstream libfprint and the reference protocol data.
They do not install a Windows driver or run the reference driver's code.

```sh
mkdir -p upstream build
git clone --branch rev https://github.com/Popax21/synaTudor.git upstream/synaTudor-rev
git -C upstream/synaTudor-rev checkout --detach bc5a6d0e20df7a2c3edd8b40bc602801932ba40d
sha256sum -c scripts/reference-keys.sha256
```

Both reference files must report `OK`. If this checkout already exists, skip
the clone and verify its pinned commit and file checksums instead.

```sh
curl --fail --location --output build/libfprint-v1.94.7.tar.gz \
    https://gitlab.freedesktop.org/libfprint/libfprint/-/archive/v1.94.7/libfprint-v1.94.7.tar.gz
printf '%s\n' '6d2cc09c72f86865b49a911690b43e363aed7595b66e6599232a572ccce95342  build/libfprint-v1.94.7.tar.gz' | sha256sum -c -
```

Only after the archive reports `OK`, extract it:

```sh
tar -xzf build/libfprint-v1.94.7.tar.gz -C build
```

The pinned URLs, commit and archive digest are also recorded in `references.lock`.

### 5. Build and check the driver

```sh
make pairing check-pairing session
python3 scripts/build-libfprint-lab.py build/libfprint-v1.94.7 --matching
make service-bundle
make check-service-package
```

Continue when the builds and tests pass. This creates
`dist/tudor-native-service-0.20.0/` for your computer's architecture and libraries.
No sensor pairing or system-service switch happens during these commands.
Upgrade-specific tests can be skipped when the old 0.19 bundle is absent.

### 6. Pair once and check the encrypted connection

**Pairing changes the sensor's host pairing and may affect Windows Hello on a
dual-boot computer.** It creates your private identity under
`/var/lib/tudor-native-pairing-v1`.

Check whether this native driver already has pairing state:

```sh
sudo sh -c 'if [ -d /var/lib/tudor-native-pairing-v1 ]; then echo "Existing pairing: skip PAIR"; else echo "No pairing directory"; fi'
```

If the directory exists, skip the following PAIR command. If the check succeeds
and says `No pairing directory`, run it once:

```sh
sudo sh scripts/pair-experimental.sh
```

Success reports `pairing_response_verified: true` and
`sensor_certificate_verified: true`. Then test the saved pairing:

```sh
sudo sh scripts/session-experimental.sh
```

Expect `tls_established_and_verified: true` and `session_closed: true`.
If pairing or the session fails, stop and retain the output and existing state;
do not delete the pairing directory or repeat PAIR to try to fix it. Close any
other fingerprint program before these checks.

### 7. Activate the driver for fprintd

Close any fingerprint settings app or scan, then run:

```sh
sudo sh scripts/install-fprintd-experimental.sh --activate
fprintd-list "$USER"
```

Expect one device named **Synaptics Tudor experimental enrollment/verification
lab**. On a fresh installation, having no enrolled fingers yet is normal.

The installer places the library in `/usr/local/lib/tudor-native/0.20.0/` and
switches system fprintd to use it. It leaves the distribution's libfprint file
in place. It does not edit PAM/login settings. Already-enabled fingerprint
login clients can use this service immediately.

### 8. Enroll your finger and test it

Run these as your normal desktop user, **without sudo**, so enrollment belongs
to your account:

```sh
fprintd-enroll -f right-index-finger
```

Keep the sensor clear initially. Place your right index finger and hold still
for at least one second. After each `enroll-stage-passed`, lift it completely
and place it again. Repeat until **`enroll-completed`**. Enrollment needs ten
accepted scans; retry messages can mean extra placements. No Enter presses are
needed. Use a different valid finger name if you prefer another finger.

```sh
fprintd-list "$USER"
fprintd-verify -f right-index-finger
```

Expect **`verify-match (done)`** with the enrolled finger. Run verification
again with a different finger; expect **`verify-no-match (done)`**.
`verify-retry-scan (not done)` means lift and scan again; the operation continues.
These are initial functionality checks, not biometric accuracy certification.

### 9. Manage fingerprints in Mint's GUI

Open **Fingerprints** from the application menu, or run:

```sh
fingwit
```

If Fingwit is not installed on Mint, install it from Software Manager or run
`sudo apt install fingwit`. Select a finger to add or delete its enrollment.
Fingwit uses the same fprintd service and saved fingers as the terminal tools.

If Fingwit shows an **Enable** action, using it enables Mint's fingerprint
authentication profile and changes login configuration. Do this when you want
fingerprint login, after verifying the driver; retain password authentication.
Terminal enrollment and verification do not require enabling that profile.

Close Fingwit before running a separate verification command: the app claims
the sensor while open. After setup, check verification again following a reboot
and suspend/wake on your own system.

## Everyday use

Keep your finger off the sensor when a scan starts, then place it firmly and
hold still. The driver deliberately allows one second for the finger to settle.
Lift fully between enrollment samples or retries. Press Ctrl+C to cancel a
terminal scan, and let cleanup finish before starting another.

There is no need to run development capture scripts or pair again for normal
use. `sudo make install` installs only the diagnostic probe; it is not the
fprintd driver installation command.

## Troubleshooting

| What you see | What to do |
| --- | --- |
| `No devices available` | Recheck `lsusb -d 06cb:00be`, installation status and service logs below. |
| Device busy or already claimed | Close Fingwit and other fingerprint clients, then retry. |
| `verify-retry-scan` | Lift completely, replace the finger and hold still. |
| No enrolled fingers | Enroll as your normal user, without sudo. |
| Permission denied / authorization failure | Use a local desktop session and respond to the normal Polkit prompt; do not enroll as root to bypass it. |
| Checksum mismatch or unsupported profile | Stop and check the downloaded inputs or sensor details; do not bypass the check. |
| `Different payload already installed at this version` | Keep the installed bundle. Rebuilding 0.20 can change archive hashes; do not delete installed files to force a replacement. |
| `Interrupted or existing activation detected` | If 0.20 already works, use status. If activation failed/interrupted, use rollback below before retrying activation. |

Status and recent logs work even after you close or move the source folder:

```sh
sudo /usr/bin/python3 -I /usr/local/lib/tudor-native/0.20.0/service.py status
sudo journalctl -u fprintd -b -n 80 --no-pager
```

fprintd may stop when idle and start again on demand; an idle service alone is
not evidence of failure. Keep fingerprint images, templates and pairing keys
private. Share only relevant terminal errors when asking for help.

## Roll back

Close fingerprint apps and finish any scan, then run:

```sh
sudo /usr/bin/python3 -I /usr/local/lib/tudor-native/0.20.0/service.py rollback
```

After a 0.19 → 0.20 upgrade, this restores 0.19. After a fresh 0.20 installation,
it restores the distribution's fprintd configuration. Pairing, enrollment and
installed files are retained. Rollback does not undo a pairing change on the
sensor or a login profile you enabled through Fingwit.

To return all the way to the distribution configuration after restoring 0.19:

```sh
sudo /usr/bin/python3 -I /usr/local/lib/tudor-native/0.19.0/service.py rollback
```

## Other distributions

Build from source on the target machine. The Mint-built service bundle is not
a universal Linux binary. Equivalent development dependencies are:

| Component | Requirement |
| --- | --- |
| Compiler and tools | C11/C++, Make, pkg-config, Python 3, Meson >= 0.56, Ninja, binutils |
| Libraries | GLib/GIO >= 2.68, GUsb >= 0.4.8, JSON-GLib, OpenSSL >= 3 |
| Service integration | systemd, system D-Bus/Polkit, compatible root-run fprintd with file storage and `STATE_DIRECTORY` support |
| Reference inputs | The pinned libfprint 1.94.7 archive and protocol data in step 4 |

Install those dependencies using your distribution's package manager, then use
the build steps above. The installer checks for fprintd at `/usr/libexec/fprintd`
or `/usr/lib/fprintd/fprintd` and refuses unsupported storage configurations.
Different service layouts or SELinux/AppArmor policies may require distribution
integration work; disabling those protections is not part of this guide.

Use your desktop's fprintd-compatible fingerprint settings if Fingwit is not
available. Non-systemd installations need a different service integration.
Other distributions and architectures have not yet been validated by this project.

For protocol details, development tools and test history, see [README.md](README.md).
