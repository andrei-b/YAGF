"""Exercise artifact naming/staging without downloading tools or compiling Qt."""
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / 'packaging/build-appimage.sh'
MOCK = r'''
import json
import os
from pathlib import Path
import subprocess
import sys

name = Path(sys.argv[0]).name
with open(os.environ['YAGF_TEST_CALLS'], 'a') as out:
    out.write(json.dumps([name, sys.argv[1:]]) + '\n')
if name == 'cmake':
    if '-B' in sys.argv:
        build = Path(sys.argv[sys.argv.index('-B')+1]); build.mkdir(parents=True,exist_ok=True)
        (build/'CMakeCache.txt').write_text('CMAKE_PROJECT_VERSION:STATIC=9.8.7\n')
    elif '--install' in sys.argv:
        root = Path(os.environ['DESTDIR'])/'usr'
        (root/'share/doc/yagf2').mkdir(parents=True)
        (root/'bin').mkdir(parents=True)
        for binary in ('yagf2', 'yagf2-benchmark'):
            (root/'bin'/binary).write_text('placeholder')
elif name == 'linuxdeploy' and not os.environ.get('YAGF_TEST_NO_OUTPUT'):
    Path(os.environ['LDAI_OUTPUT']).write_bytes(b'AppImage fixture')
elif name == 'cpack' and not os.environ.get('YAGF_TEST_NO_OUTPUT'):
    output = Path(sys.argv[sys.argv.index('-B')+1])
    fixture = output/'fixture'; (fixture/'DEBIAN').mkdir(parents=True)
    arch = subprocess.check_output(['dpkg','--print-architecture'],text=True).strip()
    (fixture/'DEBIAN/control').write_text('Package: yagf2\nVersion: 9.8.7-1\nArchitecture: '+arch+'\nMaintainer: Test builder\nDescription: package fixture\n')
    (fixture/'usr/bin').mkdir(parents=True)
    (fixture/'usr/bin/yagf2').write_text('package fixture')
    subprocess.run(['dpkg-deb','--build','--root-owner-group',str(fixture),str(output/('yagf2_9.8.7-1_'+arch+'.deb'))],check=True,stdout=subprocess.DEVNULL)
'''


