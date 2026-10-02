#!/usr/bin/python3
# SPDX-License-Identifier: MIT
"""Run the installed fprintd on a private bus with a separate template store.

The tiny authorization service is test infrastructure, scoped to this daemon
and its one client. It is never registered on the system or desktop bus.
"""
import argparse
import configparser
import fcntl
import json
import os
from pathlib import Path
import pwd
import resource
import selectors
import signal
import stat
import subprocess
import sys
import tempfile
import time

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

ROOT = Path(__file__).resolve().parent.parent
SERVICE = 'net.reactivated.Fprint'
DEVICE = SERVICE + '.Device'
AUTH = 'org.freedesktop.PolicyKit1.Authority'
AUTH_PATH = '/org/freedesktop/PolicyKit1/Authority'
STORE = Path('/var/lib/tudor-native-fprintd-lab-v1')
FINGERS = [side + '-' + finger for side in ('left', 'right') for finger in
           ('thumb', 'index-finger', 'middle-finger', 'ring-finger', 'little-finger')]
ACTIONS = {'net.reactivated.fprint.device.' + action for action in
           ('enroll', 'verify', 'setusername')}


class Authority(dbus.service.Object):
    def __init__(self, bus):
        self.name = dbus.service.BusName('org.freedesktop.PolicyKit1', bus=bus,
                                       do_not_queue=True)
        super().__init__(self.name, AUTH_PATH)
        self.bus = bus
        self.daemon = None
        self.client = None

    def owns(self, name, process):
        if process is None or process.poll() is not None or not str(name).startswith(':'):
            return False
        proxy = dbus.Interface(self.bus.get_object('org.freedesktop.DBus',
                              '/org/freedesktop/DBus'), 'org.freedesktop.DBus')
        return (int(proxy.GetConnectionUnixProcessID(name)) == process.pid and
                int(proxy.GetConnectionUnixUser(name)) == os.geteuid())

    @dbus.service.method(AUTH, in_signature='(sa{sv})sa{ss}us',
                         out_signature='(bba{ss})', sender_keyword='sender')
    def CheckAuthorization(self, subject, action, details, flags, cancellation_id, sender=None):
        allowed = False
        try:
            allowed = (action in ACTIONS and subject[0] == 'system-bus-name' and
                       self.owns(sender, self.daemon) and
                       self.owns(subject[1].get('name', ''), self.client))
        except dbus.DBusException:
            pass
        return (bool(allowed), False, dbus.Dictionary({}, signature='ss'))

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface):
        if interface != AUTH:
            return {}
        return {'BackendName': 'tudor-private-lab', 'BackendVersion': '1',
                'BackendFeatures': dbus.UInt32(1)}

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, interface, key):
        return self.GetAll(interface)[key]


def enrolled(device, username):
    try:
        return list(map(str, device.ListEnrolledFingers(username)))
    except dbus.DBusException as error:
        if error.get_dbus_name() == SERVICE + '.Error.NoEnrolledPrints':
            return []
        raise


