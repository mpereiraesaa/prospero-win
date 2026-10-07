#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib,importlib.util,json,struct,sys,tempfile,unittest
from pathlib import Path
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parent.parent;sys.path.insert(0,str(ROOT/'tools'))
import package_mesa_zink as p

def fixture(directory):
    files={}
    for arch,machine,optional in [('i386-windows',0x14c,0x10b),('x86_64-windows',0x8664,0x20b)]:
        for name in ['opengl32.dll','libgallium_wgl.dll']:
            data=bytearray(90);data[:2]=b'MZ';struct.pack_into('<I',data,60,64);data[64:68]=b'PE\0\0';struct.pack_into('<H',data,68,machine);struct.pack_into('<H',data,88,optional)
            file=directory/arch/name;file.parent.mkdir(parents=True,exist_ok=True);file.write_bytes(data);files[arch+'/'+name]={'machine':machine,'sha256':p.digest(file),'imports':['vulkan-1.dll']}
    license=directory/'LICENSES/mesa/MIT';license.parent.mkdir(parents=True);license.write_text('fixture license')
    result={'driver':'zink','architectures':['i386-windows','x86_64-windows'],'mesa_commit':'1'*40,'llvm_mingw':'20260922-ucrt','llvm_mingw_archive_sha256':'2'*64,'files':files,'license_sha256':{'LICENSES/mesa/MIT':p.digest(license)}}
    (directory/'manifest.json').write_text(json.dumps(result));return result

class Packaging(unittest.TestCase):
    def test_both_architectures_and_licences(self):
        with tempfile.TemporaryDirectory() as t:
            t=Path(t);artifacts=t/'artifacts';artifacts.mkdir();m=fixture(artifacts);app=t/'app';app.mkdir();(app/'SOURCES.txt').write_text('owner source\n')
            builtin=app/'win/wine/lib/wine/i386-windows/opengl32.dll';builtin.parent.mkdir(parents=True);builtin.write_bytes(b'original Wine OpenGL')
            p.package(artifacts,app)
            self.assertEqual(builtin.read_bytes(),b'original Wine OpenGL')
            for name,record in m['files'].items():self.assertEqual(p.digest(app/'win/mesa-zink'/name),record['sha256'])
            self.assertEqual((app/'LICENSES/mesa/MIT').read_text(),'fixture license');self.assertIn('owner source',(app/'SOURCES.txt').read_text())
    def test_all_invalid_inputs_reject_before_copy(self):
        for problem in ['corrupt-dll','mixed-machine','missing-arch','missing-licence','unsafe-licence','missing-runtime','bad-source-pin','bad-compiler-pin','corrupt-licence']:
            with self.subTest(problem=problem),tempfile.TemporaryDirectory() as t:
                t=Path(t);artifacts=t/'artifacts';artifacts.mkdir();m=fixture(artifacts);app=t/'app';app.mkdir()
                if problem=='corrupt-dll':(artifacts/'i386-windows/opengl32.dll').write_bytes(b'bad')
                elif problem=='mixed-machine':
                    data=(artifacts/'x86_64-windows/opengl32.dll').read_bytes();(artifacts/'i386-windows/opengl32.dll').write_bytes(data);m['files']['i386-windows/opengl32.dll']['sha256']=hashlib.sha256(data).hexdigest()
                elif problem=='missing-arch':del m['files']['x86_64-windows/opengl32.dll']
                elif problem=='missing-licence':m['license_sha256']={}
                elif problem=='unsafe-licence':m['license_sha256']={'LICENSES/../../escape':'3'*64}
                elif problem=='missing-runtime':m['files']['i386-windows/opengl32.dll']['imports']=['libc++.dll']
                elif problem=='bad-source-pin':m['mesa_commit']='z'*40
                elif problem=='bad-compiler-pin':m['llvm_mingw_archive_sha256']='z'*64
                else:(artifacts/'LICENSES/mesa/MIT').write_text('altered license')
                (artifacts/'manifest.json').write_text(json.dumps(m))
                with self.assertRaises(ValueError):p.package(artifacts,app)
                self.assertEqual(list(app.iterdir()),[])

if __name__=='__main__':unittest.main()
