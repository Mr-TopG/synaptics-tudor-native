#!/usr/bin/python3
# SPDX-License-Identifier: MIT
"""Stage the real bundle; mock only systemctl to test activation/rollback failures."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('service', ROOT / 'integration/system/service.py')
service = importlib.util.module_from_spec(spec)
spec.loader.exec_module(service)
BUNDLE = ROOT / 'dist/tudor-native-service-0.21.0/payload'
OLD_BUNDLE = ROOT / 'dist/tudor-native-service-0.20.0/payload'
PUBLIC = ROOT / 'upstream/synaTudor-rev/pydrv/tudor/sensor/sensor_keys/10.1-kf.tsk'


class Systemctl:
    def __init__(self, active):
        self.active = active
        self.calls = []
        self.fail_next_start = False

    def __call__(self, command, check=True):
        self.calls.append(command)
        if command == 'stop':
            self.active = False
        elif command == 'start':
            if self.fail_next_start:
                self.fail_next_start = False
                raise RuntimeError('Injected daemon startup failure')
            self.active = True
        code = 3 if command == 'is-active' and not self.active else 0
        if code and check:
            raise RuntimeError('Service inactive')
        return subprocess.CompletedProcess([command], code, stdout='', stderr='')


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='tudor-install-test.')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.system = Systemctl(active=True)
        self.c = service.Controller(self.root, self.system)
        self.output = contextlib.redirect_stdout(io.StringIO())
        self.output.__enter__()
        self.addCleanup(self.output.__exit__, None, None, None)
        self.c.install(BUNDLE, PUBLIC)
        self.c.directory(service.PAIRING, 0o700)
        # Dummy pairing content: tests never execute the C backend or read real keys.
        self.c.atomic(service.PAIRING / 'test-marker', b'synthetic pairing marker')
        self.c.directory(service.STORE, 0o700)
        self.c.atomic(service.STORE / 'test-template', b'synthetic saved template')
        self.c.atomic(Path('/usr/libexec/fprintd'), b'synthetic daemon STATE_DIRECTORY\0', 0o755)

    def unchanged_private_data(self):
        self.assertEqual(self.c.regular(service.PAIRING / 'test-marker', private=True), b'synthetic pairing marker')
        self.assertEqual(self.c.regular(service.STORE / 'test-template', private=True), b'synthetic saved template')

    def previous_installation(self, originally_active=True):
        if not (OLD_BUNDLE / 'service.py').is_file():
            self.skipTest('Upgrade tests require the archived 0.20 service bundle')
        spec = importlib.util.spec_from_file_location('old_service', OLD_BUNDLE / 'service.py')
        old = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(old)
        controller = old.Controller(self.root, self.system)
        controller.install(OLD_BUNDLE, PUBLIC)
        self.system.active = originally_active
        controller.activate()
        previous = controller.regular(service.PREVIOUS_JOURNAL, private=True)
        self.system.calls.clear()
        return controller, previous

    def previous_preserved(self, record):
        self.assertEqual(self.c.regular(service.DROPIN), service.PREVIOUS_DROPIN_TEXT.encode())
        self.assertEqual(self.c.regular(service.PREVIOUS_JOURNAL, private=True), record)
        self.assertFalse(self.c.path(service.JOURNAL).exists())
        self.c.installed(service.PREVIOUS_VERSION)
        self.unchanged_private_data()

    def legacy_installation(self):
        bundle = ROOT / 'dist/tudor-native-service-0.19.0/payload'
        if not (bundle / 'service.py').is_file():
            self.skipTest('Requires archived 0.19 bundle')
        spec = importlib.util.spec_from_file_location('legacy_service', bundle / 'service.py')
        legacy = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(legacy)
        controller = legacy.Controller(self.root, self.system)
        controller.install(bundle, PUBLIC)
        controller.activate()
        self.system.calls.clear()
        return controller

    def test_three_version_rollback_chain(self):
        legacy = self.legacy_installation()
        old, record = self.previous_installation()
        self.c.activate()
        self.c.rollback()
        self.previous_preserved(record)
        old.rollback()
        self.assertEqual(self.c.managed_version(), '0.19.0')
        legacy.rollback()
        self.assertIsNone(self.c.managed_version())
        self.unchanged_private_data()

    def test_direct_legacy_upgrade(self):
        legacy = self.legacy_installation()
        self.c.activate()
        self.c.rollback()
        self.assertEqual(self.c.managed_version(), '0.19.0')
        legacy.rollback()
        self.assertIsNone(self.c.managed_version())

    def test_missing_ancestor_journal_refused(self):
        self.legacy_installation()
        self.previous_installation()
        self.c.path(service.OLDER_JOURNALS['0.19.0']).unlink()
        with self.assertRaises(FileNotFoundError):
            self.c.activate()
        self.assertEqual(self.system.calls, [])

    def test_ancestor_changed_after_upgrade_refused(self):
        self.legacy_installation()
        self.previous_installation()
        self.c.activate()
        self.c.atomic(service.OLDER_JOURNALS['0.19.0'],
                      b'{"version":"0.19.0","was_active":false}')
        self.system.calls.clear()
        with self.assertRaisesRegex(RuntimeError, 'Previous activation changed'):
            self.c.rollback()
        self.assertEqual(self.system.calls, [])

    def test_stray_ancestor_journal_refused(self):
        self.previous_installation()
        self.c.atomic(service.OLDER_JOURNALS['0.19.0'],
                      b'{"version":"0.19.0","was_active":true}')
        with self.assertRaisesRegex(RuntimeError, 'Unexpected previous activation'):
            self.c.activate()
        self.assertEqual(self.system.calls, [])

    def test_launch_selects_500_ms(self):
        controller = service.Controller(Path('/'))
        with patch.object(controller, 'preflight', return_value='/mock/fprintd'), \
                patch.object(service.os, 'open', return_value=100), \
                patch.object(service.fcntl, 'flock'), \
                patch.object(service.os, 'set_inheritable'), \
                patch.object(service.os, 'umask'), \
                patch.object(service.resource, 'setrlimit'), \
                patch.object(service.os, 'execve') as execute:
            controller.launch()
        args = execute.call_args.args
        self.assertEqual(args[0], '/mock/fprintd')
        self.assertEqual(args[2]['TUDOR_NATIVE_SETTLE_MS'], '500')
        self.assertEqual(args[2]['LD_LIBRARY_PATH'], str(service.PREFIX))

    def test_passive_install_and_permissions(self):
        self.assertEqual(self.system.calls, [])
        self.assertFalse(self.c.path(service.DROPIN).exists())
        self.assertFalse(self.c.path('/etc/pam.d').exists())
        for path in self.c.path(service.PREFIX).iterdir():
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o644)
            self.assertEqual(path.stat().st_uid, os.geteuid())
        self.assertEqual(stat.S_IMODE(self.c.path(service.STATE).stat().st_mode), 0o700)
        self.c.installed()
        self.assertEqual(self.c.regular(service.PREFIX / 'service.py'),
                         (ROOT / 'integration/system/service.py').read_bytes())
        self.c.install(BUNDLE, PUBLIC)  # Identical install is idempotent.

    def test_activation_and_rollback(self):
        other = service.DROPIN.parent / '20-administrator.conf'
        self.c.atomic(other, b'# unrelated administrator settings\n', 0o644)
        self.c.activate()
        self.assertTrue(self.system.active)
        self.assertEqual(self.c.regular(service.DROPIN), service.DROPIN_TEXT.encode())
        self.c.rollback()
        self.assertFalse(self.c.path(service.DROPIN).exists())
        self.assertFalse(self.c.path(service.JOURNAL).exists())
        self.assertTrue(self.system.active)
        self.assertEqual(self.c.regular(other), b'# unrelated administrator settings\n')
        self.unchanged_private_data()
        self.c.rollback()  # No-op.

    def test_failed_activation_restores_active_service(self):
        self.system.fail_next_start = True
        with self.assertRaisesRegex(RuntimeError, 'previous service configuration restored'):
            self.c.activate()
        self.assertTrue(self.system.active)
        self.assertFalse(self.c.path(service.DROPIN).exists())
        self.assertFalse(self.c.path(service.JOURNAL).exists())
        self.unchanged_private_data()

    def test_failed_activation_restores_inactive_service(self):
        self.system.active = False
        self.system.fail_next_start = True
        with self.assertRaisesRegex(RuntimeError, 'previous service configuration restored'):
            self.c.activate()
        self.assertFalse(self.system.active)
        self.assertFalse(self.c.path(service.DROPIN).exists())
        self.unchanged_private_data()

    def test_interrupted_activation_recoverable(self):
        self.c.atomic(service.JOURNAL, json.dumps({'version': service.VERSION, 'was_active': True, 'previous': None}).encode())
        self.system.active = False
        with self.assertRaisesRegex(RuntimeError, 'Interrupted or existing activation'):
            self.c.activate()
        self.c.rollback()
        self.assertTrue(self.system.active)
        self.unchanged_private_data()

    def test_failed_rollback_preserves_recovery_record(self):
        self.c.activate()
        self.system.fail_next_start = True
        with self.assertRaisesRegex(RuntimeError, 'startup failure'):
            self.c.rollback()
        self.assertTrue(self.c.path(service.JOURNAL).exists())
        self.c.rollback()
        self.assertTrue(self.system.active)
        self.unchanged_private_data()

    def test_unknown_dropin_not_overwritten(self):
        self.c.atomic(service.DROPIN, b'# not ours\n', 0o644)
        with self.assertRaisesRegex(RuntimeError, 'unknown or edited'):
            self.c.activate()
        self.assertEqual(self.system.calls, [])
        self.assertEqual(self.c.regular(service.DROPIN), b'# not ours\n')

    def test_edited_dropin_not_deleted(self):
        self.c.activate()
        self.c.atomic(service.DROPIN, b'# edited configuration\n', 0o644)
        calls = list(self.system.calls)
        with self.assertRaisesRegex(RuntimeError, 'unknown or edited'):
            self.c.rollback()
        self.assertEqual(self.system.calls, calls)

    def test_tampered_library_refused_before_service_stop(self):
        self.c.atomic(service.PREFIX / 'libfprint-2.so.2', b'changed', 0o644)
        with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
            self.c.activate()
        self.assertEqual(self.system.calls, [])

    def test_symlink_install_parent_refused(self):
        root = self.root / 'other-root'
        root.mkdir()
        (root / 'usr').symlink_to(self.root / 'usr', target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, 'Unsafe directory'):
            service.Controller(root, self.system).install(BUNDLE, PUBLIC)

    def test_missing_pairing_not_created(self):
        shutil.rmtree(self.c.path(service.PAIRING))
        with self.assertRaisesRegex(RuntimeError, 'pairing is required'):
            self.c.activate()
        self.assertFalse(self.c.path(service.PAIRING).exists())
        self.assertEqual(self.system.calls, [])

    def test_staged_service_control_forbidden(self):
        with self.assertRaisesRegex(RuntimeError, 'forbidden'):
            service.Controller(self.root).real_systemctl('start')

    def test_payload_loads_without_build_environment(self):
        lib = self.c.path(service.PREFIX / 'libfprint-2.so.2')
        dynamic = subprocess.check_output(['readelf', '-d', str(lib)], text=True)
        self.assertNotIn('RUNPATH', dynamic)
        self.assertNotIn('RPATH', dynamic)
        subprocess.run(['/usr/bin/python3', '-I', '-c',
                        'import ctypes,sys; lib=ctypes.CDLL(sys.argv[1]); assert lib.fp_context_new', str(lib)],
                       env={'PATH': '/usr/bin:/bin'}, check=True)

    def test_version_upgrade_and_two_step_rollback(self):
        old, record = self.previous_installation(originally_active=False)
        self.c.activate()
        self.assertEqual(self.c.regular(service.DROPIN), service.DROPIN_TEXT.encode())
        self.assertEqual(self.c.regular(service.PREVIOUS_JOURNAL, private=True), record)
        self.assertEqual(self.c.activation_record()['previous']['version'], service.PREVIOUS_VERSION)
        self.assertTrue(self.system.active)
        self.c.rollback()
        self.previous_preserved(record)
        self.assertTrue(self.system.active)
        # The untouched old helper can still restore the original distro state.
        old.rollback()
        self.assertFalse(self.system.active)
        self.assertFalse(self.c.path(service.DROPIN).exists())
        self.assertFalse(self.c.path(service.PREVIOUS_JOURNAL).exists())
        self.unchanged_private_data()

    def test_failed_upgrade_restores_previous_version(self):
        _, record = self.previous_installation()
        self.system.fail_next_start = True
        with self.assertRaisesRegex(RuntimeError, 'previous service configuration restored'):
            self.c.activate()
        self.previous_preserved(record)
        self.assertTrue(self.system.active)

    def test_upgrade_rollback_restores_stopped_previous_service(self):
        _, record = self.previous_installation()
        self.system.active = False
        self.c.activate()
        self.assertTrue(self.system.active)
        self.c.rollback()
        self.previous_preserved(record)
        self.assertFalse(self.system.active)

    def test_interrupted_upgrade_before_replacing_dropin(self):
        _, record = self.previous_installation()
        def interrupted_stop(command, check=True):
            if command == 'stop':
                raise KeyboardInterrupt('Injected interruption after recording prior state')
            return self.system(command, check=check)
        self.c.systemctl = interrupted_stop
        with self.assertRaises(KeyboardInterrupt):
            self.c.activate()
        self.assertTrue(self.c.path(service.JOURNAL).exists())
        self.assertEqual(self.c.regular(service.DROPIN), service.PREVIOUS_DROPIN_TEXT.encode())
        self.c.systemctl = self.system
        with self.assertRaisesRegex(RuntimeError, 'Interrupted or existing activation'):
            self.c.activate()
        self.c.rollback()
        self.previous_preserved(record)

    def test_interrupted_upgrade_after_replacing_dropin(self):
        _, record = self.previous_installation()
        def interrupted_reload(command, check=True):
            if command == 'daemon-reload':
                raise KeyboardInterrupt('Injected interruption after replacing drop-in')
            return self.system(command, check=check)
        self.c.systemctl = interrupted_reload
        with self.assertRaises(KeyboardInterrupt):
            self.c.activate()
        self.assertFalse(self.system.active)
        self.assertEqual(self.c.regular(service.DROPIN), service.DROPIN_TEXT.encode())
        self.c.systemctl = self.system
        self.c.rollback()
        self.previous_preserved(record)
        self.assertTrue(self.system.active)

    def test_failed_upgrade_rollback_keeps_recovery_state(self):
        _, record = self.previous_installation()
        self.c.activate()
        self.system.fail_next_start = True
        with self.assertRaisesRegex(RuntimeError, 'startup failure'):
            self.c.rollback()
        self.assertEqual(self.c.regular(service.DROPIN), service.PREVIOUS_DROPIN_TEXT.encode())
        self.assertTrue(self.c.path(service.JOURNAL).exists())
        self.assertEqual(self.c.regular(service.PREVIOUS_JOURNAL, private=True), record)
        self.c.rollback()
        self.previous_preserved(record)
        self.assertTrue(self.system.active)

    def test_previous_activation_missing_refused(self):
        self.previous_installation()
        self.c.path(service.PREVIOUS_JOURNAL).unlink()
        with self.assertRaises(FileNotFoundError):
            self.c.activate()
        self.assertEqual(self.system.calls, [])

    def test_previous_activation_edited_refused(self):
        self.previous_installation()
        self.c.atomic(service.PREVIOUS_JOURNAL, b'{"version":"foreign","was_active":true}')
        with self.assertRaisesRegex(RuntimeError, 'Invalid previous activation'):
            self.c.activate()
        self.assertEqual(self.system.calls, [])

    def test_previous_payload_edited_refused(self):
        self.previous_installation()
        self.c.atomic(service.PREVIOUS_PREFIX / 'service.py', b'edited', 0o644)
        with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
            self.c.activate()
        self.assertEqual(self.system.calls, [])

    def test_previous_dropin_edited_refused(self):
        self.previous_installation()
        self.c.atomic(service.DROPIN, service.PREVIOUS_DROPIN_TEXT.encode() + b'# edited\n', 0o644)
        with self.assertRaisesRegex(RuntimeError, 'unknown or edited'):
            self.c.activate()
        self.assertEqual(self.system.calls, [])

    def test_prior_activation_changed_after_upgrade_refused(self):
        self.previous_installation()
        self.c.activate()
        self.system.calls.clear()
        self.c.atomic(service.PREVIOUS_JOURNAL, json.dumps({'version': service.PREVIOUS_VERSION,
                                                        'was_active': False}).encode())
        with self.assertRaisesRegex(RuntimeError, 'Previous activation changed'):
            self.c.rollback()
        self.assertEqual(self.system.calls, [])
        self.assertEqual(self.c.regular(service.DROPIN), service.DROPIN_TEXT.encode())

    def test_new_rollback_without_own_activation_leaves_old_active(self):
        _, record = self.previous_installation()
        self.c.rollback()
        self.assertEqual(self.system.calls, [])
        self.previous_preserved(record)

    def test_invalid_previous_snapshot_refused(self):
        self.previous_installation()
        self.c.activate()
        record = self.c.activation_record()
        record['previous']['dropin'] += '# edited\n'
        self.c.atomic(service.JOURNAL, json.dumps(record).encode())
        self.system.calls.clear()
        with self.assertRaisesRegex(RuntimeError, 'Previous activation changed'):
            self.c.rollback()
        self.assertEqual(self.system.calls, [])

    def test_legacy_interruption_without_dropin_refused(self):
        self.previous_installation()
        self.c.path(service.DROPIN).unlink()
        with self.assertRaisesRegex(RuntimeError, 'Interrupted activation'):
            self.c.activate()
        self.assertEqual(self.system.calls, [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
