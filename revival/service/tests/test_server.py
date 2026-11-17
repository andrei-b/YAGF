import base64
import http.client
import json
import struct
import sys
import threading
import unittest
from pathlib import Path
from http.server import ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from server import PROTOCOL, handler_for, normalize_result


class FakeBackend:
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
        self.assertEqual(normalize_result([Result()], 20, 10),
                         [{"text": "Hello world", "confidence": 0.85, "box": [1, 2, 12, 5]}])
        with self.assertRaises(ValueError):
            normalize_result([Result()], 10, 10)


if __name__ == "__main__":
    unittest.main()
