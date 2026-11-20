"""Transcribe separated rows of printed mathematics from a page or region."""
import json
import re
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
        entry = dict(box=[left, top, right-left, bottom-top])
        try:
            crop = prepare_crop(gray.crop((left, top, right, bottom)))
            text, _ = model(np.asarray(crop.convert("RGB")))
            validate_latex(text)
            entry["text"] = text
        except Exception as error:
            # Keep other successful crops; the caller retains native OCR here.
            entry["error"] = str(error)
        result.append(entry)
    return result


if __name__ == "__main__":
    boxes = json.loads(Path(sys.argv[2]).read_text()) if len(sys.argv) > 2 else None
    print(json.dumps(recognize(sys.argv[1], boxes)))
