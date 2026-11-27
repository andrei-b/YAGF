#!/usr/bin/env python3
"""Read a fixed formula-crop manifest with OpenDataLab's UniMERNet implementation."""
import argparse
import json
from pathlib import Path
import sys
import time

from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["source", "weights", "images", "boxes", "output"]:
        parser.add_argument(name, type=Path)
    parser.add_argument("--max-tokens", type=int, default=256)
    parser.add_argument("--crop-timeout", type=float, default=25)
    parser.add_argument("--batch-size", type=int, default=1)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    if args.batch_size < 1:
        parser.error("--batch-size must be positive")

    boxes_by_page = json.loads(args.boxes.read_text())
    predictions = json.loads(args.output.read_text()) if args.resume and args.output.exists() else {}
    for page, rows in predictions.items():
        if [row["box"] for row in rows] != boxes_by_page.get(page):
            raise ValueError(f"Saved boxes do not match {page}")

    import numpy as np
    import torch
    from unimernet_ocr import decode
    sys.path.insert(0, str(args.source.resolve()))
    from mineru.model.mfr.unimernet.unimernet_hf import UnimernetModel

    torch.set_num_threads(2)
    torch.set_num_interop_threads(1)
    model = UnimernetModel.from_pretrained(str(args.weights), local_files_only=True).eval()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for key, boxes in sorted(boxes_by_page.items()):
        if key in predictions:
            continue
        start = time.monotonic()
        readings = []
        with Image.open(args.images / f"{key}.png") as img:
            for left, top, right, bottom in boxes:
                if not (0 <= left < right <= img.width and 0 <= top < bottom <= img.height):
                    raise ValueError(f"Formula box outside page image {key}")
            for index in range(0, len(boxes), args.batch_size):
                batch = boxes[index:index+args.batch_size]
                views = [model.transform(np.asarray(img.crop(box).convert("RGB"))) for box in batch]
                with torch.inference_mode():
                    decoded = decode(model, views, args.crop_timeout, args.max_tokens)
                for box, latex in zip(batch, decoded, strict=True):
                    row = {"box": box}
                    if latex:
                        row["latex"] = latex
                    else:
                        row["error"] = "UniMERNet returned no complete formula"
                    readings.append(row)
        predictions[key] = readings
        temporary = args.output.with_suffix(".tmp")
        temporary.write_text(json.dumps(predictions, indent=2) + "\n")
        temporary.replace(args.output)
        print(f"{key}: {time.monotonic() - start:.2f}s", flush=True)


if __name__ == "__main__":
    main()
