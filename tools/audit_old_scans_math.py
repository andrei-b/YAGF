#!/usr/bin/env python3
"""Build a formula-crop benchmark from independently reviewed page boxes."""
import argparse
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image


def covering_ink(ink, box, proposal):
    x, y, right, bottom = box
    px, py, pright, pbottom = proposal
    intersection = np.zeros_like(ink)
    a, b = max(x, px), min(right, pright)
    c, d = max(y, py), min(bottom, pbottom)
    if a < b and c < d:
        intersection[c-y:d-y, a-x:b-x] = True
    return ink & intersection


def prepare(rules, verified_boxes, images, proposals, output):
    references = {row["id"]: row for line in rules.read_text().splitlines() if line.strip()
                  for row in [json.loads(line)]}
    layout = json.loads(proposals.read_text())
    manifest = {}
    crop_rules = []
    boxes = {}
    crop_dir = output / "images"
    crop_dir.mkdir(parents=True, exist_ok=True)
    for line in verified_boxes.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        key, *coordinates = line.split()
        if key in manifest:
            raise ValueError(f"Repeated verified box {key}")
        box = tuple(map(int, coordinates))
        reference = references[key]
        page = Path(reference["pdf"]).stem
        path = crop_dir / f"{key}.png"
        with Image.open(images / f"{page}.png") as image:
            x, y, right, bottom = box
            if not (0 <= x < right <= image.width and 0 <= y < bottom <= image.height):
                raise ValueError(f"Verified box outside the page for {key}: {box}")
            crop = image.crop(box).convert("L")
            crop.save(path)
        pixels = np.asarray(crop)
        _, raw_ink = cv2.threshold(pixels, 0, 1, cv2.THRESH_BINARY_INV | cv2.THRESH_OTSU)
        ink = raw_ink.astype(bool)
        ink_count = int(ink.sum())
        if not ink_count:
            raise ValueError(f"Empty verified crop {key}")
        covered = [covering_ink(ink, box, region) for region in layout[page]]
        best_fraction = max((int(mask.sum()) / ink_count for mask in covered), default=0)
        union_fraction = float(np.logical_or.reduce(covered).sum() / ink_count) if covered else 0
        manifest[key] = {"page": page, "page_box": box, "image": str(path.relative_to(output)),
                         "length": len(reference["math"]), "best_ink_cover": best_fraction,
                         "union_ink_cover": union_fraction}
        boxes[key] = [[0, 0, crop.width, crop.height]]
        crop_rules.append({**reference, "pdf": f"{key}.pdf", "source_pdf": reference["pdf"]})
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (output / "boxes.json").write_text(json.dumps(boxes, indent=2) + "\n")
    (output / "rules.jsonl").write_text("".join(json.dumps(row) + "\n" for row in crop_rules))
    print(f"Prepared {len(manifest)} verified crops from {len({row['page'] for row in manifest.values()})} pages")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["rules", "verified_boxes", "images", "proposals", "output"]:
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    prepare(args.rules, args.verified_boxes, args.images, args.proposals, args.output)
