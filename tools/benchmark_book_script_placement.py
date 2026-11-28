#!/usr/bin/env python3
"""Compare frozen subscript placement policies on previously matched book glyphs."""

import argparse
from collections import Counter
from hashlib import sha256
import json
from pathlib import Path

import cv2
from PIL import Image

from benchmark_gtdb_inline_scripts import parse_hocr
from benchmark_old_scans_math import write_json
from inline_script_recovery import ScriptCandidates, center
from inline_script_verifier import decision, proposals, score_items


def in_controls(box, regions):
    x, y = center(box)
    return any(l <= x < r and t <= y < b for l, t, r, b in regions)


def evaluate_page(page, root, attachment, verifier):
    hocr = (root / page['hocr']).read_bytes()
    if sha256(hocr).hexdigest() != page['hocr_sha256']:
        raise ValueError(f"Native glyph order may have changed in {page['hocr']}")
    glyphs = parse_hocr(hocr.decode())
    image_path = root / page['image']
    if sha256(image_path.read_bytes()).hexdigest() != page['image_sha256']:
        raise ValueError(f"Scan differs from the reference in {page['image']}")
    with Image.open(image_path) as scan:
        items = ScriptCandidates(scan, glyphs).build()
    proposals(items, attachment)
    score_items(items, glyphs, verifier)
    placements = {'native': [item['role'] for item in items]}
    for name, policy in verifier['policies'].items():
        placements[name] = [decision(item, policy)[0] for item in items]
    by_index = {item['index']: pos for pos, item in enumerate(items) if item['kind'] == 'ocr'}
    counts = {name: Counter() for name in placements}
    targets = []
    for reference in page['targets']:
        index = reference['index']
        glyph = None if index is None else items[by_index[index]]
        correct = glyph is not None and glyph['text'] == reference['character']
        native = glyph['role'] if glyph else None
        outcome = ('missing' if glyph is None else 'wrong_character' if not correct else
                   'exact' if native == 'sub' else 'position_missed')
        if outcome != reference['outcome']:
            raise ValueError(f"Native outcome mismatch in {page['hocr']} at glyph {index}")
        roles = {}
        for name, labels in placements.items():
            role = labels[by_index[index]] if glyph else None
            exact = correct and role == 'sub'
            counts[name].update(targets=1, exact=int(exact), eligible=int(outcome == 'position_missed'),
                                recovered=int(exact and outcome == 'position_missed'),
                                lost=int(not exact and outcome == 'exact'))
            roles[name] = role
        row = {**reference, 'box': glyph['box'] if glyph else None, 'roles': roles}
        if outcome == 'position_missed':
            proposal = glyph['proposal']
            row['attachment_candidates'] = len(glyph['relations'])
            row['proposed_role'] = proposal[1] if proposal else None
            row['verifier_score'] = glyph.get('verifier_score')
        targets.append(row)
    flags = []
    for i, item in enumerate(items):
        if not in_controls(item['box'], page['prose']):
            continue
        roles = {name: labels[i] for name, labels in placements.items()}
        for name, role in roles.items():
            counts[name].update(prose_glyphs=int(item['kind'] == 'ocr'),
                                lowered_glyphs=int(item['kind'] == 'ocr' and role == 'sub'),
                                lowered_components=int(item['kind'] != 'ocr' and role == 'sub'),
                                raised_glyphs=int(item['kind'] == 'ocr' and role == 'sup'),
                                raised_components=int(item['kind'] != 'ocr' and role == 'sup'))
        if any(role != 'base' for role in roles.values()):
            flags.append(dict(box=item['box'], text=item['text'], kind=item['kind'], roles=roles))
    return dict(image=page['image'], book=page['book'], scores=counts, targets=targets, prose_flags=flags)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path, help='JSON pages with image, hocr, their SHA-256 hashes, book, '
                        'targets (character, native glyph index or null, outcome), and prose control rectangles')
    parser.add_argument('output', type=Path)
    parser.add_argument('--inputs', type=Path, help='Root for scan and hOCR paths (default: manifest directory)')
    parser.add_argument('--attachment', required=True, type=Path)
    parser.add_argument('--verifier', required=True, type=Path)
    args = parser.parse_args()
    cv2.setNumThreads(1)
    attachment = json.loads(args.attachment.read_text())
    verifier = json.loads(args.verifier.read_text())
    pages = []
    summary = {}
    by_book = {}
    for page in json.loads(args.manifest.read_text()):
        result = evaluate_page(page, args.inputs or args.manifest.parent, attachment, verifier)
        pages.append(result)
        for name, counts in result['scores'].items():
            summary.setdefault(name, Counter()).update(counts)
            by_book.setdefault(page['book'], {}).setdefault(name, Counter()).update(counts)
        print(page['book'], Path(page['image']).name, flush=True)
    inputs = {'manifest': args.manifest, 'attachment': args.attachment, 'verifier': args.verifier}
    hashes = {name: sha256(path.read_bytes()).hexdigest() for name, path in inputs.items()}
    write_json(args.output, dict(input_sha256=hashes, summary=summary, books=by_book, pages=pages))
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
