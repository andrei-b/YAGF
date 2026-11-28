# Rereading Tesseract subscripts

Across the four samples, stock Tesseract 5.5.4 read 120/234 annotated subscript characters correctly. Its hOCR marked none of those characters as subscripts. The experimental fork used the same page images and language weights.

| Engine | Character correct | Character and subscript placement both correct |
| --- | ---: | ---: |
| Stock Tesseract 5.5.4 | 120/234 (51.3%) | 0/234 (0%) |
| Experimental fork | 124/234 (53.0%) | 94/234 (40.2%) |
| Fork with all suggested edits | 134/234 (57.3%) | 104/234 (44.4%) |

On nine Russian *Functional Analysis* pages, Tesseract with `rus+eng` placed 33 of 70 annotated subscripts correctly and read 15 of those characters correctly. Rereading the marked character boxes with two English models raised the joint score to 26/70. Nine of the eleven gains came from repeated italic `N` indices on page 29. Running English OCR on the entire page and accepting its subscript reading at an identical box reached 16/70.

| Sample | Native character and placement correct | With all suggested edits | Corrected | Previously correct readings lost |
| --- | ---: | ---: | ---: | ---: |
| Functional Analysis, nine pages | 15/70 | 26/70 | 11 | 0 |
| OpenStax, three pages | 41/86 | 40/86 | 0 | 1 |
| Subscript verification sheet | 37/62 | 37/62 | 1 | 1 |
| Bass, MLBook and 5p crops | 1/16 | 1/16 | 0 | 0 |

The tool offers a suggestion when the starter and adapted English models read the same single Latin letter or digit from the same crop and it differs from the page reading. Both models mistook the `o` in the OpenStax subscript `Total` for zero. The Functional Analysis run produced 18 suggestions. Four fell outside its labeled targets and three changed one incorrect reading to another. The 460 Functional Analysis prose-control glyphs and 170 prose-control glyphs in the other book crops received zero suggestions.

## Run on one page

Use the experimental inline-script Tesseract for the page pass. Supply the same raster and its character-level hOCR to the review tool. The two model directories each contain an `eng.traineddata`. The Python environment needs Pillow, NumPy, SciPy and OpenCV.

```sh
"$TESSERACT_BIN" page.png page --tessdata-dir "$PAGE_MODELS" -l rus+eng \
  --oem 1 --psm 3 --dpi 300 -c lstm_detect_subscripts=1 \
  -c lstm_recover_subscripts=1 -c lstm_reread_subscripts=1 \
  -c hocr_char_boxes=1 -c tessedit_create_hocr=1
python3 tools/reread_tesseract_subscripts.py page.png page.hocr \
  --tesseract "$TESSERACT_BIN" --starter "$STARTER_MODELS" \
  --adapted "$ADAPTED_MODELS" > suggestions.json
```

Each JSON suggestion includes the original word, the zero-based glyph index within it, the source box in pixels, and the proposed reading. The tool runs separately from desktop recognition. It uses the existing padded glyph crop and Tesseract single-character mode. Subscripts missed during page segmentation or placement receive no second pass.

The SHA-256 fingerprints of the English `eng.traineddata` files used in this comparison were `8280aed0782fe27257a68ea10fe7ef324ca0f8d85bd2fd145d1c2b560bcb66ba` (starter) and `9475c6d38dde44ca0ed7271a2f035e5562380e687077e16f1eb188c316a5b2ad` (adapted).

## Placement on the same book pages

The native fork read the correct character for 124 of the 234 subscripts. Of those, 30 still needed subscript placement. To measure placement alone, the verifier kept the original text, glyph boxes and reference assignments. Both pretrained policies ran on all 21 input images. The prose check counted marks on existing glyphs and on extra ink components within the control regions.

| Policy | Correct identity and placement | Recovered out of 30 | Previously correct placements lost | Falsely lowered prose glyphs |
| --- | ---: | ---: | ---: | ---: |
| Native | 94/234 | 0 | 0 | 0/630 |
| Precision | 95/234 | 1 | 0 | 0/630 |
| Review | 81/234 | 6 | 19 | 1/630 |

The precision policy recovered an `f` on the verification sheet. Its score on each book stayed at the native value. The review policy recovered four characters in *Functional Analysis*, one on the sheet and one in OpenStax. It lost three, eight and eight existing correct placements on those sources, respectively. It also lowered a Cyrillic `с` in prose and raised a `1` in another prose control. Neither policy added a script mark to an extra ink component in the controls.

| Source | Position misses with correct character | Native exact | Precision exact | Review exact |
| --- | ---: | ---: | ---: | ---: |
| Functional Analysis | 16 | 15/70 | 15/70 | 16/70 |
| OpenStax | 9 | 41/86 | 41/86 | 34/86 |
| Verification sheet | 5 | 37/62 | 38/62 | 30/62 |
| Bass, MLBook and 5p | 0 | 1/16 | 1/16 | 1/16 |

All 30 missed glyphs had at least one candidate base. The attachment model proposed a subscript for 14, proposed a superscript for three, and offered no script role for 13. The precision threshold accepted one of the 14 subscript proposals. Lowering that threshold alone could therefore reach at most 14 of the 30 in this sample. These results support keeping native placement in the desktop and showing potential additions for human review. [Coordinates and decisions for all 30 misses](data/book-script-placement-misses.tsv)

To repeat the placement comparison, arrange the original scans and character-level hOCR under the relative paths in the [frozen manifest](data/book-script-placement.json). The benchmark checks both file hashes and the original outcome of each labeled glyph. It writes the score for each book and the decisions for each target and flagged prose glyph.

```sh
python3 tools/benchmark_book_script_placement.py \
  docs/benchmarks/data/book-script-placement.json placement-results.json \
  --inputs "$BOOK_INPUTS" --attachment "$ATTACHMENT_MODEL" --verifier "$VERIFIER_MODEL"
```
