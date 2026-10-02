#!/usr/bin/python3
# SPDX-License-Identifier: MIT
"""Real installed fprintd, private bus, synthetic native backend. No USB capture."""
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    with tempfile.TemporaryDirectory(prefix='tudor-fprintd-test.', dir='/tmp') as directory:
        store = Path(directory)

        def run(operation, mode='same', code=0, extra=()):
            command = ['/usr/bin/python3', '-I', str(ROOT / 'scripts/fprintd-lab.py'),
                       '--simulate', mode, '--state', str(store), operation, *extra]
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            if result.returncode != code:
                raise AssertionError(f'{operation}/{mode}: {result.returncode}\n{result.stdout}\n{result.stderr}')
            summaries = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
            summary = summaries[-1] if summaries else {}
            assert not summary.get('system_authentication_supported', False)
            assert not summary.get('cleanup_errors')
            if operation in ('enroll', 'verify', 'delete') and summary.get('status'):
                assert summary['device_released']
            print(f'PASS fprintd {operation}/{mode}: {summary.get("status", summary.get("enrolled_fingers"))}', flush=True)
            return summary, result

        def templates():
            files = []
            for path in store.rglob('*'):
                info = path.lstat()
                assert info.st_uid == os.getuid()
                assert stat.S_IMODE(info.st_mode) == (0o700 if path.is_dir() else 0o600)
                assert not path.is_symlink()
                if path.is_file():
                    files.append(path)
            return files

        assert run('list')[0]['enrolled_fingers'] == []
        summary, output = run('enroll', 'enroll')
        assert summary['status'] == 'enroll-completed'
        # Stages one through nine, then completion for the tenth accepted scan.
        assert output.stdout.count('fprintd: enroll-stage-passed') == 9
        files = templates()
        assert len(files) == 1
        original = files[0].read_bytes()  # Synthetic data only, never physical captures.
        assert run('list')[0]['enrolled_fingers'] == ['right-index-finger']
        assert run('verify')[0]['status'] == 'verify-match'
        _, fast=run('verify',extra=('--settle-ms','500'))
        assert 'Experimental contact settling: 500 ms.' in fast.stdout
        assert 'Native timing:' in fast.stderr
        assert run('verify', 'different', code=1)[0]['status'] == 'verify-no-match'
        summary, output = run('verify', 'wait', code=130, extra=('--cancel-after', '3'))
        assert summary['status'] == 'cancelled'
        assert 'capture is waiting for cancellation' in output.stderr
        summary, output = run('verify', 'retry', code=130, extra=('--cancel-after', '3'))
        assert summary['status'] == 'cancelled'
        assert 'verify-retry-scan' in output.stdout
        assert run('verify')[0]['status'] == 'verify-match'
        _, denied = run('list', code=1, extra=('--deny-client',))
        assert 'PermissionDenied' in denied.stderr
        _, duplicate = run('enroll', 'enroll', code=1)
        assert 'already enrolled' in duplicate.stderr
        assert files[0].read_bytes() == original
        assert templates() == files
        assert run('delete')[0]['status'] == 'deleted'
        assert run('list')[0]['enrolled_fingers'] == []
        assert not templates()
        assert run('enroll', 'wait', code=130, extra=('--cancel-after', '0.3'))[0]['status'] == 'cancelled'
        assert not templates()
    print('PASS private fprintd integration: enrollment, persistence, match/nonmatch, retries, cancellation, authorization, permissions, explicit deletion')


if __name__ == '__main__':
    main()
