"""Small, versioned HTTP adapters for optional OCR inference providers."""

import argparse
import base64
import binascii
import hmac
import json
import logging
import math
import platform
import time
import importlib.metadata
try:
    import resource
except ImportError:
    resource = None
import os
import re
import struct
import tempfile
import threading
import urllib.request
from urllib.parse import urlsplit
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from html.parser import HTMLParser


PROTOCOL = "yagf-ocr/1"
MAX_BODY = 44 * 1024 * 1024
MAX_PNG = 32 * 1024 * 1024
MAX_DIMENSION = 20000
MAX_ITEMS = 10000


def word_box(geometry, width, height):
    """Convert a docTR normalized straight rectangle to bounded pixel geometry."""
    if len(geometry) != 2 or any(len(point) != 2 for point in geometry):
        raise ValueError("docTR returned unsupported word geometry")
    (left, top), (right, bottom) = geometry
    if not all(math.isfinite(float(value)) for value in (left, top, right, bottom)):
        raise ValueError("docTR returned nonfinite word geometry")
    if not (0 <= left < right <= 1 and 0 <= top < bottom <= 1):
        raise ValueError("docTR returned a word box outside the image")
    x1, y1 = math.floor(left * width), math.floor(top * height)
    x2, y2 = math.ceil(right * width), math.ceil(bottom * height)
    return [x1, y1, x2 - x1, y2 - y1]


def normalize_doctr_result(result, width, height):
    words = []
    if len(result.pages) != 1:
        raise ValueError("docTR returned an unexpected page count")
    page = result.pages[0]
    for block_number, block in enumerate(page.blocks, 1):
        for line_number, line in enumerate(block.lines, 1):
            for word in line.words:
                value = str(word.value).strip()
                if not value:
                    continue
                score = float(word.confidence)
                if not math.isfinite(score) or not 0 <= score <= 1:
                    raise ValueError("docTR returned invalid confidence")
                words.append({"text": value, "confidence": score,
                              "box": word_box(word.geometry, width, height),
                              "block": block_number, "line": line_number})
                if len(words) > MAX_ITEMS:
                    raise ValueError("docTR returned too many words")
    return words


def png_size(data):
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("A PNG image is required")
    width, height = struct.unpack(">II", data[16:24])
    if not (0 < width <= MAX_DIMENSION and 0 < height <= MAX_DIMENSION):
        raise ValueError("Invalid PNG dimensions")
    return width, height


def normalize_paddle_result(result, width, height):
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
            if len(lines) > MAX_ITEMS:
                raise ValueError("PaddleOCR returned too many lines")
    return lines


def polygon_box(points, width, height):
    """Return a bounded pixel rectangle around a polygon or character cuts."""
    if points is None or len(points) == 0:
        raise ValueError("OCR returned empty geometry")
    coords = [(float(point[0]), float(point[1])) for point in points]
    if any(not math.isfinite(x) or not math.isfinite(y) for x, y in coords):
        raise ValueError("OCR returned nonfinite geometry")
    x1 = max(0, math.floor(min(x for x, _ in coords)))
    y1 = max(0, math.floor(min(y for _, y in coords)))
    x2 = min(width, math.ceil(max(x for x, _ in coords)))
    y2 = min(height, math.ceil(max(y for _, y in coords)))
    if x1 >= x2 or y1 >= y2:
        raise ValueError("OCR returned out-of-bounds geometry")
    return [x1, y1, x2 - x1, y2 - y1]


def normalize_rapid_result(result, width, height):
    boxes = result.boxes if result.boxes is not None else []
    texts = result.txts if result.txts is not None else []
    scores = result.scores if result.scores is not None else []
    if not (len(boxes) == len(texts) == len(scores)):
        raise ValueError("RapidOCR returned inconsistent line arrays")
    lines = []
    for polygon, value, confidence in zip(boxes, texts, scores):
        value = str(value).strip()
        if not value:
            continue
        confidence = float(confidence)
        if not math.isfinite(confidence) or not 0 <= confidence <= 1:
            raise ValueError("RapidOCR returned invalid confidence")
        lines.append({"text": value, "confidence": confidence,
                      "box": polygon_box(polygon, width, height)})
        if len(lines) > MAX_ITEMS:
            raise ValueError("RapidOCR returned too many lines")
    return lines


