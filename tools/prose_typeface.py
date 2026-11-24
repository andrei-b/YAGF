#!/usr/bin/env python3
"""Learn a book's ordinary printed letters and flag unusual glyphs for review."""
import argparse
import base64
from collections import defaultdict, Counter
from itertools import chain
import gzip
import io
import hashlib
import json
import os
import re
import shutil
from pathlib import Path
import unicodedata
import subprocess
import sys
from tempfile import NamedTemporaryFile, TemporaryDirectory
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image, ImageDraw, ImageOps

SIDE = 24
FORMAT = 'yagf-prose-typeface/1'
SAMPLES = 'yagf-prose-samples/1'


def read_json(path):
    opener = gzip.open if path.suffix == '.gz' else open
    with opener(path, 'rt', encoding='utf-8') as stream:
        return json.load(stream)


def write_json(path, value):
    # A failed or interrupted run must leave any previous result intact.
    with NamedTemporaryFile(dir=path.parent, delete=False) as tmp:
        name = tmp.name
    try:
        opener = gzip.open if path.suffix == '.gz' else open
        with opener(name, 'wt', encoding='utf-8') as stream:
            json.dump(value, stream, ensure_ascii=False, allow_nan=False, separators=(',', ':'))
            stream.write('\n')
        os.replace(name, path)
    finally:
        Path(name).unlink(missing_ok=True)


