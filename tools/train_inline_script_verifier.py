#!/usr/bin/env python3
"""Train and measure a glyph verifier and a crop-conditioned inline-script rereader."""

import argparse
import json
import random
from collections import Counter, defaultdict
from pathlib import Path
from statistics import mean
from tempfile import TemporaryDirectory

import cv2
import numpy as np
from PIL import Image

from benchmark_gtdb_inline_scripts import ROLES, batch_identity
from gtdb_inline_scripts import padded_glyph
from inline_script_recovery import ALPHANUMERIC, apply_policy
from inline_script_verifier import (candidate_vector, choose_reader, classifier, clip, decision,
                                    kind, probabilities, proposals, reading_vector, score_items)
from train_inline_script_recovery import macro, source_scores


def read(path):
    return json.loads(path.read_text())


def save(path, content):
    path.write_text(json.dumps(content, indent=2, ensure_ascii=False) + '\n')


def load_cases(path):
    cases = read(path)
    for case in cases:
        case['image'] = str((path.parent / case['image']).resolve())
    return cases


def annotate(rows, attachment, model=None):
    for row in rows:
        row['pairs'] = {int(i): j for i,j in row['pairs'].items()}
        row['original_pairs'] = {int(i): j for i,j in row['original_pairs'].items()}
        items = row['candidates']
        proposals(items, attachment)
        apply_policy(items, attachment['policies']['balanced'])
        for item in items:
            item['flags'] = {'native': item['role'], 'round3': item['placement']}
            item['anchors'] = {'round3': item.get('anchor')}
        if model:
            score_items(items, row['ocr'], model)
            for item in items:
                for name, policy in model['policies'].items():
                    item['flags'][name], item['anchors'][name] = decision(item, policy)


def fit_verifiers(rows):
    by_role = defaultdict(list)
    hard = Counter()
    for row in rows:
        inv = {candidate: i for i, candidate in row['pairs'].items()}
        for i, item in enumerate(row['candidates']):
            if not item['proposal']:
                continue
            likelihood, role, anchor = item['proposal']
            ref = row['references'][inv[i]] if i in inv else None
            correct = False
            if ref and ref['role'] == role:
                parent = row['original_pairs'].get(ref.get('parent'))
                if parent is None:
                    continue
                correct = parent == anchor
            priority = 1 + 2.5 * (not correct and likelihood >= .65)
            if priority > 1:
                hard[(role, kind(item))] += 1
            by_role[role].append((candidate_vector(item, row['ocr']), int(correct), row['article'], priority))
    models, counts = {}, {}
    for role in ROLES[1:]:
        cases = by_role[role]
        if not cases:
            raise ValueError(f'No eligible {role} candidates for training')
        vectors, gold, sources, priorities = zip(*cases)
        size, outcome = Counter(sources), Counter(gold)
        if min(outcome.values()) < 10 or len(outcome) != 2:
            raise ValueError(f'Need balanced positive and negative {role} examples')
        weights = [p / np.sqrt(size[src] * outcome[y]) for src, y, p in zip(sources, gold, priorities)]
        models[role] = classifier(vectors, gold, weights)
        counts[role] = {'positive': outcome[1], 'negative': outcome[0],
                        'hard_decoded_negatives': hard[role,'decoded'], 'hard_recovery_negatives': hard[role,'recovered']}
    return models, counts


