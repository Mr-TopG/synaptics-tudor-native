#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Encrypted peer tests for the transport-independent memory API."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from test_capture import CapturePeer, TLS_FAILURES
from test_session import certificate, exact, ec, serialization


def run(bridge, root, mode, expected, commands, client, sensor, cert):
    before = {p.name: p.read_bytes() for p in (root/'state').iterdir()}
    peer = CapturePeer(mode, client, sensor, cert)
    process = subprocess.Popen([bridge, mode, str(root/'state'), str(root/'authority.tsk')],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        while True:
            header = exact(process.stdout, 4)
            if not header:
                break
            size = int.from_bytes(header, 'big')
            assert 0 < size <= 4096
            response = peer.exchange(exact(process.stdout, size))
            process.stdin.write(len(response).to_bytes(4, 'big') + response)
            process.stdin.flush()
        rc = process.wait(timeout=10)
        logs = process.stderr.read().decode()
        assert rc == 0, (mode, rc, logs)
        closed = mode not in TLS_FAILURES + ('cancel-before','busy','idle-failure','invalid-callback','wrong-version')
        assert f'CORE_RESULT status={expected} closed={int(closed)} bytes={17898 if expected == 0 else 0}' in logs, (mode, logs)
        assert peer.commands == commands, (mode, peer.commands)
        assert before == {p.name: p.read_bytes() for p in (root/'state').iterdir()}
        assert sorted(p.name for p in root.iterdir()) == ['authority.tsk', 'state']
    finally:
        if process.poll() is None:
            process.kill(); process.wait()
        process.stdin.close(); process.stdout.close(); process.stderr.close()


def main():
    client=ec.derive_private_key(7,ec.SECP256R1())
    sensor=ec.derive_private_key(17,ec.SECP256R1())
    authority=ec.derive_private_key(19,ec.SECP256R1())
    cert=certificate(client,authority,0)
    with tempfile.TemporaryDirectory(prefix='tudor-core-test-') as tmp:
        root=Path(tmp); state=root/'state'; state.mkdir(mode=0o700)
        values={'host-key.pem':client.private_bytes(serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,serialization.NoEncryption()),
                'host.cert':cert,'sensor.cert':certificate(sensor,authority,1),'complete':b'test\n'}
        for name,data in values.items():
            path=state/name; path.write_bytes(data); path.chmod(0o600)
        point=authority.public_key().public_numbers()
        (root/'authority.tsk').write_bytes(point.x.to_bytes(68,'little')+point.y.to_bytes(68,'little')+bytes(120))
        full=[0x82,0x86,0x80,0x7f,0x81,0x86]
        before_frame=[0x82,0x86,0x80,0x81,0x86]
        cases=[('valid',0,full),('fragmented-frame',0,full),
               ('cancel-before',1,[]),('cancel-handshake',1,[]),
               ('cancel-prepare',1,[0x82]),('prepare-error',3,[0x82]),
               ('cancel-acquire',1,before_frame),('cancel-wait',1,before_frame),
               ('cancel-ready',1,before_frame),('cancel-frame',1,full),
               ('cancel-cleanup',1,full),('cancel-close',1,full),
               ('timeout',2,before_frame),('clock-error',3,before_frame),
               ('clock-backwards',3,before_frame),('short-interrupt',3,before_frame),
               ('long-interrupt',3,before_frame),('busy',3,[]),
               ('late-interrupt',2,before_frame),('clear-rejected',5,full),
               ('idle-failure',5,full),('finish-rejected',5,full),
               ('invalid-callback',-1,[]),('wrong-version',4,[]),
               ('wrong-dimensions',4,[0x82]),('acquire-busy',4,before_frame),
               ('finger-lifted',4,full),('incomplete',4,full),('wrong-length',4,full)]
        cases += [(mode,4,full[:4]) for mode in TLS_FAILURES]
        for mode,status,commands in cases:
            run(str(Path(sys.argv[1]).resolve()),root,mode,status,commands,client,sensor,cert)
    print(f'Capture core tests passed: {len(cases)} encrypted-peer scenarios, cancellation/timeout/cleanup, zero images on errors, no image files, exact decoder layout.')


if __name__=='__main__':
    main()