def feature(crop):
    """Center an ink crop without normalizing away slant or aspect ratio."""
    ink = ImageOps.autocontrast(ImageOps.invert(crop))
    bounds = ink.point(lambda x: 255 if x >= 96 else 0).getbbox()
    if not bounds or max(ink.getextrema()) < 64:
        return None
    ink = ink.crop(bounds)
    scale = (SIDE - 2) / max(ink.size)
    size = tuple(max(1, round(n * scale)) for n in ink.size)
    shape = Image.new('L', (SIDE, SIDE))
    shape.paste(ink.resize(size, Image.Resampling.BILINEAR), tuple((SIDE - n)//2 for n in size))
    return np.asarray(shape)


def encode(mask):
    return base64.b64encode(mask.tobytes()).decode('ascii')


def decode(value):
    mask = np.frombuffer(base64.b64decode(value, validate=True), dtype=np.uint8)
    if mask.size != SIDE * SIDE or not mask.any():
        raise ValueError('Invalid glyph template')
    return mask


def word_slant(crop):
    # Vertical stems concentrate ink in fewer columns when their shear is undone.
    ink = np.asarray(ImageOps.invert(crop), dtype=np.float32) / 255
    height, width = ink.shape
    padding = height // 2 + 1
    best_score, best_slope = -1., 0.
    for slope in (0, -.08, .08, -.16, .16, -.24, .24, -.32, .32, -.4, .4):
        projection = np.zeros(width + 2 * padding, dtype=np.float32)
        for y, row in enumerate(ink):
            start = padding + round(slope * (y - height / 2))
            projection[start:start + width] += row
        score = float(projection @ projection)
        if score > best_score:
            best_score, best_slope = score, slope
    return best_slope


def fields(element):
    return dict(item.strip().split(' ', 1) for item in element.get('title', '').split(';') if ' ' in item.strip())


def bbox(value, size):
    coords = tuple(map(int, value.split()))
    if len(coords) == 4:
        x, y, right, bottom = coords
        if 0 <= x < right <= size[0] and 0 <= y < bottom <= size[1]:
            return coords
    return None


def recognize(path, language):
    # Use identical pixels for OCR and clipping, keeping boxes in input raster coordinates.
    with Image.open(path) as source:
        image = source.convert('L')
        dpi = source.info.get('dpi', (300, 300))
    data = io.BytesIO()
    image.save(data, format='PNG', dpi=dpi)
    result = subprocess.run(['tesseract', 'stdin', 'stdout', '-l', language, '-c', 'hocr_char_boxes=1',
                             '-c', 'hocr_font_info=1', 'hocr'], input=data.getvalue(), capture_output=True)
    if result.returncode:
        raise ValueError(result.stderr.decode(errors='replace').strip() or 'Tesseract failed')
    root = ET.fromstring(result.stdout)
    def read_word(node):
        info = fields(node)
        box = bbox(info['bbox'], image.size)
        if not box:
            return None
        glyphs = []
        for char in node.findall(".//*[@class='ocrx_cinfo']"):
            charbox = bbox(fields(char)['x_bboxes'], image.size)
            glyphs.append((''.join(char.itertext()), charbox))
        styled = any(e.tag.split('}')[-1] in ('em', 'i', 'strong', 'b') for e in node.iter())
        return glyphs, float(info['x_wconf']), box, styled

    lines = []
    for line in root.iter():
        if line.get('class') in ('ocr_line', 'ocrx_line', 'ocr_header', 'ocr_caption', 'ocr_textfloat'):
            lines.append([w for node in line.findall(".//*[@class='ocrx_word']") if (w := read_word(node))])
    return image, lines


def lexical_core(glyphs):
    # Allow ordinary sentence punctuation, while excluding embedded digits and operators.
    start, end = 0, len(glyphs)
    punctuation = '.,:;!?«»“”„\"()'
    while start < end and glyphs[start][0] in punctuation:
        start += 1
    while end > start and glyphs[end-1][0] in punctuation:
        end -= 1
    return glyphs[start:end]


def trusted_word(glyphs, confidence, styled, lexicon, minimum, threshold):
    text = ''.join(letter for letter, _ in glyphs)
    if (styled or confidence < threshold or len(glyphs) < minimum or text.isupper()
            or not text.isalpha() or text.casefold() not in lexicon):
        return False
    # Reject failed alignments and shared boxes (e.g. unresolved ligatures).
    return all(len(c) == 1 and box for c, box in glyphs) and len({box for _, box in glyphs}) == len(glyphs)


def load_lexicon(args):
    def load(path):
        with path.open(encoding='utf-8-sig') as stream:
            return {word for line in stream if len(word := line.strip().casefold()) >= args.min_length}

    if args.wordlist:
        return load(args.wordlist)
    # Use exactly the vocabulary provided by the selected general OCR language.
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.language):
        raise ValueError('Supply --wordlist when collecting with multiple OCR languages')
    if not all(shutil.which(tool) for tool in ('combine_tessdata', 'dawg2wordlist')):
        raise ValueError('Install combine_tessdata and dawg2wordlist or provide a UTF-8 --wordlist')
    langs = subprocess.run(['tesseract', '--list-langs'], capture_output=True, text=True, check=True)
    location = re.search(r'List of available languages in "([^"]+)"', langs.stdout + langs.stderr)
    if not location:
        raise ValueError('Cannot locate the Tesseract vocabulary; supply --wordlist')
    trained = Path(location[1]) / (args.language + '.traineddata')
    with TemporaryDirectory() as directory:
        root = Path(directory)
        alphabet, dictionary = root/'lang.lstm-unicharset', root/'lang.lstm-word-dawg'
        subprocess.run(['combine_tessdata', '-e', trained, alphabet, dictionary],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['dawg2wordlist', alphabet, dictionary, root/'words.txt'],
                       check=True, stdout=subprocess.DEVNULL)
        return load(root/'words.txt')


def collect(args):
    lexicon = load_lexicon(args)
    if not lexicon:
        raise ValueError('The word list contains no eligible words')
    pages = []
    seen = set()
    for path in args.images:
        im, lines = recognize(path, args.language)
        digest = hashlib.sha256(im.tobytes()).hexdigest()
        if digest in seen:
            raise ValueError(f'Duplicate training page: {path}')
        seen.add(digest)
        examples = []
        for glyphs, confidence, box, styled in chain.from_iterable(lines):
            core = lexical_core(glyphs)
            if not trusted_word(core, confidence, styled, lexicon, args.min_length, args.confidence):
                continue
            shapes = [feature(im.crop(rect)) for _, rect in core]
            if any(shape is None for shape in shapes):
                continue
            examples.append(dict(word=''.join(c for c, _ in core), slant=word_slant(im.crop(box)),
                                 masks=[encode(m) for m in shapes]))
        pages.append(dict(name=path.name, sha256=digest, words=examples))
        print(f'{path.name}: {len(examples)} eligible words', file=sys.stderr)
    if not any(p['words'] for p in pages):
        raise ValueError('No words passed the lexicon, length and confidence checks')
    write_json(args.output, dict(format=SAMPLES, language=args.language, min_length=args.min_length,
                                 confidence=args.confidence, pages=pages))


def nearest(mask, bank):
    # Normalized ink difference retains aspect ratio and is insensitive to white area.
    difference = np.abs(bank - mask).sum(axis=1)
    return float(np.min(difference / np.maximum(1, bank.sum(axis=1) + mask.sum())))


def word_letter_offsets(text):
    """Find letters belonging to words while retaining offsets in the OCR reading."""
    # Combining accents belong to the preceding letter. Keep the original offsets
    # because a Tesseract character box may contain several Unicode code points.
    offsets = [i for i, char in enumerate(text) if not unicodedata.category(char).startswith('M')]
    # Represent every alphabet's letters uniformly. Digits, including x²'s
    # superscript, and mathematical operators naturally delimit letter runs.
    pattern = ''.join('L' if text[i].isalpha() else text[i] for i in offsets)
    protected = set()
    for match in re.finditer(r"L+(?:['’\u00ad\u2010\u2011-]L+)*", pattern):
        run = match.group()
        # Keep single-letter terms in x-y eligible. Include short parts of words
        # such as l’école and по-русски, even if recognition confidence is low.
        if 'LL' in run or "'" in run or '’' in run:
            protected.update(offsets[i] for i in range(*match.span()) if pattern[i] == 'L')
    return protected


def train(args):
    data = read_json(args.samples)
    if data['format'] != SAMPLES:
        raise ValueError('Expected a YAGF prose sample file')
    pages = [p for p in data['pages'] if p['words']]
    if len({p['sha256'] for p in pages}) != len(pages) or len(pages) < 3:
        raise ValueError('Use at least three distinct pages with trusted words for fitting and calibration')
    # Reserve whole pages, so a glyph never calibrates against itself or the same line.
    calibration = pages[2::4]
    reserved = {p['sha256'] for p in calibration}
    fit = [p for p in pages if p['sha256'] not in reserved]
    slopes = [w['slant'] for p in fit for w in p['words']]
    dominant = float(np.median(slopes))
    tolerance = float(np.clip(3 * np.median(np.abs(np.asarray(slopes) - dominant)), .12, .20))
    def accepted(word):
        return abs(word['slant'] - dominant) <= tolerance
    by_letter = defaultdict(list)
    checks = defaultdict(list)
    for page in pages:
        for word in page['words']:
            if not accepted(word):
                continue
            if len(word['word']) != len(word['masks']):
                raise ValueError('Unaligned training word')
            for char, mask in zip(word['word'], word['masks']):
                if page['sha256'] in reserved:
                    checks[char].append(decode(mask))
                else:
                    by_letter[char].append((page['sha256'], word['word'], mask))
    letters = {}
    for char, rows in sorted(by_letter.items()):
        if (len({r[0] for r in rows}) < 2 or len({r[1] for r in rows}) < 3
                or len(rows) < 12 or len(checks[char]) < 8):
            continue
        # A reproducible bounded bank; repeated printings of the same bitmap add no information.
        unique = sorted({r[2] for r in rows}, key=lambda m: hashlib.sha256(m.encode()).digest())
        templates = unique[:64]
        bank = np.array([decode(m) for m in templates], dtype=np.int16)
        distances = [nearest(m, bank) for m in checks[char]]
        letters[char] = dict(templates=templates, threshold=max(.05, float(np.quantile(distances, .975))),
                             fit=len(rows), calibration=len(distances))
    if not letters:
        raise ValueError('Insufficient repeated letter coverage across fitting and calibration pages')
    stats = dict(pages=len(pages), fit_pages=[p['name'] for p in fit], calibration_pages=[p['name'] for p in calibration],
                 eligible_words=len(slopes) + sum(len(p['words']) for p in calibration),
                 style_accepted_words=sum(accepted(w) for p in pages for w in p['words']),
                 dominant_slant=dominant, slant_tolerance=tolerance, modeled_letters=len(letters),
                 fit_glyphs=sum(v['fit'] for v in letters.values()), calibration_glyphs=sum(v['calibration'] for v in letters.values()))
    write_json(args.output, dict(format=FORMAT, language=data['language'], min_length=data['min_length'],
                                 confidence=data['confidence'], stats=stats, letters=letters))
    print(json.dumps(stats, ensure_ascii=False))


# Ordinary Latin variables and single-letter \mathit groups use math italics.
GREEK = dict(zip(
    'alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega'.split(),
    'αβγδϵζηθικλμνξοπρστυϕχψω'))
GREEK.update(dict(zip('Gamma Delta Theta Lambda Xi Pi Sigma Upsilon Phi Psi Omega'.split(), 'ΓΔΘΛΞΠΣΥΦΨΩ')))
GREEK.update(varepsilon='ε', vartheta='ϑ', varpi='ϖ', varrho='ϱ', varsigma='ς', varphi='φ', varkappa='ϰ')
MATH_ANCHORS = dict(le='≤', leq='≤', leqslant='≤', ge='≥', geq='≥', geqslant='≥',
                    ne='≠', neq='≠', prime='′', cdot='·', times='×', pm='±')
GREEK_LETTERS = set(GREEK.values()) | set('ΑΒΕΖΗΙΚΜΝΟΡΤΧ')
# General OCR also reads a printed alpha as one of these standalone signs.
GREEK_OCR_SIGNS = frozenset('@&%©')


def formula_tokens(latex):
    """Return (printed character, script position) for an unambiguous flat reading."""
    latex = latex.strip()
    for left, right in [(r'\[', r'\]'), (r'\(', r'\)'), ('$$', '$$'), ('$', '$')]:
        if latex.startswith(left) and latex.endswith(right):
            latex = latex[len(left):-len(right)]
            break
    latex = re.sub(r'\\mathit\s*\{\s*([A-Za-z])\s*\}', r' \1 ', latex)
    if re.search(r'[A-Za-z]{2}', re.sub(r'\\[A-Za-z]+', ' ', latex)):
        return []
    pieces = re.findall(r'\\[a-zA-Z]+|\\.|\S', latex)
    tokens = []
    i = 0
    while i < len(pieces):
        part = pieces[i]
        i += 1
        if part in (r'\,', r'\;', r'\!', r'\:', r'\quad', r'\qquad', r'\left', r'\right', '~'):
            continue
        script = ''
        if part in ('_', '^'):
            if not tokens or tokens[-1][1] or i >= len(pieces):
                return []
            script = part
            part = pieces[i]
            i += 1
            if part == '{':
                if i+1 >= len(pieces) or pieces[i+1] != '}':
                    return []
                part = pieces[i]
                i += 2
        if part.startswith('\\'):
            char = GREEK.get(part[1:]) or MATH_ANCHORS.get(part[1:])
        elif (part.isascii() and part.isalnum()) or part in GREEK_LETTERS or part in "=+-/()[]<>,.′≤≥≠·×±−":
            char = part
        else:
            return []
        if not char:
            return []
        tokens.append((char, script))
    return tokens


def aligned_formula_glyphs(image, latex):
    tokens = formula_tokens(latex)
    if not any(c.isalnum() for c, _ in tokens):
        return []
    ink = np.asarray(ImageOps.autocontrast(image)) < 128
    xs = np.flatnonzero(ink.any(axis=0))
    if not len(xs):
        return []
    spans = np.split(xs, np.flatnonzero(np.diff(xs) > 1) + 1)
    if len(spans) != len(tokens):
        return []
    glyphs = []
    prev_y = None
    for (char, script), span in zip(tokens, spans):
        left, right = int(span[0]), int(span[-1]) + 1
        rows = np.flatnonzero(ink[:, left:right].any(axis=1))
        top, bottom = int(rows[0]), int(rows[-1]) + 1
        # Use minus/equal signs and the height of scripts as alignment checks.
        if char in '=-−':
            bands = np.count_nonzero(np.diff(rows) > 1) + 1
            if bands != (2 if char == '=' else 1) or right-left < bottom-top:
                return []
        center = (top + bottom) / 2
        if (script == '^' and center >= prev_y) or (script == '_' and center <= prev_y):
            return []
        prev_y = center
        if char.isalnum():
            if bottom-top < 5 or right-left < 2:
                if char.isdecimal():
                    continue  # A tiny numeric script can still anchor its neighbours.
                return []
            mask = feature(image.crop((left, top, right, bottom)))
            if mask is None:
                return []
            glyphs.append((char, encode(mask)))
    return glyphs


def confident(value):
    return type(value) in (int, float) and 90 <= value <= 100


def trusted_formula_regions(run, decision):
    width, height = run['input_size']
    whole = [0, 0, width, height]
    if decision:
        area = decision.get('processed_area')
        formula_run = run['role'] == 'formula' or (run['role'] == 'manual' and 'latex' in run['output'])
        if (formula_run and decision['decision'] == 'accepted' and area and area == run.get('processed_crop')
                and area[2:] == [width, height]):
            yield decision['proposed_text'], (0, 0, width, height)
        return
    output = run['output']
    formulas = output.get('formulas', [])
    only_formula = (run['role'] == 'formula' and len(formulas) == 1
                    and not any(output.get(k) for k in ('text', 'tables', 'figures')))
    for formula in formulas:
        if not confident(formula.get('confidence')):
            continue
        rect = formula.get('box')
        if rect is None and only_formula:
            rect = formula.get('input_area', whole)
        if not rect or len(rect) != 4 or not all(type(v) is int for v in rect):
            continue
        x, y, w, h = rect
        if 0 <= x < x+w <= width and 0 <= y < y+h <= height:
            yield formula['latex'], (x, y, x+w, y+h)


def project_formula_crops(document):
    for page in document['pages']:
        history = page.get('recognition_history', {})
        decisions = {d['run_id']: d for d in history.get('decisions', [])}
        for run in history.get('runs', []):
            if run['status'] != 'succeeded' or run['role'] not in ('manual', 'formula', 'primary'):
                continue
            regions = list(trusted_formula_regions(run, decisions.get(run['id'])))
            if not regions:
                continue
            digest = run['input_sha256']
            value = history.get('inputs', {}).get(digest)
            if not value:
                continue
            raw = base64.b64decode(value, validate=True)
            if hashlib.sha256(raw).hexdigest() != digest:
                raise ValueError('Formula history crop checksum mismatch')
            with Image.open(io.BytesIO(raw)) as image:
                if list(image.size) == run['input_size']:
                    for latex, rect in regions:
                        yield image.crop(rect).convert('L'), latex


def formula_crops(path):
    data = read_json(path)
    if 'document' in data:
        yield from project_formula_crops(data['document'])
    elif data.get('format') == 'yagf-formula-crops/1':
        for row in data['formulas']:
            trusted = row['reviewed'] is True if 'reviewed' in row else confident(row.get('confidence'))
            if not trusted:
                continue
            with Image.open(path.parent / row['image']) as image:
                yield image.convert('L'), row['latex']
    else:
        raise ValueError('Expected a saved YAGF project or a YAGF formula crop manifest')


def learn_formulas(args):
    model = read_json(args.model)
    if model['format'] != FORMAT or not model['letters']:
        raise ValueError('Expected a trained YAGF prose typeface model')
    bank = {char: set(values) for char, values in model.get('formula_letters', {}).items()}
    digits = {char: set(values) for char, values in model.get('digit_controls', {}).items()}
    aligned = 0
    for image, latex in formula_crops(args.formulas):
        examples = aligned_formula_glyphs(image, latex)
        if examples:
            aligned += 1
            for char, mask in examples:
                target = digits if char.isdecimal() else bank
                target.setdefault(char, set()).add(mask)
    if not aligned:
        raise ValueError('No individually aligned letters or digit controls in trusted crops')
    for name, values in [('formula_letters', bank), ('digit_controls', digits)]:
        if values:
            model[name] = {c: sorted(v, key=lambda m: hashlib.sha256(m.encode()).digest())[:64]
                           for c, v in sorted(values.items())}
    write_json(args.output, model)
    print(json.dumps(dict(aligned_crops=aligned, templates={c: len(v) for c, v in model.get('formula_letters', {}).items()},
                          digit_controls={c: len(v) for c, v in model.get('digit_controls', {}).items()}),
                     ensure_ascii=False))


class FormulaMatcher:
    def __init__(self, letters, prose_banks, digit_controls=None):
        self.banks = {}
        for char, values in letters.items():
            if len(char) != 1 or not char.isalpha() or not 1 <= len(values) <= 64:
                raise ValueError('Invalid formula letter bank')
            self.banks[char] = np.array([decode(v) for v in values], dtype=np.int16)
        self.prose = np.concatenate([b for _, b in prose_banks.values()])
        digits = []
        self.recoverable_readings = set()
        self.repeated_greek = {c for c in letters if c in GREEK_LETTERS and len(set(letters[c])) >= 2}
        if self.repeated_greek:
            self.recoverable_readings.update(GREEK_OCR_SIGNS)
        greek = self.banks.keys() & GREEK_LETTERS
        repeated_z = len(set(letters.get('z', ()))) >= 2
        self.digit_letters = {}
        for char, values in (digit_controls or {}).items():
            if len(char) != 1 or not char.isdecimal() or not 1 <= len(values) <= 64:
                raise ValueError('Invalid digit control bank')
            digits.extend(decode(v) for v in values)
            if len(set(values)) >= 2:
                allowed = greek | {'z'} if char == '2' and repeated_z else greek
                if allowed:
                    self.digit_letters[char] = allowed
        self.recoverable_readings.update(self.digit_letters)
        self.digits = np.array(digits, dtype=np.int16)

    def match(self, mask, reading=''):
        distances = sorted((nearest(mask, bank), c) for c, bank in self.banks.items())
        distance, char = distances[0]
        if reading.isdecimal() and char not in self.digit_letters.get(reading, ()):
            return None
        limit, margin = (.18, .06) if len(reading) > 1 else (.22, .04)
        if reading in GREEK_OCR_SIGNS:
            if char not in self.repeated_greek:
                return None
            # These symbols have no book-specific negative templates.
            limit, margin = .15, .08
        if distance > limit or (len(distances) > 1 and distances[1][0] < distance + margin):
            return None
        # Some variables resemble a different prose letter than the OCR reading.
        if distance + margin >= nearest(mask, self.prose):
            return None
        if (char in GREEK_LETTERS or reading.isdecimal()) and self.digits.size and distance + margin >= nearest(mask, self.digits):
            return None
        return char, distance


SENTENCE_PUNCTUATION = '.,;:!?…«»“”„"'
MATH_NAMES = set('sin cos tan cot tg ctg sec csc sinh cosh tanh sh ch th log ln lg exp lim min max det mod re im arg'.split())


def prose_candidates(line, readings, recoverable=(), joined=None):
    """Map isolated letter tokens to the surrounding words on their own OCR line."""
    cores = [s.strip(SENTENCE_PUNCTUATION) for s in readings]
    for i, (reading, _) in (joined or {}).items():
        cores[i] = cores[i].replace(reading, reading[0], 1)
    anchors = {i for i, core in enumerate(cores)
               if len(core) >= 2 and core.isalpha() and core.casefold() not in MATH_NAMES and line[i][1] >= 55}
    if not any(len(cores[i]) >= 3 for i in anchors):
        return {}
    found = {}
    for i, core in enumerate(cores):
        # A quotation can start or end inside a prose parenthesis.
        if core.startswith(('(«', '(“', '(„', '("')) and i+1 in anchors:
            core = core[2:]
        if core.endswith(('»)', '”)', '“)', '")')) and i-1 in anchors:
            core = core[:-2]
        # A sentence dash may be attached to a variable, e.g. “α—вещественный
        # параметр”. The following prose word distinguishes it from “x—1”.
        if len(core) == 2 and core.endswith('—') and i+1 in anchors:
            core = core[0]
        wrapped = len(core) == 3 and (core[0], core[-1]) in (('(', ')'), ('[', ']'))
        letter = core[1] if wrapped else core
        if len(letter) != 1 or not (letter.isalpha() or letter in recoverable):
            continue
        # Formula OCR can split "x = 0", "sin x" or "f (x)" into separate words.
        prev = cores[i-1] if i else ''
        following = cores[i+1] if i+1 < len(cores) else ''
        operators = '=+-−*/×÷±∓<>≤≥≠≈∼→←∈^_'
        if (prev and (prev[-1] in operators + '([' or prev.casefold() in MATH_NAMES)
                or following and following[0] in operators + '(['):
            continue
        # A formula frequently arrives as two tokens, e.g. Greek Γ read as
        # Cyrillic Г followed by "(p)". Require a prose word before the opening
        # delimiter when there is a preceding token.
        if wrapped and prev and i-1 not in anchors:
            continue
        x, y, right, bottom = line[i][2]
        center = (y + bottom) / 2
        for j in range(max(0, i-2), min(len(line), i+3)):
            if j not in anchors:
                continue
            l, t, r, b = line[j][2]
            height = b - t
            gap = max(0, l-right, x-r)
            if t <= center <= b and gap <= 3 * height:
                found[i] = ' '.join(readings[max(0, i-3):i+4])
                break
    return found


def joined_glyph(image, word):
    """Recover one connected ink shape that OCR divided into two overlapping letters."""
    glyphs, confidence, _, _ = word
    if confidence >= 50:
        return None
    core = lexical_core(glyphs)
    if len(core) != 2 or any(not b or len(c) != 1 or not c.isalpha() for c, b in core):
        return None
    (_, a), (_, b) = core
    box = (min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3]))
    width = box[2]-box[0]
    if (box not in (tuple(a), tuple(b)) or min(a[2]-a[0], b[2]-b[0]) > .6*width
            or width > 1.4*(box[3]-box[1])):
        return None
    ink = image.crop(box).point(lambda v: 255 if v < 128 else 0)
    bounds = ink.getbbox()
    if not bounds:
        return None
    y = bounds[1]
    x = next(x for x in range(bounds[0], bounds[2]) if ink.getpixel((x, y)))
    ImageDraw.floodfill(ink, (x, y), 0)
    if ink.getbbox():
        return None
    return ''.join(c for c, _ in core), box


