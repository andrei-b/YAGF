#!/usr/bin/env python3
"""Continue Tesseract training with GTDB glyphs and earlier transcribed lines."""

import argparse
import json
import random
import subprocess
from hashlib import file_digest
from pathlib import Path

from train_gtdb_script_glyphs import run


def digest(path):
    with path.open('rb') as stream:
        return file_digest(stream, 'sha256').hexdigest()


def train(output, tools, weights, starter, inputs, iterations):
    output.mkdir(parents=True, exist_ok=False)
    rows = []
    seen = set()
    for manifest in inputs:
        count = 0
        for line in manifest.read_text().splitlines():
            if not line.strip():
                continue
            path = (manifest.parent / line).resolve(strict=True)
            if path.suffix != '.lstmf' or not path.is_file() or not path.stat().st_size:
                raise ValueError(f'Invalid LSTM sample {path}')
            checksum = digest(path)
            if checksum not in seen:
                seen.add(checksum)
                rows.append({'input': manifest.name, 'file': str(path), 'sha256': checksum})
                count += 1
        if not count:
            raise ValueError(f'Input contributed no unique samples: {manifest}')
        print(f'{manifest.name}: {count} unique samples', flush=True)
    random.Random(20261127).shuffle(rows)
    sample_list = output / 'mixed-train.txt'
    sample_list.write_text(''.join(row['file'] + '\n' for row in rows))
    (output / 'mixed-manifest.json').write_text(json.dumps(rows, indent=2) + '\n')
    initialization = output / 'initial.lstm'
    prefix = output / 'eng'
    traineddata = output / 'eng.traineddata'
    with (output / 'training.log').open('w') as log:
        log_options = {'stdout': log, 'stderr': subprocess.STDOUT}
        run([tools / 'combine_tessdata', '-e', weights, initialization], **log_options)
        run([tools / 'lstmtraining', '--continue_from', initialization, '--traineddata', starter,
             '--max_iterations', str(iterations), '--learning_rate', '0.0001', '--target_error_rate', '0.01',
             '--train_listfile', sample_list, '--model_output', prefix], **log_options)
        run([tools / 'lstmtraining', '--stop_training', '--continue_from', output / 'eng_checkpoint',
             '--traineddata', starter, '--model_output', traineddata], **log_options)
    record = {'examples': len(rows), 'max_iterations': iterations, 'learning_rate': 0.0001,
              'starter_sha256': digest(starter), 'initial_weights_sha256': digest(weights),
              'candidate_sha256': digest(traineddata)}
    (output / 'training-run.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'Trained {traineddata} on {len(rows)} unique samples.', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--tools', type=Path, required=True)
    parser.add_argument('--weights', type=Path, required=True, help='Fine-tuned model used to initialize the run')
    parser.add_argument('--starter', type=Path, required=True, help='Starter traineddata supplying the English character set')
    parser.add_argument('--list', dest='inputs', type=Path, action='append', required=True)
    parser.add_argument('--iterations', type=int, default=3200)
    args = parser.parse_args()
    train(args.output.resolve(), args.tools.resolve(), args.weights.resolve(), args.starter.resolve(),
          [source.resolve() for source in args.inputs], args.iterations)


if __name__ == '__main__':
    main()
