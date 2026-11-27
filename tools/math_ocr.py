"""Transcribe separated rows of printed mathematics from a page or region."""
import json
import argparse
import contextlib
import hashlib
import os
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
from rapid_latex_ocr.utils_load import ONNXRuntimeError


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


def local_weights():
    models = Path(rapid_latex_ocr.__file__).parent / "models"
    return {key: str(models / name) for key, name in {
        "image_resizer_path": "image_resizer.onnx", "encoder_path": "encoder.onnx",
        "decoder_path": "decoder.onnx", "tokenizer_json": "tokenizer.json",
    }.items()}


def weight_signature(weights):
    fingerprints = []
    for path in weights.values():
        digest = hashlib.sha256()
        with open(path, "rb") as input_file:
            for chunk in iter(lambda: input_file.read(1024 * 1024), b""):
                digest.update(chunk)
        fingerprints.append(digest.hexdigest())
    return hashlib.sha256("".join(fingerprints).encode("ascii")).hexdigest()


def formula_context(model, crop):
    tensor = model.loop_image_resizer(model.load_img(np.asarray(crop.convert("RGB"))))
    return model.encoder_decoder.encoder([tensor])[0]


class DecoderAdapter:
    """A small input-token-conditioned correction to frozen decoder logits."""

    def __init__(self, path, weights):
        with np.load(path, allow_pickle=False) as data:
            if data["version"].item() != 1 or data["weights"].item() != weight_signature(weights):
                raise ValueError("Math adapter does not match the installed RapidLaTeXOCR weights")
            self.delta = data["delta"].astype(np.float32)
            self.crop_rule = data["crop_rule"].astype(np.float32) if "crop_rule" in data else None
        if self.delta.ndim != 2 or self.delta.shape[0] != self.delta.shape[1] or not self.delta.size:
            raise ValueError("Invalid math decoder adapter shape")
        if not np.isfinite(self.delta).all() or np.abs(self.delta).max() > 2:
            raise ValueError("Invalid math decoder adapter")
        if self.crop_rule is not None and (self.crop_rule.shape != (6,) or not np.isfinite(self.crop_rule).all()):
            raise ValueError("Invalid math crop rule")

    def prepare(self, crop):
        if self.crop_rule is None:
            return prepare_crop(crop)
        crop, glyph_height = clean_crop(crop)
        enhance = crop_features(crop, glyph_height) @ self.crop_rule > 0
        return resize_crop(crop, glyph_height, enhance)

    def decode(self, model, context):
        return decode_view(model, context, self.delta)[0]


def decode_view(model, context, delta=None, score=False):
    decoder = model.encoder_decoder
    vocab = model.tokenizer.tokenizer.get_vocab_size()
    if delta is not None and delta.shape != (vocab, vocab):
        raise ValueError("Math adapter vocabulary does not match the decoder")
    prefix = np.full((1, decoder.max_seq_len + 1), decoder.bos_token, dtype=np.int64)
    mask = np.ones_like(prefix, dtype=bool)
    loss = 0.0
    for length in range(1, prefix.shape[1]):
        logits = decoder.decoder.session([prefix[:, :length], mask[:, :length], context])[0][0, -1, :vocab]
        if delta is not None:
            logits = logits + delta[prefix[0, length - 1]]
        predicted = np.argmax(logits)
        if score:
            loss += float(np.log(np.exp(logits - logits[predicted]).sum()))
        if predicted == decoder.eos_token:
            tokens = prefix[0, 1:length]
            text = model.post_process(model.tokenizer.token2str(tokens)[0])
            return text, loss / length if score else None
        prefix[0, length] = predicted
    raise ValueError("Math decoder reached its token limit")


def read_formula_view(model, crop, enhance, seconds):
    try:
        with crop_deadline(seconds):
            tight, glyph_height = clean_crop(crop)
            prepared = resize_crop(tight, glyph_height, enhance)
            text, loss = decode_view(model, formula_context(model, prepared), score=True)
            validate_latex(text)
    except (ValueError, TimeoutError, ONNXRuntimeError) as error:
        return {"error": str(error), "loss": None}
    return {"text": text, "loss": loss}


def should_retry(primary, gate):
    return primary["loss"] is None or primary["loss"] >= gate


def prefer_retry(primary, retry, gate, margin):
    if retry["loss"] is None:
        return False
    if primary["loss"] is None:
        return retry["loss"] <= gate
    return retry["loss"] <= primary["loss"] - margin


