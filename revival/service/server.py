"""Small, versioned HTTP adapter for PaddleOCR 3.x inference."""

import argparse
import base64
import binascii
import hmac
import json
import logging
import os
import struct
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


PROTOCOL = "yagf-ocr/1"
MAX_BODY = 44 * 1024 * 1024
MAX_PNG = 32 * 1024 * 1024
MAX_DIMENSION = 20000
MAX_LINES = 10000


def png_size(data):
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("A PNG image is required")
    width, height = struct.unpack(">II", data[16:24])
    if not (0 < width <= MAX_DIMENSION and 0 < height <= MAX_DIMENSION):
        raise ValueError("Invalid PNG dimensions")
    return width, height


def normalize_result(result, width, height):
    """Convert PaddleOCR 3.x result objects to the YAGF line schema."""
    lines = []
    for item in result:
        data = item.json
        if callable(data):
            data = data()
        data = data.get("res", data)
        texts = data.get("rec_texts", [])
        scores = data.get("rec_scores", [])
        boxes = data.get("rec_boxes", [])
        if not (len(texts) == len(scores) == len(boxes)):
            raise ValueError("PaddleOCR returned inconsistent line arrays")
        for text, score, box in zip(texts, scores, boxes):
            text = str(text).strip()
            if not text:
                continue
            x1, y1, x2, y2 = [int(value) for value in box]
            if not (0 <= x1 < x2 <= width and 0 <= y1 < y2 <= height):
                raise ValueError("PaddleOCR returned a box outside the image")
            score = float(score)
            if not 0 <= score <= 1:
                raise ValueError("PaddleOCR returned invalid confidence")
            lines.append({"text": text, "confidence": score,
                          "box": [x1, y1, x2 - x1, y2 - y1]})
            if len(lines) > MAX_LINES:
                raise ValueError("PaddleOCR returned too many lines")
    return lines


class PaddleOcrBackend:
    def __init__(self, device, language):
        from paddleocr import PaddleOCR

        self.language = language
        self.lock = threading.Lock()
        self.pipeline = PaddleOCR(
            lang=language, device=device,
            use_doc_orientation_classify=False,
            use_doc_unwarping=False,
            use_textline_orientation=False,
        )

    def recognize(self, png, width, height, language):
        if language != self.language:
            raise ValueError(f"This service is configured for language '{self.language}'")
        with tempfile.NamedTemporaryFile(suffix=".png") as image:
            image.write(png)
            image.flush()
            with self.lock:
                return normalize_result(self.pipeline.predict(image.name), width, height)


def handler_for(backend, token=""):
    class Handler(BaseHTTPRequestHandler):
        def send_json(self, code, payload):
            data = json.dumps(payload, ensure_ascii=False, allow_nan=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path == "/v1/health":
                self.send_json(200, {"protocol": PROTOCOL, "engine_id": "paddleocr",
                                     "status": "ready"})
            else:
                self.send_json(404, {"error": "Unknown endpoint"})

        def do_POST(self):
            if self.path != "/v1/recognize":
                self.send_json(404, {"error": "Unknown endpoint"})
                return
            authorization = self.headers.get("Authorization", "")
            if token and not hmac.compare_digest(authorization, "Bearer " + token):
                self.send_json(401, {"error": "Unauthorized"})
                return
            try:
                length = int(self.headers.get("Content-Length", "-1"))
                if not 0 < length <= MAX_BODY:
                    raise ValueError("Request is empty or too large")
                if self.headers.get_content_type() != "application/json":
                    raise ValueError("JSON content type is required")
                request = json.loads(self.rfile.read(length))
                if not isinstance(request, dict):
                    raise ValueError("JSON object is required")
                if request.get("protocol") != PROTOCOL:
                    raise ValueError("Incompatible OCR protocol")
                page_number = request.get("page_number")
                if type(page_number) is not int or not 1 <= page_number <= 100000:
                    raise ValueError("Invalid page number")
                encoded = request.get("image_png")
                if not isinstance(encoded, str):
                    raise ValueError("Missing image_png")
                png = base64.b64decode(encoded, validate=True)
                if len(png) > MAX_PNG:
                    raise ValueError("PNG image is too large")
                width, height = png_size(png)
                language = request.get("language") or backend.language
                if not isinstance(language, str) or len(language) > 64:
                    raise ValueError("Invalid language")
                lines = backend.recognize(png, width, height, language)
                self.send_json(200, {"protocol": PROTOCOL, "engine_id": "paddleocr",
                                     "width": width, "height": height,
                                     "language": language, "lines": lines})
            except (ValueError, TypeError, KeyError, binascii.Error, json.JSONDecodeError) as exc:
                self.send_json(400, {"error": str(exc)})
            except Exception:
                self.send_json(500, {"error": "PaddleOCR inference failed; see server log"})
                logging.exception("PaddleOCR inference failed")

    return Handler


def main():
    parser = argparse.ArgumentParser(description="YAGF PaddleOCR inference service")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--device", default="gpu", help="Paddle device, e.g. gpu or cpu")
    parser.add_argument("--language", default="en", help="PaddleOCR language code")
    args = parser.parse_args()
    token = os.environ.get("YAGF_OCR_TOKEN", "")
    if args.host not in ("127.0.0.1", "localhost", "::1") and not token:
        parser.error("YAGF_OCR_TOKEN is required when binding to a network address")
    backend = PaddleOcrBackend(args.device, args.language)
    server = ThreadingHTTPServer((args.host, args.port), handler_for(backend, token))
    print(f"YAGF OCR service listening on {args.host}:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