def client(args):
    """Separate process: synchronous calls must not block the authority's loop."""
    bus = dbus.bus.BusConnection(os.environ['TUDOR_PRIVATE_BUS'])
    manager = dbus.Interface(bus.get_object(SERVICE, '/net/reactivated/Fprint/Manager'),
                             SERVICE + '.Manager')
    path = manager.GetDefaultDevice()
    proxy = bus.get_object(SERVICE, path)
    device = dbus.Interface(proxy, DEVICE)
    props = dbus.Interface(proxy, 'org.freedesktop.DBus.Properties')
    if str(props.Get(DEVICE, 'name')) != 'Synaptics Tudor experimental enrollment/verification lab':
        raise RuntimeError('Unexpected device on private bus')
    result = {'private_fprintd': True, 'operation': args.operation,
              'system_authentication_supported': False, 'user': args.user}
    if args.operation == 'list':
        result['enrolled_fingers'] = enrolled(device, args.user)
        print(json.dumps(result), flush=True)
        return 0
    claimed = started = False
    loop = GLib.MainLoop()
    status = None
    interrupted = False
    errors = []

    def cancel(*unused):
        nonlocal interrupted
        interrupted = True
        print('Stopping private fprintd operation; waiting for capture cleanup.', flush=True)
        loop.quit()
        return GLib.SOURCE_REMOVE

    def progress(value, done):
        nonlocal status
        print('fprintd: ' + str(value), flush=True)
        if done:
            status = str(value)
            loop.quit()

    signal.signal(signal.SIGINT, cancel)
    signal.signal(signal.SIGTERM, cancel)
    try:
        device.Claim(args.user, timeout=45)
        claimed = True
        if interrupted:
            raise RuntimeError('Cancelled during device claim')
        if int(props.Get(DEVICE, 'num-enroll-stages')) != 10:
            raise RuntimeError('Expected ten enrollment stages')
        if args.operation == 'delete':
            device.DeleteEnrolledFinger(args.finger, timeout=45)
            status = 'deleted'
        else:
            fingers = enrolled(device, args.user)
            if args.operation == 'enroll' and args.finger in fingers:
                raise RuntimeError('Finger already enrolled in the private store; verify it or explicitly delete it first')
            if args.operation == 'verify' and args.finger not in fingers:
                raise RuntimeError('Enroll this named finger in the private fprintd store first')
            prefix = 'Enroll' if args.operation == 'enroll' else 'Verify'
            device.connect_to_signal(prefix + 'Status', progress)
            print(f'Private fprintd {args.operation}: {args.finger}. Follow lift/place prompts.', flush=True)
            getattr(device, prefix + 'Start')(args.finger, timeout=45)
            started = True
            GLib.timeout_add_seconds(600 if args.operation == 'enroll' else 90, cancel)
            if args.cancel_after:
                GLib.timeout_add(int(args.cancel_after * 1000), cancel)
            if status is None and not interrupted:
                loop.run()
    finally:
        if started:
            try:
                getattr(device, prefix + 'Stop')(timeout=45)
            except dbus.DBusException as error:
                errors.append(str(error))
        if claimed:
            try:
                device.Release(timeout=45)
            except dbus.DBusException as error:
                errors.append(str(error))
        result.update(status='cancelled' if interrupted else status,
                      device_released=claimed and not errors, cleanup_errors=errors)
        print(json.dumps(result), flush=True)
    if errors:
        return 1
    if interrupted:
        return 130
    return 0 if status in ('enroll-completed', 'verify-match', 'deleted') else 1


def private_store(path):
    """Pin and lock the directory; reject a symlink or publicly accessible store."""
    try:
        path.mkdir(mode=0o700)
    except FileExistsError:
        pass
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    info = os.fstat(fd)
    if info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
        os.close(fd)
        raise RuntimeError('Template directory must be owned by this user with mode 0700')
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except Exception:
        os.close(fd)
        raise
    return fd


def stop(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=45)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        raise RuntimeError('Private child exceeded cleanup deadline; it was stopped')