class BlockTextParser(HTMLParser):
    def __init__(self, label):
        super().__init__(convert_charrefs=True)
        self.label = label
        self.parts = []
        self.rows = []
        self.cell_parts = None

    def append(self, value):
        self.parts.append(value)
        if self.cell_parts is not None:
            self.cell_parts.append(value)

    def handle_starttag(self, tag, attrs):
        if tag == "math":
            self.append(r"\[" if self.label == "equation" else r"\(")
        elif tag == "tr":
            self.rows.append([])
            self.append("\n")
        elif tag in ("td", "th") and self.rows:
            attributes = dict(attrs)
            cell = {"text": "", "header": tag == "th"}
            for name in ("colspan", "rowspan"):
                value = attributes.get(name, "1")
                if not value or not re.fullmatch(r"[1-9][0-9]{0,2}", value):
                    raise ValueError("Invalid table span")
                cell[name] = int(value)
            self.rows[-1].append(cell)
            self.cell_parts = []
        elif tag in ("br", "p", "div", "li"):
            self.append("\n")

    def handle_endtag(self, tag):
        if tag == "math":
            self.append(r"\]" if self.label == "equation" else r"\)")
        elif tag in ("td", "th") and self.cell_parts is not None:
            self.rows[-1][-1]["text"] = " ".join("".join(self.cell_parts).split())
            self.cell_parts = None
            self.parts.append(" ")
        elif tag in ("p", "div", "tr", "li", "table"):
            self.append("\n")

    def handle_data(self, data):
        self.append(data)

    def text(self):
        if self.label == "table" and self.rows:
            return "\n".join("\t".join(cell["text"] for cell in row) for row in self.rows)
        return "\n".join(" ".join(line.split()) for line in
                         "".join(self.parts).splitlines() if line.strip()).strip()


def normalize_surya_result(result, width, height):
    if len(result) != 1:
        raise ValueError("Surya returned an unexpected page count")
    blocks = []
    for block in sorted(result[0].blocks, key=lambda item: item.reading_order):
        label = str(block.label).strip().lower() or "text"
        visual = label in ("figure", "picture", "diagram", "image")
        if block.error or (block.skipped and not visual):
            continue
        parser = BlockTextParser(label)
        parser.feed(block.html or "")
        parser.close()
        value = parser.text()
        if not value.strip():
            if not visual:
                continue
            value = "[" + label.capitalize() + "]"
        score = float(block.confidence) if block.confidence is not None else None
        if score is not None and (not math.isfinite(score) or not 0 <= score <= 1):
            raise ValueError("Surya returned invalid confidence")
        box = polygon_box(block.polygon, width, height)
        item = {"text": value, "confidence": score, "box": box,
                "block": len(blocks) + 1, "line": 1, "type": label}
        if label == "table" and parser.rows:
            item["table_layout"] = [[{k: v for k, v in cell.items() if k != "text"}
                                     for cell in row] for row in parser.rows]
        blocks.append(item)
        if len(blocks) > MAX_ITEMS:
            raise ValueError("Surya returned too many blocks")
    return blocks


