#!/usr/bin/env python3
"""Build controlled glyph cases from IBEM and the published InftyCDB-1 example figure."""

import argparse
import json
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageOps

from benchmark_gtdb_inline_scripts import NATIVE_FLAGS, command, parse_hocr
from train_inline_script_recovery import recover_case


def components(image):
    _, mask = cv2.threshold(np.asarray(image), 0, 1, cv2.THRESH_BINARY_INV | cv2.THRESH_OTSU)
    _, _, stats, _ = cv2.connectedComponentsWithStats(mask, connectivity=8)
    return sorted(([int(x), int(y), int(x + w), int(y + h)] for x, y, w, h, n in stats[1:]
                   if n >= 5 and w >= 2 and h >= 2), key=lambda box: box[0] + box[2])


def assign(image, tokens):
    boxes = components(image)
    if len(boxes) != len(tokens):
        return None
    for i, target in enumerate(tokens):
        if target['role'] == 'base':
            continue
        base = boxes[target['parent']]
        low = boxes[i]
        height = base[3] - base[1]
        center_delta = ((low[1] + low[3]) - (base[1] + base[3])) / (2 * height)
        if low[3] - low[1] > height * 1.18:
            return None
        if not (.12 <= (center_delta if target['role'] == 'sub' else -center_delta) <= .95):
            return None
    return [{**token, 'box': box} for token, box in zip(tokens, boxes, strict=True)]


def make_case(path, refs, article, split, dataset, tess, base, hocr):
    cli = command(tess, base, path, 7, '--dpi', '300', *NATIVE_FLAGS, hocr)
    return recover_case(dict(article=article, split=split, coverage='boxes', dataset=dataset,
                             image=str(path), references=refs, ocr=parse_hocr(cli.stdout)))


def ibem_page(job):
    root, row, tess, base, hocr = job
    cv2.setNumThreads(1)
    with Image.open(root / row['image']) as source:
        page = source.convert('L')
    out = []
    for n, example in enumerate(row['examples']):
        left, top, right, bottom = example['box']
        box = (max(0, int(left * page.width / 100) - 1), max(0, int(top * page.height / 100) - 1),
               min(page.width, int(np.ceil(right * page.width / 100)) + 1),
               min(page.height, int(np.ceil(bottom * page.height / 100)) + 1))
        if box[0] >= box[2] or box[1] >= box[3]:
            continue
        crop = ImageOps.expand(page.crop(box), 14, fill=255)
        refs = assign(crop, example['tokens'])
        if refs is None:
            continue
        path = root / 'examples' / f'ibem-{row["page"]}-{n:02d}.png'
        crop.save(path, dpi=(300,300))
        case = make_case(path, refs, f'ibem-{row["document"]}', row['split'], 'IBEM', tess, base, hocr)
        case['page'] = row['page']
        case['latex'] = example['latex']
        case['page_box'] = box
        out.append(case)
    return out


def infty_examples(root, tess, base, hocr):
    with Image.open(root / 'infty-objects/image-0037.png') as source:
        page = ImageOps.flip(source.convert('L'))
    specimens = [
        ('normal-clean', (10, 9, 393, 93), 'approaches'),
        ('normal-fragmented', (10, 95, 393, 175), 'approaches'),
        ('formula-spaced', (10, 193, 388, 275), 'DΩ[u]<∞'),
        ('formula-tight', (10, 277, 389, 358), 'DΩ[u]<∞'),
    ]
    for name, box, text in specimens:
        crop = page.crop(box)
        labels = [{'text': ch, 'role': 'base'} for ch in text]
        if 'Ω' in text:
            labels[1].update(role='sub', parent=0)
        refs = assign(crop, labels)
        if refs is None:
            raise ValueError(f'Cannot align published InftyCDB-1 specimen {name}')
        scaled = crop.resize(tuple(round(value * .62) for value in crop.size), Image.Resampling.LANCZOS)
        sx, sy = scaled.width / crop.width, scaled.height / crop.height
        for row in refs:
            row['box'] = [round(value * scale) + 16 for value, scale in zip(row['box'], [sx, sy, sx, sy])]
        crop = ImageOps.expand(scaled, 16, fill=255)
        path = root / 'examples' / f'infty-{name}.png'
        crop.save(path, dpi=(300,300))
        yield make_case(path, refs, 'infty-cdb1-published-figure', 'diagnostic', 'InftyCDB-1 example figure', tess, base, hocr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('data', type=Path)
    for name in ('tesseract','base','hocr'):
        parser.add_argument(f'--{name}', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=3)
    args = parser.parse_args()
    root = args.data.resolve()
    (root / 'examples').mkdir(exist_ok=True)
    subset = json.loads((root / 'ibem-subset.json').read_text())
    jobs = [(root, row, args.tesseract, args.base, args.hocr) for row in subset]
    cv2.setNumThreads(1)
    cases = []
    with ProcessPoolExecutor(args.jobs) as pool:
        for page, rows in zip(subset, pool.map(ibem_page, jobs)):
            print(page['split'], page['page'], len(rows), flush=True)
            cases.extend(rows)
    cases.extend(infty_examples(root, args.tesseract, args.base, args.hocr))
    (root / 'example-candidates.json').write_text(json.dumps(cases, ensure_ascii=False) + '\n')
    print(Counter((r['dataset'], r['split']) for r in cases), flush=True)


if __name__ == '__main__':
    main()
