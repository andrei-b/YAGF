# Reading small symbols in scanned formulas

On 34 tightly located Old Scans Math formulas, UniMERNet made 16 superscript edits, 5 subscript edits and 7 arithmetic operator edits at its usual input size. Doubling the PDF render resolution recovered two whole formulas and lost two. A larger encoder input recovered two and lost five. Use the extra readings to review a doubtful symbol in the original scan. The desktop reader continues to use its usual input size.

| Reading | Superscript edits (88 reference scripts) | Subscript edits (24) | Operator edits (118) | Whole formulas correct (34) |
| --- | ---: | ---: | ---: | ---: |
| Usual PDF render and encoder | 16 | 5 | 7 | 9 |
| Double PDF render resolution | 20 | 5 | 7 | 9 |
| Double PDF render resolution and larger encoder input | 16 | 6 | 7 | 6 |

On integral calculus page 238, the usual reading gave both `y` and `a` an exponent of `3/2`. Both detail readings recovered `1/2` for each. On page `3_pg204`, the double-resolution render recovered the missing plus sign between two terms with ψ. Agreement can also reproduce a mistake. Both detail readings changed the correct `x^m` to `x^n` in an integral on page `1_pg72`. Across the three readings, 12 distinct formulas had at least one correct transcription. Selecting those readings required the references.

Three of the usual reader’s five subscript edits came from a single equation on page `3_pg204`. It read the three occurrences of subscript `x` as ω, ω and α. A separate case read subscript 3 as 8. The fifth edit introduced a subscript on an integral.

## Reread an individual subscript

On the 24 reference subscripts in the audit, a local rereader improved glyph identity from **20/24 to 23/24**. It recovered `x` after each of `f`, `φ` and `ψ`. On `ω₃` it ranked 3 just ahead of 8 and abstained. It preserved every correct reading. The test used manually selected rectangles around the base and its subscript. It measures glyph identity within known pairs. The spurious subscript on the integral falls outside this test.

| Test group | Pairs | UniMERNet on pair only, correct | Exemplar matches offered | Correct matches | Usual formula reading with matches applied |
| --- | ---: | ---: | ---: | ---: | ---: |
| Audited pairs, pages 101 and 204 | 24 | 17 | 13 | 13 | 23/24 |
| Additional pairs, page 634 | 16 | 3 | 4 | 4 | Unmeasured |

All three pages came from the same book. The 20 labeled comparison glyphs came from pages 94 and 199. They cover 1, 2, 3, 8, x, y, α and ω. The test included four occurrences of z or t, which the comparison set omitted. The rereader abstained on all four. The audited pages informed development of the crop geometry and shape comparison. The page 634 check used the finished settings and illustrates the lower coverage on another page. [Selected pairs, predictions and outcomes](data/subscript-pairs.tsv) · [Labeled comparison glyphs](data/subscript-exemplars.tsv)

To check the first disputed `x`, run the following from the repository with the Old Scans Math PDFs in `$SCANS`. The command uses Python with NumPy, Pillow and OpenCV, plus Poppler’s `pdftoppm`.

```sh
python3 tools/reread_formula_subscripts.py "$SCANS/3_pg204.pdf" \
  --exemplars docs/benchmarks/data/subscript-exemplars.tsv \
  --pair 732 344 777 399
```

The result gives the proposed label and the pixel rectangle of the isolated glyph. A null `suggested` means retain the current reading. To reproduce a whole page of reviewed pairs, replace `--pair 732 344 777 399` with `--pairs docs/benchmarks/data/subscript-pairs.tsv`. The reader uses only the PDF, page and coordinates from the pair table.

For another book, make an exemplar TSV in the same format using isolated, labeled glyphs from that book. Include easily confused letters and digits. A suggestion requires support from at least two distinct boxes for its label. Coordinates refer to a page rendered with `pdftoppm -gray -scale-to 2900` by default. Use a rectangle around **both** the base and the subscript for each query. Repeat `--pair` for multiple selections. For multi-page PDFs, specify a one-based `--page` and add a one-based `page` column to the exemplar TSV.

The base establishes the relative position and size of the small mark. The reader isolates a separate lower-right ink component, preserves its aspect ratio and compares its shape with the labeled glyphs. It abstains when ink is joined, evidence is weak, or another label scores too closely. Suggestions are available through the command line for review.

## Reread a formula

Run the following command with the Python environment, MinerU checkout and UniMERNet weights configured for YAGF. It uses Poppler’s `pdftoppm`. Supply a local KaTeX JavaScript bundle and Node.js for the symbol comparison. The four pixel edges refer to the base render with its long edge at 1450 pixels. For a multi-page PDF, pass the one-based page number with `--page`.

```sh
"$YAGF_UNIMERNET_PYTHON" tools/reread_formula_symbols.py "$SCANS/2_pg238.pdf" \
  --box 649 111 795 147 --enlarged --katex "$SCANS/official/katex.min.js"
```

The JSON contains the usual and detail LaTeX readings, the mapped crop edges and the changed symbols in reading order. Each script entry gives its placement, base and content. For example, `["sup", "y", "mfrac(1,2)"]` denotes a one-half exponent on `y`. Repeat `--box` for multiple expressions on the same page. Omit `--enlarged` for the two-render comparison.

The `agreed_symbol_change` flag marks a change when both detail readings return identical LaTeX, they differ from the usual reading on a tracked symbol, and the surrounding notation matches after removing scripts and tracked operators. This flagged two formulas. One gained two correct exponents and one lost a correct exponent. Treat the flag as a prompt to inspect the scan.

## Measurement

The sample contains every [audited full-page output](old-scans-full-page.md) with at least 95% reference ink coverage and intersection over union of at least 0.5. It covers nine scanned pages from five books. The study used the same physical crop for each reading and rounded its edges outward when mapping to the larger render. The base and detail page renders had long edges of 1450 and 2900 pixels. The usual encoder canvas was 192 × 672 pixels, and the enlarged canvas was 256 × 896 (height × width). All readings used pretrained UniMERNet small 2503 on CPU, greedy decoding, a 256-token limit and a 40-second decode budget. The two standard-canvas views shared a batch. The enlarged view was decoded separately. All 102 readings produced parseable formulas.

The symbol scorer converts each reference and reading to presentation MathML with the benchmark’s KaTeX. It compares the ordered superscript and subscript sequences separately. A script includes the base to which it is attached, so a wrong base also costs an edit. It separately compares the operator sequence for `+ − = × ÷ ⋅ ± ∓ /`, including operators inside scripts. An operator moved between terms can retain its place in that sequence and escape this metric. Each insertion, deletion or substitution costs one edit. A missing operator inside an exponent can affect both columns. Whole-expression correctness uses the bidirectional rendered comparison from the full-page study.

The TSV records source rectangles, references, all three LaTeX readings, per-crop symbol edits and whole-formula scores. The symbol scores can be reproduced locally from this file. [Per-formula data](data/formula-small-symbols.tsv) · [Symbol scorer](../../tools/benchmark_formula_symbols.py)

```sh
python3 tools/benchmark_formula_symbols.py docs/benchmarks/data/formula-small-symbols.tsv scores.json \
  --modes normal detail enlarged --katex "$SCANS/official/katex.min.js"
```
