#!/usr/bin/env python3
"""Export formula regions from the PDF-Extract-Kit formula detector."""
import argparse
import hashlib
import math
from pathlib import Path
import time

from benchmark_old_scans_math import write_json


def image_box(coordinates, width, height):
    x, y, right, bottom = coordinates
    return [max(0, math.floor(x)), max(0, math.floor(y)),
            min(width, math.ceil(right)), min(height, math.ceil(bottom))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("weights", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("images", type=Path, nargs="+")
    parser.add_argument("--details", type=Path)
    parser.add_argument("--sha256", required=True, help="Expected checkpoint digest from its publisher")
    parser.add_argument("--image-size", type=int, default=1280)
    parser.add_argument("--confidence", type=float, default=0.25)
    parser.add_argument("--iou", type=float, default=0.45)
    args = parser.parse_args()
    if len({path.stem for path in args.images}) != len(args.images):
        parser.error("Page image basenames must be distinct")
    with args.weights.open("rb") as weight_file:
        checked = hashlib.file_digest(weight_file, "sha256").hexdigest()
    if checked != args.sha256:
        raise ValueError("Formula detector checkpoint digest does not match")

    import torch
    from ultralytics import YOLO
    torch.set_num_threads(2)
    torch.set_num_interop_threads(1)
    model = YOLO(str(args.weights), task="detect")
    regions, details = {}, {}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.details:
        args.details.parent.mkdir(parents=True, exist_ok=True)
    for path in sorted(args.images):
        start = time.monotonic()
        result = model.predict(str(path), imgsz=args.image_size, conf=args.confidence, iou=args.iou,
                               device="cpu", verbose=False)[0]
        height, width = result.orig_shape
        by_box = {}
        for xyxy, confidence, cls in zip(result.boxes.xyxy.tolist(), result.boxes.conf.tolist(),
                                         result.boxes.cls.tolist(), strict=True):
            box = image_box(xyxy, width, height)
            key = tuple(box)
            if box[0] < box[2] and box[1] < box[3] and (
                key not in by_box or confidence > by_box[key]["confidence"]
            ):
                by_box[key] = {"box": box, "label": result.names[int(cls)], "confidence": confidence}
        candidates = list(by_box.values())
        candidates.sort(key=lambda row: (row["box"][1] + row["box"][3], row["box"][0]))
        details[path.stem] = candidates
        regions[path.stem] = [row["box"] for row in candidates]
        write_json(args.output, regions)
        if args.details:
            write_json(args.details, details)
        print(f"{path.stem}: {len(candidates)} formula regions in {time.monotonic() - start:.2f}s", flush=True)


if __name__ == "__main__":
    main()
