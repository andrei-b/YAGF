# Whole-formula recognition on Old Scans Math

YAGF’s **Recognize** action recovered **93/458 complete reference expressions (20.3%)** on the 36 pages. The two RapidLaTeXOCR baselines recovered 6/458 with Smart Blocks and 19/458 with PDF-Extract-Kit. Against the latter, 78 rules changed from a miss to a pass and 4 changed from a pass to a miss.

| Full-page pipeline | Whole expression | Reference contained in output | Prose controls with formula output | Distinct outputs in controls |
| --- | ---: | ---: | ---: | ---: |
| Smart Blocks + RapidLaTeXOCR | 6/458 | 20/458 | 0/11 | 0 |
| PDF-Extract-Kit + RapidLaTeXOCR | 19/458 | 64/458 | 0/11 | 0 |
| YAGF Recognize with PDF-Extract-Kit + UniMERNet | 93/458 | 228/458 | 0/11 | 0 |

Both recognition metrics use the same frozen rules and page images. A whole-expression pass requires an exact LaTeX string or a successful rendered comparison in both directions. The containment column uses the published one-way comparison and can credit part of a larger expression. Both metrics search the outputs on the reference page. The prose audit counts an output when at least half of its box lies inside a reviewed prose or heading rectangle. The eleven controls sample specific regions on ten pages.

## Whole expressions by book

| Source volume | Rules | Smart Blocks + Rapid | Detector + Rapid | YAGF Recognize |
| --- | ---: | ---: | ---: | ---: |
| advancedalgebra00schu | 123 | 0 | 3 | 43 |
| doctrinepermuta00newtgoog | 38 | 0 | 0 | 0 |
| in.ernet.dli.2015.212519 | 112 | 2 | 2 | 10 |
| in.ernet.dli.2015.222025 | 104 | 0 | 3 | 8 |
| integralcalculus00philrich | 81 | 4 | 11 | 32 |

The desktop controller returned 690 formula blocks. Eighteen pages reported a timeout or an exhausted formula budget during at least one recognition stage. The previously recorded RapidLaTeXOCR baselines transcribed the proposals from each detector directly. The YAGF run includes page OCR, the pretrained UniMERNet reader, fallback and the desktop’s CPU time budgets. A timed-out or rejected expression earns a miss unless another output on the same page satisfies the rule. [Outcomes for every rule](data/old-scans-full-page.tsv)

## Audit at the annotated location

For 62 references with independently reviewed source rectangles, the evaluator chose the accepted formula box with the greatest coverage of reference ink and used intersection over union to break ties. A box qualified when it covered at least 95% of the ink.

- 19/62 had no qualifying output box. For ten of these, the raw detector had proposed a qualifying box. For the other nine, the raw detector also missed the coverage threshold.
- 43/62 had a qualifying output. Reading only that box gave 9 whole-expression passes and 34 misses.
- 34/62 had a qualifying selected box with intersection over union of 0.5 or better. Of those 34, nine were read correctly and 25 were missed.

A well-covered box may still contain several neighboring expressions. The next reader experiment can compare the 25 missed readings from tightly localized boxes with readings from their manually verified crops. This isolates the effect of crop boundaries on the same expressions. A separate reader check on 44 manually isolated formulas from Bass, LNM 966 and Dudeney scored 9/44 for both pretrained UniMERNet and the trained candidate.

## Reproduce the page run

Configure the detector, reader and local Tesseract language data as in the [user guide](../user-guide.md). This run used English Tesseract, PDF-Extract-Kit 1.0 MFD, the UniMERNet small 2503 weights and CPU inference. Use the 36 frozen PNGs, the official Old Scans Math JSONL rules and the archived olmOCR KaTeX renderer. The scorer environment needs Playwright, a compatible Chrome executable and the renderer’s Python dependencies.

```sh
cmake --build "$BUILD_DIR" --target benchmark_mixed_formulas
"$BUILD_DIR/benchmark_mixed_formulas" fullpage.json "$SCANS"/*.png
python3 tools/benchmark_old_scans_math.py score \
  "$SCANS/old_scans_math.jsonl" "$SCANS/official/render.py" \
  strict-fullpage.json fullpage.json --strict --chrome "$CHROME" --render-workers 2
```

Run the scorer without `--strict` for the containment column. The frozen rules have SHA-256 `aa514fb0c4c06aa077cadf316c2aede9716f62088ea1011683e059f7be67a8bd`. The renderer has SHA-256 `7e04305b57255013362ecc43e183ccd319fb187d1b900c941df575b871c9df0b`.
