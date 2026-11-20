"""Page-area detection with the official PP-DocLayoutV3 ONNX model."""
import argparse
import base64
import binascii
import hashlib
import hmac
import io
import json
import logging
import math
import os
from pathlib import Path
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROTOCOL = "yagf-layout/1"
MODEL_ID = "PP-DocLayoutV3"
MODEL_REVISION = "46bbdf188bb0a772c08aed74882ce7e51a8f1ea6"
MODEL_SHA256 = "45bf71750b00739a41fc209f132eb104a4d6b5bb29483c9078164d8b87cf28ba"
LABELS = ("abstract", "algorithm", "aside_text", "chart", "content", "display_formula",
          "doc_title", "figure_title", "footer", "footer_image", "footnote", "formula_number",
          "header", "header_image", "image", "inline_formula", "number", "paragraph_title",
          "reference", "reference_content", "seal", "table", "text", "vertical_text", "vision_footnote")
PICTURES = {"image", "chart", "seal", "header_image", "footer_image"}


def kind(label):
    return "picture" if label in PICTURES else "math" if label == "display_formula" else "mixed" if label == "inline_formula" else "text"


def normalize_boxes(rows, width, height, threshold):
    # The exported DETR top-k output can contain several classes for the same
    # query. A query has exactly the same box and reading-order value for each
    # class. Keep those actual scores; neighboring/overlapping queries are not
    # evidence of an alternative classification of this block.
    queries = {}
    for index, row in enumerate(rows):
        if len(row) != 7 or any(not math.isfinite(float(x)) for x in row):
            raise ValueError("Invalid model output")
        category, score, left, top, right, bottom, order = map(float, row)
        if not category.is_integer() or not 0 <= category < len(LABELS) or not 0 <= score <= 1:
            raise ValueError("Invalid model label or score")
        x1, y1 = max(0, math.floor(left)), max(0, math.floor(top))
        x2, y2 = min(width, math.ceil(right)), min(height, math.ceil(bottom))
        if x2 <= x1 or y2 <= y1:
            continue
        label = LABELS[int(category)]
        queries.setdefault((left, top, right, bottom, order), []).append(
            {"id": "area-" + str(index), "label": label, "type": kind(label),
             "score": score, "box": [x1, y1, x2-x1, y2-y1], "order": order})
        if len(queries) > 1000:
            raise ValueError("Too many page areas")
    result = []
    for predictions in queries.values():
        predictions.sort(key=lambda item: item["score"], reverse=True)
        primary = predictions[0]
        if primary["score"] < threshold:
            continue
        seen = {primary["type"]}
        alternatives = []
        for candidate in predictions[1:]:
            if candidate["type"] in seen or candidate["score"] < .4 or primary["score"]-candidate["score"] > .2+1e-6:
                continue
            alternatives.append({key: candidate[key] for key in ("label", "type", "score")})
            seen.add(candidate["type"])
        primary["alternatives"] = alternatives
        result.append(primary)
    return sorted(result, key=lambda item: item["order"])


class OnnxLayout:
    def __init__(self, path, threads=2):
        import numpy as np
        import onnxruntime as ort
        from PIL import Image
        digest = hashlib.sha256()
        with open(path, "rb") as stream:
            for chunk in iter(lambda: stream.read(1024*1024), b""):
                digest.update(chunk)
        if digest.hexdigest() != MODEL_SHA256:
            raise ValueError("The model does not match the official pinned PP-DocLayoutV3 weights")
        options = ort.SessionOptions()
        options.intra_op_num_threads = threads
        options.inter_op_num_threads = 1
        self.session = ort.InferenceSession(str(path), sess_options=options, providers=["CPUExecutionProvider"])
        if {i.name for i in self.session.get_inputs()} != {"im_shape", "image", "scale_factor"}:
            raise ValueError("Unexpected model inputs")
        self.np, self.Image = np, Image
        self.outputs = [o.name for o in self.session.get_outputs()[:2]]
        self.lock = threading.Lock()

    def detect(self, png, width, height, threshold):
        np, Image = self.np, self.Image
        with Image.open(io.BytesIO(png)) as original:
            if original.format != "PNG" or original.size != (width, height):
                raise ValueError("PNG geometry does not match its header")
            if original.mode in ("RGBA", "LA") or "transparency" in original.info:
                rgba = original.convert("RGBA")
                image = Image.new("RGB", rgba.size, "white")
                image.paste(rgba, mask=rgba.getchannel("A"))
            else:
                image = original.convert("RGB")
            # Official inference.yml: RGB, 800x800 without preserving aspect,
            # OpenCV INTER_LINEAR, scale=1/255, mean=0, std=1, then HWC->CHW.
            # cv2 is optional; Pillow's bilinear path supports minimal hosts.
            try:
                import cv2
                resized = cv2.resize(np.asarray(image), (800, 800), interpolation=cv2.INTER_LINEAR)
            except ImportError:
                resized = np.asarray(image.resize((800, 800), Image.Resampling.BILINEAR))
        data = np.ascontiguousarray(resized.transpose(2, 0, 1)[None], dtype=np.float32) / np.float32(255.)
        feed = {"image": data, "im_shape": np.asarray([[800., 800.]], dtype=np.float32),
                "scale_factor": np.asarray([[800./height, 800./width]], dtype=np.float32)}
        with self.lock:
            boxes, counts = self.session.run(self.outputs, feed)
        if len(counts) != 1 or int(counts[0]) != len(boxes):
            raise ValueError("Unexpected model detection count")
        return normalize_boxes(boxes.tolist(), width, height, threshold)


