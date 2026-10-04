#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Native checks of the actual added ABI and client cache/hit bodies.

Wine signals/server lifecycle/APCs/exception behavior are not exercised.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'wine/patches/0820-ntdll-ps5-shared-mutex-client.patch'


def additions(patch, file):
    parts = patch.read_text().split('diff --git ')[1:]
    part = next(p for p in parts if p.splitlines()[0].endswith(' b/' + file))
    return '\n'.join(line[1:] for line in part.splitlines()
                     if line.startswith('+') and not line.startswith('+++')) + '\n'


def function_body(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    for end in range(brace + 1, len(text)):
        if text[end] == '{': depth += 1
        if text[end] == '}': depth -= 1
        if not depth: return text[start:end + 1] + '\n'
    raise AssertionError('unterminated source function')


def main():
    body = additions(PATCH, 'dlls/ntdll/unix/server.c')
    cache = body[body.index('/* Exact canonical handle table,'):]
    cache = cache[:cache.index('\n#endif')]
    cache += '\n' + function_body(body, 'void server_clear_shared_mutex_slot(')
    # Every existing close_inproc_sync call has its preceding added clear.
    for file, expected in [('dlls/ntdll/unix/server.c', 3), ('dlls/ntdll/unix/thread.c', 1)]:
        part = next(p for p in PATCH.read_text().split('diff --git ')[1:]
                    if p.splitlines()[0].endswith(' b/' + file))
        lines = part.splitlines()
        pairs = [(lines[i - 1], line) for i, line in enumerate(lines)
                 if line.startswith(' ') and 'close_inproc_sync( ' in line]
        assert len(pairs) == expected
        assert all('server_clear_shared_mutex_slot( ' in prior for prior, line in pairs)
    with tempfile.TemporaryDirectory(prefix='pw-shared-mutex-client-') as temp:
        folder = Path(temp)
        (folder / 'ps5_mutex_word.h').write_text(additions(
            ROOT / 'wine/patches/0810-server-ps5-shared-mutex-word.patch', 'include/wine/ps5_mutex_word.h'))
        (folder / 'ps5_mutex_backend.h').write_text(additions(PATCH, 'include/wine/ps5_mutex_backend.h'))
        # 0820 generalizes the complete switch body added by 0790. Check
        # each replacement against the actual changed lines before compiling
        # that resulting function, rather than reproducing the parser logic.
        switch = function_body(additions(
            ROOT / 'wine/patches/0790-server-ps5-immediate-mutex-calls.patch',
            'dlls/ntdll/unix/server.c'), 'static int server_mutex_fast_enabled(')
        replacements = [
            ('static int server_mutex_fast_enabled(void)',
             'static int server_mutex_switch_enabled( const char *environment, const char *file_name )'),
            ('const char *env = getenv( "WINE_PS5_MUTEX_FAST" );',
             'const char *env = getenv( environment );'),
            ('asprintf( &path, "%s/pw_mutex_fast", config_dir )',
             'asprintf( &path, "%s/%s", config_dir, file_name )'),
        ]
        for before, after in replacements:
            assert switch.count(before) == 1 and after in body
            switch = switch.replace(before, after)
        (folder / 'shared_mutex_switch.inc').write_text(switch)
        (folder / 'shared_mutex_client.inc').write_text(cache)
        server = additions(PATCH, 'server/request.c')
        server_abi = function_body(server, 'static int get_shared_mutex_word(')
        server_abi += function_body(server, 'DECLSPEC_EXPORT const struct pw_mutex_backend *pw_wineserver_mutex_backend(')
        (folder / 'shared_mutex_server_abi.inc').write_text(server_abi)
        command = shlex.split(os.environ.get('CC', 'cc'))
        command += shlex.split(os.environ.get('CFLAGS', '-O2 -g -Wall -Wextra -Werror'))
        command += ['-std=gnu11', '-pthread', '-I', str(folder),
                    str(ROOT / 'tests/fixtures/wine_shared_mutex_client.c'), '-o', str(folder / 'test')]
        subprocess.run(command, check=True)
        subprocess.run([str(folder / 'test'), str(folder)], check=True, timeout=60)


if __name__ == '__main__':
    main()
