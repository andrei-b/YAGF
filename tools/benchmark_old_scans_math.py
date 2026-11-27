#!/usr/bin/env python3
"""Compare math backends with the olmOCR-Bench Old Scans Math page rules."""
import argparse
from collections import defaultdict
import importlib.util
import json
from pathlib import Path
import sys
from unittest.mock import patch
from urllib.parse import urlparse
import weakref


def write_json(path, data):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")
    temporary.replace(path)


def recognize(args):
    import cv2
    import numpy as np
    from PIL import Image
    from rapid_latex_ocr.utils_load import ONNXRuntimeError
    from math_ocr import (DecoderAdapter, FormulaRetryPolicy, create_model, crop_deadline, formula_context,
                          local_weights, prepare_crop, validate_latex)

    cv2.setNumThreads(1)
    weights = local_weights()
    adapter = DecoderAdapter(args.adapter, weights) if args.adapter else None
    retry_policy = FormulaRetryPolicy(args.retry_policy, weights) if args.retry_policy else None
    model = create_model(weights)
    boxes_by_page = {page: [list(box) for box in dict.fromkeys(map(tuple, boxes))]
                     for page, boxes in json.loads(args.boxes.read_text()).items()}
    predictions = json.loads(args.output.read_text()) if args.resume and args.output.exists() else {}
    for page, readings in predictions.items():
        if [row["box"] for row in readings] != boxes_by_page.get(page):
            raise ValueError(f"Resume file has mismatched formula boxes on {page}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for page, boxes in sorted(boxes_by_page.items()):
        if page in predictions:
            continue
        with Image.open(args.images / f"{page}.png") as source:
            image = source.convert("L")
        readings = []
        for box in boxes:
            left, top, right, bottom = box
            if not (0 <= left < right <= image.width and 0 <= top < bottom <= image.height):
                raise ValueError(f"Cannot apply crop {box} to {page} at {image.size}")
            result = {"box": box}
            try:
                crop = image.crop(box)
                if retry_policy:
                    result["retry"] = {}
                    text = retry_policy.read(model, crop, args.crop_timeout, result["retry"])
                else:
                    with crop_deadline(args.crop_timeout):
                        if adapter is None or args.stock_decoder:
                            prepared = adapter.prepare(crop) if adapter else prepare_crop(crop)
                            text, _ = model(np.asarray(prepared.convert("RGB")))
                        else:
                            text = adapter.decode(model, formula_context(model, adapter.prepare(crop)))
                validate_latex(text)
                result["latex"] = text
            except (ValueError, TimeoutError, ONNXRuntimeError) as error:
                result["error"] = str(error)
            readings.append(result)
        predictions[page] = readings
        write_json(args.output, predictions)
        print(f"{page}: {sum('latex' in row for row in readings)}/{len(readings)} formulas", flush=True)


def official_renderer(path, cache_root, chrome):
    spec = importlib.util.spec_from_file_location("olmocr_benchmark_render", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    with patch.object(Path, "home", return_value=cache_root):
        spec.loader.exec_module(module)
    if chrome:
        class SystemBrowser:
            def __init__(self):
                self.playwright = module.sync_playwright().start()
                try:
                    self.browser = self.playwright.chromium.launch(executable_path=str(chrome))
                except Exception:
                    self.playwright.stop()
                    raise
                self.finalizer = weakref.finalize(self, module._cleanup_playwright, self.playwright, self.browser)
        module._BrowserOwner = SystemBrowser
    return module


def rendered_successfully(rendering):
    return rendering is not None and not rendering.error


def check_prediction_boxes(predictions, expected_specs):
    expected = {}
    for spec in expected_specs:
        name, separator, path = spec.partition("=")
        if not separator or name in expected:
            raise ValueError("Give one --boxes CONDITION=PATH for each prediction condition")
        expected[name] = json.loads(Path(path).read_text())
    if expected and expected.keys() != predictions.keys():
        raise ValueError("Expected box manifests must cover all prediction conditions")
    first_prediction = next(iter(predictions.values()))
    for name, prediction in predictions.items():
        if expected and prediction.keys() != expected[name].keys():
            raise ValueError(f"{name} predictions and box manifests must cover the same pages")
        for page, rows in prediction.items():
            boxes = expected[name][page] if expected else [row["box"] for row in first_prediction[page]]
            if [row["box"] for row in rows] != boxes:
                raise ValueError(f"{name} predictions used different formula boxes on {page}")


def score(args):
    rows = [json.loads(line) for line in args.rules.read_text().splitlines() if line.strip()]
    if not rows or any(row["type"] != "math" or row["page"] != 1 for row in rows):
        raise ValueError("Expected the official Old Scans Math single-page formula rules")
    rules_by_page = defaultdict(list)
    for row in rows:
        rules_by_page[Path(row["pdf"]).stem].append(row)
    input_predictions = {path.stem: json.loads(path.read_text()) for path in args.predictions}
    if len(input_predictions) != len(args.predictions):
        raise ValueError("Prediction file stems must be distinct")
    for name, prediction in input_predictions.items():
        if prediction.keys() != rules_by_page.keys():
            raise ValueError(f"{name} predictions must cover all pages in the rule set")
    check_prediction_boxes(input_predictions, args.boxes)
    renderer = official_renderer(args.renderer, args.output.parent, args.chrome)
    tested = {}
    skipped = []
    from concurrent.futures import ThreadPoolExecutor
    for page, rules in rules_by_page.items():
        page_predictions = {name: {row["latex"] for row in prediction[page] if "latex" in row}
                            for name, prediction in input_predictions.items()}
        all_latex = sorted({row["math"] for row in rules}.union(*page_predictions.values()))
        with ThreadPoolExecutor(max_workers=args.render_workers) as pool:
            rendered = dict(zip(all_latex, pool.map(renderer.render_equation, all_latex), strict=True))
        for row in rules:
            reference = row["math"]
            if not rendered_successfully(rendered[reference]):
                if args.skip_unrenderable:
                    skipped.append(row["id"])
                    continue
                raise ValueError(f"Official reference did not render for {row['id']}")
            measures = {}
            for name, hypotheses in page_predictions.items():
                measures[name] = reference in hypotheses or any(
                    renderer.compare_rendered_equations(rendered[reference], rendered[hypothesis]) and
                    (not args.strict or renderer.compare_rendered_equations(rendered[hypothesis], rendered[reference]))
                    for hypothesis in sorted(hypotheses) if rendered_successfully(rendered[hypothesis]))
            book = row.get("book")
            if not book:
                book = urlparse(row["url"]).path.split("/")[2] if row.get("url") else "Unknown"
            tested[row["id"]] = {"book": book, "page": page, "passed": measures}
        print(f"{page}: scored {len(rules)} rules", flush=True)
    if not tested:
        raise ValueError("No reference formula could be scored")
    summary = {}
    for name in input_predictions:
        rates_per_page = []
        for page in rules_by_page:
            checks = [row["passed"][name] for row in tested.values() if row["page"] == page]
            if checks:
                rates_per_page.append(sum(checks) / len(checks))
        summary[name] = {
            "passed": sum(row["passed"][name] for row in tested.values()),
            "tests": len(tested), "pages": len(rates_per_page),
            "mean_page_rate": sum(rates_per_page) / len(rates_per_page),
        }
    write_json(args.output, {"summary": summary, "rules": tested, "unrenderable_references": skipped})
    print(json.dumps(summary, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    predict = commands.add_parser("predict")
    predict.add_argument("images", type=Path)
    predict.add_argument("boxes", type=Path)
    predict.add_argument("output", type=Path)
    engine = predict.add_mutually_exclusive_group()
    engine.add_argument("--adapter", type=Path)
    engine.add_argument("--retry-policy", type=Path)
    predict.add_argument("--stock-decoder", action="store_true", help="Use an adapter's crop rules with the original decoder")
    predict.add_argument("--crop-timeout", type=float, default=15)
    predict.add_argument("--resume", action="store_true", help="Reuse completed pages from the same model run")
    evaluate = commands.add_parser("score")
    evaluate.add_argument("rules", type=Path)
    evaluate.add_argument("renderer", type=Path)
    evaluate.add_argument("output", type=Path)
    evaluate.add_argument("predictions", type=Path, nargs="+")
    evaluate.add_argument("--chrome", type=Path)
    evaluate.add_argument("--boxes", action="append", default=[], metavar="CONDITION=PATH",
                          help="Check each condition against its frozen box manifest")
    evaluate.add_argument("--render-workers", type=int, default=4)
    evaluate.add_argument("--strict", action="store_true", help="Require equivalence in both directions for verified crops")
    evaluate.add_argument("--skip-unrenderable", action="store_true", help="Skip unrenderable non-benchmark references")
    args = parser.parse_args()
    (recognize if args.command == "predict" else score)(args)


if __name__ == "__main__":
    main()
