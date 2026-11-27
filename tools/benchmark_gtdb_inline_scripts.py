#!/usr/bin/env python3
"""Score glyph transcription, geometry and their joint result on held-out GTDB articles."""

import argparse
import json
import os
import re
import subprocess
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from tempfile import TemporaryDirectory

import numpy as np
from PIL import Image
from scipy.optimize import linear_sum_assignment, minimize
from scipy.special import logsumexp, softmax

from gtdb_inline_scripts import padded_glyph, read_pages, transform_boxes


ROLES = ('base', 'sub', 'sup')
NATIVE_FLAGS = ['-c', 'lstm_detect_subscripts=1', '-c', 'lstm_recover_subscripts=1',
                '-c', 'lstm_reread_subscripts=1', '-c', 'hocr_char_boxes=1']


def command(tess, data, image, psm, *args):
    return subprocess.run([tess, image, 'stdout', '--tessdata-dir', data, '-l', 'eng',
                           '--oem', '1', '--psm', str(psm), *args],
                          env={**os.environ, 'OMP_THREAD_LIMIT': '1'}, check=True, capture_output=True, text=True)


def properties(element):
    return {words[0]: words[1:] for field in element.get('title', '').split(';') if (words := field.split())}


def parse_hocr(result):
    rows = []
    for line in ET.fromstring(result).iter():
        if line.get('class') not in ('ocr_line', 'ocr_header', 'ocr_textfloat', 'ocr_caption'):
            continue
        p = properties(line)
        line_box = [int(value) for value in p['bbox']]
        slope, intercept = map(float, p.get('baseline', ('0', '0')))
        words = []
        for word in line.iter():
            if word.get('class') != 'ocrx_word':
                continue
            glyphs = []
            for span in word.iter():
                if span.get('class') != 'ocrx_cinfo':
                    continue
                attrs = properties(span)
                if 'x_bboxes' not in attrs:
                    continue
                tags = {child.tag.rsplit('}', 1)[-1] for child in span}
                glyphs.append({'box': [int(value) for value in attrs['x_bboxes']],
                               'text': ''.join(span.itertext()).strip(),
                               'role': next((role for role in ROLES[1:] if role in tags), 'base')})
            if glyphs:
                words.append(glyphs)
        heights = [row['box'][3] - row['box'][1] for word in words for row in word]
        if not heights:
            continue
        scale = max(1, np.percentile(heights, 58))
        for glyphs in words:
            for index, glyph in enumerate(glyphs):
                x, y, u, v = glyph['box']
                center = (x + u) / 2
                baseline = line_box[3] + intercept + slope * (center - line_box[0])
                features = [
                    (y - baseline) / scale, (v - baseline) / scale,
                    (v - y) / scale, (u - x) / scale,
                    float(index == 0), float(index == len(glyphs) - 1), min(len(glyphs), 12) / 12,
                ]
                for offset in (-1, 1):
                    neighbor_index = index + offset
                    if 0 <= neighbor_index < len(glyphs):
                        a, b, c, d = glyphs[neighbor_index]['box']
                        gap = x - c if offset == -1 else a - u
                        features.extend(((y - b) / scale, (v - d) / scale, gap / scale, 1))
                    else:
                        features.extend((0, 0, 0, 0))
                glyph['features'] = features
                rows.append(glyph)
    return rows


