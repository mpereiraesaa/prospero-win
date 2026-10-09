#!/usr/bin/env python3
"""Build actual Unix adapter with native Wine headers and a controlled driver."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import runpy
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', required=True, help='Full pinned or staged Wine source')
parser.add_argument('--build', required=True, help='Configured native Wine build (config headers)')
parser.add_argument('--runtime-root', help='Repository containing the adapter under test')
parser.add_argument('--output', required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
runtime = Path(args.runtime_root).resolve() if args.runtime_root else repo
source, build, output = (Path(value).resolve() for value in (args.source, args.build, args.output))
output.mkdir(parents=True, exist_ok=True)
directory = source / 'dlls/winevulkan'
header = (directory / 'vulkan_private.h').read_text()
if 'struct wine_cmd_buffer\n' not in header:
    stage = runpy.run_path(str(runtime / 'tools/stage_vk_replay_lifecycle.py'))
    header, _ = stage['transform'](header, (directory / 'vulkan.c').read_text())
(output / 'vulkan_private.h').write_text(header)
loader = (directory / 'loader_thunks.h').read_text()
if 'unix_pw_vk_batch,' not in loader:
    loader = loader.replace('    unix_count,', '    unix_pw_vk_batch,\n    unix_count,')
(output / 'loader_thunks.h').write_text(loader)
for name in ('vulkan_loader.h', 'vulkan_thunks.h'):
    (output / name).write_bytes((directory / name).read_bytes())
classifier = runpy.run_path(str(runtime / 'tools/generate_vk_replay_dispatch.py'))
eligible, _ = classifier['classify']((directory / 'vulkan_thunks.c').read_text(), loader,
                                    (source / 'include/wine/vulkan_driver.h').read_text())
(output / 'pw_vk_replay_dispatch.h').write_text(classifier['generate'](eligible))
# Copy the exact adapter: quoted includes then resolve against the staged fixture
# headers, rather than accidentally selecting a different staged generation.
for name in ('pw_vk_batch_unix.c', 'pw_vk_async.h'):
    (output / name).write_bytes((runtime / 'wine/ps5/vulkan' / name).read_bytes())
flags = ['-std=gnu11', '-g', '-Wall', '-Wextra', '-Werror', '-D__WINESRC__', '-DWINE_UNIX_LIB', '-D_WIN64',
         '-I' + str(output), '-I' + str(build / 'include'), '-I' + str(source / 'include'),
         '-I' + str(directory), '-I' + str(runtime / 'wine/ps5'), '-I' + str(runtime / 'wine/ps5/vulkan')]
inputs = [repo / 'tests/lab/vk_replay_async.c'] + [runtime / 'wine/ps5' / name for name in
          ('pw_vk_wire.c', 'pw_vk_command_stream.c', 'pw_vk_replay.c')]
commands = []
def execute(command, failure=False):
    result = subprocess.run(command, text=True, capture_output=True, timeout=45,
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    commands.append(dict(command=command, exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr,
                         expected_failure=failure))
    if failure:
        assert result.returncode == -6 and 'PW_VK_REPLAY_FATAL' in result.stderr, result.stdout + result.stderr
    else:
        assert result.returncode == 0, result.stdout + result.stderr
    return result
for name, extra in [('host', []), ('sanitize', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'])]:
    execute(['cc', *flags, *extra, *map(str, inputs), '-pthread', '-o', str(output / name)])
    execute([str(output / name)])
    execute([str(output / name), 'legacy'])
execute([str(output / 'host'), 'fatal'], failure=True)
receipt = dict(status='pass', console_accessed=False, actual_unix_adapter=True, actual_manual_codec=True,
               generated_codec_tested=False, commands=commands,
               sha256={str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in
                       inputs + [output / 'pw_vk_batch_unix.c', output / 'vulkan_private.h', output / 'pw_vk_replay_dispatch.h']})
(output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print('PASS ' + str(output / 'receipt.json'))
