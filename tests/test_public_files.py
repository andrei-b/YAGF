import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
SCRIPT=ROOT/'packaging/check-public-files.py'
spec=importlib.util.spec_from_file_location('public_files',SCRIPT)
audit=importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class PublicFileTests(unittest.TestCase):
    def test_local_paths_keys_and_network_addresses_are_detected(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'profile.txt'
            path.write_bytes(b'/'+b'home/'+b'example-user/models\n'
                             +b'http://'+b'10.'+b'20.30.40:8765\n'
                             +b'-----BEGIN '+b'PRIVATE KEY-----\n')
            issues=audit.inspect(path,'service/profile.txt')
            self.assertIn('personal home path',issues)
            self.assertIn('concrete service IP address',issues)
            self.assertIn('private key material',issues)
            path.write_bytes(b'/'+b'home/'+b'example-user\n/'+b'mnt/'+b'example-disk\n')
            issues=audit.inspect(path,'service/profile.txt')
            self.assertIn('personal home path',issues)
            self.assertIn('deployment mount path',issues)

    def test_generic_examples_and_loopback_defaults_are_allowed(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'example.txt'
            path.write_text('http://127.0.0.1:8765\nhttp://ocr-host.example:8765\n/srv/yagf/models\n')
            self.assertEqual(audit.inspect(path,'service/example.txt'),[])

    def test_package_audit_checks_owned_files_without_printing_values(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            path=root/'usr/share/yagf2/service/local.env'
            path.parent.mkdir(parents=True)
            secret='http://'+'10.'+'20.30.40:8765'
            path.write_text(secret)
            result=subprocess.run([sys.executable,str(SCRIPT),'--root',str(root)],capture_output=True,text=True)
            self.assertEqual(result.returncode,1)
            self.assertIn('concrete service IP address',result.stdout)
            self.assertNotIn(secret,result.stdout)

    def test_public_source_passes_the_audit(self):
        result=subprocess.run([sys.executable,str(SCRIPT)],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_published_screenshots_and_reports_exclude_capture_host_metadata(self):
        payload=b'Creation Time\0example timestamp'
        png=b'\x89PNG\r\n\x1a\n'+struct.pack('>I',len(payload))+b'tEXt'+payload+b'\0'*4
        self.assertIn('screenshot capture metadata',audit.inspect_bytes(png,'docs/screenshots/example.png'))
        self.assertIn('published host/run metadata',audit.inspect_bytes(b'{"host":{}}','docs/benchmarks/example.json'))


class CommitHookTests(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory(prefix='yagf-commit-audit-')
        self.addCleanup(temporary.cleanup)
        self.repo=Path(temporary.name)
        self.git('init','--quiet')
        self.git('config','user.name','Audit test')
        self.git('config','user.email','audit@example.invalid')
        (self.repo/'packaging').mkdir()
        shutil.copy2(SCRIPT,self.repo/'packaging/check-public-files.py')
        shutil.copy2(ROOT/'packaging/install-git-hooks.sh',self.repo/'packaging/install-git-hooks.sh')
        shutil.copytree(ROOT/'.githooks',self.repo/'.githooks')
        self.install()
        (self.repo/'README.md').write_text('Public example\n')
        self.git('add','.')

    def git(self,*args,check=True):
        return subprocess.run(['git','-C',str(self.repo),*args],capture_output=True,text=True,check=check)

    def install(self,check=True):
        return subprocess.run(['bash',str(self.repo/'packaging/install-git-hooks.sh')],
                              capture_output=True,text=True,check=check)

    def test_commit_rejects_private_staged_content_despite_clean_working_copy(self):
        path=self.repo/'README.md'
        path.write_text('http://'+'10.'+'20.30.40:8765\n')
        self.git('add','README.md')
        path.write_text('http://ocr-host.example:8765\n')
        result=self.git('commit','-m','Unsafe staged snapshot',check=False)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('concrete service IP address',result.stdout+result.stderr)
        self.assertNotIn('10.'+'20.30.40',result.stdout+result.stderr)
        self.assertNotEqual(self.git('rev-parse','--verify','HEAD',check=False).returncode,0)

    def test_commit_uses_clean_staged_snapshot_despite_private_working_copy(self):
        (self.repo/'README.md').write_text('http://'+'10.'+'20.30.40:8765\n')
        result=self.git('commit','-m','Clean staged snapshot',check=False)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual(self.git('show','HEAD:README.md').stdout,'Public example\n')

    def test_installer_preserves_other_hook_configuration(self):
        self.git('config','core.hooksPath','custom-hooks')
        result=self.install(check=False)
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(self.git('config','--get','core.hooksPath').stdout.strip(),'custom-hooks')

    def test_commit_all_audits_gits_temporary_index(self):
        self.git('commit','-m','Clean baseline')
        (self.repo/'README.md').write_text('http://'+'10.'+'20.30.40:8765\n')
        result=self.git('commit','--all','-m','Unsafe automatic staging',check=False)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('concrete service IP address',result.stdout+result.stderr)
        self.assertEqual(self.git('show','HEAD:README.md').stdout,'Public example\n')

    def test_merge_and_applypatch_hooks_run_the_same_audit(self):
        path=self.repo/'README.md';path.write_text('http://'+'10.'+'20.30.40:8765\n')
        self.git('add','README.md')
        for name in ('pre-merge-commit','pre-applypatch'):
            result=subprocess.run([str(self.repo/'.githooks'/name)],cwd=self.repo,capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('concrete service IP address',result.stdout+result.stderr)


if __name__=='__main__':
    unittest.main()
