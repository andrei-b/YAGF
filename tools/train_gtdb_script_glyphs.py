#!/usr/bin/env python3
"""Fine-tune an English Tesseract LSTM on GTDB glyph identities."""

import argparse
import json
import os
import random
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PIL import Image


def run(command, **kwargs):
    return subprocess.run(command, check=True, env={**os.environ, 'OMP_THREAD_LIMIT': '1'}, **kwargs)


def train(work, tess, tools, base, config, iterations, jobs):
    output = work / (f'run-{iterations}' if iterations else 'lstm-examples')
    output.mkdir(exist_ok=False)
    samples = output / 'samples'
    samples.mkdir()
    training = json.loads((work / 'glyph-manifest.json').read_text())
    examples = [row for row in training if row['split'] == 'train']
    if not examples:
        raise ValueError('The glyph manifest contains no training examples')
    def make_example(row):
        source = work / row['image']
        image = samples / source.name
        image.write_bytes(source.read_bytes())
        width, height = Image.open(image).size
        prefix = image.with_suffix('')
        prefix.with_suffix('.box').write_text(f"WordStr 0 0 {width} {height} 0 #{row['text']}\n")
        example = prefix.with_suffix('.lstmf')
        for psm in ('10', '13'):
            run([tess, image, prefix, '--tessdata-dir', base.parent, '-l', 'eng', '--psm', psm, config],
                capture_output=True, text=True)
            if example.is_file() and example.stat().st_size:
                return example
        return None

    random.Random(20261127).shuffle(examples)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(make_example, examples))
    files = [path for path in results if path is not None]
    skipped = [row['image'] for row, path in zip(examples, results) if path is None]
    (output / 'rejected.json').write_text(json.dumps(skipped, indent=2) + '\n')
    if len(files) < len(examples) * 0.95:
        raise ValueError(f'Tesseract produced examples for only {len(files)} of {len(examples)} glyphs')
    train_list = output / 'train.txt'
    train_list.write_text('\n'.join(str(path) for path in files) + '\n')
    if not iterations:
        print(f'Prepared {len(files)} examples in {train_list}.')
        return
    base_lstm = output / 'english.lstm'
    checkpoint = output / 'eng'
    with (output / 'training.log').open('w') as log:
        subprocess_options = {'stdout': log, 'stderr': subprocess.STDOUT}
        run([tools / 'combine_tessdata', '-e', base, base_lstm], **subprocess_options)
        run([tools / 'lstmtraining', '--continue_from', base_lstm, '--traineddata', base,
             '--max_iterations', str(iterations), '--learning_rate', '0.0001', '--target_error_rate', '0.01',
             '--train_listfile', train_list, '--model_output', checkpoint], **subprocess_options)
        run([tools / 'lstmtraining', '--stop_training', '--continue_from', output / 'eng_checkpoint',
             '--traineddata', base, '--model_output', output / 'eng.traineddata'], **subprocess_options)
    print(f'Trained on {len(files)} glyphs. Model saved to {output / "eng.traineddata"}.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('work', type=Path)
    parser.add_argument('--tesseract', type=Path, required=True)
    parser.add_argument('--tools', type=Path, required=True, help='Directory containing combine_tessdata and lstmtraining')
    parser.add_argument('--base', type=Path, required=True, help='Floating-point English eng.traineddata')
    parser.add_argument('--config', type=Path, required=True, help='Path to the lstm.train config file')
    parser.add_argument('--iterations', type=int, default=1600, help='Use 0 to generate examples without training')
    parser.add_argument('--jobs', type=int, default=3)
    args = parser.parse_args()
    train(args.work.resolve(), args.tesseract.resolve(), args.tools.resolve(), args.base.resolve(),
          args.config.resolve(), args.iterations, args.jobs)


if __name__ == '__main__':
    main()
