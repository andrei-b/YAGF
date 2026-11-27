#!/usr/bin/env python3
"""Prepare new document-level holdouts for the script recovery experiment."""

import argparse
import json
import subprocess
import xml.etree.ElementTree as ET
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

from benchmark_gtdb_inline_scripts import NATIVE_FLAGS, command, parse_hocr
from gtdb_inline_scripts import process_page, read_pages
from train_inline_script_recovery import add_parents, recover_case


# Freeze document and page selections before observing any OCR results.
PDF_PAGES = {'adam': (2, 3, 4, 5), 'attention': (3, 4, 5, 6), 'diffusion': (2, 3, 4, 5)}


def raster(pdf, page, dpi, destination):
    subprocess.run(['pdftoppm', '-gray', '-r', str(dpi), '-f', str(page), '-l', str(page),
                    '-singlefile', '-png', str(pdf), str(destination.with_suffix(''))], check=True)


def tight_box(image, box):
    l, t, r, b = box
    l, r = np.clip([l, r], 0, image.shape[1])
    t, b = np.clip([t, b], 0, image.shape[0])
    rows, columns = np.nonzero(image[t:b, l:r] < 150)
    if len(columns) < 3:
        return None
    return [int(l + columns.min()), int(t + rows.min()), int(l + columns.max() + 1), int(t + rows.max() + 1)]


def pdf_lines(pdf, page, raster_image):
    process = subprocess.run(['mutool', 'draw', '-F', 'stext', '-o', '-', str(pdf), str(page)],
                             check=True, capture_output=True)
    xml = ET.fromstring(process.stdout).find('page')
    if xml is None:
        return []
    sx, sy = raster_image.width / float(xml.attrib['width']), raster_image.height / float(xml.attrib['height'])
    pixels = np.asarray(raster_image)
    result = []
    for line in xml.iter('line'):
        if line.get('dir') != '1 0':
            continue
        glyphs = []
        for font in line.findall('font'):
            size = float(font.attrib['size'])
            for char in font.findall('char'):
                text = char.attrib['c']
                if not text.strip():
                    continue
                coordinates = np.asarray(char.attrib['quad'].split(), float).reshape(4, 2)
                low, high = coordinates.min(0), coordinates.max(0)
                box = np.rint([low[0] * sx, low[1] * sy, high[0] * sx, high[1] * sy]).astype(int)
                tight = tight_box(pixels, box)
                if tight:
                    glyphs.append((tight, text, size, float(char.attrib['y'])))
        baseline_glyphs = [(round(size, 1), round(y, 1)) for _, text, size, y in glyphs if text.isalpha()]
        if len(baseline_glyphs) < 12:
            continue
        (size, baseline), support = Counter(baseline_glyphs).most_common(1)[0]
        if support < 10:
            continue
        refs = []
        parent = None
        for box, text, glyph_size, y in glyphs:
            offset = (y - baseline) / size
            if abs(offset) <= .12:
                refs.append({'box': box, 'text': text, 'role': 'base'})
                parent = len(refs) - 1
            elif .45 <= glyph_size / size <= .87 and .14 <= abs(offset) <= .72 and parent is not None:
                anchor = refs[parent]['box']
                if -3 * sx <= (box[0] - anchor[2]) <= 1.65 * size * sx:
                    refs.append({'box': box, 'text': text, 'role': 'sub' if offset > 0 else 'sup', 'parent': parent})
        if any(ref['role'] != 'base' for ref in refs):
            result.append(refs)
    return result


def restrict(glyphs, lines):
    refs, regions = [], []
    for line in lines:
        base = len(refs)
        for row in line:
            if row.get('parent') is not None:
                row['parent'] += base
            refs.append(row)
        boxes = np.asarray([ref['box'] for ref in line])
        regions.append((boxes[:, :2].min(0) - 8, boxes[:, 2:].max(0) + 8))
    filtered = []
    for glyph in glyphs:
        box = np.asarray(glyph['box'])
        center = (box[:2] + box[2:]) / 2
        if any((center >= low).all() and (center <= high).all() for low, high in regions):
            filtered.append(glyph)
    return refs, filtered


def prepare_page(job):
    work, article, page, annotations, tess, base, hocr = job
    cv2.setNumThreads(1)
    pdf = work / 'sources' / f'{article}.pdf'
    output = work / 'fresh/pages' / f'{article}-{page:03d}.png'
    if annotations:
        sheet, raw = annotations
        path = work / 'fresh/native' / output.name
        raster(pdf, page, 600, path)
        scan, _, rows = process_page((work / 'fresh', article, 'test', page, sheet, raw, path))
        if not scan.get('accepted') or not rows:
            return None
        with Image.open(path) as native:
            image = native.convert('L').resize(((native.width + 1) // 2, (native.height + 1) // 2), Image.Resampling.LANCZOS)
            sx, sy = image.width / native.width, image.height / native.height
            image.save(output, dpi=(300, 300))
        lines = []
        for row in rows:
            index = int(Path(row['image']).stem.rsplit('-', 1)[1])
            refs = [{'box': np.rint(np.asarray(ref['native_box']) * [sx, sy, sx, sy]).astype(int).tolist(),
                     'role': ref['role'], 'text': ref['text']} for ref in row['references']]
            add_parents(refs, [raw[index]])
            lines.append(refs)
        coverage = 'GTDB-2'
    else:
        raster(pdf, page, 300, output)
        with Image.open(output) as source:
            lines = pdf_lines(pdf, page, source.convert('L'))
        if not lines:
            return None
        coverage = 'PDF text metadata'
    data = command(tess, base, output, 3, '--dpi', '300', *NATIVE_FLAGS, hocr).stdout
    refs, ocr = restrict(parse_hocr(data), lines)
    if not refs:
        return None
    return recover_case(dict(article=article, page=page, split='fresh', image=str(output), references=refs, ocr=ocr,
                             coverage=coverage))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('archive', 'tesseract', 'base', 'hocr'):
        parser.add_argument(f'--{name}', type=Path, required=True)
    parser.add_argument('work', type=Path)
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    work = args.work.resolve()
    for folder in ('pages', 'native', 'lines', 'glyphs'):
        (work / 'fresh' / folder).mkdir(parents=True, exist_ok=True)
    data = read_pages(args.archive, 'Bergweiler83', 'GTDB-2')
    pages = [('bergweiler', i, data[i - 1]) for i in range(1, 37)]
    pages.extend((article, i, None) for article, values in PDF_PAGES.items() for i in values)
    jobs = [(work, article, page, annotation, args.tesseract, args.base, args.hocr)
            for article, page, annotation in pages]
    rows = []
    with ProcessPoolExecutor(args.jobs) as pool:
        for (article, page, _), row in zip(pages, pool.map(prepare_page, jobs)):
            print(article, page, 'skip' if row is None else len(row['references']), flush=True)
            if row:
                rows.append(row)
    (work / 'fresh-candidates.json').write_text(json.dumps(rows, ensure_ascii=False) + '\n')


if __name__ == '__main__':
    main()
