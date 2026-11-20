"""Transcribe separated rows of printed mathematics from a page or region."""
import json
import argparse
import contextlib
import signal
import time
import re
import sys
from pathlib import Path

import cv2
import numpy as np
import rapid_latex_ocr
from PIL import Image, ImageOps
from rapid_latex_ocr import LaTeXOCR


def create_model(weights):
    # rapid_latex_ocr creates three independent ONNX pools with an unrestricted
    # default. Bound all three inside this isolated helper process.
    from rapid_latex_ocr import main, models
    from rapid_latex_ocr.utils_load import OrtInferSession
    class CpuSession(OrtInferSession):
        def __init__(self, path): super().__init__(path, num_threads=2)
        def _init_sess_opt(self):
            super()._init_sess_opt()
            self.sess_opt.inter_op_num_threads = 1
            self.sess_opt.add_session_config_entry('session.intra_op.allow_spinning', '0')
    previous = main.OrtInferSession, models.OrtInferSession
    try:
        main.OrtInferSession = models.OrtInferSession = CpuSession
        return LaTeXOCR(**weights)
    finally:
        main.OrtInferSession, models.OrtInferSession = previous


@contextlib.contextmanager
def crop_deadline(seconds):
    if not seconds or not hasattr(signal, 'setitimer'):
        yield
        return
    def expired(*_): raise TimeoutError('Formula crop exceeded its time limit')
    previous = signal.signal(signal.SIGALRM, expired)
    timer = signal.setitimer(signal.ITIMER_REAL, seconds)
    try: yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, *timer)
        signal.signal(signal.SIGALRM, previous)


def equation_boxes(gray):
    # Use glyph height to bridge the white space inside stacked fractions.
    ink = (np.asarray(gray) < 175).astype(np.uint8)
    contours, _ = cv2.findContours(ink, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    heights = [h for _, _, w, h in map(cv2.boundingRect, contours) if h >= 3 and w >= 2]
    rows = np.flatnonzero(ink.any(axis=1))
    if not len(rows):
        return
    gap = max(3, round(float(np.median(heights)) * 0.85)) if heights else 3
    for band in np.split(rows, np.flatnonzero(np.diff(rows) > gap + 1) + 1):
        top, bottom = int(band[0]), int(band[-1]) + 1
        columns = np.flatnonzero(ink[top:bottom].any(axis=0))
        if bottom-top >= 3 and int(columns[-1])-int(columns[0]) >= 2:
            yield int(columns[0]), top, int(columns[-1]) + 1, bottom


def clean_crop(crop):
    pixels = np.asarray(crop.convert("L")).copy()
    _, labels, stats, _ = cv2.connectedComponentsWithStats((pixels < 175).astype(np.uint8), 8)
    glyph_heights = [h for _, _, w, h, area in stats[1:] if h >= 3 and w >= 2 and area >= 4]
    if not glyph_heights:
        raise ValueError("No mathematical ink in the selected region")
    height = float(np.median(glyph_heights))
    minimum_area = max(4, round(height * 0.7))
    substantial = [box for box in stats[1:] if box[4] >= minimum_area]
    if not substantial:
        raise ValueError("No mathematical ink in the selected region")
    left = min(x for x, _, _, _, _ in substantial)
    top = min(y for _, y, _, _, _ in substantial)
    right = max(x+w for x, _, w, _, _ in substantial)
    bottom = max(y+h for _, y, _, h, _ in substantial)
    # Keep small marks beside symbols: decimal points, primes and subscripts.
    # Remove only distant specks that would widen the formula's input image.
    for label, (x, y, w, h, area) in enumerate(stats[1:], 1):
        if area < minimum_area and (x+w < left-height or x > right+height or
                                    y+h < top-height or y > bottom+height):
            pixels[labels == label] = 255
    ys, xs = np.nonzero(pixels < 175)
    crop = Image.fromarray(pixels).crop((int(xs.min()), int(ys.min()), int(xs.max())+1, int(ys.max())+1))
    return crop, height


def prepare_crop(crop):
    crop, glyph_height = clean_crop(crop)
    scale = min(48 / crop.height, 672 / crop.width)
    if glyph_height * scale < 3:
        raise ValueError("Too many lines in the math crop. Turn off Math only for pages containing prose, "
                         "or select a smaller equation region.")
    size = max(1, round(crop.width * scale)), max(1, round(crop.height * scale))
    return ImageOps.expand(crop.resize(size, Image.Resampling.LANCZOS), border=8, fill=255)


def validate_latex(text):
    if not text.strip() or text.count("{}") > 20:
        raise ValueError("Empty or repetitive formula reading")
    braces, environments = 0, []
    for token in re.finditer(r"\\(begin|end)\s*\{([^{}]+)\}|\\.|[{}]", text):
        if token.group(1) == "begin":
            environments.append(token.group(2))
        elif token.group(1) == "end":
            if not environments or environments.pop() != token.group(2):
                raise ValueError("Unbalanced formula environment")
        elif token.group() == "{":
            braces += 1
        elif token.group() == "}":
            braces -= 1
            if braces < 0:
                raise ValueError("Unbalanced formula braces")
    if braces or environments:
        raise ValueError("Unclosed formula group or environment")


def recognize(path, boxes=None, time_budget=None, crop_timeout=15, progress=None):
    started = time.monotonic()
    with Image.open(path) as source:
        gray = source.convert("L")
        if "A" in source.getbands() or "transparency" in source.info:
            white = Image.new("L", source.size, 255)
            gray = Image.composite(gray, white, source.convert("RGBA").getchannel("A"))
    if boxes is None:
        boxes = list(equation_boxes(gray))
    if not boxes:
        return []
    models = Path(rapid_latex_ocr.__file__).parent / "models"
    weights = dict(image_resizer_path="image_resizer.onnx", encoder_path="encoder.onnx",
                   decoder_path="decoder.onnx", tokenizer_json="tokenizer.json")
    model = create_model({key: str(models / name) for key, name in weights.items()})
    result = []
    for left, top, right, bottom in boxes:
        entry = dict(box=[left, top, right-left, bottom-top])
        try:
            remaining = time_budget - (time.monotonic()-started) if time_budget is not None else crop_timeout
            if remaining <= 0:
                entry['error'] = 'Formula time budget exhausted'
            else:
                with crop_deadline(min(crop_timeout, remaining)):
                    crop = prepare_crop(gray.crop((left, top, right, bottom)))
                    text, _ = model(np.asarray(crop.convert("RGB")))
                    validate_latex(text)
                    entry["text"] = text
        except Exception as error:
            # Keep other successful crops; the caller retains native OCR here.
            entry["error"] = str(error)
        result.append(entry)
        if progress: progress(len(result), len(boxes))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('image'); parser.add_argument('boxes', nargs='?')
    parser.add_argument('--time-budget', type=float, default=300)
    parser.add_argument('--crop-timeout', type=float, default=15)
    args = parser.parse_args()
    boxes = json.loads(Path(args.boxes).read_text()) if args.boxes else None
    def progress(done, total): print('YAGF_PROGRESS '+json.dumps(dict(done=done,total=total)),file=sys.stderr,flush=True)
    print(json.dumps(recognize(args.image, boxes, args.time_budget, args.crop_timeout, progress)))