class FormulaRetryPolicy:
    """Allow one higher-resolution retry within the existing crop deadline."""

    def __init__(self, path, weights):
        policy = json.loads(Path(path).read_text())
        if policy.get("version") != 1 or policy.get("weights") != weight_signature(weights):
            raise ValueError("Math retry policy does not match the installed RapidLaTeXOCR weights")
        self.gate, self.margin = (float(policy[key]) for key in ("gate", "margin"))
        if not np.isfinite([self.gate, self.margin]).all() or min(self.gate, self.margin) < 0:
            raise ValueError("Invalid math retry thresholds")

    def read(self, model, crop, seconds, trace=None):
        started = time.monotonic()
        primary = read_formula_view(model, crop, False, seconds)
        selected = primary
        retry = None
        remaining = seconds - (time.monotonic() - started)
        if remaining > 0 and should_retry(selected, self.gate):
            retry = read_formula_view(model, crop, True, remaining)
            if prefer_retry(selected, retry, self.gate, self.margin):
                selected = retry
        if trace is not None:
            trace.update(attempted=retry is not None, selected=selected is retry,
                         normal_loss=primary["loss"], retry_loss=retry["loss"] if retry is not None else None)
        if "text" in selected:
            return selected["text"]
        raise ValueError(selected["error"])


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
    return resize_crop(crop, glyph_height)


def crop_features(crop, glyph_height):
    pixels = np.asarray(crop)
    fore = pixels < 175
    return np.array([1, np.log2(crop.width / crop.height), np.log2(glyph_height) - 4,
                     float(fore.mean()) - 0.2, float(pixels[fore].mean()) / 255,
                     float(pixels[~fore].mean()) / 255 - 1 if (~fore).any() else 0], dtype=np.float32)


def resize_crop(crop, glyph_height, enhance=False):
    scale = min((80 if enhance else 48) / crop.height, 672 / crop.width)
    if glyph_height * scale < 3:
        raise ValueError("Too many lines in the math crop. Turn off Math only for pages containing prose, "
                         "or select a smaller equation region.")
    size = max(1, round(crop.width * scale)), max(1, round(crop.height * scale))
    if enhance:
        crop = ImageOps.autocontrast(crop, cutoff=1)
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


def recognize(path, boxes=None, time_budget=None, crop_timeout=15, progress=None,
              adapter_path=None, retry_policy_path=None):
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
    weights = local_weights()
    if adapter_path and retry_policy_path:
        raise ValueError("Choose one math adapter or retry policy")
    adapter = DecoderAdapter(adapter_path, weights) if adapter_path else None
    retry_policy = FormulaRetryPolicy(retry_policy_path, weights) if retry_policy_path else None
    model = create_model(weights)
    result = []
    for left, top, right, bottom in boxes:
        entry = dict(box=[left, top, right-left, bottom-top])
        try:
            remaining = time_budget - (time.monotonic()-started) if time_budget is not None else crop_timeout
            if remaining <= 0:
                entry['error'] = 'Formula time budget exhausted'
            elif retry_policy:
                entry["text"] = retry_policy.read(model, gray.crop((left, top, right, bottom)),
                                                  min(crop_timeout, remaining))
            else:
                with crop_deadline(min(crop_timeout, remaining)):
                    crop = gray.crop((left, top, right, bottom))
                    if adapter is None:
                        text, _ = model(np.asarray(prepare_crop(crop).convert("RGB")))
                    else:
                        text = adapter.decode(model, formula_context(model, adapter.prepare(crop)))
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
    engine = parser.add_mutually_exclusive_group()
    engine.add_argument('--adapter')
    engine.add_argument('--retry-policy')
    args = parser.parse_args()
    boxes = json.loads(Path(args.boxes).read_text()) if args.boxes else None
    def progress(done, total): print('YAGF_PROGRESS '+json.dumps(dict(done=done,total=total)),file=sys.stderr,flush=True)
    adapter = args.adapter
    policy = args.retry_policy
    if adapter is None and policy is None:
        adapter = os.environ.get('YAGF_MATH_DECODER_ADAPTER')
        policy = os.environ.get('YAGF_MATH_RETRY_POLICY')
    print(json.dumps(recognize(args.image, boxes, args.time_budget, args.crop_timeout, progress, adapter, policy)))