def separate_punctuation(image, glyphs, word_box):
    """Repair overlapping OCR boxes only when the ink gives one span per character."""
    if len(glyphs) < 2 or any(not b for _, b in glyphs):
        return glyphs
    if all(left[1][2] <= right[1][0] for left, right in zip(glyphs, glyphs[1:])):
        return glyphs
    ink = np.asarray(image.crop(word_box)) < 128
    xs = np.flatnonzero(ink.any(axis=0))
    if not len(xs):
        return glyphs
    spans = np.split(xs, np.flatnonzero(np.diff(xs) > 1) + 1)
    if len(spans) != len(glyphs):
        return glyphs
    left, top = word_box[:2]
    repaired = []
    for (char, _), span in zip(glyphs, spans):
        l, r = int(span[0]), int(span[-1])+1
        ys = np.flatnonzero(ink[:, l:r].any(axis=1))
        repaired.append((char, (left+l, top+int(ys[0]), left+r, top+int(ys[-1])+1)))
    return repaired


def review_glyphs(lines, scope, counts, recoverable=(), image=None):
    for line in lines:
        readings = [''.join(c for c, _ in glyphs) for glyphs, _, _, _ in line]
        joined = {}
        if scope == 'prose' and image is not None:
            joined = {i: glyph for i, word in enumerate(line) if (glyph := joined_glyph(image, word))}
        candidates = prose_candidates(line, readings, recoverable, joined) if scope == 'prose' else None
        for i, (glyphs, _, word_box, _) in enumerate(line):
            word = readings[i]
            if i in joined and i in candidates:
                reading, box = joined[i]
                yield dict(char=reading, word=word, box=box, context=candidates[i])
                continue
            protected = word_letter_offsets(word)
            counts['word_letters'] += len(protected)
            if candidates is not None and i not in candidates:
                continue
            if candidates is not None and image is not None:
                glyphs = separate_punctuation(image, glyphs, word_box)
            offset = 0
            for char, box in glyphs:
                start = offset
                offset += len(char)
                if start in protected or not box or len(char) != 1:
                    continue
                if not (char.isalpha() or (candidates is not None and char in recoverable)
                        or (candidates is None and unicodedata.category(char) == 'Sm')):
                    continue
                item = dict(char=char, word=word, box=box)
                if candidates is not None:
                    item['context'] = candidates[i]
                yield item


