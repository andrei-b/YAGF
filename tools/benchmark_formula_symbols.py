#!/usr/bin/env python3
"""Measure transcription of attached scripts and arithmetic operators in paired formula readings."""

import argparse
from collections import Counter
import csv
from difflib import SequenceMatcher
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET


# Use the benchmark's own KaTeX grammar, including its normal expansion of LaTeX macros.
RENDER = """
const katex = require(process.argv[1]);
const inputs = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const rows = inputs.map(latex => {
  if (!latex.trim()) return null;
  try { return katex.renderToString(latex, {output: 'mathml', displayMode: true}); }
  catch (error) { if (error instanceof katex.ParseError) return null; throw error; }
});
process.stdout.write(JSON.stringify(rows));
"""
OPERATORS = {'+', '−', '=', '×', '÷', '⋅', '±', '∓', '/'}
INVISIBLE = {'mspace', 'annotation', 'mphantom'}


def tag(node):
    return node.tag.rsplit('}', 1)[-1]


def notation(node, omit_targets=False):
    """Retain structural boundaries in the anchor and script but ignore layout-only wrappers."""
    kind = tag(node)
    if kind in INVISIBLE:
        return ''
    if omit_targets and kind in {'msub', 'msup', 'msubsup'}:
        return notation(node[0], omit_targets=True)
    if kind in {'mi', 'mn', 'mo', 'mtext'}:
        if omit_targets and kind in {'mi', 'mo'} and node.text in OPERATORS:
            return ''
        return ''.join(''.join(node.itertext()).split())
    parts = [notation(child, omit_targets) for child in node]
    if kind in {'mrow', 'mstyle', 'mpadded'}:
        return ''.join(parts)
    return f"{kind}({','.join(parts)})"


def symbols(mathml):
    root = ET.fromstring(mathml).find('.//{*}semantics')[0]
    scripts, operators = [], []
    def visit(node):
        kind = tag(node)
        if kind in INVISIBLE:
            return
        if kind in {'msub', 'msubsup'}:
            scripts.append(('sub', notation(node[0]), notation(node[1])))
        if kind in {'msup', 'msubsup'}:
            scripts.append(('sup', notation(node[0]), notation(node[-1])))
        if kind in {'mo', 'mi'} and node.text in OPERATORS:
            operators.append(node.text)
        for child in node:
            visit(child)
    visit(root)
    return {'scripts': scripts, 'operators': operators, 'context': notation(root, omit_targets=True)}


def parse_formulas(expressions, katex):
    unique = list(dict.fromkeys(expressions))
    process = subprocess.run(['node', '-e', RENDER, str(katex.resolve())],
                             input=json.dumps(unique), capture_output=True, text=True)
    if process.returncode:
        raise ValueError(f'KaTeX renderer failed: {process.stderr.strip()}')
    return {latex: symbols(markup) if markup else None
            for latex, markup in zip(unique, json.loads(process.stdout), strict=True)}


def differences(before, after):
    matcher = SequenceMatcher(None, before, after, autojunk=False)
    return [{'start': i, 'original': before[i:j], 'alternative': after[k:l]}
            for action, i, j, k, l in matcher.get_opcodes() if action != 'equal']


def edits(reference, reading):
    previous = list(range(len(reading) + 1))
    for i, expected in enumerate(reference, 1):
        row = [i]
        for j, actual in enumerate(reading, 1):
            row.append(min(previous[j] + 1, row[-1] + 1, previous[j-1] + (expected != actual)))
        previous = row
    return previous[-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('readings', type=Path, help='TSV with rule, reference and named LaTeX reading columns')
    parser.add_argument('output', type=Path)
    parser.add_argument('--modes', nargs='+', default=['normal', 'detail'], help='Reading names to compare')
    parser.add_argument('--katex', type=Path, required=True, help='Local katex.min.js')
    args = parser.parse_args()
    modes = args.modes
    if len(set(modes)) != len(modes):
        parser.error('Each reading name must be unique')
    with args.readings.open(newline='') as stream:
        reader = csv.DictReader(stream, delimiter='\t')
        if not {'rule', 'reference', *modes}.issubset(reader.fieldnames or []):
            parser.error('The TSV must include rule, reference and all requested reading columns')
        cases = {}
        for row in reader:
            key = row['rule']
            if not key or key in cases or any(row[field] is None for field in ['reference', *modes]):
                parser.error(f'Incomplete or duplicate row at line {reader.line_num}')
            cases[key] = row
    if not cases:
        parser.error('The TSV must contain at least one crop')
    expressions = [case['reference'] for case in cases.values()]
    expressions.extend(row[mode] for row in cases.values() for mode in modes)
    parsed = parse_formulas(expressions, args.katex)
    totals = {group: {mode: Counter() for mode in modes} for group in ('all', 'all_renderable')}
    rows = {}
    for key, case in cases.items():
        truth = parsed[case['reference']]
        if truth is None:
            raise ValueError(f'Unrenderable reference {key}')
        rows[key] = {}
        all_renderable = all(parsed[case[m]] is not None for m in modes)
        for mode in modes:
            prediction = parsed[case[mode]]
            counts = Counter(crops=1, renderable=int(prediction is not None))
            for name in ('scripts', 'operators'):
                expected = truth[name]
                observed = prediction[name] if prediction is not None else []
                counts[name] = len(expected)
                counts[name+'_edits'] = edits(expected, observed)
                if name == 'scripts':
                    for placement in ('sup', 'sub'):
                        target = [s for s in expected if s[0] == placement]
                        counts[placement] = len(target)
                        counts[placement+'_edits'] = edits(target, [s for s in observed if s[0] == placement])
            totals['all'][mode].update(counts)
            if all_renderable:
                totals['all_renderable'][mode].update(counts)
            rows[key][mode] = dict(counts)
    args.output.write_text(json.dumps(dict(summary=totals, cases=rows), indent=2) + '\n')
    print(json.dumps(totals, indent=2))


if __name__ == '__main__':
    main()
