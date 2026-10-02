#!/usr/bin/python3
# SPDX-License-Identifier: MIT
"""Install and explicitly activate the experimental native fprintd library.

Installation is passive. Activation changes only our named systemd drop-in;
rollback restores the prior service version and retains pairing/enrollment data.
"""
import argparse
import configparser
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import shutil
import stat
import subprocess
import sys
import tempfile

VERSION = '0.21.0'
PREVIOUS_VERSION = '0.20.0'
PREFIX = Path('/usr/local/lib/tudor-native') / VERSION
PREVIOUS_PREFIX = PREFIX.parent / PREVIOUS_VERSION
DROPIN = Path('/etc/systemd/system/fprintd.service.d/90-tudor-native.conf')
STATE = Path('/var/lib/tudor-native-service')
JOURNAL = STATE / ('activation-' + VERSION + '.json')
PREVIOUS_JOURNAL = STATE / 'activation-0.20.0.json'
OLDER_JOURNALS = {'0.20.0': PREVIOUS_JOURNAL, '0.19.0': STATE / 'activation.json'}
STORE = Path('/var/lib/tudor-native-fprintd-lab-v1')
PAIRING = Path('/var/lib/tudor-native-pairing-v1')
AUTHORITY_HASH = '45d9be106f46857f50443d5cb30ca82173b12534f6bec28e854d383fccf6ec4e'
PAYLOAD_NAMES = {'libfprint-2.so.2', 'service.py', 'LICENSE', 'COPYING.libfprint',
                 'libfprint-source.tar.gz', 'project-source.tar.gz'}
# Recognize only the released previous controller, including its rollback format.
OLDER_CONTROLLER_HASHES = {
    '0.20.0': 'b3152148cf27a78f012eeffc2436828fe2f2df3ab63acde50b6c5f19f479ab0c',
    '0.19.0': '59003a4e6cdaf6b7f6f851d60438019e845354597dcd8d51dd9886568b6aaa1a',
}


def dropin_text(version):
    return f'''# Managed by tudor-native-service {version}; remove with rollback.
[Service]
ExecStart=
ExecStart=/usr/bin/python3 -I {PREFIX.parent / version}/service.py launch
StateDirectory=
StateDirectory=tudor-native-fprintd-lab-v1
StateDirectoryMode=0700
UMask=0077
LimitCORE=0
TimeoutStopSec=45
'''


DROPIN_TEXT = dropin_text(VERSION)
PREVIOUS_DROPIN_TEXT = dropin_text(PREVIOUS_VERSION)


def digest(data):
    return hashlib.sha256(data).hexdigest()