def calibrate(rows, preserve):
    by_source = defaultdict(lambda: {r: {'refs': 0, 'tp': 0, 'flags': 0, 'decoded': [], 'recovered': []} for r in ROLES[1:]})
    for row in rows:
        summary = by_source[row['article']]
        matched = {j: row['references'][i]['role'] for i, j in row['pairs'].items()}
        for ref in row['references']:
            if ref['role'] != 'base':
                summary[ref['role']]['refs'] += 1
        for i,item in enumerate(row['candidates']):
            if item['role'] != 'base':
                rec = summary[item['role']]
                rec['flags'] += 1
                rec['tp'] += matched.get(i) == item['role']
                if preserve:
                    continue
            if item['proposal']:
                role = item['proposal'][1]
                summary[role][kind(item)].append((item['verifier_score'], int(matched.get(i) == role)))
    cutoffs, targets = {}, {}
    cut = np.append(np.arange(.22, 1.0, .025), [1.01])
    for role in ROLES[1:]:
        articles = [v[role] for v in by_source.values()]
        native = [v['tp']/v['flags'] for v in articles if v['flags']]
        minimum = mean(native) if native else .9
        tables = []
        for v in articles:
            def sweep(key):
                p = np.asarray(v[key]).reshape(-1,2)
                mask = p[:,0,None] >= cut
                return (mask * p[:,1,None]).sum(0), mask.sum(0)
            td, nd = sweep('decoded')
            tr, nr = sweep('recovered')
            tp = td[:,None] + tr[None,:] + (v['tp'] if preserve else 0)
            flags = nd[:,None] + nr[None,:] + (v['flags'] if preserve else 0)
            tables.append((v['refs'], tp, flags))
        best = (-1, 1.01, 1.01)
        for i, decoded in enumerate(cut):
            for j, recovered in enumerate(cut):
                precision, value = [], []
                for n,tp,flags in tables:
                    a,b = tp[i,j],flags[i,j]
                    if b:
                        precision.append(a/b)
                    if n:
                        value.append(a/n if preserve else 2*a/(n+b))
                if not value or (preserve and (not precision or mean(precision) < minimum - 1e-10)):
                    continue
                best = max(best, (mean(value), float(decoded), float(recovered)))
        cutoffs[role] = {'decoded': best[1], 'recovered': best[2]}
        targets[role] = minimum
    return {'preserve_native': preserve, 'thresholds': cutoffs, 'native_dev_precision': targets}


def flag_results(rows):
    sparse = bool(rows) and all(row.get('coverage') in ('points','control') for row in rows)
    output = {}
    for policy in ('native','round3','review','precision'):
        scores = source_scores(rows, policy, 'text')
        control = defaultdict(Counter)
        for row in rows:
            for i,j in row['pairs'].items():
                if row['references'][i]['role']=='base':
                    c = control[row['article']]
                    c['base'] += 1
                    c['false_base_flags'] += row['candidates'][j]['flags'][policy] != 'base'
            if row.get('coverage') == 'control':
                control[row['article']]['control_flags'] += sum(v['flags'][policy] != 'base' for v in row['candidates'])
        output[policy] = dict(per_source=scores, macro=macro(scores, sparse), controls=control)
    return output


def collect_readings(rows, engine, starter, adapted, jobs, limit=None):
    collection = defaultdict(list)
    for i, row in enumerate(rows):
        for target, index in row['pairs'].items():
            ref = row['references'][target]
            item = row['candidates'][index]
            if ref['role'] != 'base' and ref.get('text') and ALPHANUMERIC.fullmatch(ref['text']):
                label = ref['role']
                if any(item['flags'][p] == label for p in ('round3','review')):
                    collection[row['article'], label].append((i, index, ref['text']))
    rng = random.Random(8337)
    selected = defaultdict(list)
    for (source, role), values in sorted(collection.items()):
        for i, index, text in rng.sample(values, min(len(values), limit or len(values))):
            selected[i].append((source, role, index, text))
    paths, targets, quality = [], [], []
    with TemporaryDirectory() as folder:
        for i, values in selected.items():
            row = rows[i]
            with Image.open(row['image']) as src:
                image = src.convert('L')
            for source, role, index, text in values:
                item = row['candidates'][index]
                raw, padded = clip(image,item['box']), padded_glyph(image,item['box'])
                if raw is None or padded is None:
                    continue
                path = Path(folder) / f'{len(paths):07}.png'
                padded.save(path, dpi=(300,300))
                paths.append(path)
                quality.append((raw,item))
                targets.append(dict(source=source, role=role, expected=text, original=item['text'],
                                    kind=kind(item), selected=[p for p in ('round3','review') if item['flags'][p]==role]))
        base = batch_identity(engine, starter, paths, jobs)
        trained = batch_identity(engine, adapted, paths, jobs)
    for row,(raw,item),a,b in zip(targets,quality,base,trained,strict=True):
        row.update(starter=a,adapted=b,features=reading_vector(raw,item,row['role'],a,b))
    return targets


def rerender(row, reader):
    selected = row[reader]
    return selected if ALPHANUMERIC.fullmatch(selected) else row['original']


