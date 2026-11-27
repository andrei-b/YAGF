#!/usr/bin/env python3
"""Fit and verify script recovery, parent attachment and crop-specific OCR."""

import argparse
import json
import random
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from statistics import mean
from tempfile import TemporaryDirectory

import cv2
import numpy as np
from PIL import Image
from scipy.optimize import minimize
from scipy.special import logsumexp, softmax

from benchmark_gtdb_inline_scripts import ROLES, apply_placement, label_placements, match_boxes
from benchmark_mixed_inline_scripts import load, match_points, save
from gtdb_inline_scripts import SCRIPT_LINKS, padded_glyph, read_pages
from inline_script_recovery import ALPHANUMERIC, ScriptCandidates, apply_policy, area, intersection, predict, read_crops
from prepare_mixed_inline_verification import SOURCES


def parent_index(glyph, glyphs, offset):
    by_id = {g.number: i for i, g in enumerate(glyphs)}
    visited = set()
    while glyph.number not in visited:
        visited.add(glyph.number)
        parent = by_id.get(glyph.parent)
        if parent is None:
            return None
        if glyph.link in SCRIPT_LINKS:
            return offset + parent
        if glyph.link != 0 or glyph.math != glyphs[parent].math:
            return None
        glyph = glyphs[parent]
    return None


def add_parents(targets, raw_lines):
    offset = 0
    for glyphs in raw_lines:
        for index, glyph in enumerate(glyphs):
            target = targets[offset + index]
            if target['role'] != 'base':
                target['parent'] = parent_index(glyph, glyphs, offset)
        offset += len(glyphs)
    if offset != len(targets):
        raise ValueError('The OCR cache no longer matches the annotation rows')


def gtdb_cases(prior, mixed):
    archive = prior / 'GTDB-1.zip'
    resolved = {a: a for a in {r['article'] for r in load(prior / 'registration.json')}}
    resolved.update({a: annotation for a, annotation, *_ in SOURCES})
    sheets = {a: read_pages(archive, a) for a in sorted(set(resolved.values()))}
    pages = {}
    for root in (prior, mixed):
        for row in load(root / 'registration.json'):
            pages[row['article'], row['page']] = row['sheet']
    by_sheet = {sheet: rows for pages in sheets.values() for sheet, rows in pages}
    for row in load(prior / 'ocr-lines.json'):
        if row['split'] == 'test':
            continue
        line = int(Path(row['image']).stem.rsplit('-', 1)[1])
        name = pages[row['article'], row['page']]
        add_parents(row['references'], [by_sheet[name][line]])
        yield {**row, 'image': str(prior / row['image'])}
    for root, cache in [(prior, 'ocr-pages.json'), (mixed, 'ocr-mixed.json')]:
        line_indices = defaultdict(list)
        for row in load(root / 'line-manifest.json'):
            line_indices[row['article'], row['page']].append(int(Path(row['image']).stem.rsplit('-', 1)[1]))
        for row in load(root / cache):
            if root == mixed and row['coverage'] != 'dense':
                continue
            key = row['article'], row['page']
            add_parents(row['references'], [by_sheet[pages[key]][i] for i in line_indices[key]])
            yield {**row, 'image': str(root / row['image'])}


def recover_case(row):
    cv2.setNumThreads(1)
    with Image.open(row['image']) as source:
        items = ScriptCandidates(source, row['ocr']).build()
    match = match_points if row.get('coverage') == 'points' else match_boxes
    first = match(row['references'], row['ocr'])
    rest = [i for i in range(len(row['references'])) if i not in first]
    extra = match([row['references'][i] for i in rest], items[len(row['ocr']):])
    pairs = {**first, **{rest[i]: j + len(row['ocr']) for i, j in extra.items()}}
    return {**row, 'candidates': items, 'original_pairs': first, 'pairs': pairs}


def relationship_pairs(row):
    found = {int(target): candidate for target, candidate in row['pairs'].items()}
    base_matches = {int(target): candidate for target, candidate in row['original_pairs'].items()}
    by_candidate = {candidate: row['references'][target] for target, candidate in found.items()}
    for index, item in enumerate(row['candidates']):
        target = by_candidate.get(index)
        if target and target['role'] != 'base':
            parent = target.get('parent')
            base = base_matches.get(parent)
            if base is None or not any(rel['base'] == base for rel in item['relations']):
                continue
        else:
            base = None
        for rel in item['relations']:
            role = target['role'] if target and rel['base'] == base else 'base'
            yield rel['features'], ROLES.index(role), row['article']


