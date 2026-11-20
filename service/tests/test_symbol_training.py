import base64
import importlib.util
import json
from pathlib import Path
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

spec = importlib.util.spec_from_file_location("symbol_training", Path(__file__).resolve().parents[1] / "symbol_training.py")
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


def payload():
    def encoded(raw):
        return base64.b64encode(raw).decode()
    a = bytes([0x0f] * 16 + [0] * 16)
    b = bytes([0x0f] * 15 + [0x1f] + [0] * 16)
    negative = bytes([0] * 16 + [0xf0] * 16)
    return {"protocol": worker.PROTOCOL, "training_hash": "a" * 64,
            "examples": [{"mask": encoded(a), "decision": "positive"},
                         {"mask": encoded(b), "decision": "positive"},
                         {"mask": encoded(negative), "decision": "negative"}]}


class SymbolTrainingTests(unittest.TestCase):
    def test_training_and_review_gate(self):
        request = payload()
        model = worker.train(request)
        self.assertEqual(model["algorithm"], "symbol-prototypes/1")
        self.assertEqual(model["training_hash"], request["training_hash"])
        self.assertEqual(len(model["positive"]), 2)
        self.assertTrue(.04 <= model["radius"] <= .3)
        request["examples"][0]["decision"] = "pending"
        with self.assertRaises(ValueError):
            worker.train(request)

    def test_bad_and_conflicting_examples(self):
        for change in (lambda p: p.update(protocol="unknown"),
                       lambda p: p.update(training_hash="secret"),
                       lambda p: p["examples"][0].update(mask="!"),
                       lambda p: p["examples"][2].update(mask=p["examples"][0]["mask"])):
            request = payload()
            change(request)
            with self.assertRaises((ValueError, TypeError)):
                worker.train(request)

    def test_http_auth_and_protocol(self):
        server = ThreadingHTTPServer(("127.0.0.1", 0), worker.handler("test-token"))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f"http://127.0.0.1:{server.server_port}"
        try:
            with urllib.request.urlopen(base + "/v1/health") as response:
                self.assertEqual(json.load(response)["protocol"], worker.PROTOCOL)
            request = urllib.request.Request(base + "/v1/train", json.dumps(payload()).encode(),
                                             {"Content-Type": "application/json"})
            with self.assertRaises(urllib.error.HTTPError) as error:
                urllib.request.urlopen(request)
            self.assertEqual(error.exception.code, 401)
            request.add_header("Authorization", "Bearer test-token")
            with urllib.request.urlopen(request) as response:
                self.assertEqual(json.load(response)["model"], worker.train(payload()))
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    unittest.main()
