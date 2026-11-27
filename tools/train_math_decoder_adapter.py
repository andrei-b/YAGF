#!/usr/bin/env python3
"""Fit a bounded RapidLaTeXOCR output adapter from labeled formula images."""
import argparse
from collections import Counter
import hashlib
import io
import json
from pathlib import Path
import random
import re
import zipfile

import cv2
import numpy as np
from PIL import Image, ImageEnhance, ImageFilter, ImageOps

from math_ocr import (clean_crop, create_model, crop_features, formula_context, local_weights,
                      prepare_crop, resize_crop, validate_latex, weight_signature)


def reference_key(label):
    return re.sub(r"(?<=[{}^_]) | (?=[{}^_])", "", " ".join(label.split()))


def unwrap_formula(label):
    text = label.strip()
    for opening, closing in (("$$", "$$"), ("$", "$"), (r"\(", r"\)"), (r"\[", r"\]")):
        if len(text) > len(opening) + len(closing) and text.startswith(opening) and text.endswith(closing):
            return text[len(opening):-len(closing)].strip()
    return text


def complex_formula(label):
    return len(label) >= 60 or label.count(r"\frac") >= 2 or bool(re.search(
        r"\\(?:sum|lim|begin\s*\{(?:align|array|cases|[bBpPvV]?matrix))", label))


def focused_examples(archive, count, dev_count, seed):
    quotas = {}
    for split, size in (("train", count), ("development", dev_count)):
        complex_count = round(size * 0.8)
        quotas[split, True] = complex_count
        quotas[split, False] = size - complex_count
    selected = Counter()
    for row in archive_examples(archive, 3 * count, 3 * dev_count, seed):
        stratum = row[3], complex_formula(row[1])
        if selected[stratum] < quotas[stratum]:
            selected[stratum] += 1
            yield row
            if selected == quotas:
                return
    raise ValueError(f"Could not fill focused UniMER-1M strata: {selected}")


def archive_examples(archive, count, dev_count, seed):
    names = archive.namelist()
    text_members = [name for name in names if name.lower().endswith((".txt", ".jsonl", ".tsv"))]
    if len(text_members) != 1:
        raise ValueError(f"Expected a UniMER-1M label file, found {text_members}")
    label_path = text_members[0]
    label_root = Path(label_path).parent
    image_paths = {Path(name).stem: name for name in names
                   if name.startswith(f"{label_root}/images/") and name.endswith(".png")}
    samples = []
    seen = set()
    with archive.open(label_path) as labels:
        for index, raw in enumerate(labels):
            key = f"{index:07d}"
            latex = raw.decode("utf-8").strip()
            if key in image_paths and latex:
                normalized = reference_key(latex)
                if normalized not in seen:
                    seen.add(normalized)
                    samples.append((image_paths[key], latex))
    if len(samples) < count + dev_count:
        raise ValueError(f"Found only {len(samples)} distinct labeled UniMER-1M images")
    random.Random(seed).shuffle(samples)
    for index, (key, latex) in enumerate(samples[:count + dev_count]):
        split = "development" if index < dev_count else "train"
        yield key, latex, "UniMER-1M", split


def previous_examples(regressions, ibem):
    if regressions:
        manifest = json.loads(regressions.read_text())
        for case in manifest["cases"]:
            if case.get("expected_latex"):
                yield regressions.parent / case["image"], case["expected_latex"], "YAGF regression crops", "train"
    if ibem:
        for case in json.loads(ibem.read_text()):
            if case.get("dataset") == "IBEM" and case.get("latex"):
                split = {"train": "train", "dev": "development"}.get(case["split"])
                if split:
                    yield ibem.parent / case["image"], unwrap_formula(case["latex"]), "IBEM", split


def grayscale_image(image, mode):
    image = ImageOps.exif_transpose(image).convert("RGBA")
    white = Image.new("RGBA", image.size, "white")
    image = Image.alpha_composite(white, image).convert("L")
    if mode == 1:
        # Reduce resolution and contrast without changing the target formula.
        small = tuple(max(1, round(0.73 * size)) for size in image.size)
        image = image.resize(small, Image.Resampling.LANCZOS).resize(image.size, Image.Resampling.BICUBIC)
        image = ImageEnhance.Contrast(image.filter(ImageFilter.GaussianBlur(0.4))).enhance(0.76)
    return image


def open_example(archive, key, mode):
    source = io.BytesIO(archive.read(key)) if isinstance(key, str) else key
    with Image.open(source) as image:
        return grayscale_image(image, mode)