def inspect_page(args):
    model = read_json(args.model)
    if model['format'] != FORMAT or not model['letters']:
        raise ValueError('Expected a trained YAGF prose typeface model')
    banks = {}
    for char, record in model['letters'].items():
        threshold = float(record['threshold'])
        if not 0 < threshold <= 1 or not 1 <= len(record['templates']) <= 64:
            raise ValueError('Invalid model threshold or template count')
        banks[char] = threshold, np.array([decode(m) for m in record['templates']], dtype=np.int16)
    matcher = FormulaMatcher(model['formula_letters'], banks, model.get('digit_controls')) if model.get('formula_letters') else None
    image, lines = recognize(args.image, model['language'])
    counts = Counter()
    findings = []
    for item in review_glyphs(lines, args.scope, counts, matcher.recoverable_readings if matcher else (),
                              image if matcher else None):
        char, box = item['char'], item['box']
        mask = feature(image.crop(box)) if matcher or char in banks else None
        if matcher and mask is not None:
            match = matcher.match(mask.ravel(), char)
            if match:
                suggested, distance = match
                counts['formula_match'] += 1
                findings.append(dict(item, status='formula_match', suggested=suggested, distance=round(distance, 4)))
                continue
        if len(char) > 1 or char.isdecimal() or char in GREEK_OCR_SIGNS:
            continue
        if char not in banks:
            counts['unknown'] += 1
            findings.append(dict(item, status='unknown'))
            continue
        if mask is None:
            counts['unusable'] += 1
            continue
        threshold, bank = banks[char]
        ratio = nearest(mask.ravel(), bank) / threshold
        if ratio <= 1:
            counts['ordinary'] += 1
            continue
        counts['unusual'] += 1
        findings.append(dict(item, status='unusual', score=round(ratio, 4)))
    findings.sort(key=lambda row: (row['status'] != 'formula_match', row.get('distance', -row.get('score', 0))))
    write_json(args.output, dict(image=args.image.name, size=image.size, scope=args.scope, counts=counts, findings=findings))
    print(json.dumps(counts))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    p = commands.add_parser('collect', help='Extract trusted word shapes from page images')
    p.add_argument('--language', required=True, help='Tesseract language code')
    p.add_argument('--wordlist', type=Path, help='UTF-8 list of word forms (default: selected Tesseract dictionary)')
    p.add_argument('--min-length', default=5, type=int)
    p.add_argument('--confidence', default=90, type=float, help='Minimum Tesseract word confidence (0–100)')
    p.add_argument('output', type=Path)
    p.add_argument('images', type=Path, nargs='+')
    p.set_defaults(run=collect)
    p = commands.add_parser('train', help='Fit and calibrate a portable book-specific model')
    p.add_argument('samples', type=Path)
    p.add_argument('output', type=Path)
    p.set_defaults(run=train)
    p = commands.add_parser('learn-formulas', help='Add Greek and math italic examples from trusted formulas')
    p.add_argument('model', type=Path)
    p.add_argument('formulas', type=Path, help='Saved YAGF project or manifest of transcribed formula crops')
    p.add_argument('output', type=Path)
    p.set_defaults(run=learn_formulas)
    p = commands.add_parser('inspect', help='Find isolated math letters in running text')
    p.add_argument('--scope', choices=('prose', 'page'), default='prose', help='Search running text (default) or all page content')
    p.add_argument('model', type=Path)
    p.add_argument('image', type=Path)
    p.add_argument('output', type=Path)
    p.set_defaults(run=inspect_page)
    args = parser.parse_args()
    if args.command == 'collect' and (args.min_length < 2 or not 0 <= args.confidence <= 100):
        parser.error('Use a minimum length of at least 2 and a confidence between 0 and 100')
    inputs = ([*args.images, *([args.wordlist] if args.wordlist else [])] if args.command == 'collect'
              else [args.samples] if args.command == 'train'
              else [args.model, args.formulas] if args.command == 'learn-formulas' else [args.model, args.image])
    if args.output.resolve() in {path.resolve() for path in inputs}:
        parser.error('Choose an output path different from the input files')
    try:
        args.run(args)
    except (ValueError, OSError, KeyError, TypeError, ET.ParseError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
