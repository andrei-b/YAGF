#!/usr/bin/env python3
"""Recover and reread inline scripts from a scanned page and its Tesseract hOCR."""

import argparse
import json
import re
from pathlib import Path
from tempfile import TemporaryDirectory

import cv2
import numpy as np
from PIL import Image
from scipy.spatial import cKDTree
from scipy.special import softmax

from benchmark_gtdb_inline_scripts import ROLES, batch_identity, parse_hocr
from gtdb_inline_scripts import padded_glyph


ALPHANUMERIC = re.compile(r'[A-Za-z0-9]')


def area(box):
    return max(0, box[2] - box[0]) * max(0, box[3] - box[1])


def intersection(a, b):
    return max(0, min(a[2], b[2]) - max(a[0], b[0])) * max(0, min(a[3], b[3]) - max(a[1], b[1]))


def center(box):
    return (box[0] + box[2]) / 2, (box[1] + box[3]) / 2


def line_scale(glyph):
    h = glyph['box'][3] - glyph['box'][1]
    return np.clip(h / max(0.1, glyph['features'][2]), 4, 120)


def baseline(glyph):
    return glyph['box'][1] - line_scale(glyph) * glyph['features'][0]


class ScriptCandidates:
    def __init__(self, image, ocr):
        self.ocr = ocr
        self.boxes = [row['box'] for row in ocr]
        self.scales = [line_scale(row) for row in ocr]
        self.baselines = [baseline(row) for row in ocr]
        self.tree = cKDTree(np.asarray([center(box) for box in self.boxes]).reshape(-1, 2))
        gray = np.asarray(image.convert('L'))
        _, ink = cv2.threshold(gray, 0, 1, cv2.THRESH_BINARY_INV | cv2.THRESH_OTSU)
        self.ink = cv2.integral(ink)
        _, _, stats, _ = cv2.connectedComponentsWithStats(ink, connectivity=8)
        self.components = []
        for x, y, w, h, pixels in stats[1:]:
            if w >= 2 and h >= 3 and pixels >= 5:
                self.components.append(([int(x), int(y), int(x + w), int(y + h)], int(pixels)))
        self.component_tree = cKDTree(np.asarray([center(box) for box, _ in self.components]).reshape(-1, 2))

    def density(self, box):
        left, top, right, bottom = map(int, box)
        left, right = np.clip([left, right], 0, self.ink.shape[1] - 1)
        top, bottom = np.clip([top, bottom], 0, self.ink.shape[0] - 1)
        if right <= left or bottom <= top:
            return 0.0
        return (int(self.ink[bottom, right]) - int(self.ink[top, right]) - int(self.ink[bottom, left]) +
                int(self.ink[top, left])) / ((right - left) * (bottom - top))

    def nearby(self, box, own=None):
        indices = self.tree.query_ball_point(center(box), max(18, min(240, 4.5 * (box[3] - box[1]))))
        return [i for i in indices if i != own and area(self.boxes[i])]

    def anchors(self, box, indices):
        x, y, u, v = box
        cx, cy = center(box)
        possible = []
        for i in indices:
            a, b, c, d = self.boxes[i]
            scale = self.scales[i]
            ax, ay = center(self.boxes[i])
            gap = x - c if cx >= ax else a - u
            # A local baseline and x-gap isolate the parent line and allow either side of a base.
            if not (-0.48 * scale <= gap <= 2.2 * scale and abs(cy - ay) <= 1.22 * scale and
                    (d - b) >= 0.52 * scale and abs(d - self.baselines[i]) <= 0.58 * scale):
                continue
            cost = abs(gap) / scale + 0.35 * abs(d - self.baselines[i]) / scale + 0.18 * abs(cy - ay) / scale
            possible.append((cost, i))
        return [(cost, i) for cost, i in sorted(possible)[:4]]

    def relations(self, candidate, anchors):
        box = candidate['box']
        x, y, u, v = box
        cx, cy = center(box)
        rows = []
        ink = self.density(box)
        for rank, (cost, i) in enumerate(anchors):
            a, b, c, d = self.boxes[i]
            ax, ay = center(self.boxes[i])
            scale = self.scales[i]
            yp = (cy - ay) / scale
            height = (v - y) / scale
            attachment = [
                (x - c if cx >= ax else a - u) / scale, (cx - ax) / scale, yp,
                (y - b) / scale, (v - d) / scale, height, (u - x) / scale,
                (c - a) / scale, (d - b) / scale, (y - self.baselines[i]) / scale,
                (v - self.baselines[i]) / scale, yp * height, abs(yp),
                abs((d - self.baselines[i]) / scale), ink, rank / 4, cost,
                float(candidate['kind'] == 'gap'), float(candidate['kind'] == 'split'),
                float(candidate.get('host') == i), intersection(box, self.boxes[i]) / area(box),
            ]
            rows.append({'base': i, 'features': [float(v) for v in attachment]})
        return rows

    def plausible_split(self, component, host):
        container = self.boxes[host]
        scale = self.scales[host]
        if area(component) > area(container) * .57 or container[2] - container[0] < .53 * scale:
            return False
        cx, cy = center(component)
        for j in self.component_tree.query_ball_point(center(container), 1.3 * scale):
            other, count = self.components[j]
            ox, oy = center(other)
            if (other != component and count >= .06 * scale**2 and
                    intersection(other, container) >= .62 * area(other) and
                    abs(ox - cx) >= .21 * scale and abs(oy - cy) >= .15 * scale):
                return True
        return False

    def build(self):
        result = []
        for i, glyph in enumerate(self.ocr):
            item = {**glyph, 'kind': 'ocr', 'index': i}
            item['relations'] = self.relations(item, self.anchors(item['box'], self.nearby(item['box'], i))) if area(item['box']) else []
            result.append(item)
        for box, ink in self.components:
            close = self.nearby(box)
            if not close:
                continue
            host = max(close, key=lambda i: intersection(box, self.boxes[i]))
            coverage = intersection(box, self.boxes[host]) / area(box)
            if coverage > .45:
                if coverage < .72 or not self.plausible_split(box, host):
                    continue
                kind = 'split'
            else:
                kind, host = 'gap', None
            anchors = self.anchors(box, close)
            if kind == 'split' and host not in [i for _, i in anchors]:
                # The original combined box can serve as the parent of the new component.
                anchors.insert(0, (0.0, host))
            anchors = [(cost, i) for cost, i in anchors
                       if (.23 <= (box[3] - box[1]) / self.scales[i] <= 1.1 and
                           .018 <= ink / self.scales[i]**2 <= .9 and
                           .15 <= abs(center(box)[1] - center(self.boxes[i])[1]) / self.scales[i])]
            if not anchors:
                continue
            item = {'kind': kind, 'host': host, 'box': box, 'role': 'base', 'text': ''}
            item['relations'] = self.relations(item, anchors)
            result.append(item)
        return result


