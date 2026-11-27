#!/usr/bin/env python3
"""Register additional GTDB articles for source-level inline-script verification."""

import argparse
import json
import subprocess
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

from PIL import Image

from gtdb_inline_scripts import process_page, read_pages


# These assignments use document identity and available scans, before OCR scoring.
# Each row specifies the first printed page and expected raster resolution.
SOURCES = (
    ('KJM_1999_17_36', 'KJM_1999_17_36', 'dev', 17, 600, 'KJM_1999_17_36.pdf'),
    ('JMS_1975_289_293', 'JMS_1975', 'dev', 289, 600, 'JMS_1975_289_293.jstage.pdf'),
    ('JMS_1975_497_506', 'JMS_1975', 'train', 497, 600, 'JMS_1975_497_506.jstage.pdf'),
    ('JMS_1975_281_288', 'JMS_1975', 'test', 281, 600, 'JMS_1975_281_288.jstage.pdf'),
    ('TMJ_1973_317_331', 'TMJ_1973', 'test', 317, 600, 'TMJ_1973_317_331.jstage.pdf'),
    ('TMJ_1973_333_338', 'TMJ_1973', 'test', 333, 600, 'TMJ_1973_333_338.jstage.pdf'),
    ('TMJ_1990_163_193', 'TMJ_1990_163_193', 'test', 163, 300, 'TMJ_1990_163_193.jstage.pdf'),
)


def prepare(work, archive, jobs):
    for directory in ('lines', 'glyphs', 'scans'):
        (work / directory).mkdir(exist_ok=True)
    queue = []
    for article, annotations, split, first, dpi, filename in SOURCES:
        scans = work / 'scans' / article
        scans.mkdir(exist_ok=True)
        if not list(scans.glob('page-*.png')):
            subprocess.run(['pdfimages', '-png', str(work / 'sources' / filename), str(scans / 'page')], check=True)
        files = sorted(scans.glob('page-*.png'))
        sheets = {int(Path(sheet).stem.rsplit('_', 1)[-1]): (sheet, lines)
                  for sheet, lines in read_pages(archive, annotations)}
        for page, file in enumerate(files, 1):
            sheet, lines = sheets[first + page - 1]
            if dpi == 300:
                scaled = scans / f'native-{page:03d}.png'
                if not scaled.is_file():
                    with Image.open(file) as image:
                        image = image.convert('L').resize((image.width * 2, image.height * 2), Image.Resampling.BICUBIC)
                        image.save(scaled)
                file = scaled
            queue.append((work, article, split, page, sheet, lines, file))
    scans, glyphs, lines = [], [], []
    with ProcessPoolExecutor(max_workers=jobs) as pool:
        for job, result in zip(queue, pool.map(process_page, queue)):
            scan, page_glyphs, page_lines = result
            scan['image'] = str(job[-1].relative_to(work))
            scans.append(scan)
            glyphs.extend(page_glyphs)
            lines.extend(page_lines)
            print(scan['article'], scan['page'], round(scan.get('coverage', 0), 3),
                  len(page_glyphs), len(page_lines), flush=True)
    for name, rows in [('registration', scans), ('glyph-manifest', glyphs), ('line-manifest', lines)]:
        (work / f'{name}.json').write_text(json.dumps(rows, ensure_ascii=False) + '\n')
    print('registered', Counter(row['article'] for row in scans if row.get('accepted')), flush=True)
    print('lines', Counter(row['article'] for row in lines), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('work', type=Path)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=3)
    args = parser.parse_args()
    prepare(args.work.resolve(), args.archive.resolve(), args.jobs)


if __name__ == '__main__':
    main()
