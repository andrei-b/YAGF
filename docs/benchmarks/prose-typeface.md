# Book-specific prose typeface pilot

The desktop rendered the Russian calculus scan used in the formula regression manifest at 300 DPI. Tesseract with its Russian language model supplied the word readings, confidence and character boxes. Its LSTM word dictionary provided the accepted spellings. A Jetson AGX Orin fit the model on CPU from the resulting compressed letter samples.

| Training measurement | Result |
| --- | ---: |
| Pages submitted | 16 |
| Confident long dictionary words | 1,975 |
| Words retained after the slant check | 1,785 |
| Fitting glyphs in modeled letter classes | 10,196 |
| Calibration glyphs in modeled letter classes | 3,257 |
| Distinct modeled letters, case-sensitive | 32 |

The input page order was 60–68, 70, 71 and 74–78. Pages 62, 66, 71 and 77 set the per-letter thresholds. The remaining twelve pages supplied the templates.

## Results on unseen scans with the word filter

Pages 69, 72, 73, 79 and 119 were held out from fitting and calibration. A formula region counted as reached when it contained the center of at least one reported glyph. The [regression manifest](../../tests/data/math/regressions/manifest.json) supplies the rectangles at 150 DPI, scaled to the inspected images.

| File page | Letters skipped inside words | Unusual shapes | Unmodeled characters | Known formula regions containing an unusual shape | Regions also counting unmodeled characters |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 69 | 1,295 | 14 | 25 | 7 of 10 | 10 of 10 |
| 72 | 2,055 | 6 | 27 | 1 of 1 | 1 of 1 |
| 73 | 1,835 | 29 | 53 | 1 of 1 | 1 of 1 |
| 79 | 1,638 | 27 | 20 | 0 of 1 | 1 of 1 |
| 119 | 1,522 | 21 | 36 | 1 of 1 | 1 of 1 |

The original page 69 review flagged 152 unusual shapes. Eleven of its top 20 boxes landed on prose or incorrectly segmented letters. With the word filter, all 14 remaining unusual-shape boxes were inspected. Twelve landed on mathematical material. The other two landed on a single-letter conjunction and a one-letter abbreviation. The italic words *контура* and *функция* disappeared from both review lists. A box inside a formula may still clip the glyph.

Across the five pages, filtering reduced unusual-shape flags from 731 to 97 and protected all 5,382 letters in the held-out long dictionary words. The Laurent expansion on page 79 lost its unusual-shape flags when the filter skipped letter strings inside the expression. It still had characters without a prose reference. The annotated regions cover selected expressions, so these region counts do not measure character-level recall.

## Adding positive examples from recognized formulas

The Jetson learned 46 distinct glyph templates for 22 letters from 27 visually checked formula readings on 15 other pages of the same book. The Greek examples were α, β and π. The other 19 classes were Latin variables. Expressions with unsupported layout, uncertain readings or ambiguous letter boundaries did not supply templates. The five evaluation pages, their OCR language, their resolution and their word filter were the same for both models.

| Measure on the five held-out pages | Prose model | With formula examples |
| --- | ---: | ---: |
| Clean math letters among the first five default results on each page | 15/25 (60%) | 22/25 (88%) |
| Clean math letters among all default results | 73/97 (75.3%) | 80/104 (76.9%) |
| Printed target letters individually located in 14 annotated expressions | 13/105 (12.4%) | 13/105 (12.4%) |
| Annotated expressions reached by any default box | 10/14 | 10/14 |
| Letters flagged inside 5,382 held-out dictionary-word glyphs | 0 | 0 |

The visual audit covered the union of all default flags. Crops were shuffled and shown without model scores or suggested readings. A hit required one reasonably complete printed Greek letter or italic Latin variable inside the box. A clipped stroke, a multi-letter fragment, an operator or prose counted as a miss. For recall, the denominator was counted directly on the scans of the 14 annotated expressions, including letters missing from general OCR. Superscripts and subscripts counted. Digits, operators and upright function names did not. These expressions are a selected sample from one book.

All 28 suggested letter identities agreed with the scan. They included nine n, five π, five x, four k, two f, two z and one β. Twenty-one boxes had previously appeared as unusual shapes. The other seven had required enabling characters without a prose reference. All seven were outside the 14 annotated expressions. The two models produced the same 24 default boxes graded as misses.

The strongest improvement was the order of the review list. Letter-level coverage in the annotated expressions stayed low. For example, general OCR read the printed *z* of *z = 0* as the digit 2, which bypasses the letter matcher. The word filter also matters independently of the shape match. Bypassing that filter for a diagnostic pass produced 18 formula lookalikes among the 5,382 dictionary-word letters.

## Isolated math letters in running text

A second evaluation used five complete pages from the same scan, file pages 80, 82, 83, 84 and 86. They were outside model training and development of the prose filter. Before running the comparison, a visual reading of those pages identified 18 standalone mathematical letters in sentences. Examples include the order *p* of a zero, a semicircle of radius *n*, and the contour *(L)*. Parenthesized names counted. Indexed terms, expressions, figures and letters inside words were outside this target. These 18 examples were all Latin letters. The [reference points](prose-inline-letters.json) use the original 300 DPI raster coordinates.

