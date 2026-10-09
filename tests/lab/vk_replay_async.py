#!/usr/bin/env python3
"""Build actual Unix adapter with native Wine headers and a controlled driver."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import resource
import runpy
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', required=True, help='Full pinned or staged Wine source')
parser.add_argument('--build', required=True, help='Configured native Wine build (config headers)')
parser.add_argument('--runtime-root', help='Repository containing the adapter under test')
parser.add_argument('--output', required=True)
parser.add_argument('--baseline-adapter', help='Optional previous actual adapter for isolated handoff comparison')
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
if 'PW_VK_POOL_MAX_SLOTS' not in header:
    life = runpy.run_path(str(runtime / 'tools/stage_vk_replay_lifecycle.py'))
    barriers = runpy.run_path(str(runtime / 'tools/stage_vk_replay_barriers.py'))
    fanout = runpy.run_path(str(runtime / 'tools/stage_vk_replay_fanout.py'))
    original_header, original_source = life['transform']((directory / 'vulkan_private.h').read_text(), (directory / 'vulkan.c').read_text())
    staged = barriers['transform'](original_header, original_source, (directory / 'vulkan_thunks.c').read_text(), (directory / 'make_vulkan').read_text())
    header, _, _, _ = fanout['transform'](*staged)
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
ordered = classifier['classify_ordered']((directory / 'vulkan_thunks.c').read_text(), loader,
                                        (source / 'include/wine/vulkan_driver.h').read_text())
(output / 'pw_vk_replay_dispatch.h').write_text(classifier['generate'](eligible, ordered))
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
def execute(command, failure=False, expected_message='PW_VK_REPLAY_FATAL', trace=False):
    result = subprocess.run(command, text=True, capture_output=True, timeout=45,
                            env={**os.environ, "PW_VK_REPLAY_TRACE": "1" if trace else "0"},
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    commands.append(dict(command=command, exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr,
                         expected_failure=failure))
    if failure:
        assert result.returncode == -6 and expected_message in result.stderr, result.stdout + result.stderr
    else:
        assert result.returncode == 0, result.stdout + result.stderr
    return result
for name, extra in [('host', []), ('sanitize', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'])]:
    execute(['cc', *flags, *extra, *map(str, inputs), '-pthread', '-o', str(output / name)])
    execute([str(output / name)])
    execute([str(output / name), 'legacy'])
    execute([str(output / name), 'fanout'])
    execute([str(output / name), 'init-race'])
    epoch = execute([str(output / name), 'epoch-workload'])
    assert 'jobs=1 admission_global_waits=0' in epoch.stdout, epoch.stdout
    execute([str(output / name), 'epoch-overlap'])
traced = execute([str(output / 'host')], trace=True)
for event in ('initialize_begin', 'initialize_end', 'create_begin', 'create_end',
              'worker_enter', 'enqueue_begin', 'enqueue_end', 'job_begin', 'job_end',
              'dispatch_begin', 'dispatch_end'):
    assert 'event=' + event in traced.stderr, (event, traced.stderr)
for event in ('enqueue_begin', 'enqueue_end', 'job_begin', 'job_end', 'dispatch_begin', 'dispatch_end'):
    assert 1 <= traced.stderr.count('event=' + event + ' ') <= (256 if event.startswith('dispatch') else 8), (event, traced.stderr)
bounded = execute([str(output / 'host'), 'trace-bounds'], trace=True)
for event in ('dispatch_begin', 'dispatch_end'):
    assert bounded.stderr.count('event=' + event + ' ') == 8 * 32, bounded.stderr
    for job in range(1, 9):
        assert bounded.stderr.count('event=' + event + ' job=' + str(job) + ' ') == 32
    assert 'event=' + event + ' job=9 ' not in bounded.stderr
assert 'event=stack_default ' in bounded.stderr
execute([str(output / 'host'), 'fatal'], failure=True)
execute([str(output / 'host'), 'fatal-epoch'], failure=True)
# Recreate the missing-initialization-lock regression; the paused constructor
# makes the raw hook's premature return deterministic, without a timing race.
actual_adapter = (output / 'pw_vk_batch_unix.c').read_text()
start = actual_adapter.index('static void initialize_workers(void)')
end = actual_adapter.index('\nstatic void must_complete', start)
init = actual_adapter[start:end]
assert init.count('pthread_mutex_lock(&admission);') == 1
assert init.count('pthread_mutex_unlock(&admission);') == 1
broken = init.replace('pthread_mutex_lock(&admission);', '').replace('pthread_mutex_unlock(&admission);', ';')
(output / 'pw_vk_batch_unix.c').write_text(actual_adapter[:start] + broken + actual_adapter[end:])
try:
    execute(['cc', *flags, *map(str, inputs), '-pthread', '-o', str(output / 'negative-init')])
finally:
    (output / 'pw_vk_batch_unix.c').write_text(actual_adapter)
execute([str(output / 'negative-init'), 'init-race'], failure=True, expected_message='!waiter_done')

baseline_comparison = None
if args.baseline_adapter:
    baseline = Path(args.baseline_adapter).read_bytes()
    (output / 'pw_vk_batch_unix.c').write_bytes(baseline)
    try:
        execute(['cc', *flags, *map(str, inputs), '-pthread', '-o', str(output / 'baseline')])
    finally:
        (output / 'pw_vk_batch_unix.c').write_text(actual_adapter)
    before = execute([str(output / 'baseline'), 'epoch-workload'])
    assert 'jobs=24 admission_global_waits=24' in before.stdout, before.stdout
    baseline_comparison = dict(adapter_sha256=hashlib.sha256(baseline).hexdigest(),
                               before=before.stdout.strip(), after=epoch.stdout.strip())

receipt = dict(status='pass', baseline_comparison=baseline_comparison, console_accessed=False, actual_unix_adapter=True, actual_manual_codec=True,
               generated_codec_tested=False, commands=commands,
               sha256={str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in
                       inputs + [output / 'pw_vk_batch_unix.c', output / 'vulkan_private.h', output / 'pw_vk_replay_dispatch.h']})
(output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print('PASS ' + str(output / 'receipt.json'))
