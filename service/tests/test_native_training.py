import base64
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import urllib.request
import urllib.error
import zlib
from http.server import ThreadingHTTPServer

SERVICE=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(SERVICE))
import native_training_server as worker


def png(seed):
    def chunk(kind,data): return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',30,12,8,2,0,0,0))+chunk(b'IDAT',zlib.compress((b'\0'+bytes([seed,255-seed,0])*30)*12))+chunk(b'IEND',b'')


def samples():
    return [dict(id='sample-'+str(i),page=1,text='Text '+str(i),decision='approved',partition='validation' if i==2 else 'train',png_base64=base64.b64encode(png(i+5)).decode()) for i in range(3)]


class NativeTrainingTests(unittest.TestCase):
    def test_review_and_holdout_gate(self):
        self.assertEqual(len(worker.validate_samples(samples())),3)
        for change in [lambda s:s[0].update(decision='pending'),lambda s:s[0].update(text='two\nlines'),
                       lambda s:s[2].update(partition='train'),lambda s:s[1].update(png_base64=s[0]['png_base64']),
                       lambda s:s[1].update(id=s[0]['id']),lambda s:s[0].update(page=2),
                       lambda s:s[0].update(png_base64='!')]:
            data=samples(); change(data)
            # Moving the first example to another page makes page 1 cross the
            # training/validation boundary, which is disallowed for multi-page sets.
            with self.assertRaises((ValueError,TypeError)): worker.validate_samples(data)
        self.assertEqual(worker.distance('русский ψ','русский ф'),1)
        self.assertEqual(worker.normalized('е\u0308  ψ'), 'ё ψ')

    def test_native_job_validation_and_model_endpoint(self):
        with tempfile.TemporaryDirectory() as root:
            profile={'id':'tess','engine':'tesseract','label':'Test native model','base_model':str(Path(root)/'base.traineddata'),'language':'rus'}
            Path(profile['base_model']).write_bytes(b'base')
            host=worker.TrainingHost(Path(root)/'jobs',[profile])
            def train(p,directory,*args):
                model=directory/'candidate.traineddata'; model.write_bytes(b'trained'); return model
            def recognize(p,model,image,run,line=False):
                if line: return 'Text 2' if 'candidate' in str(model) else 'Wrong'
                return [{'text':'ψ','confidence':.9,'box':[1,1,10,8],'block':1,'line':1}]
            with patch.object(worker.native_engines,'readiness',return_value=(True,'')),patch.object(worker.native_engines,'train',side_effect=train),patch.object(worker.native_engines,'recognize',side_effect=recognize):
                result=host.submit(dict(profile_id='tess',source_id='a'*64,samples=samples(),iterations=1,scope='shared'))
                for _ in range(100):
                    result=host.state(result['id'])
                    if result['status'] in ('complete','failed'): break
                    time.sleep(.01)
                self.assertEqual(result['status'],'complete',result)
                self.assertEqual(result['candidate']['cer'],0)
                self.assertGreater(result['baseline']['cer'],0)
                self.assertTrue(result['eligible'])
                self.assertEqual(Path(profile['base_model']).read_bytes(),b'base')
                output=host.recognize(result['id'],dict(image_png=base64.b64encode(png(9)).decode()))
                self.assertEqual(output['language'],'rus'); self.assertEqual(output['engine_id'],'native'); self.assertEqual(output['words'][0]['text'],'ψ')
            with self.assertRaises(ValueError): host.directory('../outside')
            with patch.object(worker.native_engines,'readiness',return_value=(True,'')),patch.object(host,'work'):
                # Versions continue from the validated weights, not the stock model.
                version=host.submit(dict(profile_id='tess',source_id='b'*64,samples=samples(),iterations=1,base_model_id=result['id']))
                try:
                    self.assertEqual(version['base_model_id'],result['id'])
                    inherited=json.loads((host.directory(version['id'])/'profile.json').read_text())
                    self.assertEqual(inherited['base_model'],result['model_path'])
                finally: host.lock.release()
                # Training images in an ancestor can never be relabeled holdout.
                data=samples(); data[0]['partition']='validation'; data[2]['partition']='train'
                with self.assertRaisesRegex(ValueError,'unseen validation'):
                    host.submit(dict(profile_id='tess',source_id='b'*64,samples=data,iterations=1,base_model_id=result['id']))
                host.update(result['id'],scope='project')
                with self.assertRaisesRegex(ValueError,'another book'):
                    host.submit(dict(profile_id='tess',source_id='b'*64,samples=samples(),iterations=1,base_model_id=result['id']))
            host.update(result['id'],eligible=False)
            with self.assertRaises(ValueError): host.recognize(result['id'],{})

    def test_cancel_stops_native_subprocess(self):
        with tempfile.TemporaryDirectory() as root:
            profile=dict(id='tess',engine='tesseract',base_model=str(Path(root)/'base.traineddata'))
            Path(profile['base_model']).write_bytes(b'base')
            host=worker.TrainingHost(Path(root)/'jobs',[profile]); entered=threading.Event()
            def train(profile,directory,samples,iterations,run,progress):
                entered.set(); run([sys.executable,'-c','import time; time.sleep(30)'])
                raise AssertionError('Cancelled trainer should never reach export')
            with patch.object(worker.native_engines,'readiness',return_value=(True,'')),patch.object(worker.native_engines,'train',side_effect=train),patch.object(worker.native_engines,'recognize_many',return_value=['Text 2']):
                result=host.submit(dict(profile_id='tess',source_id='a'*64,samples=samples(),iterations=1))
                self.assertTrue(entered.wait(2)); host.cancel[result['id']].set()
                for _ in range(200):
                    result=host.state(result['id'])
                    if result['status']=='cancelled': break
                    time.sleep(.01)
                self.assertEqual(result['status'],'cancelled',result)
                self.assertNotIn('model_path',result)
                self.assertEqual(Path(profile['base_model']).read_bytes(),b'base')

    def test_http_auth_and_capabilities(self):
        with tempfile.TemporaryDirectory() as root:
            host=worker.TrainingHost(root,[dict(id='unconfigured',engine='paddleocr',label='PaddleOCR',base_model='missing')])
            server=ThreadingHTTPServer(('127.0.0.1',0),worker.handler_for(host,'test-token'))
            thread=threading.Thread(target=server.serve_forever,daemon=True); thread.start()
            url='http://127.0.0.1:%d'%server.server_port
            try:
                with self.assertRaises(urllib.error.HTTPError) as error: urllib.request.urlopen(url+'/v1/capabilities')
                self.assertEqual(error.exception.code,401)
                request=urllib.request.Request(url+'/v1/capabilities',headers={'Authorization':'Bearer test-token'})
                with urllib.request.urlopen(request) as response: result=json.load(response)
                self.assertFalse(result['profiles'][0]['ready']); self.assertTrue(result['profiles'][0]['reason'])
                request=urllib.request.Request(url+'/v1/jobs',json.dumps(dict(protocol=worker.PROTOCOL,profile_id='unconfigured',source_id='a'*64,samples=samples())).encode(),headers={'Authorization':'Bearer test-token','Content-Type':'application/json'})
                with self.assertRaises(urllib.error.HTTPError) as error: urllib.request.urlopen(request)
                self.assertEqual(error.exception.code,400)
            finally: server.shutdown(); server.server_close(); thread.join()

    def test_tesseract_word_order_across_paragraphs(self):
        tsv='level\tblock_num\tpar_num\tline_num\tleft\ttop\twidth\theight\tconf\ttext\n'
        tsv+='5\t1\t1\t1\t1\t2\t10\t8\t90\tone\n5\t1\t2\t1\t1\t20\t10\t8\t80\ttwo\n'
        words=worker.native_engines.tesseract_words(tsv)
        self.assertEqual([w['line'] for w in words],[1,2]); self.assertEqual(words[0]['confidence'],.9)

if __name__=='__main__': unittest.main()
