import base64
import http.client
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch
from http.server import ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import optional_backends as adapters
from server import PaddleOcrBackend, handler_for


def modules(name, **values):
    module = ModuleType(name)
    module.__dict__.update(values)
    return module


class OptionalOcrTests(unittest.TestCase):
    def test_catalog_is_opt_in_and_cli_loads_no_inference_dependencies(self):
        catalog = adapters.model_catalog()
        self.assertEqual(len(catalog), 7)
        self.assertEqual(len({e['id'] for e in catalog}), len(catalog))
        self.assertEqual(adapters.model_spec('doctr-formulanet')['profile']['tasks'], ['formula'])
        self.assertEqual(adapters.model_spec('pp-ocrv6-small')['profile']['tasks'], ['text'])
        process = subprocess.run([sys.executable, '-S', str(Path(adapters.__file__).with_name('server.py')), '--list-models'],
                                 capture_output=True, text=True, timeout=10)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(json.loads(process.stdout), catalog)
        self.assertNotIn('url', process.stdout)

    def test_paddle_v6_selects_both_models_and_keeps_legacy_defaults(self):
        calls = []
        pipeline = SimpleNamespace(predict=lambda _: [])
        def create(**kwargs):
            calls.append(kwargs)
            return pipeline
        with patch.dict(sys.modules, {'paddleocr': modules('paddleocr', PaddleOCR=create)}):
            for variant in ('small', 'medium'):
                backend = PaddleOcrBackend('cpu', 'auto', variant)
                self.assertEqual(backend.model, 'PP-OCRv6_' + variant)
                self.assertEqual(calls[-1]['text_detection_model_name'], backend.model + '_det')
                self.assertEqual(calls[-1]['text_recognition_model_name'], backend.model + '_rec')
                self.assertNotIn('lang', calls[-1])
                self.assertEqual(backend.profile['geometry'], 'line')
            PaddleOcrBackend('gpu', 'en')
            self.assertEqual(calls[-1]['lang'], 'en')
            self.assertNotIn('text_detection_model_name', calls[-1])

    def test_legacy_standalone_worker_help_needs_no_optional_modules(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'server.py'
            path.write_bytes(Path(adapters.__file__).with_name('server.py').read_bytes())
            result = subprocess.run([sys.executable, '-S', str(path), '--help'], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('--engine', result.stdout)

    def response(self, text, finish='stop'):
        return io.BytesIO(json.dumps({'choices': [{'finish_reason': finish, 'message': {'content': text}}]}).encode())

    def test_glm_uses_task_prompts_and_retains_unlocated_formula_evidence(self):
        backend = adapters.ChatBackend('glm-ocr', 'http://127.0.0.1:8082')
        for task, prompt in [('ocr', 'Text Recognition:'), ('formula', 'Formula Recognition:'), ('table', 'Table Recognition:')]:
            opener = SimpleNamespace(open=lambda *_args, **_kwargs: self.response('\\[x^2\\]'))
            with patch('optional_backends.urllib.request.build_opener', return_value=opener), patch.object(opener, 'open', wraps=opener.open) as send:
                result = backend.recognize_evidence(b'png', 200, 100, 'auto', task, 8)
            payload = json.loads(send.call_args.args[0].data)
            self.assertEqual(payload['messages'][0]['content'][1]['text'], prompt)
            self.assertEqual(payload['model'], 'zai-org/GLM-OCR')
            self.assertEqual(result['provenance']['page_number'], 8)
            if task == 'formula':
                self.assertEqual(result['formulas'][0]['latex'], 'x^2')
                self.assertNotIn('box', result['formulas'][0])
                self.assertEqual(result['formulas'][0]['confidence'], -1)
            elif task == 'ocr':
                self.assertTrue(result['markdown'])
                self.assertEqual(result['text'], [])

    def test_inline_formula_and_math_cell_ownership(self):
        result = {kind: [] for kind in ('text', 'formulas', 'tables', 'figures')}
        adapters.add_block(result, 'inline_formula', r'\(x^2\)')
        self.assertFalse(result['formulas'][0]['display'])
        self.assertEqual(result['formulas'][0]['latex'], 'x^2')
        adapters.add_block(result, 'table', '<tr><th colspan="2">Rate</th></tr>'
                           '<tr><td><math>x^2</math></td><td>м</td></tr>', html_content=True)
        table = result['tables'][0]
        self.assertEqual(table['text'], 'Rate\n' + r'\(x^2\)' + '\tм')
        self.assertEqual(table['structure']['table_layout'][0][0],
                         {'header': True, 'colspan': 2, 'rowspan': 1})

    def test_chat_rejects_invalid_truncated_and_oversized_outputs(self):
        backend = adapters.ChatBackend('glm-ocr', 'http://127.0.0.1:8082/v1')
        self.assertEqual(backend.endpoint, 'http://127.0.0.1:8082/v1/chat/completions')
        for response in (self.response('partial', 'length'), self.response(None), self.response('  '), io.BytesIO(b'{}'), io.BytesIO(b'X' * (adapters.MAX_RESPONSE + 1))):
            with patch('optional_backends.urllib.request.build_opener', return_value=SimpleNamespace(open=lambda *_a, **_k: response)):
                with self.assertRaises(ValueError): backend.recognize_evidence(b'png', 20, 10, 'auto', 'ocr', 1)
        with self.assertRaises(ValueError): backend.recognize_evidence(b'png', 20, 10, 'rus', 'ocr', 1)
        for url in ('file:///tmp/model', 'http://user:password@host.example', 'http://host.example/?token=dummy'):
            with self.assertRaises(ValueError): adapters.ChatBackend('glm-ocr', url)

    def test_lighton_grounding_preserves_block_types_and_scaled_coordinates(self):
        backend = adapters.ChatBackend('lightonocr', 'http://127.0.0.1:8082')
        raw = '![text](0,0,500,200)\nProse\n![formula](500,100,1000,600)\nx^2\n![table](0,600,500,900)\n<table><tr><td>A</td></tr></table>\n![image](500,600,1000,1000)\nA diagram.'
        opener = SimpleNamespace(open=lambda *_a, **_k: self.response(raw))
        with patch('optional_backends.urllib.request.build_opener', return_value=opener), patch.object(opener, 'open', wraps=opener.open) as send:
            result = backend.recognize_evidence(b'png', 200, 100, 'auto', 'layout', 1)
        payload = json.loads(send.call_args.args[0].data)
        self.assertEqual(payload['messages'][0]['content'][1]['text'], 'grounding')
        self.assertEqual(payload['chat_template_kwargs'], {'enable_thinking': False})
        self.assertEqual(result['text'][0]['box'], [0, 0, 100, 20])
        self.assertEqual(result['formulas'][0]['box'], [100, 10, 100, 50])
        self.assertIn('<table>', result['tables'][0]['html'])
        self.assertEqual(result['figures'][0]['caption'], 'A diagram.')
        self.assertNotIn('![text]', result['markdown'])
        with self.assertRaises(ValueError): adapters.lighton_result(backend, 'No boxes', 200, 100, 'layout', 1)
        with self.assertRaises(ValueError): adapters.lighton_result(backend, '![text](0,0,1200,200)\ninvalid', 200, 100, 'ocr', 1)

    def test_lighton_formula_uses_image_only_prompt(self):
        backend = adapters.ChatBackend('lightonocr', 'http://127.0.0.1:8082')
        opener = SimpleNamespace(open=lambda *_a, **_k: self.response('$$x^2$$'))
        with patch('optional_backends.urllib.request.build_opener', return_value=opener), patch.object(opener, 'open', wraps=opener.open) as send:
            result = backend.recognize_evidence(b'png', 200, 100, 'auto', 'formula', 1)
        self.assertEqual(len(json.loads(send.call_args.args[0].data)['messages'][0]['content']), 1)
        self.assertEqual(result['formulas'][0]['latex'], 'x^2')
        self.assertNotIn('box', result['formulas'][0])

    def test_doctr_formula_predictor_is_distinct_from_paddle_formulanet(self):
        calls = []
        class Predictor:
            def to(self, device):
                calls.append(device)
                return self
            def __call__(self, images):
                calls.append(images)
                return [('x^2', 0.75)]
        def predictor(*args, **kwargs):
            calls.append((args, kwargs))
            return Predictor()
        with patch.dict(sys.modules, {'doctr': modules('doctr'), 'doctr.models': modules('doctr.models', formula_predictor=predictor),
                                      'numpy': modules('numpy', asarray=lambda image: image)}), patch.object(adapters, 'page_image', return_value='formula pixels'):
            backend = adapters.FormulaNetBackend('gpu')
            result = backend.recognize_evidence(b'png', 200, 100, 'auto', 'formula', 3)
        self.assertEqual(calls[0], (('formulanet',), {'pretrained': True}))
        self.assertEqual(calls[1], 'cuda')
        self.assertEqual(result['engine_id'], 'doctr')
        self.assertEqual(result['formulas'][0]['confidence'], 75)
        self.assertNotIn('box', result['formulas'][0])
        self.assertEqual(backend.profile['tasks'], ['formula'])

    def test_mineru_sdk_keeps_real_coordinates_and_structured_blocks(self):
        blocks = [dict(type='text', bbox=[0, 0, .5, .2], content='Prose'),
                  dict(type='equation', bbox=[.5, 0, 1, .4], content='x^2'),
                  dict(type='table', bbox=[0, .5, 1, 1], content='<table><tr><td>A</td></tr></table>')]
        calls = []
        def client(**kwargs):
            calls.append(kwargs)
            return SimpleNamespace(two_step_extract=lambda _image: blocks)
        with patch.dict(sys.modules, {'mineru_vl_utils': modules('mineru_vl_utils', MinerUClient=client),
                                      'mineru_vl_utils.post_process': modules('mineru_vl_utils.post_process', json2md=lambda _blocks: 'Markdown')}), patch.object(adapters, 'page_image', return_value=object()):
            backend = adapters.MinerUBackend('http://127.0.0.1:8082/v1/chat/completions')
            result = backend.recognize_evidence(b'png', 200, 100, 'auto', 'ocr', 1)
        self.assertEqual(calls[0]['server_url'], 'http://127.0.0.1:8082/v1')
        self.assertEqual(calls[0]['backend'], 'http-client')
        self.assertEqual(result['formulas'][0]['box'], [100, 0, 100, 40])
        self.assertEqual(result['tables'][0]['box'], [0, 50, 200, 50])
        self.assertEqual(result['structured_output']['blocks'], blocks)

    def test_chandra_does_not_accept_sdk_fabricated_input_extent_boxes(self):
        calls = []
        raw = '<div data-label="Text">Unlocated prose</div><div data-label="Equation" data-bbox="[500, 0, 1000, 400]"><math>x^2</math></div>'
        page = SimpleNamespace(error=False, raw=raw, markdown='Prose\n$$x^2$$', chunks=[dict(bbox=[0, 0, 200, 100])])
        def generate(batch, **kwargs):
            calls.append((batch, kwargs))
            return [page]
        with patch.dict(sys.modules, {'chandra': modules('chandra', InferenceManager=lambda **_kw: SimpleNamespace(generate=generate),
                                                       BatchInputItem=lambda **kwargs: kwargs)}), patch.object(adapters, 'page_image', return_value=object()):
            backend = adapters.ChandraBackend('http://127.0.0.1:8082')
            result = backend.recognize_evidence(b'png', 200, 100, 'auto', 'ocr', 1)
            page.raw = '<div data-label="Text">Truncated'
            with self.assertRaises(ValueError): backend.recognize_evidence(b'png', 200, 100, 'auto', 'ocr', 1)
            page.raw = raw
        self.assertEqual(calls[0][1]['vllm_model_name'], 'datalab-to/chandra-ocr-2')
        self.assertEqual(calls[0][0][0]['prompt_type'], 'ocr_layout')
        self.assertNotIn('box', result['text'][0])
        self.assertEqual(result['formulas'][0]['box'], [100, 0, 100, 40])
        self.assertEqual(result['structured_output']['html'], raw)

    def test_optional_worker_http_advertises_and_returns_native_evidence(self):
        backend = adapters.ChatBackend('glm-ocr', 'http://127.0.0.1:8082')
        server = ThreadingHTTPServer(('127.0.0.1', 0), handler_for(backend))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        png = b'\x89PNG\r\n\x1a\n' + struct.pack('>I', 13) + b'IHDR' + struct.pack('>II', 20, 10)
        try:
            connection = http.client.HTTPConnection('127.0.0.1', server.server_port)
            connection.request('GET', '/v1/health')
            health = json.loads(connection.getresponse().read())
            self.assertEqual(health['profile']['model'], 'zai-org/GLM-OCR')
            self.assertEqual(health['profile']['tasks'], ['text', 'formula', 'table'])
            payload = dict(protocol='yagf-ocr/1', model=backend.model, image_png=base64.b64encode(png).decode(), page_number=2, task='formula')
            with patch.object(backend, 'recognize_evidence', return_value=adapters.evidence(backend, 20, 10, 'formula', 2)) as recognize:
                connection.request('POST', '/v1/recognize', json.dumps(payload), {'Content-Type': 'application/json'})
                response = connection.getresponse(); self.assertEqual(response.status, 200)
                result = json.loads(response.read())
                self.assertEqual(result['recognition']['provenance']['page_number'], 2)
                self.assertNotIn('lines', result)
                payload['task'] = 'layout'
                connection.request('POST', '/v1/recognize', json.dumps(payload), {'Content-Type': 'application/json'})
                response = connection.getresponse(); self.assertEqual(response.status, 400); response.read()
                self.assertEqual(recognize.call_count, 1)
            connection.close()
        finally:
            server.shutdown(); server.server_close(); thread.join()


if __name__ == '__main__':
    unittest.main()
