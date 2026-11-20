import base64
import json
from pathlib import Path
import struct
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import urllib.request
import urllib.error
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import native_training_server as worker
from ocr_catalog import LocalOcrCatalog


def png():
    def chunk(kind,data): return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',20,12,8,2,0,0,0))+chunk(b'IDAT',zlib.compress((b'\0'+b'\xff'*60)*12))+chunk(b'IEND',b'')


class Upstream(BaseHTTPRequestHandler):
    calls=0
    def log_message(self,*_): pass
    def send(self,value):
        data=json.dumps(value).encode(); self.send_response(200); self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)
    def do_GET(self): self.send(dict(protocol='yagf-ocr/1',engine_id='doctr',status='ready',language='auto',model='Test docTR'))
    def do_POST(self):
        request=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        type(self).calls+=1
        self.send(dict(protocol='yagf-ocr/1',engine_id='doctr',width=20,height=12,language=request['language'],words=[dict(text='Example',confidence=.9,box=[1,1,15,8],block=1,line=1)]))


class CatalogTests(unittest.TestCase):
    def test_only_compatible_services_and_paddle_vl_models_are_discovered(self):
        catalog=LocalOcrCatalog([8765,8766],8082)
        def response(url):
            if ':8765/' in url: return dict(protocol='yagf-ocr/1',engine_id='doctr',status='ready')
            if ':8766/' in url: return dict(protocol='unrelated',engine_id='surya',status='ready')
            return dict(data=[dict(id='unrelated-chat-model')])
        with patch('ocr_catalog.get_json',side_effect=response):
            self.assertEqual([e['provider'] for e in catalog.discover()],['doctr'])
        catalog.checked=0
        with patch('ocr_catalog.get_json',side_effect=lambda url:dict(data=[dict(id='PaddleOCR-VL-1.6')]) if ':8082/' in url else response(url)):
            entries=catalog.discover()
            self.assertEqual({e['provider'] for e in entries},{'doctr','paddleocr'})
            self.assertEqual(next(e for e in entries if e['kind']=='vl')['port'],8082)
        with self.assertRaises(ValueError): catalog.recognize('9999',{})

    def test_catalog_and_proxy_work_through_one_authenticated_forward(self):
        upstream=ThreadingHTTPServer(('127.0.0.1',0),Upstream)
        thread=threading.Thread(target=upstream.serve_forever,daemon=True); thread.start()
        with tempfile.TemporaryDirectory() as root:
            host=worker.TrainingHost(root,[dict(id='rus',engine='tesseract',label='Russian',language='rus',base_model='missing')])
            host.catalog=LocalOcrCatalog([upstream.server_port],None)
            server=ThreadingHTTPServer(('127.0.0.1',0),worker.handler_for(host,'session-token'))
            runner=threading.Thread(target=server.serve_forever,daemon=True); runner.start()
            base='http://127.0.0.1:%d'%server.server_port
            def call(path,body=None,authenticated=True):
                headers={'Content-Type':'application/json'}
                if authenticated: headers['Authorization']='Bearer session-token'
                request=urllib.request.Request(base+path,json.dumps(body).encode() if body is not None else None,headers=headers)
                with urllib.request.urlopen(request,timeout=3) as response:return json.load(response)
            try:
                with self.assertRaises(urllib.error.HTTPError) as error: call('/v1/health',authenticated=False)
                self.assertEqual(error.exception.code,401)
                health=call('/v1/health'); self.assertTrue(health['requires_token'])
                self.assertEqual(health['protocol'],'yagf-ocr/1')
                self.assertTrue(any(e['path']=='/profiles/rus' and not e['ready'] for e in health['services']))
                port=str(upstream.server_port); path='/services/'+port
                self.assertEqual(call(path+'/v1/health')['engine_id'],'doctr')
                result=call(path+'/v1/recognize',dict(protocol='yagf-ocr/1',language='auto',page_number=1,image_png=base64.b64encode(png()).decode()))
                self.assertEqual(result['words'][0]['text'],'Example'); self.assertGreater(Upstream.calls,0)
                with self.assertRaises(urllib.error.HTTPError) as error: call('/services/9999/v1/recognize',dict(protocol='yagf-ocr/1'))
                self.assertEqual(error.exception.code,400)
                with patch.object(worker.native_engines,'readiness',return_value=(True,'')),patch.object(worker.native_engines,'recognize',return_value=result['words']):
                    profile=call('/profiles/rus/v1/recognize',dict(protocol='yagf-ocr/1',image_png=base64.b64encode(png()).decode()))
                    self.assertEqual(profile['engine_id'],'native'); self.assertEqual(profile['language'],'rus')
            finally:
                server.shutdown(); server.server_close(); runner.join()
                upstream.shutdown(); upstream.server_close(); thread.join()

    def test_book_models_are_not_broadcast_as_reusable_models(self):
        with tempfile.TemporaryDirectory() as root:
            host=worker.TrainingHost(root,[]); host.catalog=LocalOcrCatalog([],None)
            for index,scope in [(1,'project'),(2,'shared')]:
                jid='00000000-0000-0000-0000-%012d'%index
                directory=Path(root)/jid; directory.mkdir()
                (directory/'state.json').write_text(json.dumps(dict(id=jid,status='complete',eligible=True,scope=scope,label='Test',model_url='/models/'+jid)))
            entries=host.health()['services']
            self.assertEqual(len(entries),1); self.assertTrue(entries[0]['path'].endswith('000000000002'))

if __name__=='__main__': unittest.main()
