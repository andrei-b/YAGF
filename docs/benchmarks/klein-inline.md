# Inline mathematics on Klein file pages 74 and 75

The reference is a visual transcription of five expressions in the PDF associated with `klein1.yagf.json`. The project contains 435 pages and has no words marked as reviewed. Its saved OCR was used as editor input. File pages 74 and 75 correspond to printed pages 70 and 71. Other pages from the Klein volume have previously supplied regression fixtures, so this is a held-out **page** sample.

Render each page with `pdftoppm -f N -l N -singlefile -r 150 -png`. Coordinates below are pixel rectangles in x, y, width, height order. Both the desktop and Jetson received crops from the same renders. Comparisons ignore outer delimiters, LaTeX spacing, grouping around exponents and trailing sentence punctuation.

| File page | Crop | Verified reading | RapidLaTeXOCR on desktop | PaddleOCR‑VL‑1.6 on Jetson |
| --- | --- | --- | --- | --- |
| 74 | 141,907,120,40 | `\xi=\frac{m^2-n^2}{m^2+n^2}` | Wrong initial symbol and relation | Exact |
| 74 | 297,907,128,40 | `\eta=\frac{2mn}{m^2+n^2}` | Wrong initial symbol and relation | Exact |
| 75 | 105,66,120,24 | `a=m^2-n^2` | Exact | Exact |
| 75 | 249,67,88,23 | `b=2mn` | Exact | Exact |
| 75 | 361,66,120,24 | `c=m^2+n^2` | Read the first squared term as `p n^2` | Exact |

## Detection and notation

The desktop block run used Russian Tesseract text OCR and local RapidLaTeXOCR through `recognizeSmartBlocks`. The window run added the candidates returned by `paragraphFormulaWindows`, keeping the same recognizer and resolution. The search areas were (9,852,559,102) on page 74 and (14,37,559,131) on page 75. Two prose controls on page 75 used (16,176,560,83) and (16,700,560,304).

| Candidates offered for review | Candidates | Matches at IoU ≥ 0.5 | Precision | Recall | Exact expressions available |
| --- | ---: | ---: | ---: | ---: | ---: |
| Block recognition | 3 | 2 | 2/3 | 2/5 | 1/5 |
| Blocks plus windows | 10 | 4 | 4/10 | 4/5 | 1/5 |

Each reference can match once. The two additional geometric matches contained fractions from page 74 but missed their identifiers. Page 75 produced three partial readings near equals signs. Neither prose control produced a window candidate. Searching each area took 4–10 ms on this desktop. The window sweep made zero model calls. Reading all seven proposals added seven crop inferences.

Notation review uses the same crops. On the three checked left-hand identifiers from the saved page 75 row, the initial readings were Latin a, Cyrillic ф, and Latin c. The top-ranked choices retained all three, leaving one wrong-script variable in three. Invoking the ranker on the 124 letters in OCR items 40–58 of the following prose paragraph produced zero substitutions. This sample does not establish an accuracy gain for notation ranking. Automatic application is disabled. The operator can enable experimental suggestions inside the inline editor and inspect each crop and reading.

## Desktop and Jetson measurements

Three repetitions used the five verified crops, one request per crop. The page totals aggregate two requests on page 74 and three on page 75. Percentiles use the nearest-rank method. The desktop target measurement includes startup of the local helper and model. The Jetson measurement uses its existing warm PaddleOCR‑VL‑1.6 worker over loopback.

| Operation | Observations | Median | 95th percentile |
| --- | ---: | ---: | ---: |
| Desktop, one selected crop | 15 | 1.381 s | 1.721 s |
| Desktop, selected crops per page | 6 | 3.204 s | 4.213 s |
| Desktop, full block OCR per page | 6 | 3.111 s | 3.697 s |
| Jetson AGX Orin, one selected crop | 15 | 0.143 s | 0.282 s |
| Jetson AGX Orin, selected crops per page | 6 | 0.420 s | 0.481 s |

Maximum observed desktop child-process RSS was 402,252 KiB. Sampling the Jetson worker's resident memory at 50 ms intervals gave a peak of 15,132,180 KiB, including the resident model. Full-page block and window processing ran on the desktop. The Jetson served formula and table inference.

## Preservation and tables

Opening all 435 project pages, saving a temporary copy, and reopening it preserved each page's word count, canonical text and word bounds, plus the number of image regions. The inline GUI regression separately checks a precise substring edit, independent crop geometry, cancellation, recognition failure, undo, redo, reopening and inline LaTeX and DOCX export.

The Jetson read both a ruled and a borderless synthetic table as three rows and two columns, including an inline squared variable. Both results passed through the native YAGF table adapter. The backend emitted ordinary cells for the first row. Separate regression fixtures exercise explicit header flags and merged rows and columns.