def fit(train):
    examples = [ex for row in train for ex in relationship_pairs(row)]
    vectors, labels, sources = zip(*examples)
    values = np.clip(np.asarray(vectors), -6, 6)
    mean_value = values.mean(0)
    scale = np.maximum(values.std(0), .01)
    x = np.column_stack([(values - mean_value) / scale, np.ones(len(values))])
    classes = np.eye(len(ROLES))[list(labels)]
    source_size = Counter(sources)
    weights = np.array([1 / np.sqrt(source_size[s]) for s in sources])
    class_weight = np.sqrt(weights.sum() / (weights @ classes))
    weights *= classes @ class_weight
    weights /= weights.mean()

    def objective(flat):
        params = flat.reshape(x.shape[1], -1)
        logits = x @ params
        value = (weights * (logsumexp(logits, axis=1) - (classes * logits).sum(1))).mean()
        value += .001 * np.square(params[:-1]).sum() / 2
        gradient = x.T @ ((softmax(logits, axis=1) - classes) * weights[:, None]) / len(x)
        gradient[:-1] += .001 * params[:-1]
        return value, gradient.ravel()

    result = minimize(objective, np.zeros(x.shape[1] * len(ROLES)), jac=True, method='L-BFGS-B')
    if not result.success:
        raise ValueError(result.message)
    return {'mean': mean_value.tolist(), 'scale': scale.tolist(), 'weights': result.x.reshape(x.shape[1], -1).tolist(),
            'training_pairs': dict(Counter(ROLES[i] for i in labels)), 'training_sources': sorted(source_size)}


def mine_crops(work, rows):
    reservoir = defaultdict(list)
    for case_number, row in enumerate(rows):
        for ref_index, item_index in row['pairs'].items():
            ref = row['references'][int(ref_index)]
            item = row['candidates'][item_index]
            if not ref['text'] or not ALPHANUMERIC.fullmatch(ref['text']):
                continue
            if intersection(ref['box'], item['box']) / max(1, area(item['box'])) < .56:
                continue
            if ref['role'] == 'base' and not item['relations']:
                continue
            reservoir[row['article'], ref['role']].append((case_number, item['box'], ref['text'], item['kind']))
    rng = random.Random(34071)
    selected = defaultdict(list)
    for (article, role), examples in sorted(reservoir.items()):
        for case, box, text, kind in rng.sample(examples, min(175 if role == 'base' else 300, len(examples))):
            selected[case].append((article, role, box, text, kind))
    glyphs = work / 'glyphs'
    glyphs.mkdir(exist_ok=True)
    manifest = []
    for case, labels in selected.items():
        with Image.open(rows[case]['image']) as source:
            image = source.convert('L')
        for article, role, box, text, kind in labels:
            crop = padded_glyph(image, box)
            if crop is None:
                continue
            name = f'glyphs/crop-{len(manifest):05}.png'
            crop.save(work / name, dpi=(300, 300))
            manifest.append(dict(image=name, text=text, article=article, split='train', role=role, kind=kind))
    save(work / 'glyph-manifest.json', manifest)
    print('Mined OCR crops', dict(Counter(row['role'] for row in manifest)), flush=True)


