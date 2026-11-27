#!/usr/bin/env python3
"""Prepare GTDB prose lines and isolated glyphs for an inline-script OCR experiment."""

import argparse
import csv
import json
import random
import re
import subprocess
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from zipfile import ZipFile

import cv2
import numpy as np
from PIL import Image, ImageOps
from scipy.optimize import minimize


# Assign complete articles before fitting or scoring any model.
ARTICLES = {
    'AIF_1999_375_404': 'train',
    'ASENS_1970_273_284': 'train',
    'BSMF_1998_245_271': 'train',
    'AIF_1970_493_498': 'dev',
    'ASENS_1997_367_384': 'test',
    'BSMF_1970_165_192': 'test',
}
SCRIPT_LINKS = {1: 'sup', 2: 'sub', 3: 'sup', 4: 'sub'}


@dataclass(frozen=True)
class Glyph:
    number: int
    box: tuple[int, int, int, int]
    math: bool
    link: int
    parent: int
    code: str


def read_pages(archive, article, collection='GTDB-1'):
    with ZipFile(archive) as source:
        raw = source.read(f'{collection}/{article}.csv').decode('ascii', errors='replace')
    pages = []
    for row in csv.reader(raw.splitlines()):
        if not row:
            continue
        if row[0] == 'Sheet':
            pages.append((row[2], []))
            line = None
        elif row[0] == 'Line':
            line = []
            pages[-1][1].append(line)
        elif row[0] in ('Text', 'Image'):
            line = None
        elif row[0] == 'Chardata' and line is not None:
            number, left, top, right, bottom, mode, link, parent = map(int, row[1:9])
            if left < right and top < bottom:
                line.append(Glyph(number, (left, top, right, bottom), mode == 1, link, parent, row[9]))
    return pages


def glyph_text(code):
    # Infty codes include shape/style variants. This verified subset covers Latin
    # letters and digits in the English model. Unknown codes remain geometry labels.
    if re.fullmatch(r'(01|41)(3[0-9]|4[1-9A-F]|5[0-9A]|6[1-9A-F]|7[0-9A])', code):
        return chr(int(code[2:], 16))
    return None


def position(glyph, by_id):
    seen = set()
    while glyph.number not in seen:
        seen.add(glyph.number)
        if glyph.link in SCRIPT_LINKS:
            return SCRIPT_LINKS[glyph.link]
        if glyph.link in (5, 6):
            return 'other'
        parent = by_id.get(glyph.parent)
        if glyph.link != 0 or parent is None or parent.math != glyph.math:
            return 'base'
        glyph = parent
    return 'other'