def token_sequence(model, label):
    tokenizer = model.tokenizer.tokenizer
    decoder = model.encoder_decoder
    ids = tokenizer.encode(label, add_special_tokens=False).ids
    if not ids or any(token < 3 for token in ids) or len(ids) >= decoder.max_seq_len:
        raise ValueError("Reference cannot be represented by the decoder")
    if len(ids) > 160:
        raise ValueError("Reference exceeds the training token limit")
    validate_latex(label)
    return np.array([decoder.bos_token, *ids, decoder.eos_token], dtype=np.int64)


def extract_examples(archive, records, model, seed):
    decoder = model.encoder_decoder
    vocab = model.tokenizer.tokenizer.get_vocab_size()
    stats = Counter()
    for index, (key, label, source, split) in enumerate(records):
        try:
            tokens = token_sequence(model, label)
            mode = 0
            if source == "UniMER-1M":
                mode = int.from_bytes(hashlib.blake2s(f"{seed}:{key}".encode(), digest_size=2).digest()) % 2
            crop = prepare_crop(open_example(archive, key, mode))
        except (OSError, ValueError):
            stats[source, split, "skipped"] += 1
            continue
        context = formula_context(model, crop)
        prefix = tokens[:-1][None]
        logits = decoder.decoder.session([prefix, np.ones_like(prefix, dtype=bool), context])[0][0, :, :vocab]
        target = tokens[1:, None]
        # Retain the target and its strongest competitors. A grossly
        # unsupported reference token has little evidence for a local update.
        top = np.argpartition(logits, -12, axis=1)[:, -12:]
        top = np.where(top == target, 0, top)
        candidates = np.concatenate((target, top), axis=1)
        values = np.take_along_axis(logits, candidates, axis=1)
        values[candidates == 0] = -1e4
        values -= values[:, 1:].max(axis=1, keepdims=True).clip(min=-1e3)
        use = values[:, 0] >= -8
        if use.any():
            stats[source, split, "used"] += 1
            yield {"prefix": tokens[:-1][use].astype(np.int16),
                   "candidate": candidates[use].astype(np.int16),
                   "logit": values[use].astype(np.float16), "source": source,
                   "split": split, "image": str(key), "label": label}
        else:
            stats[source, split, "skipped"] += 1
        if (index + 1) % 250 == 0:
            print(f"Prepared {index + 1} formulas", flush=True)
    print({"/".join(key): v for key, v in sorted(stats.items())}, flush=True)


def make_batch(rows, vocab):
    prefix = np.concatenate([row["prefix"] for row in rows]).astype(np.int32)
    candidates = np.concatenate([row["candidate"] for row in rows]).astype(np.int32)
    logits = np.concatenate([row["logit"] for row in rows]).astype(np.float32)
    weights = np.repeat([1.0 if row["source"] == "UniMER-1M" else 3.0 for row in rows],
                        [len(row["prefix"]) for row in rows]).astype(np.float32)
    return candidates, prefix[:, None] * vocab + candidates, logits, weights


def fit(batch, vocab):
    candidates, pairs, logits, weights = batch
    bias = np.zeros(vocab, np.float32)
    context_bias = np.zeros(vocab * vocab, np.float32)
    candidate_count = np.bincount(candidates.ravel(), minlength=vocab)
    pair_count = np.bincount(pairs.ravel(), minlength=vocab * vocab)
    single_rate = (1.0 / (15.0 + candidate_count / 4)).astype(np.float32)
    pair_rate = (1.0 / (7.0 + pair_count / 4)).astype(np.float32)
    for _ in range(32):
        scores = logits + bias[candidates] + context_bias[pairs]
        exp = np.exp(scores - scores.max(axis=1, keepdims=True))
        prob = exp / exp.sum(axis=1, keepdims=True)
        prob[:, 0] -= 1
        gradient = (prob * weights[:, None]).ravel()
        bias_grad = np.bincount(candidates.ravel(), weights=gradient, minlength=vocab)
        pair_grad = np.bincount(pairs.ravel(), weights=gradient, minlength=vocab * vocab)
        bias -= single_rate * (bias_grad + 15 * bias)
        context_bias -= pair_rate * (pair_grad + 7 * context_bias)
    return np.clip(bias + context_bias.reshape(vocab, vocab), -2.0, 2.0)


def evaluate(batch, delta, strength):
    _, pairs, logits, weights = batch
    scores = logits + delta.ravel()[pairs] * strength
    correct = scores.argmax(axis=1) == 0
    loss = np.log(np.exp(scores - scores.max(axis=1, keepdims=True)).sum(axis=1))
    loss += scores.max(axis=1) - scores[:, 0]
    return dict(tokens=len(scores), accuracy=float(np.average(correct, weights=weights)),
                loss=float(np.average(loss, weights=weights)))