def audit_unmatched(work, cases):
    """Write an inspectable training-only sample of the two recovery situations."""
    from PIL import ImageDraw
    choices = defaultdict(list)
    for row in cases:
        first = row['original_pairs']
        for ref_index, target in enumerate(row['references']):
            if ref_index in first or target['role'] == 'base':
                continue
            coverage = max((intersection(target['box'], g['box']) / area(target['box'])
                            for g in row['ocr']), default=0)
            kind = 'overlapping OCR box' if coverage >= .60 else 'uncovered ink' if coverage < .15 else 'partial overlap'
            choices[kind].append((row, target))
    rng = random.Random(943)
    audit = Image.new('RGB', (870, 600), 'white')
    draw = ImageDraw.Draw(audit)
    for col, (kind, examples) in enumerate(sorted(choices.items())):
        draw.text((col * 290 + 8, 3), f'{kind} ({len(examples)} targets)', fill='black')
        for index, (row, target) in enumerate(rng.sample(examples, min(len(examples), 7))):
            x, y, u, v = target['box']
            left, top, right, bottom = x - 48, y - 38, u + 48, v + 32
            with Image.open(row['image']) as source:
                left, top = max(0, left), max(0, top)
                right, bottom = min(source.width, right), min(source.height, bottom)
                patch = source.convert('RGB').crop((left, top, right, bottom))
            pen = ImageDraw.Draw(patch)
            for glyph in row['ocr']:
                if intersection(glyph['box'], [left, top, right, bottom]):
                    a, b, c, d = glyph['box']
                    pen.rectangle([a - left, b - top, c - left, d - top], outline='#2672b9')
            pen.rectangle([x - left, y - top, u - left, v - top], outline='#d3003d', width=2)
            patch.thumbnail((276, 67))
            audit.paste(patch, (col * 290 + 8, 18 + 81 * index))
            draw.text((col * 290 + 8, 85 + 81 * index), row['article'], fill='black')
    audit.save(work / 'miss-audit.png')
    save(work / 'miss-geometry.json', {kind: len(examples) for kind, examples in choices.items()})


def prepare(work, prior, mixed, jobs):
    inputs = list(gtdb_cases(prior, mixed))
    with ProcessPoolExecutor(jobs) as pool:
        for split in ('train', 'dev', 'test'):
            rows = []
            for row in pool.map(recover_case, [case for case in inputs if case['split'] == split], chunksize=8):
                rows.append(row)
                if len(rows) % 100 == 0:
                    print(split, len(rows), flush=True)
            (work / f'{split}-candidates.json').write_text(json.dumps(rows, ensure_ascii=False) + '\n')
            if split == 'train':
                save(work / 'attachment-model.json', fit(rows))
                mine_crops(work, rows)
                audit_unmatched(work, rows)
            print(f'{split}: {len(rows)} inputs', flush=True)


def prepare_legacy(work, mixed, jobs):
    rows = [row for row in load(mixed / 'ocr-mixed.json') if row['coverage'] in ('points', 'control')]
    with ProcessPoolExecutor(jobs) as pool:
        cases = list(pool.map(recover_case, rows, chunksize=2))
    (work / 'legacy-candidates.json').write_text(json.dumps(cases, ensure_ascii=False) + '\n')


def dev_thresholds(rows, precision_matched):
    """Choose role-specific cutoffs with equal article weight on development data."""
    stats = {}
    for row in rows:
        article = stats.setdefault(row['article'], {})
        by_candidate = {int(candidate): row['references'][int(target)]['role'] for target, candidate in row['pairs'].items()}
        for role in ROLES[1:]:
            if role not in article:
                article[role] = dict(reference=0, native_tp=0, native_flags=0, decoded=[], recovered=[])
            article[role]['reference'] += sum(ref['role'] == role for ref in row['references'])
        for i, item in enumerate(row['candidates']):
            if item['role'] != 'base':
                counters = article[item['role']]
                counters['native_flags'] += 1
                counters['native_tp'] += by_candidate.get(i) == item['role']
                if precision_matched:
                    continue
            if item.get('proposal') is None:
                continue
            score, role, _ = item['proposal']
            article[role]['decoded' if item['kind'] == 'ocr' else 'recovered'].append((score, by_candidate.get(i) == role))
    thresholds = {'decoded': {}, 'recovered': {}}
    floors = {}
    for role in ROLES[1:]:
        summaries = [article[role] for article in stats.values()]
        native_precision = [row['native_tp'] / row['native_flags'] for row in summaries if row['native_flags']]
        target = mean(native_precision) if native_precision else .9
        best = (-1, 1.01, 1.01)
        for decoded in [.35, .45, .55, .65, .75, .85, .9, .94, .97, .99, 1.01]:
            for recovered in [.35, .45, .55, .65, .75, .85, .9, .94, .97, .99, 1.01]:
                recalls, precisions, f_scores = [], [], []
                for row in summaries:
                    accepted = [good for key, floor in [('decoded', decoded), ('recovered', recovered)]
                                for score, good in row[key] if score >= floor]
                    count = (row['native_flags'] if precision_matched else 0) + len(accepted)
                    tp = (row['native_tp'] if precision_matched else 0) + sum(accepted)
                    if count:
                        precisions.append(tp / count)
                    if row['reference']:
                        recalls.append(tp / row['reference'])
                        f_scores.append(2 * tp / (count + row['reference']))
                if not recalls:
                    continue
                if precision_matched and (not precisions or mean(precisions) + 1e-9 < target):
                    continue
                quality = mean(recalls if precision_matched else f_scores)
                best = max(best, (quality, decoded, recovered))
        thresholds['decoded'][role], thresholds['recovered'][role] = best[1:]
        floors[role] = target
    return thresholds, floors