class Controller:
    def __init__(self, root=Path('/'), systemctl=None):
        self.root = root.resolve()
        self.uid = os.geteuid()
        self.systemctl = systemctl or self.real_systemctl

    def path(self, path):
        return self.root / str(path).lstrip('/')

    def real_systemctl(self, *arguments, check=True):
        if self.root != Path('/'):
            raise RuntimeError('Service control is forbidden for a staging root')
        result = subprocess.run(['/usr/bin/systemctl', *arguments, 'fprintd.service']
                              if arguments[0] != 'daemon-reload' else
                              ['/usr/bin/systemctl', 'daemon-reload'],
                              check=False, capture_output=True, text=True, timeout=60)
        if check and result.returncode:
            raise RuntimeError('systemctl ' + ' '.join(arguments) + ': ' +
                               (result.stderr or result.stdout).strip())
        return result

    def directory(self, path, mode=0o755):
        """Reject symlink components and group/other-writable install parents."""
        current = self.root
        for part in Path(path).parts[1:]:
            current /= part
            try:
                info = current.lstat()
            except FileNotFoundError:
                try:
                    current.mkdir(mode=mode if current == self.path(path) else 0o755)
                except FileExistsError:
                    pass
                info = current.lstat()
            if (not stat.S_ISDIR(info.st_mode) or info.st_uid != self.uid or
                    stat.S_IMODE(info.st_mode) & 0o022):
                raise RuntimeError('Unsafe directory: ' + str(current))
        if mode == 0o700 and stat.S_IMODE(current.stat().st_mode) != 0o700:
            raise RuntimeError('Expected private directory: ' + str(current))
        return current

    def regular(self, path, private=False):
        fd = os.open(self.path(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            info = os.fstat(fd)
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != self.uid or
                    info.st_nlink != 1 or info.st_mode & (0o077 if private else 0o022)):
                raise RuntimeError('Unsafe file: ' + str(path))
            with os.fdopen(fd, 'rb', closefd=False) as stream:
                return stream.read()
        finally:
            os.close(fd)

    def atomic(self, path, data, mode=0o600):
        parent = self.directory(path.parent)
        fd, name = tempfile.mkstemp(prefix='.tudor-', dir=parent)
        try:
            with os.fdopen(fd, 'wb') as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
                os.fchmod(stream.fileno(), mode)
            os.replace(name, self.path(path))
            self.sync_directory(parent)
        finally:
            if os.path.exists(name):
                os.unlink(name)

    @staticmethod
    def sync_directory(path):
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)

    def installed(self, version=VERSION):
        prefix = PREFIX.parent / version
        if not self.path(prefix).exists():
            raise RuntimeError('Service version is not installed: ' + version)
        self.directory(prefix)
        manifest = json.loads(self.regular(prefix / 'manifest.json'))
        if (not isinstance(manifest, dict) or
                manifest.get('version') != version or manifest.get('synthetic') is not False or
                manifest.get('architecture') != platform.machine() or
                set(manifest.get('files', {})) != PAYLOAD_NAMES):
            raise RuntimeError('Unexpected installed version')
        for name, expected in manifest['files'].items():
            if '/' in name or name in ('.', '..', ''):
                raise RuntimeError('Invalid manifest path')
            if digest(self.regular(prefix / name)) != expected:
                raise RuntimeError('Installed file checksum mismatch: ' + name)
        if (version in OLDER_CONTROLLER_HASHES and
                manifest['files']['service.py'] != OLDER_CONTROLLER_HASHES[version]):
            raise RuntimeError('Unknown previous service controller')
        return manifest

    def install(self, bundle, authority):
        manifest = json.loads((bundle / 'manifest.json').read_text())
        if manifest.get('version') != VERSION or manifest.get('synthetic'):
            raise RuntimeError('Expected a hardware service bundle for ' + VERSION)
        if manifest.get('architecture') != platform.machine():
            raise RuntimeError('Bundle architecture differs; rebuild on the target host')
        names = PAYLOAD_NAMES
        if set(manifest['files']) != names:
            raise RuntimeError('Unexpected package contents')
        contents = {}
        for name in names:
            path = bundle / name
            if path.is_symlink() or not path.is_file():
                raise RuntimeError('Package payload must contain regular files')
            contents[name] = path.read_bytes()
            if digest(contents[name]) != manifest['files'][name]:
                raise RuntimeError('Package checksum mismatch: ' + name)
        fd = os.open(authority, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_size != 256:
                raise RuntimeError('Expected the 256-byte public sensor authority file')
            public = os.read(fd, 257)
        finally:
            os.close(fd)
        if digest(public) != AUTHORITY_HASH:
            raise RuntimeError('Sensor public authority checksum mismatch')
        parent = self.directory(PREFIX.parent)
        if self.path(PREFIX).exists():
            self.installed()
            if self.regular(PREFIX / 'manifest.json') != (bundle / 'manifest.json').read_bytes():
                raise RuntimeError('Different payload already installed at this version')
        else:
            temp = Path(tempfile.mkdtemp(prefix='.install-', dir=parent))
            try:
                for name, data in contents.items():
                    (temp / name).write_bytes(data)
                    (temp / name).chmod(0o644)
                (temp / 'manifest.json').write_bytes((bundle / 'manifest.json').read_bytes())
                (temp / 'manifest.json').chmod(0o644)
                temp.chmod(0o755)
                os.rename(temp, self.path(PREFIX))
                self.sync_directory(parent)
            finally:
                if temp.exists():
                    shutil.rmtree(temp)
        self.directory(STATE, 0o700)
        self.atomic(STATE / 'sensor-authority.tsk', public)
        print('Installed passive native library. No service or PAM configuration changed.')

    def preflight(self):
        self.installed()
        if digest(self.regular(STATE / 'sensor-authority.tsk', private=True)) != AUTHORITY_HASH:
            raise RuntimeError('Installed sensor authority mismatch')
        if not self.path(PAIRING).exists():
            raise RuntimeError('Existing native pairing is required; no re-pairing is performed')
        self.directory(PAIRING, 0o700)
        # Check existence/ownership only: the C backend validates keys/certificates.
        if not any(self.path(PAIRING).iterdir()):
            raise RuntimeError('Existing native pairing is required; no re-pairing is performed')
        self.directory(STORE, 0o700)
        config = configparser.ConfigParser()
        config.read(self.path('/etc/fprintd.conf'))
        if config.get('storage', 'type', fallback='file') != 'file':
            raise RuntimeError('fprintd must use file storage')
        for name in ('/usr/libexec/fprintd', '/usr/lib/fprintd/fprintd'):
            if self.path(name).is_file():
                if b'STATE_DIRECTORY\0' not in self.regular(Path(name)):
                    raise RuntimeError('fprintd lacks the separate storage override')
                return name
        raise RuntimeError('Distribution fprintd executable not found')

    def managed_version(self):
        if self.path(DROPIN).exists() or self.path(DROPIN).is_symlink():
            data = self.regular(DROPIN)
            for version in (VERSION, *OLDER_JOURNALS):
                if data == dropin_text(version).encode():
                    return version
            raise RuntimeError('Refusing to modify an unknown or edited fprintd drop-in')
        return None

    def older_journal_present(self, versions=OLDER_JOURNALS):
        return any(self.path(OLDER_JOURNALS[v]).exists() or
                   self.path(OLDER_JOURNALS[v]).is_symlink() for v in versions)

    def validate_previous(self, previous, allowed):
        if (not isinstance(previous, dict) or
                set(previous) != {'version', 'dropin', 'activation_record'} or
                previous['version'] not in allowed or
                previous['dropin'] != dropin_text(previous['version']) or
                previous['activation_record'] != self.previous_activation(previous['version'])):
            raise RuntimeError('Previous activation changed; refusing to overwrite it')

    def previous_activation(self, version=PREVIOUS_VERSION):
        """Validate the complete older rollback chain without changing its journals."""
        self.installed(version)
        data = self.regular(OLDER_JOURNALS[version], private=True)
        record = json.loads(data)
        keys = {'version', 'was_active'}
        if version == '0.20.0':
            keys.add('previous')
        if (not isinstance(record, dict) or set(record) != keys or
                record.get('version') != version or
                type(record.get('was_active')) is not bool):
            raise RuntimeError('Invalid previous activation record')
        if version == '0.20.0':
            if record['previous'] is not None:
                self.validate_previous(record['previous'], ('0.19.0',))
            elif self.older_journal_present(('0.19.0',)):
                raise RuntimeError('Unexpected previous activation record')
        return data.decode('utf-8')

    def activation_record(self):
        record = json.loads(self.regular(JOURNAL, private=True))
        if (not isinstance(record, dict) or set(record) != {'version', 'was_active', 'previous'} or
                record.get('version') != VERSION or
                type(record.get('was_active')) is not bool):
            raise RuntimeError('Invalid activation record')
        previous = record['previous']
        if previous is not None:
            try:
                self.validate_previous(previous, OLDER_JOURNALS)
            except (RuntimeError, ValueError, OSError) as error:
                raise RuntimeError('Previous activation changed; refusing to overwrite it') from error
        elif self.older_journal_present():
            raise RuntimeError('Unexpected previous activation record')
        return record

    def activate(self):
        self.preflight()
        if self.root == Path('/'):
            # Resolve the package's dependencies before touching the running daemon.
            subprocess.run(['/usr/bin/python3', '-I', '-c',
                            'import ctypes,sys; ctypes.CDLL(sys.argv[1])',
                            str(PREFIX / 'libfprint-2.so.2')],
                           env={'PATH': '/usr/bin:/bin'}, check=True, timeout=15)
        managed = self.managed_version()
        if self.path(JOURNAL).exists() or self.path(JOURNAL).is_symlink():
            raise RuntimeError('Interrupted or existing activation detected; run this version\'s rollback first')
        if managed == VERSION:
            raise RuntimeError('Already activated; use status or rollback')
        previous = None
        if managed in OLDER_JOURNALS:
            # An unrelated newer journal indicates an interrupted upgrade.
            if managed == '0.19.0' and self.older_journal_present(('0.20.0',)):
                raise RuntimeError('Interrupted activation detected for 0.20.0; run its rollback first')
            previous = {'version': managed, 'dropin': dropin_text(managed),
                        'activation_record': self.previous_activation(managed)}
        elif self.older_journal_present():
            raise RuntimeError('Interrupted activation detected; run the older controller rollback first')
        was_active = self.systemctl('is-active', check=False).returncode == 0
        record = json.dumps({'version': VERSION, 'was_active': was_active,
                             'previous': previous}).encode()
        self.atomic(JOURNAL, record)
        try:
            self.systemctl('stop')
            self.atomic(DROPIN, DROPIN_TEXT.encode(), 0o644)
            self.systemctl('daemon-reload')
            self.systemctl('start')
            self.systemctl('is-active')
        except Exception as error:
            try:
                self.rollback()
            except Exception as recovery:
                raise RuntimeError(f'Activation failed ({error}); rollback also failed ({recovery}). Run rollback again.') from error
            raise RuntimeError('Activation failed; previous service configuration restored: ' + str(error)) from error
        print('Experimental native fprintd activated on the system bus. Normal Polkit applies.')
        print('Existing fingerprint-enabled login clients can now use this experimental matcher; no PAM files were edited.')

    def rollback(self):
        managed = self.managed_version()
        journal = self.path(JOURNAL)
        if not journal.exists() and not journal.is_symlink():
            if managed == VERSION:
                raise RuntimeError('Managed drop-in has no activation record; refusing to guess prior state')
            print('No ' + VERSION + ' activation to roll back; existing service configuration retained.')
            return
        record = self.activation_record()
        previous = record['previous']
        if previous is None and managed in OLDER_JOURNALS:
            raise RuntimeError('Unexpected previous drop-in; refusing to modify it')
        self.systemctl('stop')
        if previous is not None:
            self.atomic(DROPIN, previous['dropin'].encode(), 0o644)
        elif managed:
            self.path(DROPIN).unlink()
            self.sync_directory(self.path(DROPIN.parent))
        self.systemctl('daemon-reload')
        if record['was_active']:
            self.systemctl('start')
            self.systemctl('is-active')
        journal.unlink()
        self.sync_directory(journal.parent)
        restored = previous['version'] if previous else 'Distribution'
        print(restored + ' fprintd configuration restored. Pairing and templates retained.')

    def launch(self):
        if self.root != Path('/'):
            raise RuntimeError('Cannot launch a hardware daemon in a staging root')
        daemon = self.preflight()
        lock = os.open(STORE, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        os.set_inheritable(lock, True)
        os.umask(0o077)
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'STATE_DIRECTORY': str(STORE),
               'LD_LIBRARY_PATH': str(PREFIX), 'FP_DRIVERS_ALLOWLIST': 'tudor_native_lab',
               'TUDOR_NATIVE_EXPERIMENTAL': '1', 'TUDOR_NATIVE_MATCHING_EXPERIMENTAL': '1',
               'TUDOR_NATIVE_SETTLE_MS': '500',
               'TUDOR_NATIVE_AUTOMATIC_CONTACT': '1', 'TUDOR_NATIVE_PAIRING_DIR': str(PAIRING),
               'TUDOR_NATIVE_AUTHORITY_FILE': str(STATE / 'sensor-authority.tsk')}
        os.execve(daemon, [daemon], env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('install', 'activate', 'rollback', 'status', 'launch'))
    parser.add_argument('--bundle', type=Path)
    parser.add_argument('--authority', type=Path)
    parser.add_argument('--root', type=Path, default=Path('/'), help='Staged installation only; never controls system services')
    args = parser.parse_args()
    if args.root.resolve() == Path('/') and os.geteuid() != 0:
        parser.error('Run with sudo for system installation or service control')
    if args.root.resolve() != Path('/') and args.command != 'install':
        parser.error('--root supports passive staged installation only')
    controller = Controller(args.root)
    os.umask(0o077)
    lock = None
    try:
        if args.command in ('install', 'activate', 'rollback'):
            controller.directory(STATE, 0o700)
            lock = os.open(controller.path(STATE), os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.command == 'install':
            if not args.bundle or not args.authority:
                parser.error('install requires --bundle and --authority (the existing public 10.1-kf.tsk file)')
            controller.install(args.bundle, args.authority)
        elif args.command == 'status':
            active_version = controller.managed_version()
            print(json.dumps({'version': VERSION, 'dropin_present': active_version is not None,
                              'active_dropin_version': active_version,
                              'activation_record_present': controller.path(JOURNAL).exists(),
                              'template_store': str(STORE), 'biometric_accuracy_validated': False}))
            result = controller.systemctl('status', check=False)
            print(result.stdout, end='')
        else:
            getattr(controller, args.command)()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print('Native service: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if lock is not None:
            os.close(lock)
    return 0


if __name__ == '__main__':
    sys.exit(main())
