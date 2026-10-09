#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check builder opt-in metadata with a fake compiler; never a binary proof."""
import json, pathlib, runpy, sys, tempfile
from unittest.mock import patch
root=pathlib.Path(__file__).resolve().parents[2]
def compiler(command,**kwargs):
    pathlib.Path(command[command.index('-o')+1]).write_bytes(b'metadata-only-not-a-PE')
    class Result:
        returncode=0
        stdout=stderr=''
    return Result()
with tempfile.TemporaryDirectory() as temporary:
    for draws in (False,True):
        for pipeline in (False,True):
            output=pathlib.Path(temporary)/f'{draws}-{pipeline}'
            argv=[str(root/'tests/lab/d3d9_game_build.py'),'--output',str(output)]
            if draws:argv+=['--draws']
            if pipeline:argv+=['--pipeline']
            with patch.object(sys,'argv',argv),patch('subprocess.run',compiler):
                runpy.run_path(argv[0],run_name='__main__')
            receipt=json.loads((output/'receipt.json').read_text())
            expected=63487|(196608 if draws else 0)|(262144 if pipeline else 0)
            assert receipt['features']==expected
            assert receipt['pipeline_compiled']==pipeline and receipt['draws_compiled']==draws
            assert receipt['frame_bytes']==8192 and receipt['ring_bytes']==(65536 if pipeline else 8192)
            assert len(receipt['builds'])==2
            for build in receipt['builds']:
                command=build['command']
                assert ('-DPW_D3D9_ENABLE_PIPELINE' in command)==pipeline
                assert any(s.endswith('/pw_d3d9_batch_pipeline.c') for s in command)==pipeline
                assert ('-DPW_D3D9_ENABLE_DRAW_BATCH' in command)==draws
                assert '-DPW_D3D9_ENABLE_API_OBSERVE' not in command
print('PIPELINE_BUILD_METADATA PASS four configurations; fake compiler only')
