#!/usr/bin/env python3
"""Verify inline-script proposals and choose a rereader using image evidence."""

import argparse
import json
from pathlib import Path
from tempfile import TemporaryDirectory

import cv2
import numpy as np
from PIL import Image
from scipy.optimize import minimize
from scipy.special import expit

from benchmark_gtdb_inline_scripts import ROLES, batch_identity, parse_hocr
from gtdb_inline_scripts import padded_glyph
from inline_script_recovery import ALPHANUMERIC, ScriptCandidates, label, predict


def classifier(vectors, targets, weights, penalty=.02):
    values = np.clip(np.asarray(vectors), -8, 8)
    center = values.mean(0)
    scale = np.maximum(values.std(0), .02)
    x = np.column_stack([(values - center) / scale, np.ones(len(values))])
    targets = np.asarray(targets)
    weights = np.asarray(weights) / sum(weights)

    def objective(params):
        logit = x @ params
        loss = (weights * (np.logaddexp(0, logit) - targets * logit)).sum()
        gradient = x.T @ (weights * (expit(logit) - targets))
        gradient[:-1] += penalty * params[:-1]
        return loss + penalty * np.square(params[:-1]).sum() / 2, gradient

    result = minimize(objective, np.zeros(x.shape[1]), jac=True, method='L-BFGS-B')
    if not result.success:
        raise ValueError(result.message)
    return {'center': center.tolist(), 'scale': scale.tolist(), 'weights': result.x.tolist()}


def probabilities(vectors, model):
    if not len(vectors):
        return []
    x = (np.clip(np.asarray(vectors), -8, 8) - model['center']) / model['scale']
    params = model['weights']
    return expit(x @ params[:-1] + params[-1])


def candidate_vector(item, ocr):
    score, role, anchor = item['proposal']
    index = ROLES.index(role)
    opposite = 3 - index
    rel = next(r for r in item['relations'] if r['base'] == anchor)
    opponents = [r['probabilities'][index] for r in item['relations'] if r['base'] != anchor]
    rival = max(opponents, default=0)
    typical = item.get('features', [0] * 7)
    text = item['text']
    base = ocr[anchor]['text']
    return [*rel['features'], *typical[:7], score, rival, score - rival,
            score - rel['probabilities'][0], max(r['probabilities'][opposite] for r in item['relations']),
            len(opponents) / 3, float(bool(text) and text.isdigit()), float(bool(text) and text.isupper()),
            float(text in ('g','j','p','q','y')), float(text in ('.',',',':',';','-','′','’')),
            float(bool(base) and base.isalnum()), float(item['role'] == role)]


def proposals(items, relation_model):
    predict(items, relation_model)
    label(items, {'decoded': {'sub': 0, 'sup': 0}, 'recovered': {'sub': 0, 'sup': 0}})


def score_items(items, ocr, model):
    for role in ROLES[1:]:
        selected = [item for item in items if item['proposal'] and item['proposal'][1] == role]
        for item, probability in zip(selected,
                probabilities([candidate_vector(item, ocr) for item in selected], model['classifiers'][role]), strict=True):
            item['verifier_score'] = float(probability)


def kind(item):
    return 'decoded' if item['kind'] == 'ocr' else 'recovered'


def decision(item, policy):
    if policy['preserve_native'] and item['role'] != 'base':
        role = item['role']
        possible = item['relations']
        anchor = max(possible, key=lambda r: r['probabilities'][ROLES.index(role)])['base'] if possible else None
        return role, anchor
    best = item['proposal']
    if best:
        _, role, anchor = best
        if item['verifier_score'] >= policy['thresholds'][role][kind(item)]:
            return role, anchor
    return 'base', None


def reading_vector(raw, item, role, starter, adapted):
    gray = np.asarray(raw.convert('L'))
    ink = gray < 160
    resized = np.asarray(raw.resize((32,32), Image.Resampling.BILINEAR).convert('L'))
    laplacian = cv2.Laplacian(resized, cv2.CV_64F)
    return [float(raw.width / max(1, raw.height)), raw.height / 30, float(ink.mean()),
            float(gray.std() / 100), float(((gray > 30) & (gray < 220)).mean()),
            float(np.log1p(laplacian.var()) / 10), float(item['kind'] != 'ocr'), float(role == 'sub'),
            float(bool(ALPHANUMERIC.fullmatch(starter))), float(bool(ALPHANUMERIC.fullmatch(adapted))),
            float(starter.isdigit()), float(adapted.isdigit()),
            float(item['text'] == starter), float(item['text'] == adapted)]


def choose_reader(row, model):
    starter, adapted = row['starter'], row['adapted']
    if starter == adapted or not ALPHANUMERIC.fullmatch(adapted) or 'classifier' not in model:
        return 'starter'
    score = probabilities([row['features']], model['classifier'])[0]
    return 'adapted' if score >= model['thresholds'][row['role']] else 'starter'


def clip(image, box):
    l,t,r,b = box
    l,t,r,b = max(0,l),max(0,t),min(image.width,r),min(image.height,b)
    return image.crop((l,t,r,b)) if l < r and t < b else None


def review(image_path, hocr, relation_model, model, mode, tess, starter, adapted, jobs):
    ocr = parse_hocr(hocr.read_text())
    with Image.open(image_path) as source:
        image = source.convert('L')
    items = ScriptCandidates(image, ocr).build()
    proposals(items, relation_model)
    score_items(items, ocr, model)
    accepted = []
    with TemporaryDirectory() as directory:
        for item in items:
            role, anchor = decision(item, model['policies'][mode])
            if role == 'base':
                continue
            padded = padded_glyph(image, item['box'])
            raw = clip(image, item['box'])
            if padded is None or raw is None:
                if item['text']:
                    accepted.append((item, role, anchor, None, None))
                continue
            path = Path(directory) / f'{len(accepted):06}.png'
            padded.save(path, dpi=(300,300))
            accepted.append((item, role, anchor, raw, path))
        files = [case[-1] for case in accepted if case[-1] is not None]
        first = iter(batch_identity(tess, starter, files, jobs))
        second = iter(batch_identity(tess, adapted, files, jobs))
        output = []
        for item, role, anchor, raw, path in accepted:
            original = item['text']
            chosen, text = 'original', original
            if path is not None:
                readings = {'starter': next(first), 'adapted': next(second)}
                row = dict(role=role, **readings, features=reading_vector(raw, item, role, **readings))
                selected = choose_reader(row, model['router'])
                if ALPHANUMERIC.fullmatch(readings[selected]):
                    chosen, text = selected, readings[selected]
            if text:
                parent = ocr[anchor] if anchor is not None else None
                output.append(dict(kind=item['kind'], box=item['box'], placement=role, text=text, original=original,
                                   reader=chosen, original_index=item.get('index'),
                                   anchor=dict(index=anchor, box=parent['box'], text=parent['text']) if parent else None))
    return {'image': str(image_path), 'policy': mode, 'script_candidates': output}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('hocr', type=Path)
    for name in ('attachment', 'verifier', 'tesseract', 'starter', 'adapted'):
        parser.add_argument(f'--{name}', type=Path, required=True)
    parser.add_argument('--policy', choices=('precision','review'), default='precision')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    cv2.setNumThreads(1)
    result = review(args.image, args.hocr, json.loads(args.attachment.read_text()), json.loads(args.verifier.read_text()),
                    args.policy, args.tesseract, args.starter, args.adapted, args.jobs)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
