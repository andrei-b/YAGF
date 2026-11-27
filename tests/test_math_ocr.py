import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch
import tempfile
import time
import signal
from types import SimpleNamespace

import numpy as np
from PIL import Image, ImageDraw

spec = importlib.util.spec_from_file_location("math_ocr", Path(__file__).resolve().parents[1] / "tools/math_ocr.py")
math_ocr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(math_ocr)


class MathInputTests(unittest.TestCase):
    def test_distant_specks_do_not_widen_formula(self):
        image = Image.new("L", (120, 50), 255)
        draw = ImageDraw.Draw(image)
        draw.rectangle((40, 15, 49, 34), fill=0)
        draw.rectangle((70, 15, 79, 34), fill=0)
        draw.rectangle((3, 20, 5, 21), fill=0)
        draw.point((115, 25), fill=0)
        cleaned, _ = math_ocr.clean_crop(image)
        self.assertEqual(cleaned.size, (40, 20))

    def test_decimal_point_and_prime_are_retained(self):
        image = Image.new("L", (120, 60), 255)
        draw = ImageDraw.Draw(image)
        draw.rectangle((40, 20, 49, 39), fill=0)
        draw.rectangle((70, 20, 79, 39), fill=0)
        draw.rectangle((58, 37, 59, 38), fill=0)
        draw.rectangle((82, 14, 83, 16), fill=0)
        cleaned, _ = math_ocr.clean_crop(image)
        self.assertEqual(cleaned.size, (44, 26))
        self.assertEqual(cleaned.getpixel((18, 23)), 0)
        self.assertEqual(cleaned.getpixel((42, 0)), 0)

    def test_prose_page_is_not_squeezed_into_one_formula(self):
        image = Image.new("L", (400, 1000), 255)
        draw = ImageDraw.Draw(image)
        for y in range(10, 990, 25):
            for x in range(10, 390, 15):
                draw.rectangle((x, y, x+7, y+12), fill=0)
        with self.assertRaisesRegex(ValueError, "Turn off Math only"):
            math_ocr.prepare_crop(image)

    def test_empty_crop_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "No mathematical ink"):
            math_ocr.prepare_crop(Image.new("L", (100, 50), 255))

    def test_isolated_speck_is_not_an_equation(self):
        image = Image.new("L", (100, 80), 255)
        image.putpixel((10, 10), 0)
        self.assertEqual(list(math_ocr.equation_boxes(image)), [])

    def test_formula_syntax_is_checked(self):
        for text in [r"\frac{1}{3}", r"\begin{array}{cc}a&b\\c&d\end{array}", r"\{x\}"]:
            math_ocr.validate_latex(text)
        for text in [r"\frac{1}{", r"\begin{array}x\end{matrix}", "}x{", "{}" * 21]:
            with self.subTest(text=text), self.assertRaises(ValueError):
                math_ocr.validate_latex(text)

    def test_decoder_adapter_changes_token_and_requires_matching_weights(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            weights = {"decoder": path / "weights"}
            weights["decoder"].write_bytes(b"base decoder")
            delta = np.zeros((5, 5))
            delta[1, 4] = 1
            np.savez(path / "adapter.npz", version=1, weights=math_ocr.weight_signature(weights), delta=delta)
            adapter = math_ocr.DecoderAdapter(path / "adapter.npz", weights)
            weights["decoder"].write_bytes(b"different decoder")
            with self.assertRaisesRegex(ValueError, "does not match"):
                math_ocr.DecoderAdapter(path / "adapter.npz", weights)

        logits = [np.array([0, 0, 0, 2, 1.5]), np.array([0, 0, 1, 0, 0])]
        decoder = SimpleNamespace(bos_token=1, eos_token=2, max_seq_len=5,
                                  decoder=SimpleNamespace(session=lambda _: [logits.pop(0)[None, None]]))
        tokenizer = SimpleNamespace(tokenizer=SimpleNamespace(get_vocab_size=lambda: 5),
                                    token2str=lambda tokens: [str(tokens.item())])
        reader = SimpleNamespace(encoder_decoder=decoder, tokenizer=tokenizer, post_process=str)
        self.assertEqual(adapter.decode(reader, None), "4")

    def test_retry_preserves_first_reading_when_second_view_times_out(self):
        policy = math_ocr.FormulaRetryPolicy.__new__(math_ocr.FormulaRetryPolicy)
        policy.gate, policy.margin = 0.20, 0.08
        trace = {}
        reads = [{"text": "x=1", "loss": 0.5}, {"error": "Timed out", "loss": None}]
        with patch.object(math_ocr, "read_formula_view", side_effect=reads) as view:
            with patch.object(math_ocr.time, "monotonic", side_effect=[0.0, 1.0]):
                self.assertEqual(policy.read(None, None, 3.0, trace), "x=1")
        self.assertEqual(view.call_args_list[1].args[-1], 2.0)
        self.assertTrue(trace["attempted"])
        self.assertFalse(trace["selected"])

    def test_one_bad_crop_does_not_discard_other_equations(self):
        image = Image.new("L", (100, 60), 255)
        ImageDraw.Draw(image).rectangle((10, 10, 19, 29), fill=0)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "scan.png"
            image.save(path)
            with patch.object(math_ocr, "LaTeXOCR") as constructor:
                constructor.return_value.return_value = (r"x=1", 0)
                result = math_ocr.recognize(path, [(0, 0, 30, 40), (50, 0, 80, 40), (0, 0, 30, 40)])
        self.assertEqual([entry.get("text") for entry in result], ["x=1", None, "x=1"])
        self.assertIn("No mathematical ink", result[1]["error"])
        self.assertEqual(constructor.return_value.call_count, 2)

    def test_time_budget_preserves_completed_readings(self):
        image = Image.new('L', (30, 40), 255)
        ImageDraw.Draw(image).rectangle((5, 5, 15, 25), fill=0)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'scan.png'; image.save(path)
            with patch.object(math_ocr, 'create_model') as constructor, patch.object(math_ocr.time, 'monotonic', side_effect=[0,0,2,2]):
                constructor.return_value.return_value = ('x=1', 0)
                progress = []
                result = math_ocr.recognize(path, [(0,0,30,40)]*3, time_budget=1, progress=lambda n,total: progress.append((n,total)))
        self.assertEqual(result[0]['text'], 'x=1')
        self.assertEqual([r.get('error') for r in result[1:]], ['Formula time budget exhausted']*2)
        self.assertEqual(progress, [(1,3),(2,3),(3,3)])
        constructor.return_value.assert_called_once()

    @unittest.skipUnless(hasattr(signal, 'setitimer'), 'Requires Unix crop deadlines')
    def test_slow_crop_does_not_block_other_formulas(self):
        image = Image.new('L', (30, 40), 255)
        ImageDraw.Draw(image).rectangle((5, 5, 15, 25), fill=0)
        calls = 0
        def model(_):
            nonlocal calls
            calls += 1
            if calls == 1: time.sleep(.2)
            return 'x=1', 0
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'scan.png'; image.save(path)
            with patch.object(math_ocr, 'create_model', return_value=model):
                result = math_ocr.recognize(path, [(0,0,30,40)]*2, crop_timeout=.02)
        self.assertIn('time limit', result[0]['error'])
        self.assertEqual(result[1]['text'], 'x=1')


if __name__ == "__main__":
    unittest.main()
