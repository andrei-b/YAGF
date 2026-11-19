"""Transcribe separated rows of printed mathematics from a page or region."""
import json
import sys
from pathlib import Path

import cv2
import numpy as np
import rapid_latex_ocr
from PIL import Image, ImageOps
from rapid_latex_ocr import LaTeXOCR


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
        yield int(columns[0]), top, int(columns[-1]) + 1, bottom


def recognize(path, boxes=None):
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
    model = LaTeXOCR(**{key: str(models / name) for key, name in weights.items()})
    result = []
    for left, top, right, bottom in boxes:
        crop = gray.crop((left, top, right, bottom))
        scale = min(48 / crop.height, 672 / crop.width)
        size = max(1, round(crop.width * scale)), max(1, round(crop.height * scale))
        crop = ImageOps.expand(crop.resize(size, Image.Resampling.LANCZOS), border=8, fill=255)
        text, _ = model(np.asarray(crop.convert("RGB")))
        if not text.strip():
            raise ValueError(f"No formula reading for image rows {top}–{bottom}")
        result.append(dict(box=[left, top, right-left, bottom-top], text=text))
    return result


if __name__ == "__main__":
    boxes = json.loads(Path(sys.argv[2]).read_text()) if len(sys.argv) > 2 else None
    print(json.dumps(recognize(sys.argv[1], boxes)))
