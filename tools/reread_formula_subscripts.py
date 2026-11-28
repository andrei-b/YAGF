#!/usr/bin/env python3
"""Reread a subscript next to its base using labeled glyphs from the same typeface."""

import argparse
import csv
import json
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

import cv2
import numpy as np

from reread_formula_symbols import render


EDGES = ('left', 'top', 'right', 'bottom')


def read_boxes(path):
    with path.open(newline='') as stream:
        return list(csv.DictReader(stream, delimiter='\t'))


def edges(row):
    return tuple(int(row[key]) for key in EDGES)


def ink_crop(image, rect):
    x, y, r, b = rect
    if not (0 <= x < r <= image.width and 0 <= y < b <= image.height):
        raise ValueError(f'Box {rect} lies outside the {image.width} by {image.height} render')
    grey = np.asarray(image.crop(rect).convert('L'))
    return cv2.threshold(grey, 0, 255, cv2.THRESH_BINARY_INV | cv2.THRESH_OTSU)[1]


def descriptor(ink):
    """Fit ink to a fixed square without stretching narrow characters."""
    x, y, w, h = cv2.boundingRect(ink)
    if min(w, h) < 2:
        return None
    scale = 32 / max(w, h)
    width, height = max(1, round(w * scale)), max(1, round(h * scale))
    glyph = cv2.resize(ink[y:y+h, x:x+w], (width, height), interpolation=cv2.INTER_AREA)
    canvas = np.zeros((36, 36), np.float32)
    left, top = (36-width)//2, (36-height)//2
    canvas[top:top+height, left:left+width] = glyph / 255
    # Downweight one-source-pixel differences along the edge of a printed stroke.
    return cv2.GaussianBlur(canvas, (5, 5), 1.0)


def script_component(ink):
    """Find a separate smaller component at the lower right of the selected base."""
    _, labels, stats, _ = cv2.connectedComponentsWithStats(ink)
    components = [(index, rect) for index, rect in enumerate(stats[1:], 1)
                  if rect[cv2.CC_STAT_AREA] >= 4]
    if len(components) < 2:
        return None
    index, (x, y, w, h, _) = max(components, key=lambda item: item[1][0] + item[1][2])
    # Group fragments of the base, e.g. the two strokes of a poorly printed psi.
    base = [rect for i, rect in components if i != index and rect[0] < x]
    if not base or len(base) + 1 != len(components):
        return None
    top = min(r[1] for r in base)
    right = max(r[0] + r[2] for r in base)
    height = max(r[1] + r[3] for r in base) - top
    # A lowered digit can be as tall as an x-height letter such as omega.
    if (h > 1.20 * height or h < 0.20 * height or y - top < height * 0.18
            or x < right - 0.35*w or x > right + height*0.55):
        return None
    mask = np.uint8(labels[y:y+h, x:x+w] == index) * 255
    return (int(x), int(y), int(x+w), int(y+h)), mask


def load_exemplars(table, directory, size, work):
    groups = {}
    locations = set()
    for row in read_boxes(table):
        pdf = (directory / row['pdf']).resolve()
        page = int(row.get('page') or 1)
        location = (pdf, page, edges(row))
        if page < 1 or location in locations:
            raise ValueError(f'Invalid page or duplicate exemplar in {row}')
        locations.add(location)
        groups.setdefault((pdf, page), []).append(row)
    bank = {}
    for (pdf, page), rows in groups.items():
        with render(pdf, page, size, work/'sample') as image:
            for row in rows:
                shape = descriptor(ink_crop(image, edges(row)))
                if shape is None or not row['label'].strip():
                    raise ValueError(f'Empty exemplar or label in {row}')
                bank.setdefault(row['label'], []).append(shape)
    if len(bank) < 2:
        raise ValueError('Include examples of at least two different labels')
    return bank


def compare(ink, bank):
    glyph = descriptor(ink)
    if glyph is None:
        return None
    # A small translation compensates for edge quantization in the original scan.
    padded = np.pad(glyph, 2)
    scores = {}
    for label, examples in bank.items():
        scores[label] = sorted((float(cv2.matchTemplate(padded, example, cv2.TM_CCORR_NORMED).max())
                                for example in examples), reverse=True)
    first, second = sorted(scores, key=lambda key: scores[key][0], reverse=True)[:2]
    best = scores[first]
    margin = best[0] - scores[second][0]
    # Require a second independent specimen as well as separation from competing labels.
    supported = len(best) >= 2 and best[1] >= 0.88 and best[0] >= 0.90 and margin >= 0.04
    return dict(suggested=first if supported else None, closest=first, runner_up=second,
                similarity=round(best[0], 4), margin=round(margin, 4))


def reread(image, rect, bank):
    result = dict(pair_box=list(rect), suggested=None)
    component = script_component(ink_crop(image, rect))
    if component is None:
        return {**result, 'status': 'no_separate_subscript'}
    box, ink = component
    result['glyph_box'] = [box[i] + rect[i % 2] for i in range(4)]
    match = compare(ink, bank)
    if match is not None:
        result.update(match)
    result['status'] = 'matched' if result['suggested'] is not None else 'uncertain'
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pdf', type=Path)
    parser.add_argument('--page', type=int, default=1)
    parser.add_argument('--render-size', type=int, default=2900, help='Long edge in pixels for all boxes')
    select = parser.add_mutually_exclusive_group(required=True)
    select.add_argument('--pair', nargs=4, type=int, action='append', metavar=('LEFT','TOP','RIGHT','BOTTOM'),
                        help='Box around one base and its subscript in render coordinates. Repeat for more pairs.')
    select.add_argument('--pairs', type=Path, help='TSV with pdf and pixel edges. Read rows for this PDF and page.')
    parser.add_argument('--exemplars', type=Path, required=True, help='TSV with pdf, label and pixel edges of each isolated exemplar')
    parser.add_argument('--exemplar-dir', type=Path, help='Directory containing exemplar PDFs (default: input PDF directory)')
    args = parser.parse_args()
    if args.page < 1 or args.render_size < 1:
        parser.error('Page and render size must be positive')
    try:
        rects = args.pair or [edges(row) for row in read_boxes(args.pairs)
                              if row['pdf'] == args.pdf.name and int(row.get('page') or 1) == args.page]
        if not rects:
            parser.error('No selected pairs on this page')
        with TemporaryDirectory() as folder:
            work = Path(folder)
            bank = load_exemplars(args.exemplars, args.exemplar_dir or args.pdf.parent, args.render_size, work)
            with render(args.pdf, args.page, args.render_size, work/'input') as image:
                rows = [reread(image, rect, bank) for rect in rects]
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    print(json.dumps(dict(pdf=str(args.pdf), page=args.page, render_size=args.render_size, readings=rows),
                     indent=2, ensure_ascii=False))


if __name__ == '__main__':
    main()