def match_boxes(reference, predicted):
    """Assign boxes by overlap. OCR identity and placement never enter matching."""
    if not reference or not predicted:
        return {}
    expected = np.array([row['box'] for row in reference])
    observed = np.array([row['box'] for row in predicted])
    starts = np.maximum(expected[:, None, :2], observed[None, :, :2])
    ends = np.minimum(expected[:, None, 2:], observed[None, :, 2:])
    intersection = np.prod(np.maximum(ends - starts, 0), axis=-1)
    size_a = np.prod(expected[:, 2:] - expected[:, :2], axis=1)
    size_b = np.prod(observed[:, 2:] - observed[:, :2], axis=1)
    overlaps = intersection / np.maximum(1, size_a[:, None] + size_b[None, :] - intersection)
    centers_obs = (observed[:, :2] + observed[:, 2:]) / 2
    centers_ref = (expected[:, :2] + expected[:, 2:]) / 2
    ref_inside = ((centers_ref[:, None] >= observed[None, :, :2]) &
                  (centers_ref[:, None] <= observed[None, :, 2:])).all(axis=-1)
    obs_inside = ((centers_obs[None, :] >= expected[:, None, :2]) &
                  (centers_obs[None, :] <= expected[:, None, 2:])).all(axis=-1)
    usable = (overlaps >= 0.35) | ((overlaps >= 0.22) & (ref_inside | obs_inside))
    scores = np.where(usable, overlaps, 0)
    a, b = linear_sum_assignment(scores, maximize=True)
    return {int(i): int(j) for i, j in zip(a, b) if scores[i, j] > 0}


def read_lines(work, tess, base, hocr, jobs):
    cache = work / 'ocr-lines.json'
    lines = json.loads((work / 'line-manifest.json').read_text())

    def recognize(line):
        result = command(tess, base, work / line['image'], 7, *NATIVE_FLAGS, hocr)
        predictions = parse_hocr(result.stdout)
        pairs = match_boxes(line['references'], predictions)
        return {**line, 'ocr': predictions, 'pairs': pairs}

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        rows = list(pool.map(recognize, lines))
    cache.write_text(json.dumps(rows, ensure_ascii=False) + '\n')
    return rows


