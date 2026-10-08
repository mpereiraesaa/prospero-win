#!/usr/bin/env python3
"""Compatibility invariants of the additive native replay staging patch."""
import ast,pathlib,re,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class Stage(unittest.TestCase):
 def test_native_dispatch_generator(self):
  tree=ast.parse((ROOT/'tools/stage_vk_batch.py').read_text());fn=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='generator')
  def once(s,a,b):
   self.assertEqual(s.count(a),1);return s.replace(a,b)
  scope={'once':once,'ast':ast};exec(compile(ast.Module(body=[fn],type_ignores=[]),'stage','exec'),scope)
  original='def emit(f):\n        func=Function()\n'
  for bits in [32,64]:original+='        if True:\n            f.write(f"    {func.unixlib_entry('+str(bits)+')},\\n")\n'
  original+='        f.write("    unix_count,\\n")\n        f.write("C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_count);\\n")\n'
  transformed=scope['generator'](original)
  class Function:
   needs_thunk=True
   name='vkCmdDraw'
   def is_perf_critical(self):return True
   def unixlib_entry(self,bits):return 'thunk'+str(bits)+'_'+self.name
  class Type:
   @staticmethod
   def all(*unused):return [Function()]
  import io
  output=io.StringIO();environment={'Type':Type,'Function':Function,'func':Function()};exec(transformed,environment);environment['emit'](output)
  text=output.getvalue();self.assertIn('pw_vk_batch_dispatch_native',text)
  self.assertIn('thunk64_vkCmdDraw(args);',text);self.assertIn('thunk32_vkCmdDraw(args);',text)
  self.assertIn('return STATUS_SUCCESS;',text);self.assertEqual(text.count('    pw_vk_batch_unix,'),2)
 def test_distinct_capabilities(self):
  text=(ROOT/'wine/ps5/vulkan/pw_vk_batch.h').read_text()
  self.assertIn('"__wine_pw_vk_batch_v2"',text);self.assertIn('"__wine_pw_vk_batch_v1"',text)
  self.assertIn('0x50570201u',text);self.assertIn('0x50570101u',text)
if __name__=='__main__':unittest.main()