def normalize_kraken_records(records, width, height):
    words = []
    for line_number, record in enumerate(records, 1):
        prediction = str(record.prediction)
        if not prediction.strip():
            continue
        cuts = record.cuts
        confidences = record.confidences
        if len(cuts) != len(prediction) or len(confidences) != len(prediction):
            line = record.line
            boundary = getattr(line, "boundary", None)
            if boundary is not None and len(boundary) > 0:
                box = polygon_box(boundary, width, height)
            else:
                x1, y1, x2, y2 = line.bbox
                box = polygon_box(((x1, y1), (x2, y2)), width, height)
            scores = [float(value) for value in confidences]
            score = sum(scores) / len(scores) if scores else 0.0
            if not math.isfinite(score) or not 0 <= score <= 1:
                raise ValueError("Kraken returned invalid confidence")
            words.append({"text": prediction.strip(), "confidence": score,
                          "box": box, "block": 1, "line": line_number})
            if len(words) > MAX_ITEMS:
                raise ValueError("Kraken returned too many words")
            continue
        for match in re.finditer(r"\S+", prediction):
            points = [point for cut in cuts[match.start():match.end()] for point in cut]
            score = sum(float(value) for value in confidences[match.start():match.end()]) / len(match.group())
            if not math.isfinite(score) or not 0 <= score <= 1:
                raise ValueError("Kraken returned invalid confidence")
            words.append({"text": match.group(), "confidence": score,
                          "box": polygon_box(points, width, height),
                          "block": 1, "line": line_number})
            if len(words) > MAX_ITEMS:
                raise ValueError("Kraken returned too many words")
    return words


class TranscriptionTruncated(ValueError):
    """A bounded retry on smaller image sections can recover a dense page."""


class PaddleVlBackend:
    """Bridge a running PaddleOCR-VL chat server without inventing word geometry."""
    engine_id = "paddleocr"
    result_key = "lines"
    language = "auto"

    def __init__(self, endpoint, model):
        url = urlsplit(endpoint)
        if url.scheme not in ("http", "https") or not url.hostname or url.username or url.password or url.fragment:
            raise ValueError("Invalid PaddleOCR-VL chat endpoint")
        if not model.strip():
            raise ValueError("A PaddleOCR-VL model name is required")
        self.endpoint, self.model = endpoint, model
        self.token = os.environ.get("YAGF_VL_TOKEN", "")
        self.lock = threading.Lock()

    def recognize(self, png, width, height, language):
        prompt = "OCR:" if language in ("auto", "") else f"OCR:\nDocument language: {language}."
        payload = {"model": self.model, "temperature": 0, "stream": False,
                   "max_tokens": 4096, "messages": [{"role": "user", "content": [
                       {"type": "image_url", "image_url": {"url":
                           "data:image/png;base64," + base64.b64encode(png).decode("ascii")}},
                       {"type": "text", "text": prompt}]}]}
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        request = urllib.request.Request(self.endpoint, json.dumps(payload).encode(), headers)
        with self.lock, urllib.request.urlopen(request, timeout=170) as response:
            body = response.read(8 * 1024 * 1024 + 1)
        if len(body) > 8 * 1024 * 1024:
            raise ValueError("PaddleOCR-VL response is too large")
        result = json.loads(body)
        choices = result.get("choices") if isinstance(result, dict) else None
        if not isinstance(choices, list) or not choices or not isinstance(choices[0], dict):
            raise ValueError("PaddleOCR-VL returned no transcription")
        choice = choices[0]
        if choice.get("finish_reason") != "stop":
            raise TranscriptionTruncated("PaddleOCR-VL did not finish the transcription; retry smaller image sections")
        message = choice.get("message")
        text = message.get("content") if isinstance(message, dict) else None
        if not isinstance(text, str):
            raise ValueError("PaddleOCR-VL returned invalid transcription text")
        if not text.strip():
            return []
        return [{"text": text.strip(), "confidence": None, "box": [0, 0, width, height],
                 "layout_block": True}]


class PaddleOcrBackend:
    engine_id = "paddleocr"
    result_key = "lines"

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
                return normalize_paddle_result(self.pipeline.predict(image.name), width, height)


