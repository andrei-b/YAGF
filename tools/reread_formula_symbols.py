#!/usr/bin/env python3
"""Compare UniMERNet readings of the same formula at two PDF render resolutions."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

from PIL import Image


def detail_box(box, base_size, detail_size):
    """Map pixel edges outward to keep every source stroke inside the second crop."""
    x, y, right, bottom = box
    w, h = base_size
    dw, dh = detail_size
    if not (0 <= x < right <= w and 0 <= y < bottom <= h):
        raise ValueError(f'Crop {box} is outside the {w} x {h} base render.')
    return [x * dw // w, y * dh // h, (right * dw + w - 1) // w, (bottom * dh + h - 1) // h]


def read_pair(model, base, detail, box, timeout, enlarged_transform=None):
    import numpy as np
    import torch
    from unimernet_ocr import decode

    high_box = detail_box(box, base.size, detail.size)
    with torch.inference_mode():
        crops = [np.asarray(image.crop(rect).convert('RGB'))
                 for image, rect in [(base, box), (detail, high_box)]]
        normal, high = decode(model, [model.transform(crop) for crop in crops], timeout)
        result = dict(box=list(box), high_box=high_box, normal=normal, detail=high)
        if enlarged_transform:
            result['enlarged'], = decode(model, [enlarged_transform(crops[1])], timeout)
    return result


def render(pdf, page, size, dest):
    subprocess.run(['pdftoppm', '-f', str(page), '-l', str(page), '-scale-to', str(size),
                    '-gray', '-png', '-singlefile', pdf.resolve(), dest], check=True)
    return Image.open(dest.with_suffix('.png'))


def agreed_symbol_change(row, parsed):
    """Identify a localized change that both detail readings reproduce."""
    if not row.get('detail') or row['detail'] != row.get('enlarged'):
        return False
    original, alternative = parsed[row['normal']], parsed[row['detail']]
    return (original is not None and alternative is not None
            and original['context'] == alternative['context']
            and any(original[kind] != alternative[kind] for kind in ('scripts', 'operators')))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pdf', type=Path)
    parser.add_argument('--page', type=int, default=1, help='One-based PDF page number')
    parser.add_argument('--box', nargs=4, type=int, action='append', required=True,
                        metavar=('LEFT', 'TOP', 'RIGHT', 'BOTTOM'), help='Edges in the base render')
    parser.add_argument('--base-size', type=int, default=1450, help='Base render long edge in pixels')
    parser.add_argument('--detail-size', type=int, default=2900, help='Second render long edge in pixels')
    parser.add_argument('--crop-timeout', type=float, default=40, help='Time budget in seconds per decode')
    parser.add_argument('--enlarged', action='store_true', help='Also read the detail crop on a 256 by 896 canvas')
    parser.add_argument('--source', default=os.getenv('YAGF_UNIMERNET_SOURCE'), help='MinerU checkout')
    parser.add_argument('--weights', default=os.getenv('YAGF_UNIMERNET_WEIGHTS'), help='Local UniMERNet weights')
    parser.add_argument('--katex', type=Path, help='Local katex.min.js to locate changes in scripts and operators')
    args = parser.parse_args()
    if not args.source or not args.weights:
        parser.error('Set the UniMERNet source and weights paths.')
    if args.page < 1 or not 0 < args.base_size < args.detail_size or args.crop_timeout <= 0:
        parser.error('Use a positive page, increasing render sizes and a positive timeout.')

    from contextlib import redirect_stdout
    with TemporaryDirectory() as folder, redirect_stdout(sys.stderr):
        folder = Path(folder)
        with render(args.pdf, args.page, args.base_size, folder/'base') as base, \
                render(args.pdf, args.page, args.detail_size, folder/'detail') as detail:
            for box in args.box:
                detail_box(box, base.size, detail.size)
            import torch
            sys.path.insert(0, str(Path(args.source).resolve()))
            from mineru.model.mfr.unimernet.unimernet_hf import UnimernetModel

            torch.set_num_threads(2)
            torch.set_num_interop_threads(1)
            model = UnimernetModel.from_pretrained(args.weights, local_files_only=True).eval()
            enlarged = type(model.transform)(image_size=(256, 896)) if args.enlarged else None
            output = dict(page=args.page, base_size=base.size, detail_size=detail.size,
                          readings=[read_pair(model, base, detail, box, args.crop_timeout, enlarged)
                                    for box in args.box])
    if args.katex:
        from benchmark_formula_symbols import parse_formulas, differences
        modes = ['detail', 'enlarged'] if args.enlarged else ['detail']
        parsed = parse_formulas([row[mode] for row in output['readings'] for mode in ['normal', *modes]], args.katex)
        for row in output['readings']:
            original = parsed[row['normal']]
            row['symbol_changes'] = {}
            for mode in modes:
                alternative = parsed[row[mode]]
                changes = None
                if original is not None and alternative is not None:
                    changes = {kind: differences(original[kind], alternative[kind])
                               for kind in ('scripts', 'operators')}
                    changes['same_surrounding_notation'] = original['context'] == alternative['context']
                row['symbol_changes'][mode] = changes
            if args.enlarged:
                row['agreed_symbol_change'] = agreed_symbol_change(row, parsed)
    print(json.dumps(output, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
