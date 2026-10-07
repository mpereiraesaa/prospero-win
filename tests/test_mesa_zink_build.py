#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
import importlib.util,struct,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('manifest',ROOT/'tools/mesa_zink_manifest.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

def pe(machine,optional):
    data=bytearray(90);data[:2]=b'MZ';struct.pack_into('<I',data,60,64);data[64:68]=b'PE\0\0';struct.pack_into('<H',data,68,machine);struct.pack_into('<H',data,88,optional);return data

class BuildContracts(unittest.TestCase):
    def test_pe_architecture_and_corrupt_headers(self):
        with tempfile.TemporaryDirectory() as t:
            path=Path(t)/'module.dll'
            for machine,optional in [(0x14c,0x10b),(0x8664,0x20b)]:
                path.write_bytes(pe(machine,optional));self.assertEqual(m.pe_machine(path),machine)
            for data in [b'',b'MZ',pe(0x14c,0x20b),pe(0x8664,0x10b),pe(0x1c4,0x10b),bytes(pe(0x14c,0x10b))[:65]]:
                path.write_bytes(data)
                with self.assertRaises(ValueError):m.pe_machine(path)
    def test_source_pin_rejects_changed_and_missing_files(self):
        with tempfile.TemporaryDirectory() as t:
            repo=Path(t)/'repo';repo.mkdir();subprocess.run(['git','init','-q',str(repo)],check=True)
            (repo/'VERSION').write_text('26.2.0');(repo/'source.c').write_text('int x;');(repo/'alias').symlink_to('source.c')
            subprocess.run(['git','-C',str(repo),'add','.'],check=True)
            subprocess.run(['git','-C',str(repo),'-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-qm','source'],check=True)
            rev=subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip();m.verify_source(repo,repo,rev)
            (repo/'source.c').write_text('int changed;')
            with self.assertRaises(ValueError):m.verify_source(repo,repo,rev)
            (repo/'source.c').unlink()
            with self.assertRaises(FileNotFoundError):m.verify_source(repo,repo,rev)
    def test_archive_eol_conversion(self):
        import tarfile
        with tempfile.TemporaryDirectory() as t:
            t=Path(t);repo=t/'repo';repo.mkdir();source=t/'export';source.mkdir()
            subprocess.run(['git','init','-q',str(repo)],check=True)
            (repo/'.gitattributes').write_text('*.csv eol=crlf\n')
            (repo/'data.csv').write_bytes(b'one,two\n')
            subprocess.run(['git','-C',str(repo),'add','.'],check=True)
            subprocess.run(['git','-C',str(repo),'-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-qm','source'],check=True)
            data=subprocess.check_output(['git','-C',str(repo),'archive','HEAD'])
            import io
            with tarfile.open(fileobj=io.BytesIO(data)) as archive:archive.extractall(source,filter='data')
            self.assertEqual((source/'data.csv').read_bytes(),b'one,two\r\n')
            m.verify_source(source,repo,'HEAD')
            (source/'data.csv').write_bytes(b'one,two\n')
            with self.assertRaises(ValueError):m.verify_source(source,repo,'HEAD')
    def test_bad_archive_rejects_before_work_creation(self):
        with tempfile.TemporaryDirectory() as t:
            t=Path(t);archive=t/'compiler.tar.xz';archive.write_bytes(b'bad');work=t/'work'
            result=subprocess.run(['bash',str(ROOT/'tools/build_mesa_zink.sh'),'--work',str(work),'--mesa',str(t),'--llvm-mingw',str(archive)],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0);self.assertFalse(work.exists());self.assertIn('archive hash mismatch',result.stderr)
    def test_invalid_jobs(self):
        for jobs in ['0','-1','abc']:
            result=subprocess.run(['bash',str(ROOT/'tools/build_mesa_zink.sh'),'--jobs',jobs],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0);self.assertIn('positive',result.stderr)

if __name__=='__main__':unittest.main()
