import base64
import contextlib
import hashlib
import io
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path
from types import SimpleNamespace

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit(77)

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import prose_typeface as pt


class ProseTypefaceTest(unittest.TestCase):
    def test_running_text_regions_and_word_boundaries(self):
        def line(*tokens):
            x = 0
            result = []
            for token in tokens:
                text, top = token if isinstance(token, tuple) else (token, 20)
                glyphs = [(c, (x+7*i, top, x+7*i+7, top+12)) for i, c in enumerate(text)]
                right = glyphs[-1][1][2]
                result.append((glyphs, 95, (x, top, right, top+12), True))
                x = right+4
            return result
        lines = [line('для', 'каждого', 'x,', 'а', 'также', 'β.'),
                 line('области', '(G)', 'точки'), line('x', '=', 'y'),
                 line('предел', 'x', '=', '0', 'существует'),
                 line('функция', 'f(z)', 'непрерывна'),
                 line('функция', 'Г.', '(р)', 'непрерывна'),
                 line('значение', 'sin', 'x', 'задано'),
                 line('слово', ('β', 2), 'напечатано'), line('italicized', 'words')]
        items = list(pt.review_glyphs(lines, 'prose', pt.Counter()))
        self.assertEqual([r['char'] for r in items], ['x', 'а', 'β', 'G'])
        self.assertIn('каждого x,', items[0]['context'])
        self.assertEqual(items[-1]['box'], lines[1][1][0][1][1])
        self.assertFalse(list(pt.review_glyphs([line('β')], 'prose', pt.Counter())))
        self.assertEqual([r['char'] for r in pt.review_glyphs([line('β')], 'page', pt.Counter())], ['β'])

    def test_formula_alignment(self):
        image = Image.new('L', (28, 16), 255)
        for rect in [(1, 1, 7, 14), (11, 5, 17, 6), (11, 9, 17, 10), (20, 4, 27, 12)]:
            image.paste(0, rect)
        self.assertEqual([c for c, _ in pt.aligned_formula_glyphs(image, r'\(\alpha=\mathit{x}\)')], ['α', 'x'])
        for reading in [r'\alpha x', r'\mathrm{a}=x', r'\sin x', r'\frac{a}{x}', 'sin', 'a_{i}^{k}', 'a^']:
            self.assertFalse(pt.aligned_formula_glyphs(image, reading), reading)
        image.paste(0, (11, 6, 17, 9))
        self.assertFalse(pt.aligned_formula_glyphs(image, r'\alpha=x'))

    def test_isolated_glyph_with_bad_ocr_boxes(self):
        image = Image.new('L', (140, 20), 255)
        image.paste(0, (34, 5, 45, 14))
        image.paste(0, (47, 12, 49, 16))
        image.paste(0, (70, 5, 81, 14))
        def token(text, boxes, confidence=96):
            bounds = (min(b[0] for b in boxes), min(b[1] for b in boxes),
                      max(b[2] for b in boxes), max(b[3] for b in boxes))
            return list(zip(text, boxes)), confidence, bounds, False
        prose = token('слово', [(3+4*i, 4, 7+4*i, 16) for i in range(5)])
        clipped = token('а,', [(34, 5, 38, 14), (34, 5, 49, 16)])
        split = token('пд', [(70, 5, 74, 14), (70, 5, 81, 14)], 29)
        following = token('слово', [(50+3*i, 4, 53+3*i, 16) for i in range(5)])
        lines = [[prose, clipped, following, split]]
        def review():
            return list(pt.review_glyphs(lines, 'prose', pt.Counter(), image=image))
        self.assertEqual([(r['char'], r['box']) for r in review()],
                         [('а', (34, 5, 45, 14)), ('пд', (70, 5, 81, 14))])
        # Even overlapping OCR boxes must represent one connected printed glyph.
        image.paste(255, (75, 5, 77, 14))
        self.assertEqual([r['char'] for r in review()], ['а'])
        image.paste(0, (75, 5, 77, 14))
        for boxes, confidence in [([split[2], split[2]], 20), ([b for _, b in split[0]], 96)]:
            lines[0][-1] = token('на', boxes, confidence)
            self.assertEqual([r['char'] for r in review()], ['а'])
        # An ambiguous image split cannot move the punctuation or letter boxes.
        image.paste(0, (45, 11, 48, 14))
        self.assertEqual(review()[0]['box'], clipped[0][0][1])
        lines[0] = [following, split, token('=', [(83, 5, 91, 14)])]
        self.assertFalse(review())

    def test_parenthesized_prose_quotes_preserve_word_and_formula_filters(self):
        def line(*tokens):
            x = 0
            result = []
            for word in tokens:
                boxes = [(x+i*5, 0, x+i*5+5, 12) for i in range(len(word))]
                x = boxes[-1][2]+4
                result.append((list(zip(word, boxes)), 95, (boxes[0][0], 0, boxes[-1][2], 12), False))
            return result
        lines = [line('(«а', 'меньше', 'или', 'равно', 'б»)'),
                 line('(«целое', 'слово»)'), line('функция', '(«а', '=', 'б»)')]
        self.assertEqual([r['char'] for r in pt.review_glyphs(lines, 'prose', pt.Counter())], ['а', 'б'])

    def test_greek_recovery_needs_distinct_print_and_prose_context(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            root = Path(directory)
            def stamp(name, rects):
                crop = Image.new('L', (20, 20), 255)
                for rect in rects:
                    crop.paste(0, rect)
                crop.save(root/name)
                return crop
            greek = stamp('z.png', [(2, 6, 18, 12)])
            stamp('z2.png', [(2, 6, 18, 13)])
            six = stamp('six.png', [(8, 1, 12, 19)])
            stamp('six2.png', [(8, 1, 14, 19)])
            samples = [('ζ', 'z.png'), ('ζ', 'z2.png'), ('6', 'six.png'), ('6', 'six2.png')]
            pt.write_json(root/'samples.json', dict(format='yagf-formula-crops/1', formulas=[
                dict(image=path, latex=c, reviewed=True) for c, path in samples]))
            ordinary = np.eye(pt.SIDE, dtype=np.uint8) * 255
            model = dict(format=pt.FORMAT, language='rus', letters={'а': dict(templates=[pt.encode(ordinary)], threshold=.1)})
            pt.write_json(root/'base.json', model)
            pt.learn_formulas(SimpleNamespace(model=root/'base.json', formulas=root/'samples.json', output=root/'model.json'))
            model = pt.read_json(root/'model.json')
            self.assertEqual(set(model['formula_letters']), {'ζ'})
            self.assertEqual(len(model['digit_controls']['6']), 2)
            page = Image.new('L', (350, 20), 255)
            def word(text, x, crop=greek):
                page.paste(crop, (x, 0))
                box = (x, 0, x+20, 20)
                return [(c, box) for c in text], 92, box, False
            lines = [[word('буква', 0), word('6', 22), word('внутри', 46), word('&', 68)],
                     [word('число', 96), word('6', 119, six), word('слово6', 142), word('7', 165)],
                     [word('6', 192), word('=', 212), word('0', 234)],
                     [word('буква', 270), word('©', 294)]]
            with patch.object(pt, 'recognize', return_value=(page, lines)):
                pt.inspect_page(SimpleNamespace(scope='prose', model=root/'model.json', image=root/'p.png', output=root/'report.json'))
            rows = pt.read_json(root/'report.json')['findings']
            self.assertCountEqual([(r['box'][0], r['char'], r['suggested']) for r in rows],
                                  [(22, '6', 'ζ'), (68, '&', 'ζ'), (294, '©', 'ζ')])
            banks = {'а': (.1, ordinary.reshape(1, -1).astype(np.int16))}
            mask = pt.feature(greek).ravel()
            positives = model['formula_letters']
            self.assertIsNone(pt.FormulaMatcher(positives, banks).match(mask, '6'))
            self.assertIsNone(pt.FormulaMatcher({'x': positives['ζ']}, banks, model['digit_controls']).match(mask, '6'))
            lookalikes = {'6': [pt.encode(mask), model['digit_controls']['6'][0]]}
            self.assertIsNone(pt.FormulaMatcher(positives, banks, lookalikes).match(mask, '6'))

    def test_standalone_two_can_match_printed_z(self):
        # The two slanted strokes represent distinguishable z exemplars. The bar
        # and the ring provide competing printed prose and numeral shapes.
        z = Image.new('L', (20, 20), 255)
        for i in range(3, 17):
            z.paste(0, (18-i, i, 21-i, i+1))
        z.paste(0, (3, 3, 18, 5))
        z.paste(0, (2, 15, 17, 17))
        two = Image.new('L', (20, 20), 255)
        two.paste(0, (4, 2, 15, 17))
        two.paste(255, (7, 5, 12, 13))
        def examples(im):
            other = im.copy()
            other.paste(0, (2, 12, 5, 17))
            return [pt.encode(pt.feature(crop)) for crop in (im, other)]
        zs, twos = examples(z), examples(two)
        normal = np.eye(pt.SIDE, dtype=np.uint8)*255
        banks = {'а': (.1, normal.reshape(1, -1).astype(np.int16))}
        mask = pt.feature(z).ravel()
        matcher = pt.FormulaMatcher({'z': zs}, banks, {'2': twos, '7': twos})
        self.assertEqual(matcher.match(mask, '2'), ('z', 0.0))
        self.assertIsNone(matcher.match(pt.feature(two).ravel(), '2'))
        self.assertIsNone(matcher.match(mask, '7'))
        for letters, digits in [
            ({'z': zs}, {}), ({'z': zs}, {'2': [twos[0]]*2}),
            ({'z': [zs[0]]*2, 'π': twos}, {'2': twos}), ({'x': zs}, {'2': twos}),
            ({'z': zs, 'x': zs}, {'2': twos}), ({'z': zs}, {'2': [zs[0], twos[0]]}),
            ({'z': zs}, {'2': twos, '5': [zs[0]]}),
        ]:
            self.assertIsNone(pt.FormulaMatcher(letters, banks, digits).match(mask, '2'))
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            root = Path(directory)
            model = dict(format=pt.FORMAT, language='rus', formula_letters={'z': zs},
                         digit_controls={'2': twos}, letters={'а': dict(templates=[pt.encode(normal)], threshold=.1)})
            pt.write_json(root/'model.json', model)
            page = Image.new('L', (110, 20), 255)
            page.paste(z, (25, 0))
            page.paste(two, (53, 0))
            def word(s, x):
                b = (x, 0, x+20, 20)
                return [(c, b) for c in s], 95, b, False
            lines = [[word('оси', 0), word('2', 25), word('2', 53)],
                     [word('число', 0), word('22', 25), word('слово2', 53)],
                     [word('точка', 0), word('2', 25), word('=', 53), word('0', 80)]]
            with patch.object(pt, 'recognize', return_value=(page, lines)):
                pt.inspect_page(SimpleNamespace(scope='prose', model=root/'model.json', image=root/'page.png',
                                               output=root/'report.json'))
            self.assertEqual([(r['word'], r['suggested'], r['box'][0]) for r in pt.read_json(root/'report.json')['findings']],
                             [('2', 'z', 25)])

    def test_project_formula_trust_and_input_coordinates(self):
        image = Image.new('L', (28, 16), 255)
        image.paste(0, (11, 4, 17, 14))
        png = io.BytesIO()
        image.save(png, format='PNG')
        raw = png.getvalue()
        digest = hashlib.sha256(raw).hexdigest()
        region = dict(latex='x', box=[10, 3, 9, 12], confidence=93)
        run = dict(id='one', role='primary', status='succeeded', input_sha256=digest, input_size=[28, 16],
                   processed_crop=[30, 50, 28, 16], output=dict(formulas=[region], text=[{'text': 'prose'}]))
        history = dict(inputs={digest: base64.b64encode(raw).decode()}, runs=[run])
        document = dict(pages=[dict(recognition_history=history)])
        crop, label = list(pt.project_formula_crops(document))[0]
        self.assertEqual((crop.size, label, crop.getpixel((1, 1))), ((9, 12), 'x', 0))
        for score in [False, -1, 89.9]:
            region['confidence'] = score
            self.assertFalse(list(pt.project_formula_crops(document)))
        run.update(role='manual', output={'latex': 'x'})
        decision = dict(run_id='one', processed_area=run['processed_crop'], decision='accepted', proposed_text=r'\beta')
        history['decisions'] = [decision]
        self.assertEqual(list(pt.project_formula_crops(document))[0][1], r'\beta')
        history['decisions'].append(dict(decision, decision='rejected'))
        self.assertFalse(list(pt.project_formula_crops(document)))
        history['decisions'] = [dict(decision, processed_area=[0, 0, 1, 2])]
        self.assertFalse(list(pt.project_formula_crops(document)))

    def test_learn_and_review_formula_letter(self):
        image = Image.new('L', (20, 20), 255)
        image.paste(0, (3, 5, 17, 10))
        box = (0, 0, 20, 20)
        words = [([(c, box) for c in s], 90, box, False) for s in ['л', 'xy', 'п']]
        normal = np.zeros((pt.SIDE, pt.SIDE), dtype=np.uint8)
        normal[:, 11] = 255
        prose = dict(templates=[pt.encode(normal)], threshold=.05)
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            root = Path(directory)
            base, enriched, samples = root/'base.json', root/'enriched.json', root/'crops.json'
            image.save(root/'letter.png')
            pt.write_json(base, dict(format=pt.FORMAT, language='rus', letters={'л': prose}))
            row = dict(image='letter.png', latex=r'\pi', reviewed=True)
            pt.write_json(samples, dict(format='yagf-formula-crops/1', formulas=[
                row, row, dict(row, latex='q', reviewed=False, confidence=99), dict(image='missing', latex='x')]))
            pt.learn_formulas(SimpleNamespace(model=base, formulas=samples, output=enriched))
            model = pt.read_json(enriched)
            self.assertEqual({c: len(v) for c, v in model['formula_letters'].items()}, {'π': 1})
            again = root/'again.json'
            pt.learn_formulas(SimpleNamespace(model=enriched, formulas=samples, output=again))
            self.assertEqual(model, pt.read_json(again))
            with patch.object(pt, 'recognize', return_value=(image, [words])):
                pt.inspect_page(SimpleNamespace(scope='page', model=enriched, image=root/'letter.png', output=root/'review.json'))
            report = pt.read_json(root/'review.json')
            self.assertEqual(report['counts'], dict(word_letters=2, formula_match=2))
            self.assertEqual([(f['char'], f['suggested']) for f in report['findings']], [('л', 'π'), ('п', 'π')])
            mask = pt.feature(image).ravel()
            own_prose = {'л': (.05, np.array([mask], dtype=np.int16))}
            self.assertIsNone(pt.FormulaMatcher(model['formula_letters'], own_prose).match(mask))
            ambiguous = {'x': [pt.encode(mask)], 'π': [pt.encode(mask)]}
            self.assertIsNone(pt.FormulaMatcher(ambiguous, {'л': (.05, normal.reshape(1, -1))}).match(mask))

    def test_inspect_skips_word_letters_in_both_review_lists(self):
        # A familiar letter with mismatched ink should be unusual on its own.
        # All other letters in this fixture have no reference in the model.
        image = Image.new('L', (30, 30), 255)
        image.paste(0, (3, 12, 27, 17))
        reference = np.zeros((pt.SIDE, pt.SIDE), dtype=np.uint8)
        reference[:, 12] = 255
        box = (0, 0, 30, 30)
        tokens = ['«контура»,', 'по-русски', "l’école", 'на', 'x-y', 'sin(x)', 'если(x=у)', 'x²', 'a\u0301b', 'е.']
        words = [([(c, box) for c in token], 20, box, True) for token in tokens]
        words.append(([('фи', box), ('к', box), ('с', box)], 98, box, False))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root/'model.json'
            report = root/'review.json'
            record = dict(threshold=.05, templates=[pt.encode(reference)])
            pt.write_json(model, dict(format=pt.FORMAT, language='rus', letters={'x': record, 'у': record}))
            with patch.object(pt, 'recognize', return_value=(image, [words])), contextlib.redirect_stdout(io.StringIO()):
                pt.inspect_page(SimpleNamespace(scope='page', model=model, image=root/'page.png', output=report))
            result = pt.read_json(report)
        self.assertCountEqual([(r['word'], r['char'], r['status']) for r in result['findings']], [
            ('x-y', 'x', 'unusual'), ('x-y', 'y', 'unknown'), ('sin(x)', 'x', 'unusual'),
            ('если(x=у)', 'x', 'unusual'), ('если(x=у)', '=', 'unknown'), ('если(x=у)', 'у', 'unusual'),
            ('x²', 'x', 'unusual'), ('е.', 'е', 'unknown')])
        self.assertEqual(result['counts']['word_letters'], sum(map(len, [
            'контура', 'порусски', 'lécole', 'на', 'sin', 'если', 'ab', 'фикс'])))

    def test_word_filter_and_alignment(self):
        def trusted(text, confidence=96, styled=False):
            glyphs = [(c, (i, 0, i+1, 8)) for i, c in enumerate(text)]
            return pt.trusted_word(pt.lexical_core(glyphs), confidence, styled, {'функцию'}, 5, 90)
        self.assertTrue(trusted('«функцию»,'))
        for text in ('x', 'функц2ию', 'функцxию', 'ФУНКЦИЮ', 'фикция'):
            self.assertFalse(trusted(text), text)
        self.assertFalse(trusted('функцию', 40))
        self.assertFalse(trusted('функцию', styled=True))
        self.assertFalse(pt.trusted_word([('а', (0, 0, 8, 8))]*5, 99, False, {'ааааа'}, 5, 90))

    def test_calibration_and_rare_letters(self):
        normal = np.zeros((pt.SIDE, pt.SIDE), dtype=np.uint8)
        normal[:, 12] = 255
        alternate = normal.T.copy()
        pages = []
        for index in range(4):
            words = [dict(word=text, slant=0, masks=[pt.encode(normal)]*len(text))
                     for text in ('ааааб', 'аааав', 'ааааг') for _ in range(3)]
            words.append(dict(word='ааааа', slant=.32, masks=[pt.encode(alternate)]*5))
            pages.append(dict(name=str(index), sha256=str(index), words=words))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dest = root/'model.json'
            samples = root/'samples.json.gz'
            pt.write_json(samples, dict(format=pt.SAMPLES, pages=pages, language='rus', confidence=90, min_length=5))
            with contextlib.redirect_stdout(io.StringIO()):
                pt.train(SimpleNamespace(samples=samples, output=dest))
            model = pt.read_json(dest)
            self.assertEqual(set(model['letters']), {'а'})
            self.assertEqual(model['stats']['calibration_pages'], ['2'])
            self.assertEqual(model['stats']['style_accepted_words'], 36)
            record = model['letters']['а']
            bank = np.array([pt.decode(m) for m in record['templates']], dtype=np.int16)
            self.assertLess(pt.nearest(normal.ravel(), bank), record['threshold'])
            self.assertGreater(pt.nearest(alternate.ravel(), bank), record['threshold'])
            pages[2]['sha256'] = '0'
            pt.write_json(samples, dict(format=pt.SAMPLES, pages=pages))
            with self.assertRaisesRegex(ValueError, 'distinct pages'):
                pt.train(SimpleNamespace(samples=samples, output=dest))
            self.assertEqual(pt.read_json(dest), model)

    def test_blank_crop_and_invalid_coordinate(self):
        self.assertIsNone(pt.feature(Image.new('L', (30, 30), 255)))
        self.assertIsNone(pt.bbox('0 0 11 10', (10, 10)))
        self.assertEqual(pt.bbox('1 2 8 9', (10, 10)), (1, 2, 8, 9))


if __name__ == '__main__':
    unittest.main()