def run(args):
    if not args.simulate and os.geteuid() != 0:
        raise RuntimeError('Run with sudo to reuse the saved pairing and capture')
    if args.simulate and (args.state is None or not args.state.is_absolute() or
                          ':' in str(args.state) or args.state.resolve().parent != Path('/tmp')):
        raise RuntimeError('Synthetic tests require a private --state directory directly under /tmp')
    if not args.simulate and (args.state is not None or args.cancel_after or args.deny_client):
        raise RuntimeError('--state, --cancel-after and --deny-client are synthetic test options only')
    config = configparser.ConfigParser()
    config.read('/etc/fprintd.conf')
    if config.get('storage', 'type', fallback='file') != 'file':
        raise RuntimeError('Installed fprintd must use its file storage backend')
    daemon_path = next((p for p in ('/usr/libexec/fprintd', '/usr/lib/fprintd/fprintd')
                        if Path(p).is_file()), None)
    if daemon_path is None:
        raise RuntimeError('Install your distribution fprintd package first')
    if b'STATE_DIRECTORY\0' not in Path(daemon_path).read_bytes():
        raise RuntimeError('This fprintd lacks the separate storage override; refusing to run it')
    lib = ROOT / 'build' / ('libfprint-service-test-build' if args.simulate else
                            'libfprint-matching-build') / 'libfprint'
    if not (lib / 'libfprint-2.so.2').is_file():
        raise RuntimeError('Build the private libfprint matching library first')
    if not args.simulate:
        subprocess.run(['sha256sum', '-c', 'scripts/reference-keys.sha256'], cwd=ROOT, check=True)
    state = args.state if args.simulate else STORE
    lock = private_store(state)
    daemon = child = bus_process = None
    connection = None
    try:
        with tempfile.TemporaryDirectory(prefix='tudor-fprintd-bus.', dir='/tmp') as runtime:
            address = 'unix:path=' + runtime + '/bus'
            env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8',
                   'DBUS_SYSTEM_BUS_ADDRESS': address, 'TUDOR_PRIVATE_BUS': address,
                   'STATE_DIRECTORY': str(state), 'LD_LIBRARY_PATH': str(lib),
                   'FP_DRIVERS_ALLOWLIST': 'tudor_native_lab',
                   'TUDOR_NATIVE_EXPERIMENTAL': '1', 'TUDOR_NATIVE_MATCHING_EXPERIMENTAL': '1',
                   'TUDOR_NATIVE_AUTOMATIC_CONTACT': '1',
                   'TUDOR_NATIVE_PAIRING_DIR': '/var/lib/tudor-native-pairing-v1',
                   'TUDOR_NATIVE_AUTHORITY_FILE': str(ROOT / 'upstream/synaTudor-rev/pydrv/tudor/sensor/sensor_keys/10.1-kf.tsk')}
            if args.simulate:
                env.update(TUDOR_FPRINTD_TEST_DEVICE='synthetic', TUDOR_SERVICE_TEST_MODE=args.simulate)
            bus_process = subprocess.Popen(['/usr/bin/dbus-daemon', '--session', '--nofork',
                '--nopidfile', '--address=' + address, '--print-address=1'],
                stdout=subprocess.PIPE, text=True, env=env, start_new_session=True)
            with selectors.DefaultSelector() as selector:
                selector.register(bus_process.stdout, selectors.EVENT_READ)
                if not selector.select(timeout=10) or not bus_process.stdout.readline().startswith(address):
                    raise RuntimeError('Private bus failed to start')
            connection = dbus.bus.BusConnection(address)
            authority = Authority(connection)
            daemon = subprocess.Popen([daemon_path, '--no-timeout'], env=env, start_new_session=True)
            authority.daemon = daemon
            loop = GLib.MainLoop()
            deadline = time.monotonic() + 20
            failure = []
            interrupted = False

            def cancel(*unused):
                nonlocal interrupted
                if not interrupted:
                    interrupted = True
                    if child is not None and child.poll() is None:
                        child.send_signal(signal.SIGINT)
                    else:
                        loop.quit()

            signal.signal(signal.SIGINT, cancel)
            signal.signal(signal.SIGTERM, cancel)

            def poll():
                nonlocal child
                if daemon.poll() is not None:
                    failure.append('Private fprintd exited unexpectedly')
                    loop.quit()
                    return GLib.SOURCE_REMOVE
                if child is None:
                    if connection.name_has_owner(SERVICE):
                        command = ['/usr/bin/python3', '-I', str(Path(__file__).resolve()),
                                   '--client', args.operation, '--finger', args.finger, '--user', args.user]
                        if args.cancel_after:
                            command += ['--cancel-after', str(args.cancel_after)]
                        try:
                            child = subprocess.Popen(command, env=env, start_new_session=True)
                        except OSError as error:
                            failure.append('Cannot start private client: ' + str(error))
                            loop.quit()
                            return GLib.SOURCE_REMOVE
                        if not args.deny_client:
                            authority.client = child
                    elif time.monotonic() > deadline:
                        failure.append('Private fprintd startup timed out')
                        loop.quit()
                        return GLib.SOURCE_REMOVE
                elif child.poll() is not None:
                    loop.quit()
                    return GLib.SOURCE_REMOVE
                return GLib.SOURCE_CONTINUE

            print('Private fprintd: separate bus and template store; system login unchanged.', flush=True)
            print('Template store: ' + str(state), flush=True)
            GLib.timeout_add(50, poll)
            loop.run()
            if failure:
                raise RuntimeError(failure[0])
            return child.returncode if child is not None and child.returncode is not None else 130
    finally:
        # Only children of this invocation are stopped. Never touch system fprintd.
        try:
            stop(child)
        finally:
            try:
                stop(daemon)
            finally:
                if connection is not None:
                    connection.close()
                stop(bus_process)
                os.close(lock)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('enroll', 'verify', 'list', 'delete'))
    parser.add_argument('--finger', choices=FINGERS, default='right-index-finger')
    parser.add_argument('--user', default=os.environ.get('SUDO_USER') or pwd.getpwuid(os.getuid()).pw_name)
    parser.add_argument('--simulate', choices=('enroll', 'same', 'different', 'retry', 'wait'), help=argparse.SUPPRESS)
    parser.add_argument('--state', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--cancel-after', type=float, default=0, help=argparse.SUPPRESS)
    parser.add_argument('--client', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--deny-client', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    # fprintd uses the username as a path component; accept actual local accounts only.
    if '/' in args.user or args.user in ('', '.', '..'):
        parser.error('Invalid username')
    pwd.getpwnam(args.user)
    os.umask(0o077)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    DBusGMainLoop(set_as_default=True)
    try:
        return client(args) if args.client else run(args)
    except (RuntimeError, OSError, dbus.DBusException, subprocess.SubprocessError) as error:
        print('Private fprintd failed: ' + str(error), file=sys.stderr, flush=True)
        return 1


if __name__ == '__main__':
    sys.exit(main())
