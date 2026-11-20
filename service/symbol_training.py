"""Small portable symbol classifier trainer; runs on a Jetson or desktop CPU.

Only explicitly reviewed masks are accepted. No document text, passwords, or
model downloads are needed. The resulting model is used locally by YAGF.
"""
import argparse
import base64
import binascii
import hmac
import json
import math
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROTOCOL = "yagf-symbol-training/1"


def mask(value):
    if not isinstance(value, str) or len(value) > 48:
        raise ValueError("Invalid mask")
    data = base64.b64decode(value, validate=True)
    ink = sum(n.bit_count() for n in data)
    if len(data) != 32 or not 4 <= ink < 250:
        raise ValueError("Invalid symbol mask")
    return data


def distance(left, right):
    union = sum((a | b).bit_count() for a, b in zip(left, right))
    return sum((a ^ b).bit_count() for a, b in zip(left, right)) / union if union else 1.0


def train(request):
    if not isinstance(request, dict) or request.get("protocol") != PROTOCOL:
        raise ValueError("Incompatible symbol training protocol")
    training_hash = request.get("training_hash")
    if not isinstance(training_hash, str) or len(training_hash) != 64 or any(c not in "0123456789abcdef" for c in training_hash):
        raise ValueError("Invalid training hash")
    samples = request.get("examples")
    if not isinstance(samples, list) or not 3 <= len(samples) <= 256:
        raise ValueError("Approve at least two matches and one non-match")
    positives, negatives = [], []
    for example in samples:
        if not isinstance(example, dict) or example.get("decision") not in ("positive", "negative"):
            raise ValueError("Only reviewed examples can be trained")
        value = example.get("mask")
        mask(value)
        (positives if example["decision"] == "positive" else negatives).append(value)
    if len(positives) < 2 or not negatives:
        raise ValueError("Approve at least two matches and one non-match")
    positive_masks = [mask(p) for p in positives]
    negative_masks = [mask(n) for n in negatives]
    spread = .04
    for i, positive in enumerate(positive_masks):
        nearest = min(distance(positive, other) for j, other in enumerate(positive_masks) if i != j)
        spread = max(spread, nearest * 1.15)
        if any(distance(positive, negative) < .04 for negative in negative_masks):
            raise ValueError("Matching and non-matching examples conflict; review the crops")
    return {"algorithm": "symbol-prototypes/1", "training_hash": training_hash,
            "positive": positives, "negative": negatives, "radius": min(.3, max(.04, spread))}


def handler(token=""):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def send(self, code, value):
            data = json.dumps(value, allow_nan=False).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path != "/v1/health":
                self.send(404, {"error": "Unknown endpoint"})
            else:
                self.send(200, {"protocol": PROTOCOL, "status": "ready", "algorithm": "symbol-prototypes/1"})

        def do_POST(self):
            if self.path != "/v1/train":
                self.send(404, {"error": "Unknown endpoint"})
                return
            if token and not hmac.compare_digest(self.headers.get("Authorization", ""), "Bearer " + token):
                self.send(401, {"error": "Unauthorized"})
                return
            try:
                length = int(self.headers.get("Content-Length", "-1"))
                if not 0 < length <= 128 * 1024 or self.headers.get_content_type() != "application/json":
                    raise ValueError("Invalid JSON request length or content type")
                request = json.loads(self.rfile.read(length))
                model = train(request)
                self.send(200, {"protocol": PROTOCOL, "model": model})
            except (ValueError, TypeError, KeyError, binascii.Error) as error:
                self.send(400, {"error": str(error)})
    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8770)
    args = parser.parse_args()
    token = os.environ.get("YAGF_SYMBOL_TOKEN", "")
    if args.host not in ("127.0.0.1", "::1", "localhost") and not token:
        parser.error("A non-loopback service requires YAGF_SYMBOL_TOKEN")
    ThreadingHTTPServer((args.host, args.port), handler(token)).serve_forever()


if __name__ == "__main__":
    main()