def classify(rows, model, previous):
    apply_placement(rows, previous)
    label_placements(rows, previous['added_thresholds'])
    for row in rows:
        items = row['candidates']
        initial = row['ocr']
        predict(items, model)
        for i, item in enumerate(items):
            role = item['role']
            item['flags'] = {'native': role, 'previous': (initial[i]['learned']
                              if role == 'base' and i < len(initial) else role)}
            item['anchors'] = {}
        for policy, settings in model['policies'].items():
            apply_policy(items, settings)
            for item in items:
                item['flags'][policy] = item['placement']
                item['anchors'][policy] = item.get('anchor')
        for item in items:
            item['placement'] = next((role for role in item['flags'].values() if role != 'base'), 'base')


def source_scores(rows, policy, reading):
    sources = {}
    for row in rows:
        stats = sources.setdefault(row['article'], {role: Counter() for role in ROLES[1:]})
        first = {int(i): j for i, j in row['original_pairs'].items()}
        both = {int(i): j for i, j in row['pairs'].items()}
        by_candidate = {idx: row['references'][i]['role'] for i, idx in both.items()}
        flags = [item['flags'][policy] for item in row['candidates']]
        for idx, item in enumerate(row['candidates']):
            role = flags[idx]
            if role == 'base':
                continue
            stats[role]['predicted'] += 1
            if item['kind'] != 'ocr':
                stats[role]['proposed_insertions'] += 1
                if by_candidate.get(idx) == role:
                    stats[role]['correct_proposed_insertions'] += 1
                reader = reading.get(role, 'text') if isinstance(reading, dict) else reading
                if item.get(reader):
                    stats[role]['readable_insertions'] += 1
                    stats[role]['correct_readable_insertions'] += by_candidate.get(idx) == role
        for i, ref in enumerate(row['references']):
            role = ref['role']
            if role == 'base':
                continue
            s = stats[role]
            s['reference'] += 1
            s['identity_reference'] += ref['text'] is not None
            s['original_coverage'] += i in first
            s['candidate_coverage'] += i in both
            if (parent := ref.get('parent')) is not None and parent in first:
                s['anchor_reference'] += 1
            if i not in both:
                continue
            idx = both[i]
            item = row['candidates'][idx]
            if flags[idx] != role:
                continue
            s['placement_correct'] += 1
            if item['anchors'].get(policy) is not None and item['anchors'][policy] == first.get(parent):
                s['attachment_correct'] += 1
            key = reading.get(role, 'text') if isinstance(reading, dict) else reading
            text = item.get(key, item['text'])
            s['joint_correct'] += ref['text'] is not None and ref['text'] == text
    return sources


def macro(sources, sparse=False):
    result = {role: {} for role in ROLES[1:]}
    keys = [('original_coverage', 'reference'), ('candidate_coverage', 'reference'), ('placement_correct', 'reference'),
            ('precision', 'predicted'), ('attachment_correct', 'anchor_reference'), ('joint_correct', 'identity_reference'),
            ('correct_readable_insertions', 'readable_insertions')]
    if sparse:
        keys = [pair for pair in keys if pair[0] not in ('precision', 'correct_readable_insertions')]
    for role in ROLES[1:]:
        for key, denom in keys:
            numerator = 'placement_correct' if key == 'precision' else key
            pairs = [(row[role][numerator], row[role][denom]) for row in sources.values() if row[role][denom]]
            values = [n / d for n, d in pairs]
            omitted = [mean(values[:i] + values[i+1:]) for i in range(len(values))] if len(values) > 1 else []
            result[role][key] = dict(avg=mean(values) if values else None,
                                     total=[sum(n for n, _ in pairs), sum(d for _, d in pairs)],
                                     omit_one_range=[min(omitted), max(omitted)] if omitted else None)
    return result


