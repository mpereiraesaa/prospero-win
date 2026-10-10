#!/usr/bin/env python3
"""Portable codec ownership/bounds checks; actual Wine model tests live in lab/."""
import pathlib,subprocess,tempfile,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class CodecTests(unittest.TestCase):
 def compile_run(self,flags):
  with tempfile.TemporaryDirectory() as d:
   exe=pathlib.Path(d)/'codec';subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',*flags,str(ROOT/'tests/test_vk_codec.c'),str(ROOT/'wine/ps5/vulkan/pw_vk_codec.c'),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
 def test_host(self):self.compile_run([])
 def test_sanitizers(self):self.compile_run(['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie'])
if __name__=='__main__':unittest.main()
