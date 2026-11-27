#!/usr/bin/env python3
"""Suggest Latin letters or digits for native Tesseract subscript boxes on one scan."""

import argparse
import filecmp
import json
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import xml.etree.ElementTree as ET

from PIL import Image

from benchmark_gtdb_inline_scripts import batch_identity, properties
from gtdb_inline_scripts import padded_glyph
from inline_script_recovery import ALPHANUMERIC


def native_subscripts(hocr, size):
    root = ET.parse(hocr).getroot()
    pages = [node for node in root.iter() if node.get('class') == 'ocr_page']
    if len(pages) != 1 or list(map(int, properties(pages[0]).get('bbox', []))) != [0, 0, *size]:
        raise ValueError('Supply single-page hOCR and the scan used for that OCR run.')
    for word in pages[0].iter():
        if word.get('class') != 'ocrx_word':
            continue
        glyphs = [node for node in word.iter() if node.get('class') == 'ocrx_cinfo'
                  and 'x_bboxes' in properties(node)]
        if not glyphs and ''.join(word.itertext()).strip():
            raise ValueError('The hOCR must include character boxes (hocr_char_boxes=1).')
        letters = [''.join(node.itertext()).strip() for node in glyphs]
        word_text = ''.join(letters)
        for index, node in enumerate(glyphs):
            if any(child.tag.rsplit('}', 1)[-1] == 'sub' for child in node.iter()):
                box = list(map(int, properties(node)['x_bboxes']))
                if len(box) != 4:
                    raise ValueError('A subscript has an invalid x_bboxes entry.')
                yield dict(word=word_text, glyph_index=index, original=letters[index], box=box)


def reread(image_path, hocr, tesseract, starter, adapted, jobs):
    if jobs < 1:
        raise ValueError('The number of jobs must be positive.')
    if filecmp.cmp(starter / 'eng.traineddata', adapted / 'eng.traineddata', shallow=False):
        raise ValueError('Supply distinct starter and adapted English models.')
    with Image.open(image_path) as source:
        image = source.convert('L')
    glyphs = list(native_subscripts(hocr, image.size))
    selected, paths = [], []
    with TemporaryDirectory() as directory:
        for glyph in glyphs:
            crop = padded_glyph(image, glyph['box'])
            if crop is None:
                continue
            path = Path(directory) / f'{len(paths)}.png'
            crop.save(path, dpi=(300, 300))
            paths.append(path)
            selected.append(glyph)
        workers = min(jobs, max(1, len(paths)))
        first = batch_identity(tesseract, starter, paths, workers)
        second = batch_identity(tesseract, adapted, paths, workers)
    suggestions = [{**glyph, 'suggested': a} for glyph, a, b in zip(selected, first, second, strict=True)
                   if a == b and a != glyph['original'] and ALPHANUMERIC.fullmatch(a)]
    return dict(image=str(image_path), hocr=str(hocr), starter=str(starter), adapted=str(adapted),
                native_subscripts=len(glyphs),
                crops_read=len(selected), suggestions=suggestions)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('hocr', type=Path)
    parser.add_argument('--tesseract', type=Path, default='tesseract')
    parser.add_argument('--starter', type=Path, required=True, help='Starter eng.traineddata directory')
    parser.add_argument('--adapted', type=Path, required=True, help='Adapted eng.traineddata directory')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    try:
        result = reread(args.image, args.hocr, args.tesseract, args.starter, args.adapted, args.jobs)
    except (OSError, ValueError, ET.ParseError) as error:
        parser.error(str(error))
    except subprocess.CalledProcessError as error:
        parser.error(f'Tesseract exited with status {error.returncode}. {error.stderr.strip()}')
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
