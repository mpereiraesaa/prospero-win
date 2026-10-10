#!/usr/bin/env python3
"""Run actual staged PE32 key wrappers against deterministic shared objects.

The provider and thread-info lookup are mocked. The shared reader and wrapper
control flow are unmodified. This does not test hardware input delivery.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--wine', default='wine')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = (args.source / 'dlls/win32u/main.c').read_text()
    start = source.index('SHORT WINAPI NtUserGetKeyState( INT key )')
    body = source[start:source.index('\n#endif', start)]
    if 'ps5_key_shared' not in body:
        raise SystemExit('Source lacks the shared input bridge')
    output = args.output or Path(tempfile.mkdtemp(prefix='wine-key-tests-'))
    output.mkdir(parents=True, exist_ok=True)
    (output / 'key_body.inc').write_text(body)
    exe = output / 'key-bridge.exe'
    subprocess.run(['i686-w64-mingw32-gcc', '-std=c11', '-O2', '-Wall', '-Wextra',
                    '-Werror', '-I' + str(root / 'wine/ps5/input'), '-I' + str(output),
                    str(root / 'tests/lab/pw_key_bridge.c'), '-o', str(exe)],
                   check=True, timeout=30)
    env = os.environ.copy()
    env.update(WINEPREFIX=str(output.resolve() / 'wine-prefix'), WINEDEBUG='-all',
               WINEDLLOVERRIDES='mscoree,mshtml=')
    for mode in ['unsupported', 'enabled', 'concurrent']:
        subprocess.run([args.wine, str(exe.resolve()), mode], env=env, check=True, timeout=60)
    print('Actual PE32 shared-key wrapper scenarios passed')


if __name__ == '__main__':
    main()