def score_all(rows, readers, sparse=False):
    cases = {}
    for policy in ('native', 'previous', 'balanced', 'precision'):
        for name, decoder in [('first pass', 'text'), ('starter', 'starter'), ('mixed', 'mixed'),
                              ('adapted', 'adapted'), ('selected', readers)]:
            report = source_scores(rows, policy, decoder)
            cases[f'{policy}:{name}'] = {'per_source': report, 'macro': macro(report, sparse)}
            if sparse:
                cases[f'{policy}:{name}']['control_flags'] = sum(
                    item['flags'][policy] != 'base' for row in rows if row['coverage'] == 'control'
                    for item in row['candidates'])
    return cases


def load_cases(work, split):
    rows = load(work / f'{split}-candidates.json')
    for row in rows:
        row['image'] = str((work / row['image']).resolve())
    return rows


def evaluate(work, mixed, tess, starter, adapted, jobs):
    previous = load(mixed / 'placement-model.json')
    model = load(work / 'attachment-model.json')
    policy = dict(thresholds={'decoded': {'sub': 0, 'sup': 0}, 'recovered': {'sub': 0, 'sup': 0}})
    dev = load_cases(work, 'dev')
    for row in dev:
        predict(row['candidates'], model)
        apply_policy(row['candidates'], policy)
    balanced, _ = dev_thresholds(dev, False)
    precision, floors = dev_thresholds(dev, True)
    model['policies'] = dict(balanced=dict(thresholds=balanced),
                             precision=dict(thresholds=precision, preserve_native=True, target_precision=floors))
    model['readers'] = {'sub': 'starter', 'sup': 'starter'}
    save(work / 'attachment-model.json', model)
    models = dict(starter=starter, mixed=mixed / 'final-model', adapted=adapted)
    classify(dev, model, previous)
    with TemporaryDirectory() as temp:
        read_crops(Path(temp), dev, tess, models, jobs)
    options = {name: macro(source_scores(dev, 'balanced', name)) for name in models}
    for role in ROLES[1:]:
        model['readers'][role] = max(models, key=lambda name: options[name][role]['joint_correct']['avg'] or 0)
    save(work / 'attachment-model.json', model)
    results = dict(dev_readers=options, chosen_readers=model['readers'], policies=model['policies'])
    del dev
    for split in ('test', 'fresh', 'legacy'):
        rows = load_cases(work, split)
        classify(rows, model, previous)
        with TemporaryDirectory() as temp:
            read_crops(Path(temp), rows, tess, models, jobs)
        results[split] = dict(documents=sorted({row['article'] for row in rows}), pages=len(rows),
                              results=score_all(rows, model['readers'], sparse=split == 'legacy'))
        if split == 'fresh':
            for coverage in sorted({row['coverage'] for row in rows}):
                subset = [row for row in rows if row['coverage'] == coverage]
                results[split][coverage] = dict(documents=sorted({row['article'] for row in subset}),
                                                 pages=len(subset), results=score_all(subset, model['readers']))
        save(work / 'recovery-metrics.json', results)
        print(split, len(rows), 'pages', flush=True)
        del rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=('prepare', 'prepare-legacy', 'evaluate'))
    parser.add_argument('work', type=Path)
    for arg in ('prior', 'mixed', 'tesseract', 'starter', 'adapted'):
        parser.add_argument(f'--{arg}', type=Path)
    parser.add_argument('--jobs', type=int, default=3)
    args = parser.parse_args()
    if args.stage == 'prepare-legacy':
        if args.mixed is None:
            parser.error('prepare-legacy requires --mixed')
        prepare_legacy(args.work.resolve(), args.mixed.resolve(), args.jobs)
    elif args.stage == 'prepare':
        if args.prior is None or args.mixed is None:
            parser.error('prepare requires --prior and --mixed')
        prepare(args.work.resolve(), args.prior.resolve(), args.mixed.resolve(), args.jobs)
    else:
        if None in (args.mixed, args.tesseract, args.starter, args.adapted):
            parser.error('evaluate requires --mixed, --tesseract, --starter, and --adapted')
        evaluate(args.work.resolve(), args.mixed.resolve(), args.tesseract.resolve(), args.starter.resolve(),
                 args.adapted.resolve(), args.jobs)


if __name__ == '__main__':
    main()
