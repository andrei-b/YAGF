"""Detect page formulas with PDF-Extract-Kit and transcribe their source crops."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import time

import numpy as np
from PIL import Image
import torch


def detect(path):
    from ultralytics import YOLO
    weights = Path(os.environ["YAGF_MFD_WEIGHTS"])
    with weights.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != "41029d5abb9b0b6df825ecaf8adf9151762ea0688ac0fc4ea0ea34ab9a5808fc":
            raise ValueError("Unexpected PDF-Extract-Kit detector checkpoint")
    detector = YOLO(str(weights), task="detect")
    result = detector.predict(str(path), imgsz=1280, conf=0.25, iou=0.45, device="cpu", verbose=False)[0]
    height, width = result.orig_shape
    regions = {}
    for xyxy, label in zip(result.boxes.xyxy.tolist(), result.boxes.cls.tolist(), strict=True):
        x, y, r, b = xyxy
        box = max(0, math.floor(x)), max(0, math.floor(y)), min(width, math.ceil(r)), min(height, math.ceil(b))
        if box[2] > box[0] and box[3] > box[1]:
            regions.setdefault(box, result.names[int(label)])
    return sorted(regions.items(), key=lambda item: (item[0][1]+item[0][3], item[0][0]))


def decode(model, views, timeout, max_tokens=256):
    from transformers import VisionEncoderDecoderModel
    pixels = torch.stack(views)
    if pixels.shape[1] == 1:
        pixels = pixels.repeat(1, 3, 1, 1)
    tokens = VisionEncoderDecoderModel.generate(
        model, pixel_values=pixels, do_sample=False, max_new_tokens=max_tokens,
        max_time=timeout, decoder_start_token_id=model.tokenizer.bos_token_id)
    decoded = model._decode_generate_outputs(tokens, return_full_result=False)["fixed_str"]
    return [latex.strip() if model.tokenizer.eos_token_id in ids[1:].tolist() else ""
            for ids, latex in zip(tokens, decoded, strict=True)]


def recognize(args):
    sys.path.insert(0, str(Path(os.environ["YAGF_UNIMERNET_SOURCE"]).resolve()))
    from mineru.model.mfr.unimernet.unimernet_hf import UnimernetModel
    deadline = time.monotonic() + args.time_budget
    torch.set_num_threads(2)
    torch.set_num_interop_threads(1)
    regions = ([(box, "isolated") for box in json.loads(Path(args.boxes).read_text())]
               if args.boxes else detect(args.image))
    if not regions:
        return []
    model = UnimernetModel.from_pretrained(os.environ["YAGF_UNIMERNET_WEIGHTS"], local_files_only=True).eval()
    rows = []
    with Image.open(args.image) as image, torch.inference_mode():
        for start in range(0, len(regions), 3):
            batch = regions[start:start+3]
            entries = []
            views = []
            for (x, y, r, b), label in batch:
                if not (0 <= x < r <= image.width and 0 <= y < b <= image.height):
                    raise ValueError("Invalid formula crop")
                entries.append(dict(box=[x, y, r-x, b-y], label=label))
                views.append(model.transform(np.asarray(image.crop((x, y, r, b)).convert("RGB"))))
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                for entry in entries:
                    entry["error"] = "Formula time budget exhausted"
            else:
                try:
                    readings = decode(model, views, min(remaining, args.crop_timeout))
                    for entry, latex in zip(entries, readings, strict=True):
                        if latex:
                            entry["text"] = latex
                        else:
                            entry["error"] = "UniMERNet returned no complete formula"
                except (RuntimeError, ValueError) as error:
                    for entry in entries:
                        entry["error"] = str(error)
            rows.extend(entries)
            print("YAGF_PROGRESS "+json.dumps(dict(done=len(rows), total=len(regions))), file=sys.stderr, flush=True)
    return rows


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("boxes", nargs="?")
    parser.add_argument("--time-budget", type=float, default=300)
    parser.add_argument("--crop-timeout", type=float, default=40)
    # Model libraries occasionally print startup diagnostics to stdout.
    from contextlib import redirect_stdout
    with redirect_stdout(sys.stderr):
        results = recognize(parser.parse_args())
    print(json.dumps(results))
