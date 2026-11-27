#!/usr/bin/env python3
"""Evaluate inline scripts across whole GTDB articles and earlier book scans."""

import argparse
import json
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from statistics import mean

import numpy as np
from PIL import Image
from scipy.optimize import linear_sum_assignment

from benchmark_gtdb_inline_scripts import (
    NATIVE_FLAGS, ROLES, apply_placement, batch_identity, command, count_edits,
    fit_placement, label_placements, match_boxes, pairs_for, parse_hocr, reread, summarize,
)
from train_mixed_inline_scripts import digest


POSITIONS = {'subscript': 'sub', 'superscript': 'sup', 'normal': 'base'}


def load(path):
    return json.loads(path.read_text())


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def match_points(references, predictions):
    if not references or not predictions:
        return {}
    xy = np.asarray([ref['point'] for ref in references])
    boxes = np.asarray([glyph['box'] for glyph in predictions])
    centers = (boxes[:, :2] + boxes[:, 2:]) / 2
    size = np.maximum(1, boxes[:, 2:] - boxes[:, :2])
    delta = abs(xy[:, None] - centers)
    within = (delta <= (size / 2) + 2).all(axis=2)
    # Prefer small centered boxes over a merged box covering multiple points.
    score = np.where(within, 1 / (1 + delta.sum(axis=2) + 0.14 * size.sum(axis=1)), 0)
    refs, preds = linear_sum_assignment(score, maximize=True)
    return {int(i): int(j) for i, j in zip(refs, preds) if score[i, j] > 0}


def legacy_cases(legacy, prototype):
    cases = []
    experiments = prototype / 'experiments/inline-math'
    inline = legacy / 'inline-patch'
    for manifest, images in [('reread-pages.json', 'selective'), ('cross-book-subscripts.json', 'next')]:
        for row in load(experiments / manifest):
            path = inline / images / row['image']
            with Image.open(path) as image:
                if list(image.size) != row['size']:
                    raise ValueError(f'{path} does not match its annotation dimensions')
            article = Path(row['image']).stem.split('-')[0]
            references = [{'point': [x, y], 'text': text, 'role': 'sub'} for x, y, text in row['subscripts']]
            control = row.get('ordinary_control', article == 'groups')
            cases.append(dict(article=article, split='test', coverage='control' if control else 'points',
                              image=str(path), references=references))
    openstax = legacy / 'moremath/openstax-punctuation/next'
    points = load(openstax / 'openstax-reannotated.json')
    for i, row in enumerate(load(openstax / 'inputs.json'), 1):
        path = openstax / 'inputs' / Path(row['file']).name
        if digest(path) != row['sha256']:
            raise ValueError(f'{path} differs from the annotated page')
        references = [dict(point=[p['x'], p['y']], text=p['text'], role='sub') for p in points if p['page'] == i]
        cases.append(dict(article='openstax', split='test', coverage='points', image=str(path), references=references))
    functional = legacy / 'moremath/subscr2-retrain/functional-analysis'
    spec = load(functional / 'functional-reference.json')
    for name, sha256 in spec['pages'].items():
        page = int(Path(name).stem.rsplit('-', 1)[1])
        path = functional / 'scans' / name
        if digest(path) != sha256:
            raise ValueError(f'{path} differs from the annotated page')
        references = [dict(point=[p['x'], p['y']], text=p['text'], role=POSITIONS[p['position']])
                      for p in spec['points'] if p['page'] == page]
        if references:
            cases.append(dict(article='functional-analysis', split='test', coverage='points', image=str(path),
                              references=references))
    for article, manifest in [('lnm0966', 'manifest.json'), ('mlbook', 'script-diagnostic/manifest.json')]:
        directory = legacy / article / Path(manifest).parent
        raw = load(legacy / article / manifest)
        lines = [r for r in raw['samples'] if r['split'] == 'train'] if isinstance(raw, dict) else [
            r for r in raw if r['split'] == 'development']
        for row in lines:
            refs = [{'box': g['box'], 'text': g['text'], 'role': POSITIONS[g['position']]}
                    for g in row.get('script_boxes', row.get('targets', [])) if len(g['text']) == 1]
            if refs:
                cases.append(dict(article=article, split='train', coverage='boxes',
                                  image=str(directory / row['image']), references=refs, psm=7))
    return cases


