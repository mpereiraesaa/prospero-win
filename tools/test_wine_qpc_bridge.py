#!/usr/bin/env python3
"""Run the staged Wine QPC body as PE32/PE64 with a deterministic provider.

Only the TSC instruction and provider calls are mocked. This is not a hardware
TSC validation or a full Wine rebuild. Uses an isolated local Wine prefix.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--wine', default='wine')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = (args.source / 'dlls/ntdll/time.c').read_text()
    start = source.index('BOOL WINAPI DECLSPEC_HOTPATCH RtlQueryPerformanceCounter')
    end = source.index('/******************************************************************************', start + 10)
    body = source[start:end]
    if 'PW_QPC_PROCESS_INFO' not in body:
        raise SystemExit('Source lacks the shared QPC bridge')
    body = body.replace('RtlQueryPerformanceCounter', 'fixture_qpc')
    body = body.replace('BOOL WINAPI DECLSPEC_HOTPATCH', 'BOOL WINAPI')
    body, replaced = re.subn(
        r'__asm__ volatile\( "lfence; rdtsc" : "=a"\(lo\), "=d"\(hi\) :: "memory" \);',
        'mock_read_tsc(&lo, &hi);', body)
    if replaced != 2:
        raise SystemExit('Unexpected ordered TSC instruction count')
    output = args.output or Path(tempfile.mkdtemp(prefix='wine-qpc-tests-'))
    output.mkdir(parents=True, exist_ok=True)
    (output / 'qpc_body.inc').write_text(body)
    env = os.environ.copy()
    env.update(WINEPREFIX=str(output.resolve() / 'wine-prefix'), WINEDEBUG='-all',
               WINEDLLOVERRIDES='mscoree,mshtml=')
    for arch in ['i686', 'x86_64']:
        exe = output / (arch + '.exe')
        subprocess.run([arch + '-w64-mingw32-gcc', '-std=c11', '-O2', '-Wall',
                        '-Wextra', '-Werror', '-I' + str(root / 'wine/ps5/time'),
                        '-I' + str(output), str(root / 'tests/lab/pw_qpc_bridge.c'),
                        '-o', str(exe)], check=True, timeout=30)
        for mode in ['unsupported', 'malformed', 'enabled']:
            subprocess.run([args.wine, str(exe.resolve()), mode], env=env,
                           check=True, timeout=60)
    print('PE32 and PE64 deterministic QPC bridge scenarios passed')


if __name__ == '__main__':
    main()
