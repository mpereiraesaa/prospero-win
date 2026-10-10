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

    def test_manifest_metadata_rejects_before_any_output_mutation(self):
        cases = {
            'missing-version': lambda m: m.pop('llvm_mingw'),
            'null-version': lambda m: m.update(llvm_mingw=None),
            'version-type': lambda m: m.update(llvm_mingw=3),
            'version-newline': lambda m: m.update(llvm_mingw='ucrt\nfalse source'),
            'version-long': lambda m: m.update(llvm_mingw='x' * 65),
            'files-type': lambda m: m.update(files=[]),
            'files-too-many': lambda m: m['files'].update({f'i386-windows/c{i}.dll': {} for i in range(65)}),
            'record-type': lambda m: m['files'].update({'i386-windows/opengl32.dll': []}),
            'hash-type': lambda m: m['files']['i386-windows/opengl32.dll'].update(sha256=3),
            'machine-type': lambda m: m['files']['i386-windows/opengl32.dll'].update(machine=True),
            'imports-missing': lambda m: m['files']['i386-windows/opengl32.dll'].pop('imports'),
            'imports-type': lambda m: m['files']['i386-windows/opengl32.dll'].update(imports='vulkan-1.dll'),
            'imports-too-many': lambda m: m['files']['i386-windows/opengl32.dll'].update(imports=['vulkan-1.dll'] * 257),
            'imports-path': lambda m: m['files']['i386-windows/opengl32.dll'].update(imports=['../libc++.dll']),
            'imports-value': lambda m: m['files']['i386-windows/opengl32.dll'].update(imports=[None]),
            'licenses-type': lambda m: m.update(license_sha256=[]),
            'license-hash-type': lambda m: m.update(license_sha256={'LICENSES/mesa/MIT': None}),
            'license-count': lambda m: m.update(license_sha256={f'LICENSES/{i}': '0'*64 for i in range(257)}),
            'license-long': lambda m: m.update(license_sha256={'LICENSES/' + 'x'*257: '0'*64}),
            'duplicate-case': lambda m: m['files'].update({'i386-windows/OPENGL32.dll': m['files']['i386-windows/opengl32.dll']}),
            'unicode-name': lambda m: m['files'].update({'i386-windows/K.dll': m['files']['i386-windows/opengl32.dll']}),
        }
        for name, mutate in cases.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp:
                base = Path(temp); artifacts = base / 'artifacts'; artifacts.mkdir()
                manifest = fixture(artifacts); mutate(manifest)
                (artifacts / 'manifest.json').write_text(json.dumps(manifest))
                app = base / 'app'; app.mkdir(); (app / 'SOURCES.txt').write_bytes(b'original source')
                (app / 'keep').write_bytes(b'original package')
                before = {str(f.relative_to(app)): f.read_bytes() for f in app.rglob('*') if f.is_file()}
                with self.assertRaises(ValueError): p.package(artifacts, app)
                self.assertEqual({str(f.relative_to(app)): f.read_bytes() for f in app.rglob('*') if f.is_file()}, before)
                self.assertFalse((app / 'win').exists())

    def test_bounded_files_and_symlink_inputs(self):
        for problem in ['manifest-size', 'dll-size', 'license-size', 'dll-symlink', 'arch-symlink']:
            with self.subTest(problem=problem), tempfile.TemporaryDirectory() as temp:
                base = Path(temp); artifacts = base / 'artifacts'; artifacts.mkdir(); manifest = fixture(artifacts)
                if problem == 'manifest-size':
                    (artifacts / 'manifest.json').write_bytes(b' ' * (p.MAX_MANIFEST + 1))
                elif problem == 'dll-size':
                    with (artifacts / 'i386-windows/opengl32.dll').open('r+b') as f: f.truncate(p.MAX_DLL + 1)
                elif problem == 'license-size':
                    with (artifacts / 'LICENSES/mesa/MIT').open('r+b') as f: f.truncate(p.MAX_LICENSE + 1)
                elif problem == 'dll-symlink':
                    target = artifacts / 'i386-windows/opengl32.dll'; target.rename(base / 'original.dll'); target.symlink_to(base / 'original.dll')
                else:
                    target = artifacts / 'i386-windows'; target.rename(base / 'arch'); target.symlink_to(base / 'arch', target_is_directory=True)
                app = base / 'app'; app.mkdir()
                with self.assertRaises(ValueError): p.package(artifacts, app)
                self.assertEqual(list(app.iterdir()), [])

if __name__=='__main__':unittest.main()