def handler_for(backend, token=""):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def send(self, status, payload):
            body = json.dumps(payload, ensure_ascii=False, allow_nan=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_GET(self):
            if self.path == "/v1/health":
                self.send(200, {"protocol": PROTOCOL, "status": "ready", "model": MODEL_ID,
                                "revision": MODEL_REVISION, "sha256": MODEL_SHA256, "device": "cpu"})
            else:
                self.send(404, {"error": "Unknown endpoint"})

        def do_POST(self):
            if self.path != "/v1/layout":
                self.send(404, {"error": "Unknown endpoint"})
                return
            if token and not hmac.compare_digest(self.headers.get("Authorization", ""), "Bearer " + token):
                self.send(401, {"error": "Unauthorized"})
                return
            try:
                length = int(self.headers.get("Content-Length", "-1"))
                if not 0 < length <= 44*1024*1024 or self.headers.get_content_type() != "application/json":
                    raise ValueError("Invalid JSON body length or content type")
                request = json.loads(self.rfile.read(length))
                if not isinstance(request, dict) or request.get("protocol") != PROTOCOL:
                    raise ValueError("Incompatible layout protocol")
                encoded = request.get("image_png")
                if not isinstance(encoded, str):
                    raise ValueError("Missing page image")
                png = base64.b64decode(encoded, validate=True)
                if not 33 <= len(png) <= 32*1024*1024 or png[:8] != b"\x89PNG\r\n\x1a\n" or png[12:16] != b"IHDR":
                    raise ValueError("Invalid or oversized PNG")
                width, height = struct.unpack(">II", png[16:24])
                if not 1 <= width <= 20000 or not 1 <= height <= 20000 or width*height > 80000000:
                    raise ValueError("Page dimensions exceed the layout limit")
                threshold = request.get("threshold", .5)
                if type(threshold) not in (int, float) or not math.isfinite(threshold) or not .1 <= threshold <= .95:
                    raise ValueError("Invalid model-score threshold")
                started = time.perf_counter()
                areas = backend.detect(png, width, height, threshold)
                self.send(200, {"protocol": PROTOCOL, "width": width, "height": height,
                                "model": MODEL_ID, "revision": MODEL_REVISION, "areas": areas,
                                "elapsed_ms": round((time.perf_counter()-started)*1000, 2)})
            except (ValueError, TypeError, KeyError, binascii.Error) as error:
                self.send(400, {"error": str(error)})
            except Exception:
                logging.exception("Layout inference failed")
                self.send(500, {"error": "Layout inference failed; check the worker log"})
    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8771)
    parser.add_argument("--threads", type=int, default=2)
    args = parser.parse_args()
    token = os.environ.get("YAGF_LAYOUT_TOKEN", "")
    if args.host not in ("127.0.0.1", "::1", "localhost") and not token:
        parser.error("A non-loopback service requires YAGF_LAYOUT_TOKEN")
    if not 1 <= args.threads <= 16:
        parser.error("Use between 1 and 16 CPU threads")
    backend = OnnxLayout(args.model, args.threads)
    ThreadingHTTPServer((args.host, args.port), handler_for(backend, token)).serve_forever()


if __name__ == "__main__":
    main()