class DocTrBackend:
    engine_id = "doctr"
    result_key = "words"
    language = "auto"

    def __init__(self, device):
        from doctr.models import ocr_predictor

        self.lock = threading.Lock()
        self.pipeline = ocr_predictor(pretrained=True, assume_straight_pages=True)
        if device == "cpu":
            self.pipeline.to("cpu")
        elif device == "gpu":
            self.pipeline.to("cuda")
        else:
            raise ValueError("docTR device must be 'cpu' or 'gpu'")

    def recognize(self, png, width, height, language):
        if language != "auto":
            raise ValueError("docTR uses its model vocabulary; set language to 'auto'")
        from doctr.io import DocumentFile

        with tempfile.NamedTemporaryFile(suffix=".png") as image:
            image.write(png)
            image.flush()
            with self.lock:
                result = self.pipeline(DocumentFile.from_images(image.name))
                return normalize_doctr_result(result, width, height)


class RapidOcrBackend:
    engine_id = "rapidocr"
    result_key = "lines"
    language = "auto"

    def __init__(self):
        from rapidocr import RapidOCR
        self.lock = threading.Lock()
        self.pipeline = RapidOCR()

    def recognize(self, png, width, height, language):
        if language != "auto":
            raise ValueError("RapidOCR uses its configured model vocabulary; set language to 'auto'")
        with tempfile.NamedTemporaryFile(suffix=".png") as image:
            image.write(png)
            image.flush()
            with self.lock:
                return normalize_rapid_result(self.pipeline(image.name), width, height)


class SuryaBackend:
    engine_id = "surya"
    result_key = "lines"
    language = "auto"

    def __init__(self):
        from surya.inference import SuryaInferenceManager
        from surya.recognition import RecognitionPredictor
        self.lock = threading.Lock()
        self.pipeline = RecognitionPredictor(SuryaInferenceManager())

    def recognize(self, png, width, height, language):
        if language != "auto":
            raise ValueError("Surya selects languages automatically; set language to 'auto'")
        from PIL import Image
        with tempfile.NamedTemporaryFile(suffix=".png") as image:
            image.write(png)
            image.flush()
            with Image.open(image.name) as page, self.lock:
                return normalize_surya_result(self.pipeline([page.convert("RGB")]), width, height)


class KrakenBackend:
    engine_id = "kraken"
    result_key = "words"
    language = "auto"

    def __init__(self, model_path, device):
        if not model_path or not os.path.isfile(model_path):
            raise ValueError("Kraken requires an existing --model file")
        from kraken.tasks import RecognitionTaskModel, SegmentationTaskModel
        from kraken.configs import RecognitionInferenceConfig, SegmentationInferenceConfig
        self.lock = threading.Lock()
        self.segmenter = SegmentationTaskModel.load_model()
        self.recognizer = RecognitionTaskModel.load_model(model_path)
        self.segment_config = SegmentationInferenceConfig(device=device)
        self.recognize_config = RecognitionInferenceConfig(device=device, num_line_workers=0)

    def recognize(self, png, width, height, language):
        if language != "auto":
            raise ValueError("Kraken uses its selected recognition model; set language to 'auto'")
        from PIL import Image
        with tempfile.NamedTemporaryFile(suffix=".png") as image:
            image.write(png)
            image.flush()
            with Image.open(image.name) as page, self.lock:
                page = page.convert("RGB")
                segmentation = self.segmenter.predict(page, self.segment_config)
                records = self.recognizer.predict(page, segmentation, self.recognize_config)
                return normalize_kraken_records(records, width, height)


def benchmark_details(backend):
    """Describe measured resources without treating a remote VL bridge as the GPU host."""
    remote = isinstance(backend, PaddleVlBackend)
    packages = {"paddleocr": "paddleocr", "doctr": "python-doctr", "rapidocr": "rapidocr",
                "surya": "surya-ocr", "kraken": "kraken"}
    version = "unreported"
    if not remote:
        try:
            version = importlib.metadata.version(packages.get(backend.engine_id, "unknown"))
        except importlib.metadata.PackageNotFoundError:
            pass
    model = getattr(backend, "model", type(backend).__name__)
    details = {"engine": backend.engine_id, "model": model if isinstance(model, str) else type(backend).__name__,
               "version": version, "architecture": platform.machine(),
               "device": "remote upstream" if remote else getattr(backend, "benchmark_device", "unreported"),
               "peak_rss_scope": "Bridge process lifetime; excludes upstream model" if remote else "Inference service process lifetime"}
    if remote:
        details["memory_note"] = "Upstream model memory requires a separate hardware collector"
    try:
        with open("/etc/nv_tegra_release", encoding="utf-8") as info:
            details["jetpack"] = info.readline().strip()[:256]
    except OSError:
        pass
    return details