def gtdb_cases(work):
    by_page = defaultdict(list)
    for line in load(work / 'line-manifest.json'):
        by_page[line['article'], line['page']].append(line['references'])
    directory = work / 'pages-300'
    directory.mkdir(exist_ok=True)
    for record in load(work / 'registration.json'):
        article, page = record['article'], record['page']
        lines = by_page.get((article, page))
        if not lines:
            continue
        path = directory / f'{article}-{page:03d}.png'
        with Image.open(work / record['image']) as native:
            image = native.convert('L').resize(((native.width + 1) // 2, (native.height + 1) // 2), Image.Resampling.LANCZOS)
            size = [image.width / native.width, image.height / native.height]
            image.save(path, dpi=(300, 300))
        references, regions = [], []
        for line in lines:
            boxes = np.rint(np.asarray([ref['native_box'] for ref in line]) * (size + size)).astype(int)
            for box, ref in zip(boxes, line):
                references.append({'box': box.tolist(), 'role': ref['role'], 'text': ref['text']})
            regions.append((boxes[:, :2].min(0) - 9, boxes[:, 2:].max(0) + 9))
        yield dict(article=article, page=page, split=record['split'], image=str(path), regions=regions,
                   references=references, coverage='dense')


def read_cases(work, legacy, prototype, tess, base, hocr, jobs):
    def recognize(case):
        result = command(tess, base, case['image'], case.get('psm', 3), '--dpi', '300', *NATIVE_FLAGS, hocr)
        glyphs = parse_hocr(result.stdout)
        refs = case['references']
        if regions := case.pop('regions', None):
            centers = np.array([(np.asarray(g['box'][:2]) + g['box'][2:]) / 2 for g in glyphs]).reshape(-1, 2)
            keep = np.zeros(len(glyphs), dtype=bool)
            for low, high in regions:
                keep |= ((centers >= low) & (centers <= high)).all(axis=1)
            glyphs = [glyph for glyph, inside in zip(glyphs, keep) if inside]
        pairs = match_points(refs, glyphs) if case['coverage'] == 'points' else match_boxes(refs, glyphs)
        return {**case, 'ocr': glyphs, 'pairs': pairs}

    cases = legacy_cases(legacy, prototype)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        data = []
        for row in pool.map(recognize, list(gtdb_cases(work)) + cases):
            data.append(row)
            print(row['article'], Path(row['image']).name, len(row['references']), len(row['pairs']), flush=True)
    (work / 'ocr-mixed.json').write_text(json.dumps(data, ensure_ascii=False) + '\n')


def grouped(rows):
    result = defaultdict(list)
    for row in rows:
        result[row['article']].append(row)
    return result


def macro_thresholds(dev, preserve_native):
    thresholds = {}
    groups = grouped(dev)
    for role in ROLES[1:]:
        index = ROLES.index(role)
        cases = []
        for rows in groups.values():
            expected_count = sum(ref['role'] == role for row in rows for ref in row['references'])
            candidates = []
            native = Counter()
            for row in rows:
                truth = {p: row['references'][i]['role'] for i, p in pairs_for(row).items()}
                for i, glyph in enumerate(row['ocr']):
                    correct = truth.get(i) == role
                    if preserve_native and glyph['role'] != 'base':
                        if glyph['role'] == role:
                            native.update(correct=int(correct), predicted=1)
                    elif np.argmax(glyph['probabilities']) == index:
                        candidates.append((glyph['probabilities'][index], int(correct)))
            cases.append((expected_count, native['correct'], native['predicted'], candidates))
        best = (-1, 1.01)
        for threshold in [*(i / 100 for i in range(30, 100)), 1.01]:
            values = []
            for n, tp, predicted, candidates in cases:
                selected = [correct for score, correct in candidates if score >= threshold]
                count = n + predicted + len(selected)
                if count:
                    values.append(2 * (tp + sum(selected)) / count)
            best = max(best, (mean(values), threshold))
        thresholds[role] = best[1]
    return thresholds


def aggregate(per_source, dense):
    report = {}
    for role in ROLES[1:]:
        report[role] = {}
        metrics = {'placement_correct': ('placement_correct', 'reference'),
                   'joint_correct': ('joint_correct', 'identity_reference'),
                   'identity_correct': ('identity_correct', 'identity_reference')}
        if dense:
            metrics['placement_precision'] = ('placement_correct', 'predicted')
        for name, (key, denominator) in metrics.items():
            scores = [(row[role][key], row[role][denominator]) for row in per_source.values() if row[role][denominator]]
            macro = [correct / total for correct, total in scores]
            drops = [mean(macro[:i] + macro[i+1:]) for i in range(len(macro))] if len(macro) > 1 else []
            report[role][name] = dict(correct=sum(correct for correct, _ in scores), total=sum(total for _, total in scores),
                                     source_count=len(macro), macro=mean(macro) if macro else None,
                                     omit_one_range=[min(drops), max(drops)] if drops else None)
    return report


def paired_deltas(before, after, metric, denominator):
    result = {}
    for role in ROLES[1:]:
        deltas = {}
        for source, stats in before.items():
            count = stats[role][denominator]
            if count:
                deltas[source] = (after[source][role][metric] - stats[role][metric]) / count
        values = list(deltas.values())
        leave_one_out = [mean(values[:i] + values[i + 1:]) for i in range(len(values))] if len(values) > 1 else []
        result[role] = dict(by_source=deltas, macro=mean(values) if values else None,
                            omit_one_range=[min(leave_one_out), max(leave_one_out)] if leave_one_out else None)
    return result


def score_glyphs(gtdb, work, tess, models, jobs):
    items = [(root / row['image'], row) for root in (gtdb, work) for row in load(root / 'glyph-manifest.json')
             if row['split'] == 'test']
    result = {}
    for name, model in models.items():
        reads = batch_identity(tess, model, [path for path, _ in items], jobs)
        per_source = defaultdict(lambda: defaultdict(Counter))
        for (_, row), text in zip(items, reads):
            per_source[row['article']][row['role']].update(reference=1, correct=int(text == row['text']))
        result[name] = per_source
    return result


def evaluate(work, gtdb, tess, base, candidate, jobs):
    new = load(work / 'ocr-mixed.json')
    old = [row for row in load(gtdb / 'ocr-lines.json') if row['split'] != 'test']
    old_test = load(gtdb / 'ocr-pages.json')
    for row in old_test:
        row.update(coverage='dense', image=str(gtdb / row['image']))
    train = [row for row in old + new if row['split'] == 'train']
    dev = [row for row in old + new if row['split'] == 'dev']
    fresh = [row for row in new if row['split'] == 'test' and row['coverage'] == 'dense']
    legacy = [row for row in new if row['split'] == 'test' and row['coverage'] in ('points', 'control')]
    test = old_test + fresh + legacy
    source_sets = [{row['article'] for row in group} for group in (train, dev, test)]
    if sum(map(len, source_sets)) != len(set.union(*source_sets)):
        raise ValueError('Source overlap between training, development and test sets')
    prior = load(gtdb / 'placement-model.json')
    apply_placement(test, prior)
    label_placements(test, prior['thresholds'])
    for row in test:
        for glyph in row['ocr']:
            glyph['prior'] = glyph['learned']
    model = fit_placement(train, balance_sources=True)
    apply_placement(dev + test, model)
    model['thresholds'] = macro_thresholds(dev, preserve_native=False)
    model['added_thresholds'] = macro_thresholds(dev, preserve_native=True)
    label_placements(test, model['thresholds'])
    for row in test:
        for glyph in row['ocr']:
            role = ROLES[np.argmax(glyph['probabilities'])]
            extra = role if glyph['probabilities'][ROLES.index(role)] >= model['added_thresholds'].get(role, 0) else 'base'
            glyph['combined'] = glyph['role'] if glyph['role'] != 'base' else extra
    save(work / 'placement-model.json', model)
    reread(test, work, tess, base, candidate, jobs, role_keys=('role', 'learned', 'combined'))
    policies = {'native': 'role', 'prior': 'prior', 'learned': 'learned', 'combined': 'combined'}
    result = {'sources': {name: sorted(group) for name, group in zip(('train', 'dev', 'test'), source_sets)},
              'thresholds': model['thresholds'],
              'added_thresholds': model['added_thresholds'], 'sets': {}}
    for name, rows in [('fresh-gtdb', fresh), ('all-gtdb', old_test + fresh), ('legacy', legacy)]:
        result['sets'][name] = stage = {'pages': len(rows), 'policies': {}}
        for policy, key in policies.items():
            for read in ('text', 'starter_reread', 'trained_reread'):
                if policy == 'prior' and read != 'text':
                    continue
                by_source = {source: summarize(items, key, read) for source, items in grouped(rows).items()}
                summary = aggregate(by_source, dense=name != 'legacy')
                values = {'sources': by_source, 'summary': summary}
                if name == 'legacy':
                    values['control_flags'] = {row['image']: sum(glyph[key] != 'base' for glyph in row['ocr'])
                                               for row in rows if row['coverage'] == 'control'}
                else:
                    values['edits'] = count_edits(rows, key, read)
                stage['policies'][f'{policy}:{read}'] = values
        outputs = stage['policies']
        stage['paired_deltas'] = {
            'prior_placement_vs_native': paired_deltas(outputs['native:text']['sources'], outputs['prior:text']['sources'],
                                                      'placement_correct', 'reference'),
            'learned_placement_vs_native': paired_deltas(outputs['native:text']['sources'], outputs['learned:text']['sources'],
                                                        'placement_correct', 'reference'),
            'combined_placement_vs_native': paired_deltas(outputs['native:text']['sources'], outputs['combined:text']['sources'],
                                                         'placement_correct', 'reference'),
            'mixed_vs_starter_at_combined_placement': paired_deltas(outputs['combined:starter_reread']['sources'],
                                                                   outputs['combined:trained_reread']['sources'],
                                                                   'joint_correct', 'identity_reference'),
        }
    save(work / 'mixed-metrics.json', result)
    save(work / 'isolated-by-source.json', score_glyphs(gtdb, work, tess, {'base': base, 'round1': gtdb / 'run-2000',
                                                               'mixed': candidate}, jobs))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=('ocr', 'evaluate'))
    parser.add_argument('work', type=Path)
    parser.add_argument('--gtdb', type=Path)
    parser.add_argument('--legacy', type=Path)
    parser.add_argument('--prototype', type=Path)
    parser.add_argument('--tesseract', type=Path, required=True)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--hocr', type=Path)
    parser.add_argument('--candidate', type=Path)
    parser.add_argument('--jobs', type=int, default=3)
    args = parser.parse_args()
    if args.stage == 'ocr':
        if None in (args.legacy, args.prototype, args.hocr):
            parser.error('ocr requires --legacy, --prototype, and --hocr')
        read_cases(args.work.resolve(), args.legacy.resolve(), args.prototype.resolve(), args.tesseract,
                   args.base, args.hocr, args.jobs)
    else:
        if None in (args.gtdb, args.candidate):
            parser.error('evaluate requires --gtdb and --candidate')
        evaluate(args.work.resolve(), args.gtdb.resolve(), args.tesseract, args.base, args.candidate, args.jobs)


if __name__ == '__main__':
    main()