class PackagingTests(unittest.TestCase):
    script = SCRIPT
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='yagf-packaging-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        tools = self.root/'fake-tools'; tools.mkdir()
        for name in ('cmake', 'linuxdeploy', 'qt-plugin', 'cpack'):
            path = tools/name
            path.write_text(f'#!{sys.executable}\n'+MOCK)
            path.chmod(0o755)
        self.calls = self.root/'calls.jsonl'
        self.build = self.root/'build'
        self.dist = self.root/'dist'
        self.env = dict(os.environ, PATH=str(tools)+os.pathsep+os.environ['PATH'],
                        LINUXDEPLOY=str(tools/'linuxdeploy'), LINUXDEPLOY_QT_PLUGIN=str(tools/'qt-plugin'),
                        YAGF_APPIMAGE_BUILD_DIR=str(self.build), YAGF_DIST_DIR=str(self.dist),
                        YAGF_TEST_CALLS=str(self.calls), CMAKE_BUILD_PARALLEL_LEVEL='2')
        self.artifact = self.dist/f'YAGF-9.8.7-{platform.machine()}.AppImage'

    def run_builder(self, *args):
        return subprocess.run(['bash', str(self.script), *args], env=self.env,
                              capture_output=True, text=True, timeout=20)

    def test_configured_version_checksum_and_both_executables(self):
        self.dist.mkdir()
        unrelated = self.dist/'older.AppImage'; unrelated.write_bytes(b'old release')
        self.build.mkdir()
        leftover = self.build/'unrelated.AppImage'; leftover.write_bytes(b'not this build')
        option = '-DOpenCV_DIR=/a path with spaces/opencv4'
        result = self.run_builder(option)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual(self.artifact.read_bytes(), b'AppImage fixture')
        digest, name = Path(str(self.artifact)+'.sha256').read_text().split()
        self.assertEqual(digest, hashlib.sha256(self.artifact.read_bytes()).hexdigest())
        self.assertEqual(name, self.artifact.name)
        self.assertEqual(unrelated.read_bytes(), b'old release')
        self.assertEqual(leftover.read_bytes(), b'not this build')
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()]
        configure = next(args for tool, args in calls if tool == 'cmake' and '-B' in args)
        self.assertIn(option, configure)
        build = next(args for tool, args in calls if tool == 'cmake' and '--build' in args)
        self.assertEqual(build[-2:], ['--parallel', '2'])
        deploy = next(args for tool, args in calls if tool == 'linuxdeploy')
        self.assertIn(str(self.build/'YAGF.AppDir/usr/bin/yagf2'), deploy)
        self.assertIn(str(self.build/'YAGF.AppDir/usr/bin/yagf2-benchmark'), deploy)
        info = (self.build/'YAGF.AppDir/usr/share/doc/yagf2/build-info.txt').read_text()
        self.assertIn('YAGF 9.8.7', info)

    def test_missing_output_does_not_publish_an_unrelated_image(self):
        self.build.mkdir()
        leftover = self.build/'stale.AppImage'; leftover.write_bytes(b'stale')
        self.env['YAGF_TEST_NO_OUTPUT'] = '1'
        result = self.run_builder()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('linuxdeploy did not create', result.stderr)
        self.assertEqual(leftover.read_bytes(), b'stale')
        self.assertFalse(self.artifact.exists())

    def test_help_does_not_create_build_directories_or_fetch_tools(self):
        result = self.run_builder('--help')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('host dependencies', result.stdout)
        self.assertFalse(self.build.exists())
        self.assertFalse(self.dist.exists())
        self.assertFalse(self.calls.exists())


class DebPackagingTests(unittest.TestCase):
    script = SCRIPT.with_name('build-deb.sh')
    run_builder = PackagingTests.run_builder

    def setUp(self):
        PackagingTests.setUp(self)
        self.env['YAGF_DEB_BUILD_DIR'] = str(self.build)
        arch = subprocess.check_output(['dpkg','--print-architecture'],text=True).strip()
        self.artifact = self.dist/f'yagf2_9.8.7-1_{arch}.deb'

    def test_native_deb_and_checksum_are_published(self):
        result = self.run_builder('-DOpenCV_DIR=/opt/example-opencv')
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        version = subprocess.check_output(['dpkg-deb','--field',str(self.artifact),'Version'],text=True).strip()
        self.assertEqual(version, '9.8.7-1')
        digest, name = Path(str(self.artifact)+'.sha256').read_text().split()
        self.assertEqual(digest, hashlib.sha256(self.artifact.read_bytes()).hexdigest())
        self.assertEqual(name, self.artifact.name)
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()]
        configure = next(args for tool,args in calls if tool=='cmake' and '-B' in args)
        self.assertIn('-DCMAKE_SKIP_INSTALL_RPATH=ON',configure)
        self.assertIn('-DOpenCV_DIR=/opt/example-opencv',configure)

    def test_failed_cpack_does_not_replace_an_existing_package(self):
        self.dist.mkdir(); self.artifact.write_bytes(b'previous package')
        self.env['YAGF_TEST_NO_OUTPUT'] = '1'
        result = self.run_builder()
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(self.artifact.read_bytes(),b'previous package')

    def test_prerequisite_print_mode_does_not_install_packages(self):
        result=subprocess.run(['bash',str(SCRIPT.with_name('install-build-deps-ubuntu.sh')),'--print'],
                              capture_output=True,text=True,check=True)
        self.assertIn('libopencv-dev',result.stdout)
        self.assertIn('dpkg-dev',result.stdout)


if __name__ == '__main__':
    unittest.main()