def fit_crop_rule(archive, records, model, output):
    decoder = model.encoder_decoder
    vocab = model.tokenizer.tokenizer.get_vocab_size()
    data = []
    selected = Counter()
    for key, label, source, split in records:
        if source == "UniMER-1M":
            selected[split] += 1
            if selected[split] > (260 if split == "train" else 100):
                continue
        try:
            tokens = token_sequence(model, label)
            crop, glyph_height = clean_crop(open_example(archive, key, int(len(data) % 2)
                                                         if source == "UniMER-1M" else 0))
            views = [resize_crop(crop, glyph_height, enhance) for enhance in (False, True)]
        except (OSError, ValueError):
            continue
        prefix = tokens[None, :-1]
        losses = []
        for view in views:
            context = formula_context(model, view)
            logits = decoder.decoder.session([prefix, np.ones_like(prefix, dtype=bool), context])[0][0, :, :vocab]
            logsum = np.log(np.exp(logits - logits.max(axis=1, keepdims=True)).sum(axis=1))
            target = np.take_along_axis(logits, tokens[1:, None], axis=1)[:, 0]
            losses.append(np.minimum(12, logsum + logits.max(axis=1) - target).mean())
        data.append((crop_features(crop, glyph_height), float(losses[0] - losses[1]), source, split))
        if len(data) % 100 == 0:
            print(f"Compared crop views on {len(data)} formulas", flush=True)
    features, changes, sources, splits = map(np.asarray, zip(*data))
    train = splits == "train"
    dev = splits == "development"
    sample_weights = np.sqrt(np.where(sources[train] == "UniMER-1M", 1, 3))
    fit_features = features[train] * sample_weights[:, None]
    fit_changes = changes[train] * sample_weights
    rule = np.linalg.solve(fit_features.T @ fit_features + np.diag([0.001, 3, 3, 3, 3, 3]),
                           fit_features.T @ fit_changes)
    configurations = []
    for margin in (None, 0.16, 0.08, 0.04, 0.0, -0.04):
        enhanced = dev & (features @ rule > margin) if margin is not None else np.zeros(len(dev), bool)
        scores = {source: {"examples": int((dev & (sources == source)).sum()),
                           "enhanced": int((enhanced & (sources == source)).sum()),
                           "loss_reduction": float(np.where(enhanced, changes, 0)[dev & (sources == source)].mean())}
                  for source in sorted(set(sources[dev]))}
        configurations.append({"margin": margin, "development": scores})
    best = max(configurations, key=lambda config: np.mean(
        [stats["loss_reduction"] for stats in config["development"].values()]))
    coefficients = None
    if best["margin"] is not None:
        rule[0] -= best["margin"]
        coefficients = rule.tolist()
    study = {"crop_rule": coefficients, "selected": best, "configurations": configurations}
    (output / "crop-study.json").write_text(json.dumps(study, indent=2) + "\n")
    print(json.dumps(study, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--regressions", type=Path)
    parser.add_argument("--ibem", type=Path)
    parser.add_argument("--train", type=int, default=4000)
    parser.add_argument("--dev", type=int, default=500)
    parser.add_argument("--seed", type=int, default=617)
    parser.add_argument("--fit-crops", action="store_true")
    parser.add_argument("--focus-structures", action="store_true")
    args = parser.parse_args()
    cv2.setNumThreads(1)
    weights = local_weights()
    model = create_model(weights)
    vocab = model.tokenizer.tokenizer.get_vocab_size()
    args.output.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.archive) as archive:
        records = list(previous_examples(args.regressions, args.ibem))
        sample = focused_examples if args.focus_structures else archive_examples
        records.extend(sample(archive, args.train, args.dev, args.seed))
        if args.fit_crops:
            fit_crop_rule(archive, records, model, args.output)
            return
        examples = list(extract_examples(archive, records, model, args.seed))
    manifest = {split: [row for row in examples if row["split"] == split] for split in ("train", "development")}
    train = make_batch(manifest["train"], vocab)
    delta = fit(train, vocab)
    development = {source: make_batch([row for row in manifest["development"] if row["source"] == source], vocab)
                   for source in sorted({row["source"] for row in manifest["development"]})}
    results = {strength: {source: evaluate(batch, delta, strength) for source, batch in development.items()}
               for strength in (0.0, 0.25, 0.5, 0.75, 1.0)}
    strength = min(results, key=lambda weight: np.mean([scores["loss"] for scores in results[weight].values()]))
    fingerprint = weight_signature(weights)
    np.savez_compressed(args.output / "decoder-adapter.npz", version=1, weights=fingerprint,
                        delta=(strength * delta).astype(np.float16))
    report = {"weights": fingerprint, "seed": args.seed, "strength": strength, "development": results,
              "focus_structures": args.focus_structures,
              "examples": [{key: row[key] for key in ("image", "source", "split", "label")} for row in examples]}
    (args.output / "training.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps({key: value for key, value in report.items() if key != "examples"}, indent=2), flush=True)


if __name__ == "__main__":
    main()