def fit_router(train, dev):
    values = []
    for row in train:
        a,b = (rerender(row,name)==row['expected'] for name in ('starter','adapted'))
        if a != b and ALPHANUMERIC.fullmatch(row['adapted']):
            values.append((row['features'], int(b), row['source']))
    out = {'samples': len(values), 'thresholds': {'sub': 1.01, 'sup': 1.01}}
    if not values or len({label for _,label,_ in values}) != 2:
        return out
    x,y,src = zip(*values)
    sizes = Counter(src)
    out['classifier'] = classifier(x,y,[1/np.sqrt(sizes[s]) for s in src],penalty=.04)
    for role in ROLES[1:]:
        dev_rows = [r for r in dev if r['role']==role and r['starter'] != r['adapted'] and ALPHANUMERIC.fullmatch(r['adapted'])]
        prediction = probabilities([r['features'] for r in dev_rows],out['classifier'])
        sources = Counter(r['source'] for r in dev if r['role']==role)
        best = (0,1.01)
        for t in np.arange(.35,.991,.025):
            gains = sum(((rerender(r,'adapted')==r['expected'])-(rerender(r,'starter')==r['expected']))/sources[r['source']]
                        for r,p in zip(dev_rows,prediction) if p>=t)
            best = max(best,(gains,float(t)))
        out['thresholds'][role] = best[1]
    return out


def rereader_results(rows, router):
    result = {}
    for policy in ('round3','review'):
        result[policy] = by_role = {}
        for role in ROLES[1:]:
            by_source = defaultdict(Counter)
            for row in rows:
                if role != row['role'] or policy not in row['selected']:
                    continue
                routed = choose_reader(row,router)
                source = by_source[row['source']]
                source.update(total=1,starter=int(rerender(row,'starter')==row['expected']),
                              adapted=int(rerender(row,'adapted')==row['expected']),
                              routed=int(rerender(row,routed)==row['expected']),
                              adapted_selected=int(routed=='adapted'))
            by_role[role] = {'sources':by_source,
                             'macro':{k:mean(c[k]/c['total'] for c in by_source.values()) if by_source else None
                                      for k in ('starter','adapted','routed')},
                             'total':sum(by_source.values(),Counter())}
    return result


def evaluate(work, prior, engine, starter, adapted, jobs):
    attachment = read(prior/'attachment-model.json')
    examples = load_cases(work/'data/example-candidates.json')
    train = load_cases(prior/'train-candidates.json') + [r for r in examples if r['split']=='train']
    annotate(train, attachment)
    classifiers, counts = fit_verifiers(train)
    dev = load_cases(prior/'dev-candidates.json')
    annotate(dev, attachment)
    model = dict(classifiers=classifiers, training=counts)
    for row in dev:
        score_items(row['candidates'], row['ocr'], model)
    model['policies'] = {'review':calibrate(dev,False),'precision':calibrate(dev,True)}
    save(work/'verifier-model.json',model)
    results = {'examples':{f'{ds}/{s}':sum(r['dataset']==ds and r['split']==s for r in examples)
                           for ds,s in sorted({(r['dataset'],r['split']) for r in examples})},'training':counts,'placement':{}}
    for row in train:
        for item in row['candidates']:
            item['flags']['review'] = item['flags']['round3']
    readings = collect_readings(train,engine,starter,adapted,jobs,limit=160)
    del train
    dev += [r for r in examples if r['split']=='dev']
    annotate(dev,attachment,model)
    calibration = collect_readings(dev,engine,starter,adapted,jobs)
    model['router'] = fit_router(readings,calibration)
    save(work/'verifier-model.json',model)
    save(work/'reader-training.json',{'train':readings,'dev':calibration})
    print('fit verifier and reader',len(readings),len(calibration),flush=True)
    del dev,readings,calibration
    results['policies'] = model['policies']
    results['router'] = model['router']
    for name,source in [('frozen-gtdb','test'),('frozen-fresh','fresh'),('earlier-books','legacy'),('ibem',None),('infty-examples',None)]:
        if source:
            rows = load_cases(prior/f'{source}-candidates.json')
            if name=='frozen-fresh':
                rows = [row for row in rows if row['coverage']=='GTDB-2']
        else:
            rows = [r for r in examples if r['split']==('test' if name=='ibem' else 'diagnostic')]
        annotate(rows,attachment,model)
        placement = flag_results(rows)
        observations = collect_readings(rows,engine,starter,adapted,jobs)
        results['placement'][name] = dict(rows=len(rows),documents=len({r['article'] for r in rows}),
                                          placement=placement,rereaders=rereader_results(observations,model['router']))
        save(work/'verifier-metrics.json',results)
        print(name,len(rows),flush=True)
        del rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('work',type=Path)
    for name in ('prior','tesseract','starter','adapted'):
        parser.add_argument(f'--{name}',type=Path,required=True)
    parser.add_argument('--jobs',type=int,default=4)
    args=parser.parse_args()
    cv2.setNumThreads(1)
    evaluate(args.work.resolve(),args.prior.resolve(),args.tesseract,args.starter,args.adapted,args.jobs)


if __name__ == '__main__':
    main()
