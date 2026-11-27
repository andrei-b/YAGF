#!/usr/bin/env python3
"""Fit a conservative formula-crop retry using decoded development examples."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import zipfile

import cv2
from PIL import Image

from benchmark_old_scans_math import write_json
from math_ocr import (create_model, equation_boxes, local_weights, prefer_retry, read_formula_view,
                      should_retry, weight_signature)
from train_math_decoder_adapter import archive_examples, complex_formula, open_example, unwrap_formula


def make_examples(args, archive):
    quotas = {("train", True): 72, ("train", False): 18,
              ("development", True): 27, ("development", False): 12}
    counts = Counter()
    for key, label, _, split in archive_examples(archive, 1000, 250, args.seed):
        stratum = split, complex_formula(label)
        if counts[stratum] >= quotas[stratum]:
            continue
        counts[stratum] += 1
        yield "unimer-" + Path(key).stem, label, "UniMER-1M", split, open_example(
            archive, key, counts[stratum] % 2)
        if counts == quotas:
            break
    if counts != quotas:
        raise ValueError(f"Insufficient UniMER examples for the requested strata: {counts}")

    manifest = json.loads(args.ibem.read_text())
    by_document = defaultdict(list)
    for row in manifest:
        if row.get("dataset") == "IBEM" and row.get("split") in {"train", "dev"} and row.get("latex"):
            by_document[row["article"]].append(row)
    for document, rows in sorted(by_document.items()):
        for row in rows[:5]:
            source = args.ibem.parent / row["image"]
            split = "development" if row["split"] == "dev" else "train"
            yield source.stem, unwrap_formula(row["latex"]), document, split, open_example(archive, source, 0)

    regressions = json.loads(args.regressions.read_text())
    for row in regressions["cases"]:
        if not row.get("expected_latex"):
            continue
        context = regressions["contexts"][row["context"]]
        book = regressions["sources"][context["source"]]["title"]
        split = "development" if "klein" in book.lower() else "train"
        source = args.regressions.parent / row["image"]
        yield "reviewed-" + row["id"], row["expected_latex"], book, split, open_example(archive, source, 0)

    with Image.open(args.demo) as image:
        gray = image.convert("L")
    labels = [r"x^{2}+1=10", r"\frac{1}{2}+\frac{1}{4}=\frac{3}{4}"]
    for index, (box, label) in enumerate(zip(equation_boxes(gray), labels, strict=True)):
        yield f"demo-{index}", label, "YAGF demo", "clean", gray.crop(box)


def measure(args):
    cv2.setNumThreads(1)
    root = args.output
    images = root / "images"
    images.mkdir(parents=True, exist_ok=True)
    weights = local_weights()
    model = create_model(weights)
    records, normal, enlarged, rule_lines = {}, {}, {}, []
    with zipfile.ZipFile(args.archive) as archive:
        for key, label, source, split, image in make_examples(args, archive):
            if key in records:
                raise ValueError(f"Repeated training ID {key}")
            box = [0, 0, *image.size]
            image.save(images / f"{key}.png")
            readings = [read_formula_view(model, image, enhance, args.timeout) for enhance in (False, True)]
            records[key] = {"source": source, "split": split, "normal": readings[0], "enlarged": readings[1]}
            for output, reading in zip((normal, enlarged), readings, strict=True):
                result = {"box": box}
                if "text" in reading:
                    result["latex"] = reading["text"]
                output[key] = [result]
            rule_lines.append(json.dumps({"id": key, "pdf": f"{key}.pdf", "page": 1, "type": "math",
                                          "math": label}))
            if len(records) % 20 == 0:
                print(f"Read two views on {len(records)} development and training crops", flush=True)
    write_json(root / "views.json", {"weights": weight_signature(weights), "records": records})
    write_json(root / "normal.json", normal)
    write_json(root / "enlarged.json", enlarged)
    (root / "rules.jsonl").write_text("\n".join(rule_lines) + "\n")
    print(dict(Counter(row["split"] for row in records.values())), flush=True)


def summarize(records, scores, gate, margin):
    counts = defaultdict(Counter)
    groups = defaultdict(Counter)
    for key, row in records.items():
        primary, retry = row["normal"], row["enlarged"]
        passed = scores[key]["passed"]
        tried = should_retry(primary, gate)
        switched = tried and prefer_retry(primary, retry, gate, margin)
        before, after = passed["normal"], passed["enlarged" if switched else "normal"]
        group = "UniMER-1M" if row["source"] == "UniMER-1M" else "reviewed"
        for subtotal in (counts[row["split"]], groups[(row["split"], group)]):
            subtotal.update(examples=1, original=int(before), selected=int(after), retries=int(tried),
                            gained=int(after and not before), lost=int(before and not after))
    return counts, groups


def objective(groups, split):
    scores = [(x["gained"] - 2 * x["lost"] - 0.02 * x["retries"]) / x["examples"]
              for (key, _), x in groups.items() if key == split]
    return sum(scores) / len(scores)


def fit(args):
    views = json.loads(args.views.read_text())
    scored = json.loads(args.scores.read_text())["rules"]
    inputs = {key: views["records"][key] for key in scored}
    if not {"train", "development", "clean"} <= {r["split"] for r in inputs.values()}:
        raise ValueError("Training, development and clean-case scores must cover the measured views")
    available_ibem = {row["split"] for key, row in inputs.items() if key.startswith("ibem-")}
    missing_ibem = {row["split"] for key, row in views["records"].items() if key.startswith("ibem-")} - available_ibem
    if missing_ibem:
        raise ValueError(f"All IBEM formula references were skipped in {', '.join(sorted(missing_ibem))}")
    candidates = []
    for gate in (0.15, 0.25, 0.40, 0.55, 0.75, 1.0, 1.25):
        for margin in (0, 0.04, 0.08, 0.14, 0.22):
            counts, groups = summarize(inputs, scored, gate, margin)
            if counts["clean"]["selected"] != counts["clean"]["examples"]:
                continue
            candidates.append({"gate": gate, "margin": margin, "counts": counts,
                               "fit": objective(groups, "train"), "development": objective(groups, "development")})
    if not candidates:
        raise ValueError("No retry setting preserved the clean demo")
    candidates.sort(key=lambda candidate: candidate["fit"], reverse=True)
    best = max(candidates[:10], key=lambda candidate: (candidate["development"], candidate["fit"]))
    policy = {"version": 1, "weights": views["weights"], "gate": best["gate"], "margin": best["margin"]}
    write_json(args.output, policy)
    write_json(args.output.with_suffix(".metrics.json"), {"selected": best, "candidates": candidates})
    print(json.dumps(best, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    collect = commands.add_parser("measure")
    for name in ("archive", "ibem", "regressions", "demo", "output"):
        collect.add_argument(name, type=Path)
    collect.add_argument("--seed", type=int, default=9017)
    collect.add_argument("--timeout", type=float, default=8)
    train = commands.add_parser("fit")
    for name in ("views", "scores", "output"):
        train.add_argument(name, type=Path)
    args = parser.parse_args()
    (measure if args.action == "measure" else fit)(args)


if __name__ == "__main__":
    main()