def read_full_pages(work, tess, base, hocr, jobs):
    """Run automatic page layout and score the same annotated prose regions."""
    selected = defaultdict(list)
    for line in json.loads((work / 'line-manifest.json').read_text()):
        if line['split'] == 'test':
            article, page, index = Path(line['image']).stem.rsplit('-', 2)
            selected[article, int(page)].append((int(index), line['references']))
    sources = {article: read_pages(work / 'GTDB-1.zip', article) for article in {key[0] for key in selected}}
    registration = {(row['article'], row['page']): np.asarray(row['parameters'])
                    for row in json.loads((work / 'registration.json').read_text()) if row.get('accepted')}
    directory = work / 'pages-300'
    directory.mkdir(exist_ok=True)

    def recognize(item):
        (article, page), lines = item
        relative = f'pages-300/{article}-{page:03d}.png'
        with Image.open(work / 'scans' / article / f'page-{page - 1:03d}.png') as native:
            image = native.convert('L').resize(((native.width + 1) // 2, (native.height + 1) // 2),
                                               Image.Resampling.LANCZOS)
            sx, sy = image.width / native.width, image.height / native.height
            image.save(work / relative, dpi=(300, 300))
        result = command(tess, base, work / relative, 3, *NATIVE_FLAGS, hocr)
        predictions = parse_hocr(result.stdout)
        centers = np.array([(np.asarray(glyph['box'][:2]) + glyph['box'][2:]) / 2 for glyph in predictions]).reshape(-1, 2)
        selected_glyphs = np.zeros(len(predictions), dtype=bool)
        references = []
        parameters = registration[article, page]
        for index, annotated in lines:
            glyphs = sources[article][page - 1][1][index]
            assert len(glyphs) == len(annotated)
            projected = transform_boxes([glyph.box for glyph in glyphs], parameters)
            boxes = np.rint(projected * [sx, sy, sx, sy]).astype(int)
            references.extend({**ref, 'box': box.tolist()} for ref, box in zip(annotated, boxes))
            low, high = boxes[:, :2].min(0) - 9, boxes[:, 2:].max(0) + 9
            selected_glyphs |= ((centers >= low) & (centers <= high)).all(axis=1)
        predictions = [glyph for glyph, keep in zip(predictions, selected_glyphs) if keep]
        return {'article': article, 'page': page, 'split': 'test', 'image': relative,
                'references': references, 'ocr': predictions, 'pairs': match_boxes(references, predictions)}

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        rows = list(pool.map(recognize, sorted(selected.items())))
    (work / 'ocr-pages.json').write_text(json.dumps(rows, ensure_ascii=False) + '\n')


def pairs_for(line):
    return {int(ref): pred for ref, pred in line['pairs'].items()}


def fit_placement(lines, balance_sources=False):
    train = [(line['ocr'][pred]['features'], ROLES.index(line['references'][ref]['role']), line['article'])
             for line in lines if line['split'] == 'train' for ref, pred in pairs_for(line).items()]
    samples, truth, sources = zip(*train)
    x = np.clip(np.asarray(samples), -5, 5)
    center, scale = x.mean(0), np.maximum(x.std(0), 0.01)
    x = np.column_stack(((x - center) / scale, np.ones(len(x))))
    target = np.eye(len(ROLES))[np.asarray(truth)]
    weights = np.ones(len(x))
    if balance_sources:
        sizes = Counter(sources)
        weights = np.array([1 / np.sqrt(sizes[source]) for source in sources])
    role_weights = np.sqrt(weights.sum() / (weights @ target))
    weights *= target @ role_weights
    weights /= weights.mean()

    def loss(flat):
        parameters = flat.reshape(x.shape[1], len(ROLES))
        logits = x @ parameters
        regularization = 0.001
        penalty = regularization * np.sum(parameters[:-1] ** 2) / 2
        error = (weights * (logsumexp(logits, axis=1) - (target * logits).sum(1))).mean()
        gradient = x.T @ ((softmax(logits, axis=1) - target) * weights[:, None]) / len(x)
        gradient[:-1] += regularization * parameters[:-1]
        return error + penalty, gradient.ravel()

    result = minimize(loss, np.zeros(x.shape[1] * len(ROLES)), jac=True, method='L-BFGS-B')
    if not result.success:
        raise ValueError(f'Placement fit failed: {result.message}')
    return {'mean': center.tolist(), 'scale': scale.tolist(),
            'weights': result.x.reshape(x.shape[1], len(ROLES)).tolist()}


def predict_placement(model, glyphs):
    x = np.clip(np.asarray([glyph['features'] for glyph in glyphs]), -5, 5)
    x = np.column_stack(((x - model['mean']) / model['scale'], np.ones(len(x))))
    return softmax(x @ model['weights'], axis=1)


def apply_placement(lines, model):
    for line in lines:
        glyphs = line['ocr']
        if not glyphs:
            continue
        for glyph, probabilities in zip(glyphs, predict_placement(model, glyphs)):
            glyph['probabilities'] = probabilities.tolist()


def summarize(lines, role_key, text_key='text'):
    fields = ('reference', 'identity_reference', 'matched', 'placement_correct',
              'identity_correct', 'joint_correct', 'predicted')
    stats = {role: Counter(dict.fromkeys(fields, 0)) for role in ROLES}
    for line in lines:
        pairs = pairs_for(line)
        for idx, ref in enumerate(line['references']):
            observed = line['ocr'][pairs[idx]] if idx in pairs else None
            group = stats[ref['role']]
            group['reference'] += 1
            if ref['text'] is not None:
                group['identity_reference'] += 1
            if observed is None:
                continue
            group['matched'] += 1
            placement = observed[role_key] == ref['role']
            identity = ref['text'] is not None and reading_for(observed, role_key, text_key) == ref['text']
            group['placement_correct'] += placement
            group['identity_correct'] += identity
            group['joint_correct'] += placement and identity
        for glyph in line['ocr']:
            stats[glyph[role_key]]['predicted'] += 1
    return {role: dict(counts) for role, counts in stats.items()}


def reading_for(glyph, role_key, text_key):
    return glyph['text'] if glyph[role_key] == 'base' else glyph.get(text_key, glyph['text'])


def count_edits(lines, role_key, text_key):
    totals = Counter()
    for line in lines:
        by_prediction = {pred: line['references'][ref] for ref, pred in pairs_for(line).items()}
        for index, glyph in enumerate(line['ocr']):
            reading = reading_for(glyph, role_key, text_key)
            if reading == glyph['text']:
                continue
            reference = by_prediction.get(index, {}).get('text')
            if reference is None:
                totals['unknown_target'] += 1
            elif reading == reference:
                totals['corrected'] += 1
            elif glyph['text'] == reference:
                totals['spoiled'] += 1
            else:
                totals['still_wrong'] += 1
    return dict(totals)


def select_thresholds(dev):
    thresholds = {}
    for index, role in enumerate(ROLES[1:], start=1):
        reference_count = sum(ref['role'] == role for line in dev for ref in line['references'])
        candidates = []
        for line in dev:
            truth = {pred: line['references'][ref]['role'] for ref, pred in pairs_for(line).items()}
            candidates.extend((glyph['probabilities'][index], truth.get(pred) == role)
                              for pred, glyph in enumerate(line['ocr']) if np.argmax(glyph['probabilities']) == index)
        best = (-1, 1.0)
        for threshold in np.arange(0.30, 1.0, 0.025):
            selected = [correct for probability, correct in candidates if probability >= threshold]
            f1 = 2 * sum(selected) / max(1, len(selected) + reference_count)
            best = max(best, (f1, float(threshold)))
        thresholds[role] = best[1]
    return thresholds


def label_placements(lines, thresholds):
    for line in lines:
        for glyph in line['ocr']:
            index = np.argmax(glyph['probabilities'])
            role = ROLES[index]
            glyph['learned'] = role if glyph['probabilities'][index] >= thresholds.get(role, 0) else 'base'


def batch_identity(tess, data, images, jobs):
    if not images:
        return []
    chunks = [images[i::jobs] for i in range(jobs)]
    with TemporaryDirectory() as folder:
        def read_chunk(item):
            index, paths = item
            if not paths:
                return []
            input_list = Path(folder) / f'{index}.txt'
            input_list.write_text(''.join(f'{image}\n' for image in paths))
            # Use the explicit page IDs when plain text drops a separator for an empty result.
            raw = command(tess, data, input_list, 10, '-c', 'page_separator=\f').stdout
            pages = raw.split('\f')
            if pages[-1] == '':
                pages.pop()
            if len(pages) == len(paths):
                return [page.strip() for page in pages]
            tsv = command(tess, data, input_list, 10, '-c', 'tessedit_create_tsv=1').stdout
            words = [[] for _ in paths]
            page_ids = []
            for line in tsv.split('\n')[1:]:
                fields = line.split('\t', 11)
                if len(fields) != 12:
                    continue
                level, page_id = map(int, fields[:2])
                if level == 1:
                    page_ids.append(page_id)
                elif level == 5 and 1 <= page_id <= len(paths):
                    words[page_id - 1].append(fields[-1])
            if page_ids == list(range(1, len(paths) + 1)):
                return [' '.join(page).strip() for page in words]
            return [command(tess, data, path, 10).stdout.strip() for path in paths]

        with ThreadPoolExecutor(max_workers=jobs) as pool:
            results = list(pool.map(read_chunk, enumerate(chunks)))
    aligned = [''] * len(images)
    for index, chunk in enumerate(results):
        aligned[index::jobs] = chunk
    return aligned


def score_isolated(work, tess, base, candidate, jobs):
    glyphs = [row for row in json.loads((work / 'glyph-manifest.json').read_text()) if row['split'] != 'train']
    paths = [work / glyph['image'] for glyph in glyphs]
    report = {}
    for name, model in [('starter', base), ('fine_tuned', candidate)]:
        stats = {}
        for glyph, reading in zip(glyphs, batch_identity(tess, model, paths, jobs)):
            counts = stats.setdefault(glyph['split'], {}).setdefault(glyph['role'], Counter())
            counts['reference'] += 1
            counts['correct'] += reading == glyph['text']
            counts['empty'] += not reading
        report[name] = stats
    return report


def reread(test, work, tess, base, candidate, jobs, role_keys=('learned', 'role')):
    glyphs, paths = [], []
    directory = work / 'ocr-glyphs'
    directory.mkdir(exist_ok=True)
    for line in test:
        selected = [glyph for glyph in line['ocr'] if any(glyph[key] != 'base' for key in role_keys)]
        if not selected:
            continue
        with Image.open(work / line['image']) as source:
            image = source.convert('L')
            for index, glyph in enumerate(selected):
                crop = padded_glyph(image, glyph['box'])
                if crop is None:
                    continue
                path = directory / f"{Path(line['image']).stem}-{index}.png"
                crop.save(path, dpi=(300, 300))
                paths.append(path)
                glyphs.append(glyph)
    for key, data in [('starter_reread', base), ('trained_reread', candidate)]:
        for glyph, reading in zip(glyphs, batch_identity(tess, data, paths, jobs)):
            if re.fullmatch('[A-Za-z0-9]', reading):
                glyph[key] = reading


def evaluate(work, tess, base, candidate, jobs, pages):
    if pages:
        test = json.loads((work / 'ocr-pages.json').read_text())
        model = json.loads((work / 'placement-model.json').read_text())
        apply_placement(test, model)
    else:
        lines = json.loads((work / 'ocr-lines.json').read_text())
        model = fit_placement(lines)
        dev = [line for line in lines if line['split'] == 'dev']
        test = [line for line in lines if line['split'] == 'test']
        apply_placement(dev + test, model)
        model['thresholds'] = select_thresholds(dev)
        (work / 'placement-model.json').write_text(json.dumps(model, indent=2) + '\n')
    label_placements(test, model['thresholds'])
    reread(test, work, tess, base, candidate, jobs)
    report = {
        'thresholds': model['thresholds'],
        'test_pages' if pages else 'test_lines': len(test),
        'native': summarize(test, 'role'),
        'native_starter_reread': summarize(test, 'role', 'starter_reread'),
        'native_trained_reread': summarize(test, 'role', 'trained_reread'),
        'learned_geometry': summarize(test, 'learned'),
        'learned_geometry_starter_reread': summarize(test, 'learned', 'starter_reread'),
        'learned_geometry_trained_reread': summarize(test, 'learned', 'trained_reread'),
        'native_starter_reread_edits': count_edits(test, 'role', 'starter_reread'),
        'native_trained_reread_edits': count_edits(test, 'role', 'trained_reread'),
        'learned_geometry_starter_reread_edits': count_edits(test, 'learned', 'starter_reread'),
        'learned_geometry_trained_reread_edits': count_edits(test, 'learned', 'trained_reread'),
    }
    if not pages:
        report['isolated'] = score_isolated(work, tess, base, candidate, jobs)
    filename = 'page-metrics.json' if pages else 'metrics.json'
    (work / filename).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=('ocr', 'evaluate', 'ocr-pages', 'evaluate-pages'))
    parser.add_argument('work', type=Path)
    parser.add_argument('--tesseract', type=Path, required=True)
    parser.add_argument('--base', type=Path, required=True, help='Directory containing the starter eng.traineddata')
    parser.add_argument('--candidate', type=Path, help='Directory containing the fine-tuned eng.traineddata')
    parser.add_argument('--hocr', type=Path, help='Path to the hOCR config')
    parser.add_argument('--jobs', type=int, default=3)
    args = parser.parse_args()
    work, tess, base = args.work.resolve(), args.tesseract.resolve(), args.base.resolve()
    if args.stage.startswith('ocr'):
        if args.hocr is None:
            parser.error(f'{args.stage} requires --hocr')
        read = read_full_pages if args.stage == 'ocr-pages' else read_lines
        read(work, tess, base, args.hocr.resolve(), args.jobs)
    else:
        if args.candidate is None:
            parser.error(f'{args.stage} requires --candidate')
        evaluate(work, tess, base, args.candidate.resolve(), args.jobs, args.stage == 'evaluate-pages')


if __name__ == '__main__':
    main()