| Search and model | Default review entries | Entries locating a target | Targets found, of 18 |
| --- | ---: | ---: | ---: |
| Whole page, with formula examples | 102 | 6 | 6 |
| Running text, prose examples only | 14 | 5 | 5 |
| Running text, with formula examples | 15 | 6 | 6 |
| Running text, with formula examples and optional unmodeled letters | 27 | 10 | 10 |

A finding located a target when its box contained the marked point on the printed letter. The default running-text results were also checked visually. Five gave an explicit letter identity, three *n*, one *p* and one *R*, all correct. A sixth flagged the unusual shape of an italic *a*. The nine other entries included a fragment of an *x*, an abbreviation and fragments of inline expressions. Four additional hits in the optional list localized letters without suggesting an identity. The five pages contained eight mentions of the contour *(L)*. The model had no positive examples for uppercase *L*.

## Greek letters in running text

The original formula model had templates for α, β and π. On page 79, Russian text OCR read two printed ζ letters in prose as 6 and returned a character box for each. We trained the updated model on the Jetson with 59 verified Greek glyphs and 101 printed digits from other pages of the same book. The resulting model covers 19 Greek letters, including ζ, λ, μ and ξ. The printed digits serve as competing shapes when a glyph looks Greek. The updated detector identified both occurrences on page 79 as ζ.

The six-page validation used file pages 181, 203, 335, 357, 411 and 481. They were selected using the book’s existing text layer for passages about parameters, angles and eigenvalues. All six were outside training and development. A visual reading of the full scans marked 29 standalone Greek letters before running the final comparison. Indexed terms, diagrams and mathematical expressions were outside the target. The [reference points](prose-greek-letters.json) are in native scan pixels.

| Greek suggestions in the default running-text review | Previous model and detector | Updated model and detector |
| --- | ---: | ---: |
| Correct identities at marked letters | 3/29 (10%) | 9/29 (31%) |
| Total Greek suggestions | 3 | 13 |
| Correct suggestions among those shown | 3/3 | 9/13 |
| Suggestions from expressions or indexed terms | 0 | 4 |

Each correct suggestion had to name the printed letter and contain its marked point. The four extra suggestions correctly identified a printed Greek glyph inside an expression or an indexed term. Five additional pages with no annotated Greek letters (80, 82, 83, 84 and 86) produced zero Greek suggestions with either version. Uppercase Λ had no training examples and accounted for five misses in validation. These counts describe running text in one typeface.

## Recovering single letters on scans from other books

The comparison used the same three-font model and Russian OCR before and after the change. The updated worker separates an isolated letter from punctuation when their OCR boxes overlap and the printed shapes have distinct horizontal spans. It also handles letters at the edge of parenthesized quotations and can match a single connected glyph that OCR divided into two overlapping letters. Seven development pages from Artin, Courant and Robbins, and Bohm had 61 marked letters. Correct names rose from 14 to 21, with six incorrect or out-of-scope names in each run.

Six fresh pages from Artin (25, 35), Grünbaum (14, 30) and Bergmann (138, 243) had 80 marked letters, including 12 Greek letters. The references were read from the scans before scoring. Page numbers are one-based file pages.

| Full-page running-text result on the six fresh pages | Previous worker | Updated worker | Updated worker with additional Greek examples |
| --- | ---: | ---: | ---: |
| Correct names for standalone letters | 8/80 | 11/80 | 11/80 |
| Correct names for standalone Greek letters | 1/12 | 1/12 | 1/12 |
| Incorrect or out-of-scope named suggestions | 6 | 5 | 5 |

The Jetson accepted 114 of 116 visually reviewed Greek crops from four other books. They supplied no additional correct names on these six pages. A separate experiment trained a model for each of two editions using seven printed examples from Artin 26 and three from Bergmann 139 and 244. On the subsequent three-page test, the Artin model found five of the six standalone σ letters on page 27 and flagged five more σ letters with a subscript or exponent. Five of the six reference glyphs on this page had a normalized bitmap identical to a training example. The Bergmann model found neither standalone λ on page 246 and produced no Greek suggestions on the control page 141. These tests measure recognition within the same digitized editions.

## Italic z read as 2 in Klein

Russian OCR read the last coordinate in «оси x, y, z одной системы» on file page 101 as 2. The running-text review now checks whether an isolated OCR 2 matches a printed italic z. Five verified z shapes and seven printed twos from other pages supplied additional examples for the Klein model. On development pages 96–110, the updated detector recovered two of three such letters with the general model and all three with the Klein model.

We visually checked all 40 eligible isolated OCR readings of 2 on 46 additional pages at 150 DPI. These pages were separate from development and training and included passages with coordinate names and numbered references. Sixteen glyphs were italic z, 23 were genuine twos and one was part of a ≠ sign.

| Running-text review | Correct z suggestions out of 16 | Genuine twos suggested as z out of 23 |
| --- | ---: | ---: |
| Previous detector, general model | 0 | 0 |
| Updated detector, general model | 15 | 0 |
| Updated detector, Klein model | 15 | 0 |

The updated review also skipped the partial ≠ sign. The missed z on file page 402 had a five-pixel-wide OCR box that clipped the printed letter. These counts cover isolated glyphs that OCR read as 2 in this scan.
