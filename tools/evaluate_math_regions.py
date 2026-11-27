#!/usr/bin/env python3
"""Compare frozen formula proposals against reviewed math and prose boxes."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
from urllib.parse import urlparse

import cv2
import numpy as np
from PIL import Image

from audit_old_scans_math import covering_ink
from benchmark_old_scans_math import write_json


def area(box):
    return (box[2] - box[0]) * (box[3] - box[1])


def overlap(first, second):
    return (max(0, min(first[2], second[2]) - max(first[0], second[0])) *
            max(0, min(first[3], second[3]) - max(first[1], second[1])))


def formula_ink(path):
    with Image.open(path) as image:
        gray = np.asarray(image.convert("L"))
    _, binary = cv2.threshold(gray, 0, 1, cv2.THRESH_BINARY_INV | cv2.THRESH_OTSU)
    return binary.astype(bool)


def measure_formula(ink, box, regions):
    ink_count = int(ink.sum())
    if not ink_count:
        raise ValueError("A verified formula has no ink")
    covered = [covering_ink(ink, box, region) for region in regions]
    coverage = [float(candidate.sum() / ink_count) for candidate in covered]
    ious = []
    for region in regions:
        shared = overlap(box, region)
        ious.append(shared / (area(box) + area(region) - shared))
    best = max(range(len(regions)), key=lambda index: (coverage[index], ious[index]), default=None)
    union = float(np.logical_or.reduce(covered).sum() / ink_count) if covered else 0
    tight = any(ratio >= 0.95 and iou >= 0.5 for ratio, iou in zip(coverage, ious, strict=True))
    return {"best_ink_cover": coverage[best] if best is not None else 0, "best_region_index": best,
            "union_ink_cover": union, "tight_complete": tight}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("audit", "rules", "prose", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--detector", action="append", required=True, metavar="NAME=BOXES_JSON")
    args = parser.parse_args()

    proposals = {}
    for spec in args.detector:
        name, sep, path = spec.partition("=")
        if not sep or name in proposals:
            parser.error("Give a distinct NAME=BOXES_JSON for each detector")
        proposals[name] = json.loads(Path(path).read_text())
    ground = json.loads((args.audit / "manifest.json").read_text())
    references = {row["id"]: row for line in args.rules.read_text().splitlines() if line.strip()
                  for row in [json.loads(line)]}
    formulas = {}
    controls = {}
    summary = {name: {"placement": Counter(), "per_book": defaultdict(Counter), "prose": Counter()}
               for name in proposals}
    for key, row in ground.items():
        reference = references[key]
        latex = reference["math"]
        book = reference.get("book")
        if not book:
            book = urlparse(reference["url"]).path.split("/")[2]
        box = row["page_box"]
        ink = formula_ink(args.audit / row["image"])
        long = len(latex) >= 60
        groups = ["all"]
        if long:
            groups.append("long")
        if r"\frac" in latex:
            groups.append("fractions")
        if "matrix}" in latex:
            groups.append("matrices")
        if r"\\" in latex:
            groups.append("multiline")
        scores = {name: measure_formula(ink, box, by_page[row["page"]]) for name, by_page in proposals.items()}
        formulas[key] = {"page": row["page"], "book": book, "groups": groups, "scores": scores}
        for name, result in scores.items():
            for group in groups:
                summary[name]["placement"].update({f"{group}_count": 1,
                    f"{group}_complete": int(result["best_ink_cover"] >= 0.95),
                    f"{group}_tight_complete": int(result["tight_complete"])})
            summary[name]["per_book"][book].update(count=1, complete=int(result["best_ink_cover"] >= 0.95))
    prose_hits = defaultdict(set)
    for line in args.prose.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        key, page, *coords = line.split()
        region = tuple(map(int, coords))
        scores = {}
        for name, by_page in proposals.items():
            hits = [index for index, candidate in enumerate(by_page[page])
                    if overlap(region, candidate) / area(candidate) >= 0.5]
            scores[name] = hits
            prose_hits[name].update((page, index) for index in hits)
            summary[name]["prose"].update(regions=1, regions_with_hits=int(bool(hits)))
        controls[key] = {"page": page, "box": region, "overlaps": scores}
    for name in proposals:
        summary[name]["prose"]["distinct_hits"] = len(prose_hits[name])
    write_json(args.output, {"summary": summary, "formulas": formulas, "prose": controls})
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
