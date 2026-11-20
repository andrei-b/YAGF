import base64
import importlib.util
import json
from pathlib import Path
import struct
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

spec = importlib.util.spec_from_file_location('layout_server', Path(__file__).resolve().parents[1] / 'layout_server.py')
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class LayoutTests(unittest.TestCase):
    def test_labels_native_coordinates_and_order(self):
        boxes = [[14, .95, -1.2, 20.5, 101, 80.1, 2],
                 [22, .9, 5, 1, 80, 15, 1], [5, .2, 5, 2, 10, 10, 0]]
        result = worker.normalize_boxes(boxes, 100, 200, .5)
        self.assertEqual([a['type'] for a in result], ['text', 'picture'])
        self.assertEqual(result[1]['box'], [0, 20, 100, 61])
        self.assertEqual(result[1]['label'], 'image')
        self.assertEqual(worker.kind('display_formula'), 'math')
        self.assertEqual(worker.kind('inline_formula'), 'mixed')
        self.assertEqual(worker.kind('table'), 'text')
        self.assertEqual(worker.normalize_boxes([[14, .9, 150, 1, 200, 5, 0]],100,200,.5), [])
        for row in ([14, float('nan'), 1, 1, 20, 20, 0], [25, .8, 1, 1, 20, 20, 0],
                    [14.5, .8, 1, 1, 20, 20, 0], [14, 2, 1, 1, 20, 20, 0], [1, 2]):
            with self.assertRaises(ValueError): worker.normalize_boxes([row],100,200,.5)

    def test_http_limits_auth_and_protocol(self):
        class Backend:
            calls = 0
            def detect(self, png, width, height, threshold):
                self.calls += 1
                assert (width, height, threshold) == (100, 200, .5)
                return worker.normalize_boxes([[14,.9,1,2,40,50,0]],width,height,threshold)
        backend = Backend()
        server = ThreadingHTTPServer(('127.0.0.1',0),worker.handler_for(backend,'test'))
        thread = threading.Thread(target=server.serve_forever,daemon=True); thread.start()
        url = 'http://127.0.0.1:%d' % server.server_port
        # Header-only fixture: the injected backend avoids Pillow/model dependencies.
        png = b'\x89PNG\r\n\x1a\n'+struct.pack('>I',13)+b'IHDR'+struct.pack('>II',100,200)+b'\x08\x02\x00\x00\x00'+bytes(4)
        payload = dict(protocol=worker.PROTOCOL,image_png=base64.b64encode(png).decode(),threshold=.5)
        def post(data, token='test'):
            req=urllib.request.Request(url+'/v1/layout',json.dumps(data).encode(),
                                       {'Content-Type':'application/json','Authorization':'Bearer '+token})
            return urllib.request.urlopen(req,timeout=3)
        try:
            with urllib.request.urlopen(url+'/v1/health',timeout=3) as r:
                self.assertEqual(json.load(r)['sha256'],worker.MODEL_SHA256)
            with post(payload) as r:
                result=json.load(r)
                self.assertEqual((result['width'],result['height']),(100,200))
                self.assertEqual(result['areas'][0]['type'],'picture')
            with self.assertRaises(urllib.error.HTTPError) as error: post(payload,'wrong')
            self.assertEqual(error.exception.code,401)
            for key, value in [('protocol','wrong'),('threshold',True),('threshold',1.1),('image_png','!'),('image_png',base64.b64encode(bytes(40)).decode())]:
                bad=dict(payload); bad[key]=value
                with self.assertRaises(urllib.error.HTTPError) as error: post(bad)
                self.assertEqual(error.exception.code,400)
            huge=png[:16]+struct.pack('>II',20000,20000)+png[24:]
            bad=dict(payload,image_png=base64.b64encode(huge).decode())
            with self.assertRaises(urllib.error.HTTPError) as error: post(bad)
            self.assertEqual(error.exception.code,400)
            self.assertEqual(backend.calls,1)
        finally:
            server.shutdown(); server.server_close(); thread.join()


if __name__ == '__main__': unittest.main()
