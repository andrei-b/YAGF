import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import reread_formula_subscripts as reader


class FormulaSubscriptTests(unittest.TestCase):
    def test_lowered_digit_can_be_taller_than_base_letter(self):
        ink = np.zeros((26, 30), np.uint8)
        ink[2:17, 2:16] = 255
        ink[9:25, 18:24] = 255
        box, mask = reader.script_component(ink)
        self.assertEqual(box, (18, 9, 24, 25))
        self.assertEqual(mask.shape, (16, 6))
        # Connected ink gives no reliable boundary for the isolated character.
        ink[10:12, 16:18] = 255
        self.assertIsNone(reader.script_component(ink))

    def test_uncertain_match_when_two_labels_have_the_same_shape(self):
        ink = np.zeros((16, 16), np.uint8)
        np.fill_diagonal(ink, 255)
        shape = reader.descriptor(ink)
        match = reader.compare(ink, {'x': [shape, shape], 'χ': [shape, shape]})
        self.assertIsNone(match['suggested'])
        self.assertEqual(match['margin'], 0)

    def test_repeated_exemplar_cannot_supply_two_votes(self):
        with TemporaryDirectory() as tmp:
            directory = Path(tmp)
            table = directory/'examples.tsv'
            table.write_text('pdf\tlabel\tleft\ttop\tright\tbottom\n' + 'one.pdf\tx\t1\t2\t8\t9\n' * 2)
            with self.assertRaisesRegex(ValueError, 'duplicate exemplar'):
                reader.load_exemplars(table, directory, 2900, directory)

    def test_box_outside_original_page_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'outside'):
            reader.ink_crop(Image.new('L', (20, 20)), (2, 2, 21, 14))


if __name__ == '__main__':
    unittest.main()
