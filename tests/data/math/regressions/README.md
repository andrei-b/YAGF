# Formula OCR regression samples

Use a formula PNG to test a formula engine directly. Use its context PNG to test block detection, deskew, reading order, or the handoff from text OCR to formula OCR. The [manifest](manifest.json) gives the expected LaTeX, crop coordinates and the feature to check for every case.

| Samples | What to check |
| --- | --- |
| [Bounded integral](formulas/integral-z0-to-z.png) · [paragraph context](context/calculus-bounded-integral.png) | The upper limit is \(z\) and the lower limit is \(z_0\). Both belong to the integral. The preceding Russian abbreviation and the following derivative have separate readings. The manifest marks the operator, each limit and the old block that clipped the upper limit. |
| [Display equations](context/calculus-displays.png) · [skew variant](context/calculus-skewed-displays.png) | The equation for the poles and the parenthesized values of \(k\) belong together. The neighboring paragraph shares a blank margin. The skew variant reproduces the same expression after a controlled 2.5° clockwise rotation. |
| [Inline expressions](context/calculus-inline-paragraph.png) | The paragraph contains three assignments or inequalities. Each must retain its fractions while allowing text on both sides of the same line. |
| [Continued fractions](context/continued-fraction-definitions.png) · [decimal convergents](context/decimal-convergents.png) | Read the nested fractions and subscripts, distinguish 333/106 from 355/113, and preserve the decimal commas. The reported three-expression row is available [in full](formulas/convergents-full-row.png) and as three individual crops. These examples are from printed page 66, file page 70. |
| [Isolated glyphs](context/symbol-strip.png) · [summation](context/typeset-sum.png) | Test italic x, z, u and v, Fraktur A, and the sum, integral and product operators individually. The typeset sum adds upper and lower limits and a neighboring quotient. |

The [formula row from page 423](context/eigenvalue-formula-row.png) reproduces a whole-row timeout. A successful retry reads three approximations in order, keeps their stacked fractions intact and preserves the trailing Russian abbreviation «и т. п.».

## Coordinates and comparison

All rectangles use integer pixels in the order `[x, y, width, height]`, measured from the top left. The right and bottom edges are exclusive. A case’s `context_xywh` selects its exact formula PNG from the context image. Landmark and neighboring-block rectangles use the same context coordinates. Add the context’s `source_xywh` origin to recover page coordinates in the named source raster. Page numbers are one-based.

The expected LaTeX records the printed mathematics. Equivalent LaTeX spacing, delimiters, limit placement commands and trailing sentence punctuation are acceptable. Check fraction structure, signs, subscripts, superscripts, decimal digits and decimal commas. The scan excerpts use the original recognition resolution. The skew variant and typeset glyph controls are identified separately in the manifest.
