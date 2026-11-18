import base64
import http.client
import json
import struct
import sys
import threading
import unittest
from pathlib import Path
from http.server import ThreadingHTTPServer
from types import SimpleNamespace
from types import ModuleType
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from server import (PROTOCOL, DocTrBackend, RapidOcrBackend, SuryaBackend, KrakenBackend,
                    handler_for, normalize_paddle_result,
                    normalize_doctr_result, normalize_rapid_result,
                    normalize_surya_result, normalize_kraken_records)


class FakeBackend:
    engine_id = "paddleocr"
    result_key = "lines"
    language = "en"

    def recognize(self, png, width, height, language):
        assert (width, height, language) == (20, 10, "en")
        return [{"text": "Hello world", "confidence": 0.9, "box": [1, 2, 12, 5]}]


class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), handler_for(FakeBackend(), "secret"))
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def request(self, method, path, body=None, token="secret"):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port)
        headers = {"Content-Type": "application/json", "Authorization": "Bearer " + token}
        connection.request(method, path, json.dumps(body).encode() if body is not None else None, headers)
        response = connection.getresponse()
        result = response.status, json.loads(response.read())
        connection.close()
        return result

    def payload(self):
        # Only the header is needed by the fake backend.
        png = b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR" + struct.pack(">II", 20, 10)
        return {"protocol": PROTOCOL, "image_png": base64.b64encode(png).decode(),
                "language": "en", "page_number": 1}

    def test_health_and_recognition(self):
        code, health = self.request("GET", "/v1/health")
        self.assertEqual((code, health["protocol"], health["status"]), (200, PROTOCOL, "ready"))
        code, result = self.request("POST", "/v1/recognize", self.payload())
        self.assertEqual(code, 200)
        self.assertEqual(result["lines"][0]["box"], [1, 2, 12, 5])

    def test_auth_and_bad_input(self):
        self.assertEqual(self.request("POST", "/v1/recognize", self.payload(), "wrong")[0], 401)
        payload = self.payload()
        payload["protocol"] = "old"
        self.assertEqual(self.request("POST", "/v1/recognize", payload)[0], 400)
        payload = self.payload()
        payload["image_png"] = "broken"
        self.assertEqual(self.request("POST", "/v1/recognize", payload)[0], 400)
        self.assertEqual(self.request("POST", "/v1/recognize", ["bad"])[0], 400)

    def test_paddle_result_conversion(self):
        class Result:
            json = {"res": {"rec_texts": ["Hello world"], "rec_scores": [0.85],
                            "rec_boxes": [[1, 2, 13, 7]]}}
        self.assertEqual(normalize_paddle_result([Result()], 20, 10),
                         [{"text": "Hello world", "confidence": 0.85, "box": [1, 2, 12, 5]}])
        with self.assertRaises(ValueError):
            normalize_paddle_result([Result()], 10, 10)

    def test_doctr_word_geometry_and_reading_order(self):
        def word(value, geometry, confidence=0.9):
            return SimpleNamespace(value=value, geometry=geometry, confidence=confidence)
        result = SimpleNamespace(pages=[SimpleNamespace(blocks=[
            SimpleNamespace(lines=[SimpleNamespace(words=[
                word("Hello", ((0.1, 0.2), (0.4, 0.7))),
                word("world", ((0.45, 0.2), (0.9, 0.7)))])]),
            SimpleNamespace(lines=[SimpleNamespace(words=[
                word("Again", ((0.1, 0.8), (0.5, 1.0)))])])])])
        words = normalize_doctr_result(result, 20, 10)
        self.assertEqual([item["text"] for item in words], ["Hello", "world", "Again"])
        self.assertEqual([item["block"] for item in words], [1, 1, 2])
        self.assertEqual(words[0]["box"], [2, 2, 6, 5])
        self.assertEqual(words[2]["box"], [2, 8, 8, 2])
        result.pages[0].blocks[0].lines[0].words[0].geometry = ((0.9, 0.2), (1.1, 0.7))
        with self.assertRaises(ValueError):
            normalize_doctr_result(result, 20, 10)

    def test_doctr_http_response(self):
        class FakeDocTrBackend:
            engine_id = "doctr"
            result_key = "words"
            language = "auto"

            def recognize(self, png, width, height, language):
                assert language == "auto"
                return [{"text": "Hello", "confidence": 0.95, "box": [1, 2, 8, 5],
                         "block": 1, "line": 1}]

        server = ThreadingHTTPServer(("127.0.0.1", 0), handler_for(FakeDocTrBackend()))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            connection = http.client.HTTPConnection("127.0.0.1", server.server_port)
            connection.request("GET", "/v1/health")
            response = connection.getresponse()
            self.assertEqual(json.loads(response.read())["engine_id"], "doctr")
            payload = self.payload()
            payload["language"] = "auto"
            connection.request("POST", "/v1/recognize", json.dumps(payload),
                               {"Content-Type": "application/json"})
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(json.loads(response.read())["words"][0]["block"], 1)
            connection.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_doctr_backend_contract(self):
        result = SimpleNamespace(pages=[SimpleNamespace(blocks=[SimpleNamespace(lines=[
            SimpleNamespace(words=[SimpleNamespace(value="Hello", confidence=0.9,
                geometry=((0.1, 0.2), (0.5, 0.7)))])])])])

        class Predictor:
            def to(self, device):
                self.device = device
                return self

            def __call__(self, pages):
                assert pages == "image page"
                return result

        predictor = Predictor()
        doctr = ModuleType("doctr")
        models = ModuleType("doctr.models")
        io = ModuleType("doctr.io")
        models.ocr_predictor = lambda **kwargs: predictor
        io.DocumentFile = SimpleNamespace(from_images=lambda path: "image page")
        with patch.dict(sys.modules, {"doctr": doctr, "doctr.models": models, "doctr.io": io}):
            backend = DocTrBackend("cpu")
            self.assertEqual(predictor.device, "cpu")
            self.assertEqual(backend.recognize(b"image", 20, 10, "auto")[0]["box"],
                             [2, 2, 8, 5])
            with self.assertRaises(ValueError):
                backend.recognize(b"image", 20, 10, "en")

    def test_rapidocr_line_polygons(self):
        result = SimpleNamespace(boxes=[[(1, 2), (13, 2), (13, 7), (1, 7)]],
                                 txts=["Hello world"], scores=[0.91])
        self.assertEqual(normalize_rapid_result(result, 20, 10),
                         [{"text": "Hello world", "confidence": 0.91, "box": [1, 2, 12, 5]}])
        result.boxes = [[(-20, 2), (-2, 2), (-2, 7), (-20, 7)]]
        with self.assertRaises(ValueError):
            normalize_rapid_result(result, 20, 10)

    def test_surya_blocks_keep_reading_order_and_types(self):
        def block(order, label, html, polygon):
            return SimpleNamespace(reading_order=order, label=label, html=html,
                                   polygon=polygon, confidence=0.8,
                                   skipped=False, error=False)
        result = [SimpleNamespace(blocks=[
            block(1, "Table", "<table><tr><td>A</td><td>B</td></tr></table>",
                  [(10, 4), (19, 4), (19, 9), (10, 9)]),
            block(0, "SectionHeader", "<h1>Chapter &amp; notes</h1>",
                  [(1, 1), (9, 1), (9, 3), (1, 3)])])]
        items = normalize_surya_result(result, 20, 10)
        self.assertEqual([(item["type"], item["block"]) for item in items],
                         [("sectionheader", 1), ("table", 2)])
        self.assertEqual(items[0]["text"], "Chapter & notes")
        self.assertIn("A", items[1]["text"])
        self.assertIn("B", items[1]["text"])

    def test_scientific_page(self):
        def block(order, label, html, skipped=False):
            return SimpleNamespace(reading_order=order, label=label, html=html,
                                   polygon=[(1, 1), (9, 1), (9, 8), (1, 8)],
                                   confidence=None, skipped=skipped, error=False)
        page = SimpleNamespace(blocks=[
            block(4, "Caption", "<p>Figure 1 &amp; details</p>"),
            block(2, "Table", '<table><tr><th colspan="2">A B</th></tr>'
                  '<tr><td></td><td><math>y_i</math></td></tr></table>'),
            block(0, "Text", "<p>Let <math>x_i</math> vary.</p>"),
            block(3, "Picture", "", skipped=True),
            block(1, "Equation", r"<math>\frac{a}{b}</math>")])
        text, equation, table, picture, caption = normalize_surya_result([page], 10, 10)
        self.assertEqual(text["text"], r"Let \(x_i\) vary.")
        self.assertEqual(equation["text"], r"\[\frac{a}{b}\]")
        self.assertEqual(table["text"], "A B\n\t" + r"\(y_i\)")
        self.assertEqual(table["table_layout"][0][0], {"header": True, "rowspan": 1, "colspan": 2})
        self.assertEqual(picture["type"], "picture")
        self.assertIsNone(picture["confidence"])
        self.assertEqual(caption["text"], "Figure 1 & details")

    def test_kraken_words_use_character_cuts(self):
        def cut(left, right):
            return [(left, 2), (right, 2), (right, 7), (left, 7)]
        record = SimpleNamespace(prediction="Hi all",
                                 cuts=[cut(i * 2, i * 2 + 2) for i in range(6)],
                                 confidences=[0.9] * 6,
                                 line=SimpleNamespace(bbox=[0, 2, 12, 7]))
        words = normalize_kraken_records([record], 20, 10)
        self.assertEqual([item["text"] for item in words], ["Hi", "all"])
        self.assertEqual([item["box"] for item in words], [[0, 2, 4, 5], [6, 2, 6, 5]])
        self.assertEqual([item["line"] for item in words], [1, 1])
        record.cuts.pop()
        fallback = normalize_kraken_records([record], 20, 10)
        self.assertEqual([(item["text"], item["box"]) for item in fallback],
                         [("Hi all", [0, 2, 12, 5])])

    def test_rapidocr_backend_contract(self):
        rapid = ModuleType("rapidocr")
        rapid.RapidOCR = lambda: lambda path: SimpleNamespace(
            boxes=[[(1, 2), (9, 2), (9, 7), (1, 7)]],
            txts=["Hello"], scores=[0.9])
        with patch.dict(sys.modules, {"rapidocr": rapid}):
            backend = RapidOcrBackend()
            self.assertEqual(backend.recognize(b"image", 20, 10, "auto")[0]["text"], "Hello")
            with self.assertRaises(ValueError):
                backend.recognize(b"image", 20, 10, "en")

    def test_surya_backend_contract(self):
        block = SimpleNamespace(reading_order=0, label="Text", html="<p>Hello</p>",
                                polygon=[(1, 2), (9, 2), (9, 7), (1, 7)],
                                confidence=0.9, skipped=False, error=False)
        predictor = lambda images: [SimpleNamespace(blocks=[block])]
        inference = ModuleType("surya.inference")
        recognition = ModuleType("surya.recognition")
        inference.SuryaInferenceManager = lambda: object()
        recognition.RecognitionPredictor = lambda manager: predictor
        pil = ModuleType("PIL")

        class FakeImage:
            def __enter__(self): return self
            def __exit__(self, *_): pass
            def convert(self, mode): return self

        pil.Image = SimpleNamespace(open=lambda path: FakeImage())
        with patch.dict(sys.modules, {"surya.inference": inference,
                                      "surya.recognition": recognition, "PIL": pil}):
            backend = SuryaBackend()
            self.assertEqual(backend.recognize(b"image", 20, 10, "auto")[0]["type"], "text")

    def test_kraken_backend_contract(self):
        record = SimpleNamespace(prediction="Old", line=SimpleNamespace(bbox=[1, 2, 9, 7]),
                                 cuts=[[(i, 2), (i + 1, 2), (i + 1, 7), (i, 7)] for i in range(3)],
                                 confidences=[0.9] * 3)
        segmenter = SimpleNamespace(predict=lambda image, config: "segments")
        recognizer = SimpleNamespace(predict=lambda image, segmentation, config: [record])
        tasks = ModuleType("kraken.tasks")
        configs = ModuleType("kraken.configs")
        tasks.SegmentationTaskModel = SimpleNamespace(load_model=lambda: segmenter)
        tasks.RecognitionTaskModel = SimpleNamespace(load_model=lambda path: recognizer)
        configs.SegmentationInferenceConfig = lambda **kwargs: kwargs
        configs.RecognitionInferenceConfig = lambda **kwargs: kwargs
        pil = ModuleType("PIL")

        class FakeImage:
            def __enter__(self): return self
            def __exit__(self, *_): pass
            def convert(self, mode): return self

        pil.Image = SimpleNamespace(open=lambda path: FakeImage())
        with patch.dict(sys.modules, {"kraken.tasks": tasks, "kraken.configs": configs,
                                      "PIL": pil}), patch("server.os.path.isfile", return_value=True):
            backend = KrakenBackend("model.safetensors", "cpu")
            self.assertEqual(backend.recognize_config["num_line_workers"], 0)
            self.assertEqual(backend.recognize(b"image", 20, 10, "auto")[0]["text"], "Old")


if __name__ == "__main__":
    unittest.main()
