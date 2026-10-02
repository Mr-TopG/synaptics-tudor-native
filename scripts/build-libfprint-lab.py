#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build private libfprint 1.94.7 capture or matching labs; never install anything."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parent.parent


def environment():
    env=dict(os.environ)
    deps=ROOT/'build/libfprint-deps/root/usr'
    if deps.exists():
        env['PATH']=str(deps/'bin')+os.pathsep+env['PATH']
        env['PYTHONPATH']=str(deps/'lib/python3/dist-packages')+os.pathsep+env.get('PYTHONPATH','')
        env['PKG_CONFIG_PATH']=os.pathsep.join([str(deps/'lib/x86_64-linux-gnu/pkgconfig'),
            str(deps/'share/pkgconfig'),env.get('PKG_CONFIG_PATH','')])
    return env


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path,help='Unmodified upstream libfprint v1.94.7 source')
    parser.add_argument('--sanitize',action='store_true',help='Separate ASan/UBSan development build')
    parser.add_argument('--matching',action='store_true',help='Separate experimental enrollment/verification build')
    parser.add_argument('--service-test',action='store_true',help='Separate synthetic-only backend for fprintd tests; no sensor access')
    args=parser.parse_args()
    if args.service_test: args.matching=True
    source=args.source.resolve()
    if "version: '1.94.7'" not in (source/'meson.build').read_text():
        parser.error('This initial adapter is pinned to upstream libfprint 1.94.7')
    name='libfprint-service-test' if args.service_test else 'libfprint-matching' if args.matching else 'libfprint-native'
    stage=ROOT/'build'/(name+'-source')
    build=ROOT/'build'/(name+('-sanitize' if args.sanitize else '-build'))
    if source==stage:
        parser.error('Source must be the original tree, not the generated staging tree')
    if not stage.exists():
        shutil.copytree(source,stage,ignore=shutil.ignore_patterns('.git','_build'))
        (stage/'.tudor-native-generated').write_text('Private generated development tree\n')
    elif not (stage/'.tudor-native-generated').is_file():
        parser.error('Refusing to modify a staging directory without our ownership marker')
    driver=stage/'libfprint/drivers/tudor_native'; driver.mkdir(exist_ok=True)
    core=('capture_core','frame_decode','tls','state','certificate','protocol')
    if args.matching: core+=('verification','image_quality','image_bank','image_score')
    for name in core:
        for suffix in ('.c','.h'): shutil.copy2(ROOT/'src'/(name+suffix),driver/(name+suffix))
    for path in (ROOT/'integration/libfprint').glob('*.[ch]'): shutil.copy2(path,driver/path.name)
    sources=['tudor_native.c','tudor_backend.c','tudor_transport.c']+[name+'.c' for name in core]
    if args.matching: sources.append('tudor_template.c')
    if args.service_test:
        sources.remove('tudor_backend.c')
        sources.append('service_test_backend.c')
    original=(source/'libfprint/meson.build').read_text()
    entry="    'tudor_native': ["+','.join(repr('drivers/tudor_native/'+s) for s in sources)+"],\n"
    if original.count('driver_sources = {')!=1 or original.count('deps = [')!=1:
        parser.error('Unexpected upstream build structure')
    patched=original.replace('driver_sources = {','driver_sources = {\n'+entry)
    patched=patched.replace('deps = [',"deps = [\n    dependency('libcrypto', version: '>= 3.0'), dependency('threads'),",1)
    patched+='''
# Local Tudor integration lab: not installed.
tudor_lab = executable('tudor-capture-lab',
    'drivers/tudor_native/capture_lab.c', dependencies: libfprint_dep, install: false)
tudor_adapter_test = executable('test-tudor-adapter',
    ['drivers/tudor_native/test_adapter.c', 'drivers/tudor_native/tudor_native.c',
     'drivers/tudor_native/frame_decode.c', 'drivers/tudor_native/protocol.c'],
    c_args: ['-DTUDOR_ADAPTER_TEST'], dependencies: libfprint_private_dep,
    link_args: get_option('b_sanitize') == 'none' ? [] : ['-no-pie'], install: false)
test('tudor-adapter', tudor_adapter_test, env: ['G_DEBUG=fatal-warnings'], timeout: 30)
tudor_transport_test = executable('test-tudor-transport',
    ['drivers/tudor_native/test_transport.c', 'drivers/tudor_native/tudor_transport.c'],
    dependencies: [dependency('gusb', version: '>= 0.4.8'), dependency('json-glib-1.0')],
    link_args: get_option('b_sanitize') == 'none' ? [] : ['-no-pie'], install: false)
test('tudor-transport', tudor_transport_test, env: ['G_DEBUG=fatal-warnings'], timeout: 10)
'''
    if args.matching:
        begin=patched.index("tudor_adapter_test = executable(")
        end=patched.index("tudor_transport_test = executable(",begin)
        matching_sources=['test_matching.c','tudor_native.c','tudor_template.c',
            'frame_decode.c','protocol.c','verification.c','image_quality.c','image_bank.c','image_score.c']
        patched=patched[:begin]+"tudor_matching_test = executable('test-tudor-matching',\n"+repr(
            ['drivers/tudor_native/'+s for s in matching_sources])+""",
    c_args: ['-DTUDOR_ADAPTER_TEST'], dependencies: libfprint_private_dep,
    link_args: get_option('b_sanitize') == 'none' ? [] : ['-no-pie'], install: false)
test('tudor-matching', tudor_matching_test, env: ['G_DEBUG=fatal-warnings'], timeout: 120)
tudor_matching_lab = executable('tudor-matching-lab',
    'drivers/tudor_native/matching_lab.c', dependencies: libfprint_dep, install: false)
tudor_storage_test = executable('test-tudor-storage',
    'drivers/tudor_native/test_template_storage.c', dependencies: libfprint_private_dep,
    link_args: get_option('b_sanitize') == 'none' ? [] : ['-no-pie'], install: false)
test('tudor-storage', tudor_storage_test, env: ['G_DEBUG=fatal-warnings'], timeout: 10)
"""+patched[end:]
    (stage/'libfprint/meson.build').write_text(patched)
    env=environment()
    if args.sanitize: env['ASAN_OPTIONS']='detect_leaks=0'
    setup=['meson','setup',str(build),str(stage),'-Ddrivers=tudor_native',
        '-Dintrospection=false','-Ddoc=false','-Dinstalled-tests=false',
        '-Dudev_rules=disabled','-Dudev_hwdb=disabled','-Dgtk-examples=false']
    if args.sanitize: setup.append('-Db_sanitize=address,undefined')
    if args.matching:
        flags='-DTUDOR_MATCHING_LAB'
        if args.service_test: flags+=' -DTUDOR_ADAPTER_TEST -DTUDOR_SERVICE_TEST'
        setup.append('-Dc_args='+flags)
    if (build/'meson-private/coredata.dat').exists(): setup.extend(['--reconfigure','--clearcache'])
    subprocess.run(setup,env=env,check=True)
    targets=['tudor-matching-lab','test-tudor-matching','test-tudor-storage'] if args.matching else ['tudor-capture-lab','test-tudor-adapter']
    tests=['tudor-matching','tudor-storage'] if args.matching else ['tudor-adapter']
    subprocess.run(['meson','compile','-C',str(build)]+targets+['test-tudor-transport'],env=env,check=True)
    subprocess.run(['meson','test','-C',str(build)]+tests+['tudor-transport','--print-errorlogs'],env=env,check=True)
    print('Private adapter and tests built. System libfprint and login are unchanged.')


if __name__=='__main__':
    main()
