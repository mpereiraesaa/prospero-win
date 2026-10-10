#!/usr/bin/env python3
"""Owned-client retirement: driver consumes prefixes before CRT frees them."""
import ast,pathlib,re,subprocess,tempfile,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
FIXTURE=r"""
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include "pw_vk_retire.h"
struct client { uint64_t native; unsigned live; };
static unsigned freed,replayed;
static void *fail(size_t n){(void)n;return NULL;}
static void release(void *p){struct client *c=p;assert(c->live&&replayed);c->live=0;freed++;free(p);}
static struct client *client(unsigned n){struct client *c=malloc(sizeof(*c));assert(c);c->live=1;c->native=n;return c;}
int main(void){
 struct pw_vk_retirement q={0};struct client *device=client(1),*pool=client(2),*cb=client(3),*oom=client(4);
 assert(pw_vk_retirement_add(&q,cb,malloc));assert(pw_vk_retirement_add(&q,pool,malloc));assert(pw_vk_retirement_add(&q,device,malloc));
 assert(!freed&&cb->live&&pool->live&&device->live);
 /* Another producer globally replays command, free-buffer, destroy-pool,
  * destroy-device in sequence. Every client prefix remains readable. */
 assert(cb->native==3&&pool->native==2&&device->native==1);replayed=1;
 pw_vk_retirement_drain(&q,release,free);assert(freed==3&&!q.head);
 pw_vk_retirement_drain(&q,release,free);assert(freed==3);
 assert(!pw_vk_retirement_add(&q,oom,fail));assert(oom->live&&!q.head);release(oom);assert(freed==4);
 assert(pw_vk_retirement_add(&q,NULL,fail));return 0;
}
"""
class Retirement(unittest.TestCase):
 def test_only_destroy_wrappers_change(self):
  tree=ast.parse((ROOT/'tools/stage_vk_batch.py').read_text());fn=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='retire_loader')
  scope={'re':re};exec(compile(ast.Module(body=[fn],type_ignores=[]),'stage-hook','exec'),scope)
  names=['vkDestroyInstance','vkDestroyDevice','vkDestroyCommandPool','vkFreeCommandBuffers']
  source='\n'.join('void WINAPI '+n+'(void *p)\n{\n    free(p);\n}\n' for n in names)+'\nvoid create_failure(void *p) { free(p); }\n'
  result=scope['retire_loader'](source)
  self.assertEqual(result.count('pw_vk_batch_retire_free(p);'),4)
  self.assertIn('void create_failure(void *p) { free(p); }',result)
  self.assertEqual(result.count('#ifdef _WIN64'),4)
 def test_owned_retirement(self):
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'test.c').write_text(FIXTURE)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(ROOT/'wine/ps5/vulkan'),str(p/'test.c'),str(ROOT/'wine/ps5/vulkan/pw_vk_retire.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