def predict(candidates, model):
    rows = [(item, relation) for item in candidates for relation in item['relations']]
    if not rows:
        return
    values = np.clip(np.asarray([r['features'] for _, r in rows]), -6, 6)
    values = (values - model['mean']) / model['scale']
    features = np.column_stack([values, np.ones(len(values))])
    probabilities = softmax(features @ model['weights'], axis=1)
    for (_, relation), p in zip(rows, probabilities):
        relation['probabilities'] = p.tolist()


def label(candidates, thresholds):
    for item in candidates:
        item['placement'] = 'base'
        item.pop('anchor', None)
        item.pop('confidence', None)
        best = None
        for relation in item['relations']:
            if item['kind'] == 'split' and relation['base'] != item['host']:
                continue
            p = relation['probabilities']
            index = int(np.argmax(p))
            if not index:
                continue
            role = ROLES[index]
            if best is None or p[index] > best[0]:
                best = (p[index], role, relation['base'])
        item['proposal'] = best
        if best is None:
            continue
        confidence, role, anchor = best
        floor = thresholds['decoded' if item['kind'] == 'ocr' else 'recovered'][role]
        if confidence >= floor:
            item.update(placement=role, anchor=anchor, confidence=confidence)


def apply_policy(items, policy):
    label(items, policy['thresholds'])
    if not policy.get('preserve_native'):
        return
    for item in items:
        if item['role'] == 'base':
            continue
        role = ROLES.index(item['role'])
        if item['relations']:
            anchor = max(item['relations'], key=lambda relation: relation['probabilities'][role])
            item['anchor'] = anchor['base']
            item['confidence'] = anchor['probabilities'][role]
        item['placement'] = item['role']


def read_crops(work, cases, tess, models, jobs):
    selected, paths = [], []
    for case in cases:
        with Image.open(work / case['image']) as original:
            image = original.convert('L')
        for item in case['candidates']:
            if item['placement'] == 'base' and item['role'] == 'base':
                continue
            crop = padded_glyph(image, item['box'])
            if crop is None:
                continue
            path = work / f'read-{len(paths):06d}.png'
            crop.save(path, dpi=(300, 300))
            selected.append(item)
            paths.append(path.resolve())
    for name, model in models.items():
        for item, text in zip(selected, batch_identity(tess, model, paths, jobs), strict=True):
            if ALPHANUMERIC.fullmatch(text):
                item[name] = text


def review(image, hocr, model, policy, tess, readers, jobs):
    with Image.open(image) as source:
        ocr = parse_hocr(hocr.read_text(encoding='utf-8'))
        items = ScriptCandidates(source, ocr).build()
    predict(items, model)
    apply_policy(items, model['policies'][policy])
    with TemporaryDirectory() as temporary:
        chosen = {name: readers[name] for name in sorted(set(model['readers'].values()))}
        read_crops(Path(temporary), [{'image': str(image.resolve()), 'candidates': items}], tess, chosen, jobs)
    output = []
    for item in items:
        if item['placement'] == 'base':
            continue
        reader = model['readers'][item['placement']]
        reading = item.get(reader, item['text'])
        if not reading:
            continue
        anchor = item.get('anchor')
        parent = ocr[anchor] if anchor is not None else None
        output.append({'box': item['box'], 'kind': item['kind'], 'placement': item['placement'],
                       'confidence': item.get('confidence'), 'text': reading, 'original_text': item['text'],
                       'anchor': {'box': parent['box'], 'text': parent['text'], 'index': anchor} if parent else None,
                       'original_index': item.get('index')})
    return {'image': str(image), 'script_candidates': output}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path, help='Same raster submitted to Tesseract')
    parser.add_argument('hocr', type=Path, help='Tesseract hOCR generated with hocr_char_boxes=1')
    parser.add_argument('--model', type=Path, required=True, help='Fitted placement model JSON')
    parser.add_argument('--tesseract', type=Path, required=True)
    parser.add_argument('--starter', type=Path, required=True, help='Directory with starter eng.traineddata')
    parser.add_argument('--mixed', type=Path, required=True, help='Directory with previous mixed eng.traineddata')
    parser.add_argument('--adapted', type=Path, required=True, help='Directory with adapted eng.traineddata')
    parser.add_argument('--policy', choices=('precision', 'balanced'), default='balanced')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    cv2.setNumThreads(1)
    result = review(args.image, args.hocr, json.loads(args.model.read_text()), args.policy, args.tesseract,
                    {'starter': args.starter, 'mixed': args.mixed, 'adapted': args.adapted}, args.jobs)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
