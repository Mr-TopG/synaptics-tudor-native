#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Automatic contact through real native TLS, independent synthetic sensor peer."""
from pathlib import Path
import subprocess
import sys
import tempfile
from test_capture import CapturePeer
from test_session import certificate, exact, ec, serialization


def main():
    client=ec.derive_private_key(7,ec.SECP256R1())
    sensor=ec.derive_private_key(17,ec.SECP256R1())
    authority=ec.derive_private_key(19,ec.SECP256R1())
    cert=certificate(client,authority,0)
    cases={'valid':0,'legacy':0,'bounce':0,'backlog':0,'clear-timeout':2,'touch-timeout':2,
           'short':4,'count':4,'empty-pending':4,'read-rejected':4,'config-rejected':4,
           'cancel-clear':1,'cancel-touch':1,'cancel-settle':1,'short-interrupt':3,
           'late':2,'clock-backwards':3,'storm':4}
    with tempfile.TemporaryDirectory(prefix='tudor-contact-test-') as tmp:
        root=Path(tmp); state=root/'state'; state.mkdir(mode=0o700)
        values={'host-key.pem':client.private_bytes(serialization.Encoding.PEM,
                    serialization.PrivateFormat.PKCS8,serialization.NoEncryption()),
                'host.cert':cert,'sensor.cert':certificate(sensor,authority,1),'complete':b'test\n'}
        for name,data in values.items():
            p=state/name; p.write_bytes(data); p.chmod(0o600)
        point=authority.public_key().public_numbers()
        (root/'authority.tsk').write_bytes(point.x.to_bytes(68,'little')+point.y.to_bytes(68,'little')+bytes(120))
        for case,expected in cases.items():
            mode='contact-'+case
            peer=CapturePeer(mode,client,sensor,cert)
            process=subprocess.Popen([sys.argv[1],mode,str(state),str(root/'authority.tsk')],
                stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            try:
                while True:
                    header=exact(process.stdout,4)
                    if not header: break
                    size=int.from_bytes(header,'big'); assert 0<size<=4096
                    response=peer.exchange(exact(process.stdout,size))
                    process.stdin.write(len(response).to_bytes(4,'big')+response); process.stdin.flush()
                rc=process.wait(timeout=10); logs=process.stderr.read().decode()
                assert rc==0 and f'CORE_RESULT status={expected} closed=1 bytes={17898 if expected==0 else 0}' in logs,(mode,rc,logs)
                assert peer.closed and peer.commands[0]==0x82 and peer.commands[-1]==0x86,(mode,peer.commands)
                assert (0x80 in peer.commands)==(expected==0),(mode,peer.commands)
                if not expected: assert peer.commands[-4:]==[0x80,0x7f,0x81,0x86]
                assert {p.name:p.read_bytes() for p in state.iterdir()}==values
            finally:
                if process.poll() is None: process.kill(); process.wait()
                process.stdin.close(); process.stdout.close(); process.stderr.close()
    print(f'Automatic contact: {len(cases)} encrypted-peer scenarios passed; no physical USB or private captures used.')


if __name__=='__main__': main()