def fit_scan(image, boxes):
    """Register the annotation boxes to the embedded scan using ink coverage."""
    if len(boxes) < 30:
        return None
    boxes = np.asarray(boxes)
    sizes = boxes[:, 2:] - boxes[:, :2]
    boxes = boxes[(sizes.min(1) >= 8) & (sizes.max(1) <= 155)]
    if len(boxes) < 30:
        return None
    sample = boxes[::max(1, len(boxes) // 1500)]
    left, top = boxes[:, :2].min(0)
    right, bottom = boxes[:, 2:].max(0)
    step, pad = 8, 40
    ink = np.asarray(image) < 160
    height, width = ink.shape
    small = cv2.resize(ink.astype(np.float32), (width // step, height // step), interpolation=cv2.INTER_AREA)
    small = np.pad(small, pad)
    template = np.zeros(((bottom - top) // step + 2, (right - left) // step + 2), np.float32)
    for x, y, u, v in boxes:
        template[(y - top) // step:(v - top + step - 1) // step,
                 (x - left) // step:(u - left + step - 1) // step] = 1

    def locate(scales):
        best_score, initial = -1, None
        for sx in scales:
            for sy in scales:
                resized = cv2.resize(template, None, fx=sx, fy=sy)
                if any(a > b for a, b in zip(resized.shape, small.shape)):
                    continue
                response = cv2.matchTemplate(small, resized, cv2.TM_CCORR_NORMED)
                _, score, _, (x, y) = cv2.minMaxLoc(response)
                if score > best_score:
                    best_score = score
                    initial = np.array([sx, sy, (x - pad) * step - left * sx, (y - pad) * step - top * sy])
        return best_score, initial

    correlation, initial = locate([1.0])
    if correlation < 0.52:
        correlation, initial = locate([0.94, 0.96, 0.98, 1.0, 1.02, 1.04, 1.06])
    if initial is None:
        return None

    integral = cv2.integral(ink.astype(np.uint8))

    def coverage(params, samples):
        x, y, u, v = transform_boxes(samples, params).T
        x, u = np.clip([x, u], 0, width)
        y, v = np.clip([y, v], 0, height)
        return (integral[v, u] + integral[y, x] - integral[v, x] - integral[y, u]) / np.maximum(1, (u - x) * (v - y))

    # A local refinement keeps the center coordinates fixed while scaling the page.
    anchor = (boxes[:, :2].min(0) + boxes[:, 2:].max(0)) / 2

    def parameters(delta):
        dx, dy, ox, oy = delta
        return initial + [dx, dy, ox - dx * anchor[0], oy - dy * anchor[1]]

    def loss(delta):
        return -float(coverage(parameters(delta), sample).mean())

    bounds = [(-0.024, 0.024), (-0.024, 0.024), (-32, 32), (-32, 32)]
    result = minimize(loss, np.zeros(4), method='Powell', bounds=bounds, options={'maxiter': 16, 'xtol': 0.0002})
    fitted = parameters(result.x) if result.fun < loss(np.zeros(4)) else initial
    densities = coverage(fitted, boxes)
    return fitted, float(densities.mean()), float((densities > 0.09).mean()), float(correlation)


def transform_boxes(boxes, parameters):
    sx, sy, tx, ty = parameters
    return np.rint(np.asarray(boxes) * [sx, sy, sx, sy] + [tx, ty, tx, ty]).astype(int)


def padded_glyph(image, box):
    left, top, right, bottom = box
    left, top = max(0, left), max(0, top)
    right, bottom = min(image.width, right), min(image.height, bottom)
    if left >= right or top >= bottom:
        return None
    crop = image.crop((left, top, right, bottom))
    ink = np.asarray(crop) < 160
    if ink.mean() < 0.10:
        return None
    scale = 42 / crop.height
    crop = crop.resize((max(1, round(crop.width * scale)), 42), Image.Resampling.LANCZOS)
    return ImageOps.expand(crop, border=18, fill=255)


def process_page(job):
    work, article, split, number, sheet, lines, source = job
    cv2.setNumThreads(1)
    image = Image.open(source).convert('L')
    fitted = fit_scan(image, [glyph.box for line in lines for glyph in line])
    scan = {'article': article, 'page': number, 'sheet': sheet, 'split': split}
    if fitted is None:
        return scan, [], []
    parameters, density, coverage, correlation = fitted
    scan.update(parameters=parameters.tolist(), density=density, coverage=coverage, correlation=correlation)
    if density < 0.22 or coverage < 0.75 or correlation < 0.48:
        return scan, [], []

    rng = random.Random(f'{article}/{number}')
    glyph_rows, line_rows = [], []
    base_budget = 16 if split == 'train' else 10
    for line_index, line in enumerate(lines):
        if sum(not glyph.math and glyph_text(glyph.code) is not None for glyph in line) < 12:
            continue
        by_id = {glyph.number: glyph for glyph in line}
        roles = [position(glyph, by_id) for glyph in line]
        types = set(roles) & {'sub', 'sup'}
        if 'other' in roles or (not types and rng.random() >= 0.10):
            continue
        projected = transform_boxes([glyph.box for glyph in line], parameters)
        left, top = projected[:, :2].min(0) - 18
        right, bottom = projected[:, 2:].max(0) + 18
        if left < 0 or top < 0 or right > image.width or bottom > image.height or bottom - top > 200:
            continue
        strip = image.crop((int(left), int(top), int(right), int(bottom)))
        strip = strip.resize(((strip.width + 1) // 2, (strip.height + 1) // 2), Image.Resampling.LANCZOS)
        x_scale, y_scale = strip.width / (right - left), strip.height / (bottom - top)
        key = f'{article}-{number:03d}-{line_index:03d}'
        path = f'lines/{key}.png'
        strip.save(work / path, dpi=(300, 300))
        references = []
        for glyph, role, box in zip(line, roles, projected):
            text = glyph_text(glyph.code)
            # Convert native scan pixels to coordinates in the half-size line crop.
            reference_box = np.rint((box - [left, top, left, top]) *
                                    [x_scale, y_scale, x_scale, y_scale]).astype(int)
            references.append({'box': reference_box.tolist(), 'native_box': box.tolist(),
                               'role': role, 'text': text})
            if text is None or (role == 'base' and (base_budget <= 0 or rng.random() > 0.035)):
                continue
            crop = padded_glyph(image, box.tolist())
            if crop is None:
                continue
            if role == 'base':
                base_budget -= 1
            name = f'{key}-{glyph.number}'
            glyph_path = f'glyphs/{name}.png'
            crop.save(work / glyph_path, dpi=(300, 300))
            glyph_rows.append({'article': article, 'split': split, 'image': glyph_path,
                               'line': path, 'role': role, 'text': text})
        line_rows.append({'article': article, 'page': number, 'split': split, 'image': path,
                          'references': references})
    scan['accepted'] = True
    return scan, glyph_rows, line_rows


def prepare(work, jobs):
    for directory in ('lines', 'glyphs', 'scans'):
        (work / directory).mkdir(exist_ok=True)
    queue = []
    for article, split in ARTICLES.items():
        pages = read_pages(work / 'GTDB-1.zip', article)
        scans = work / 'scans' / article
        scans.mkdir(exist_ok=True)
        if not (scans / f'page-{len(pages) - 1:03d}.png').is_file():
            subprocess.run(['pdfimages', '-f', '2', '-png', str(work / f'{article}.pdf'), str(scans / 'page')], check=True)
        files = sorted(scans.glob('page-*.png'))
        if len(files) != len(pages):
            raise ValueError(f'{article}: {len(files)} embedded scans for {len(pages)} annotated pages')
        for number, ((sheet, lines), image) in enumerate(zip(pages, files), 1):
            queue.append((work, article, split, number, sheet, lines, image))
    scans, glyphs, lines = [], [], []
    with ProcessPoolExecutor(max_workers=jobs) as pool:
        for scan, page_glyphs, page_lines in pool.map(process_page, queue):
            scans.append(scan)
            glyphs.extend(page_glyphs)
            lines.extend(page_lines)
            print(scan['article'], scan['page'], round(scan.get('coverage', 0), 3),
                  len(page_glyphs), len(page_lines), flush=True)
    for name, rows in [('registration', scans), ('glyph-manifest', glyphs), ('line-manifest', lines)]:
        (work / f'{name}.json').write_text(json.dumps(rows, ensure_ascii=False) + '\n', encoding='utf-8')
    print('lines', Counter(row['split'] for row in lines))
    print('glyphs', Counter((row['split'], row['role']) for row in glyphs))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('work', type=Path, help='Directory containing GTDB-1.zip and the six article PDFs')
    parser.add_argument('--jobs', type=int, default=3)
    args = parser.parse_args()
    prepare(args.work.resolve(), args.jobs)


if __name__ == '__main__':
    main()
