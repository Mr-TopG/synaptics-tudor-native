#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Create a passive local systemd deployment bundle; never install or activate it."""
import hashlib
import importlib.util
import json
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent
VERSION = '0.20.0'


def main():
    spec = importlib.util.spec_from_file_location('labbuild', ROOT / 'scripts/build-libfprint-lab.py')
    lab = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lab)
    build = ROOT / 'build/libfprint-matching-build'
    # Meson strips the build RUNPATH during DESTDIR installation.
    with tempfile.TemporaryDirectory(prefix='tudor-service-stage.', dir=ROOT / 'build') as temporary:
        subprocess.run(['meson', 'install', '-C', str(build), '--destdir', temporary, '--no-rebuild'],
                       env=lab.environment(), check=True)
        libraries = list(Path(temporary).rglob('libfprint-2.so.2.0.0'))
        if len(libraries) != 1:
            raise RuntimeError('Expected one staged native library')
        library = libraries[0]
        dynamic = subprocess.check_output(['readelf', '-d', str(library)], text=True)
        if 'RPATH' in dynamic or 'RUNPATH' in dynamic:
            raise RuntimeError('Staged library retains a runtime search path')
        data = library.read_bytes()
        if b'TUDOR_FPRINTD_TEST_DEVICE' in data or b'TUDOR_SERVICE_TEST_MODE' in data:
            raise RuntimeError('Refusing to package a synthetic driver')
        if b'tudor_native_lab\0' not in data:
            raise RuntimeError('Expected the experimental native matching driver')
        destination = ROOT / 'dist' / ('tudor-native-service-' + VERSION)
        if destination.exists():
            if not (destination / '.generated-service-bundle').is_file():
                raise RuntimeError('Refusing to replace an unmarked bundle directory')
            shutil.rmtree(destination)
        payload = destination / 'payload'
        payload.mkdir(parents=True)
        (destination / '.generated-service-bundle').write_text('Local generated bundle\n')
        shutil.copyfile(library, payload / 'libfprint-2.so.2')
        shutil.copyfile(ROOT / 'integration/system/service.py', payload / 'service.py')
        shutil.copyfile(ROOT / 'integration/system/service.py', destination / 'install.py')
        shutil.copyfile(ROOT / 'INSTALL.md', destination / 'INSTALL.md')
        (destination / 'REBUILD.txt').write_text('''This is a local experimental systemd deployment bundle, not a distribution package.
Rebuild on the target architecture and distribution. Install matching development
packages for GLib/GIO, GUsb >= 0.4.8, OpenSSL >= 3, Meson, Ninja, and a C compiler.
Corresponding source is included in payload/libfprint-source.tar.gz. Extract it:
  tar xf payload/libfprint-source.tar.gz
  meson setup out libfprint-native-source -Ddrivers=tudor_native -Dintrospection=false -Ddoc=false -Dinstalled-tests=false -Dudev_rules=disabled -Dudev_hwdb=disabled -Dgtk-examples=false -Dc_args=-DTUDOR_MATCHING_LAB
  meson compile -C out
  meson test -C out tudor-matching tudor-storage tudor-transport
  DESTDIR="$PWD/staged" meson install -C out
Use the DESTDIR-installed library: it removes the development RUNPATH.
The project source archive includes the installer, tests, and packaging recipe.
The full development workflow starts from pinned unmodified upstream libfprint
1.94.7; see references.lock and scripts/build-libfprint-lab.py in that archive.
No private pairing or fingerprints are bundled. Supply your existing public
10.1-kf.tsk authority to install.py; its fixed digest is checked. Installation
alone is passive. The installed service.py activate/rollback commands manage
only the named systemd drop-in and retain all pairing and enrollment data.
Version 0.20 supports upgrading the known 0.19 installation. Its separate
activation journal retains the previous drop-in and checks the unchanged 0.19
activation record. Run the 0.20 controller's rollback to restore 0.19; the 0.19
controller can then restore the distribution service. Interrupted upgrades can
be recovered with the 0.20 rollback command. Keep both installed directories.
''')
        shutil.copyfile(ROOT / 'LICENSE', payload / 'LICENSE')
        shutil.copyfile(ROOT / 'build/libfprint-matching-source/COPYING', payload / 'COPYING.libfprint')
        # Include corresponding source for the combined LGPL library and our recipe.
        with tarfile.open(payload / 'libfprint-source.tar.gz', 'w:gz') as archive:
            archive.add(ROOT / 'build/libfprint-matching-source', arcname='libfprint-native-source',
                        filter=lambda info: None if '.git' in Path(info.name).parts else info)
        with tarfile.open(payload / 'project-source.tar.gz', 'w:gz') as archive:
            for name in ('Makefile', 'README.md', 'INSTALL.md', 'RESEARCH.md', 'LICENSE', 'references.lock',
                         'src', 'tests', 'scripts', 'integration'):
                archive.add(ROOT / name, arcname='tudor-native-' + VERSION + '/' + name,
                            filter=lambda info: None if '__pycache__' in Path(info.name).parts or
                            info.name.endswith('.pyc') else info)
        manifest = {'version': VERSION, 'architecture': platform.machine(), 'synthetic': False,
                    'files': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(payload.iterdir())}}
        (payload / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        for path in destination.rglob('*'):
            path.chmod(0o755 if path.is_dir() else 0o644)
        with tarfile.open(str(destination) + '.tar.gz', 'w:gz') as archive:
            archive.add(destination, arcname=destination.name)
        print('Built ' + str(destination))
        print('Native library has no build RUNPATH. No private pairing or sensor authority is bundled.')
        print('Built for this host ABI; rebuild on other distributions/architectures.')


if __name__ == '__main__':
    main()