def handler_for(backend, token=""):
    provenance = benchmark_details(backend)
    inference_lock = threading.Lock()
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
                self.send_json(200, {"protocol": PROTOCOL, "engine_id": backend.engine_id,
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
                queued = time.perf_counter()
                with inference_lock:
                    started = time.perf_counter()
                    items = backend.recognize(png, width, height, language)
                    details = dict(provenance, inference_ms=(time.perf_counter()-started)*1000,
                                   queue_ms=(started-queued)*1000)
                    if resource is not None:
                        peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
                        details["peak_rss_bytes"] = peak if platform.system() == "Darwin" else peak*1024
                self.send_json(200, {"protocol": PROTOCOL, "engine_id": backend.engine_id,
                                     "width": width, "height": height,
                                     "language": language, backend.result_key: items, "run_details": details})
            except TranscriptionTruncated as exc:
                self.send_json(400, {"error": str(exc), "code": "transcription_truncated"})
            except (ValueError, TypeError, KeyError, binascii.Error, json.JSONDecodeError) as exc:
                self.send_json(400, {"error": str(exc)})
            except Exception:
                self.send_json(500, {"error": "OCR inference failed; see server log"})
                logging.exception("OCR inference failed")

    return Handler


def main():
    parser = argparse.ArgumentParser(description="YAGF OCR inference service")
    parser.add_argument("--engine", choices=("paddleocr", "paddleocr-vl", "doctr", "rapidocr", "surya", "kraken"),
                        default="paddleocr")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--device", help="Inference device: gpu or cpu; defaults vary by engine")
    parser.add_argument("--language", help="PaddleOCR language code; other engines use auto")
    parser.add_argument("--model", help="Kraken recognition model path")
    parser.add_argument("--vl-endpoint", default="http://127.0.0.1:8082/v1/chat/completions",
                        help="PaddleOCR-VL OpenAI-compatible chat endpoint")
    parser.add_argument("--vl-model", default="PaddleOCR-VL-1.6", help="PaddleOCR-VL server model name")
    args = parser.parse_args()
    token = os.environ.get("YAGF_OCR_TOKEN", "")
    if args.host not in ("127.0.0.1", "localhost", "::1") and not token:
        parser.error("YAGF_OCR_TOKEN is required when binding to a network address")
    if args.engine != "paddleocr" and args.language not in (None, "auto"):
        parser.error(f"{args.engine} uses its model vocabulary; set --language auto")
    if args.engine == "kraken" and not args.model:
        parser.error("Kraken requires --model PATH")
    if args.engine != "kraken" and args.model:
        parser.error("--model is only used with Kraken")
    device = args.device or ("cpu" if args.engine == "rapidocr" else
                             "auto" if args.engine == "kraken" else "gpu")
    if args.engine == "rapidocr" and device != "cpu":
        parser.error("RapidOCR's default ONNX backend runs on CPU; use --device cpu")
    if args.engine == "paddleocr":
        backend = PaddleOcrBackend(device, args.language or "en")
    elif args.engine == "paddleocr-vl":
        backend = PaddleVlBackend(args.vl_endpoint, args.vl_model)
    elif args.engine == "doctr":
        backend = DocTrBackend(device)
    elif args.engine == "rapidocr":
        backend = RapidOcrBackend()
    elif args.engine == "surya":
        backend = SuryaBackend()
    else:
        backend = KrakenBackend(args.model, "cuda:0" if device == "gpu" else device)
    backend.benchmark_device = device
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
