import base64
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib

BINARY = sys.argv[1]
sys.argv = sys.argv[:1]


def png(color):
    def chunk(kind, data):
        return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data)&0xffffffff)
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB',20,10,8,2,0,0,0))+chunk(b'IDAT',zlib.compress((b'\0'+bytes([color])*60)*10))+chunk(b'IEND',b'')


class CliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.env = dict(os.environ, PATH=str(self.root)+os.pathsep+os.environ['PATH'], QT_QPA_PLATFORM='offscreen', CLI_SECRET='not-in-the-report')
        images = [base64.b64encode(png(n)).decode() for n in (255,220)]
        self.helper('pdftoppm', f"import sys,base64\nn=int(sys.argv[sys.argv.index('-f')+1]);open(sys.argv[-1]+'.png','wb').write(base64.b64decode({images!r}[n-1]))\n")
        self.helper('tesseract', '''import os,sys,time
if '--version' in sys.argv:
 print('tesseract fixture');sys.exit(0)
if os.environ.get('CLI_SLOW'):
 open(os.environ['CLI_PID'],'w').write(str(os.getpid()));time.sleep(30)
print('level\\tpage_num\\tblock_num\\tpar_num\\tline_num\\tword_num\\tleft\\ttop\\twidth\\theight\\tconf\\ttext')
print('5\\t1\\t1\\t1\\t1\\t1\\t1\\t1\\t12\\t5\\t99\\talpha')
''')
        source = self.root/'source.pdf'
        source.write_bytes(b'PDF renderer fixture')
        pages = []
        for number in (1,2):
            pages.append(dict(number=number,width=20,height=10,engine='fixture',language='eng',words=[dict(page=number,block=1,paragraph=1,line=1,box=dict(x=1,y=1,width=12,height=5),original_text='alpha',text='alpha',confidence=99,reviewed=True,id=f'page-{number}-word')]))
        self.project = self.root/'source.yagf.json'
        self.project.write_text(json.dumps(dict(project_version=2,document=dict(schema_version=2,source=str(source),engine='fixture',language='eng',total_pages=2,pages=pages))))
        self.config = self.root/'pipelines.json'
        self.config.write_text(json.dumps(dict(pipelines=[dict(id='local',provider='tesseract',language='eng',dpi=150,token_env='CLI_SECRET')])))
        self.output = self.root/'report.json'
        self.command = [BINARY,str(self.project),'--pipelines',str(self.config),'--calibration','1','--held-out','2','--repetitions','1','--warmups','0','--output',str(self.output)]

    def tearDown(self):
        self.temp.cleanup()

    def helper(self,name,body):
        path = self.root/name
        path.write_text('#!'+sys.executable+'\n'+body)
        path.chmod(0o700)

    def test_complete_report_and_rejected_split(self):
        result = subprocess.run(self.command,env=self.env,capture_output=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stderr.decode())
        report = json.loads(self.output.read_text())
        self.assertEqual(report['advisory_recommendation'],'local')
        self.assertEqual(report['summary'][0]['held_out']['cer'],0)
        self.assertNotIn('not-in-the-report',self.output.read_text())
        self.output.unlink()
        command = self.command.copy();command[command.index('--held-out')+1] = '1'
        result = subprocess.run(command,env=self.env,capture_output=True,timeout=20)
        self.assertEqual(result.returncode,2)
        self.assertFalse(self.output.exists())

    def test_sigint_stops_memory_wrapper_and_child(self):
        pidfile = self.root/'child.pid'
        env = dict(self.env,CLI_SLOW='1',CLI_PID=str(pidfile))
        process = subprocess.Popen(self.command,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic()+10
            while not pidfile.exists() and time.monotonic()<deadline:
                time.sleep(.025)
            self.assertTrue(pidfile.exists())
            process.send_signal(signal.SIGINT)
            _,errors = process.communicate(timeout=6)
            self.assertEqual(process.returncode,130,errors.decode())
            self.assertTrue(json.loads(self.output.read_text())['cancelled'])
            status = Path('/proc')/pidfile.read_text()/'status'
            if status.exists():
                self.assertIn('State:\tZ',status.read_text())
        finally:
            if process.poll() is None:
                process.kill();process.wait()


if __name__ == '__main__':
    unittest.main()
