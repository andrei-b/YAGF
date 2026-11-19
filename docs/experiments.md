# OCR experiments in YAGF 2.2

Open **Tools → OCR experiments…**. The manager works on its own document snapshot; running OCR or verifying benchmark references does not replace project text, regions, or corrections.

Choose recognized pages with representative layouts and image quality. Every OCR item on a reference page must be explicitly verified, with no pending corrections. **Proofread reference…** displays the processed page beside editable text, with image zoom and panning. **Confirm verified reference** accepts the text as ground truth for this experiment only. Do not confirm an unchecked OCR transcript. Alternatively, use pages already verified in the main editor.

Assign at least one page to **Calibration** and one different page to **Held out**. Keep passages independent: two crops or exports of the same content are not a held-out test. Identical prepared images in different splits are rejected. The application cannot detect every near-duplicate scan or training-set overlap.

Select pipelines and configure provider, language, endpoint, DPI, local binarization, and optional automatic orientation/deskew. The defaults compare saved Tesseract settings with 300 DPI plus binarization and deskew. A PaddleOCR row is available but unchecked. Remote rows upload only the selected page images to their endpoints. Session tokens remain in memory and are excluded from saved reports. Saved photometric filters are retained by default; clear **Saved filters** to compare from source colors. Contrast, denoise, and binarization can be selected independently. Saved rotation/skew are retained unless automatic deskew is selected. Automatic deskew re-estimates rotation and fine angle from the source; uncertainty falls back to the saved rotation and is recorded.

Set measured repetitions and calibration warmups, then run. Warmups use calibration images only and are excluded from measured latency. Pipeline order rotates between repetitions. Book vocabulary, correction memory, ground-truth text, formatting estimation, and specialist suggestions are not fed into recognition. A pipeline must finish every selected page/repetition to qualify for the advisory recommendation. Selection uses pooled calibration CER, then calibration WER and total preparation-plus-recognition latency; held-out accuracy and latency never select the winner. No OCR engine changes automatically.

**Inspect readings…** compares the verified reference and a recorded hypothesis. Technical details include input hashes, engine information, preprocessing, timing, resource scope, errors, and full page geometry. **Save report…** and **Open report…** retain these hypotheses across sessions without changing a project. Reports contain document text and geometry; keep them with the same care as the source project.

## Measurements and limits

- **CER/WER:** Levenshtein edits divided by reference Unicode code points or whitespace-separated tokens. Text uses NFC, collapsed whitespace, and preserved case/punctuation. Rates can exceed 100%; an empty denominator is unknown. Aggregate rates pool edit counts and denominators, rather than averaging page percentages.
- **Geometry:** Saved word-box IoU coverage and reading-order concordance when both sides have word boxes and the same rotation/deskew frame. These are diagnostics against saved geometry, not separately verified layout ground truth. Passage-only providers and changed frames report unavailable values. Table-cell accuracy is not measured.
- **Latency:** End-to-end recognition, including image encoding and HTTP overhead. Rendering and preprocessing have separate measurements. Model startup is excluded. Repetitions of the same image can benefit from server caches; these timings are not cold-start claims. Throughput is the sequential recognition rate, not a concurrent capacity benchmark.
- **Memory:** Local Tesseract uses GNU `time` child-process peak RSS when available. Remote services report their process-lifetime RSS high-water mark. A PaddleOCR-VL bridge measures only the bridge, not the upstream model. Missing values remain unknown, and different scopes are not interchangeable. On Jetson, process RSS is not a GPU allocation or energy measurement.
- **Reproducibility:** Reports record source SHA-256, verified reference snapshots and hashes, prepared PNG hashes, complete hypotheses, configuration, timestamps, engine versions where reported, and local hardware/software identity. Unreported upstream versions stay unknown. Calibration and held-out text are stored explicitly for auditing.

One report supports 1–16 pipelines, 2–64 reference pages, 1–10 repetitions, 0–3 warmups, and at most 256 measured runs. Reference snapshots are bounded to 32 MiB, reports to approximately 128 MiB, and text scoring to 100 million edit cells per passage. Large experiments should be split into smaller reports. Cancellation retains completed records, makes incomplete results explicit, and suppresses a recommendation. Cancelling a local measured job terminates both the GNU `time` wrapper and its recognition child.

## Command-line runs

The installed `yagf2-benchmark` executable uses the same runner as the desktop dialog:

```sh
yagf2-benchmark book.yagf.json \
  --pipelines pipelines.json --calibration 2,4 --held-out 6,8 \
  --repetitions 2 --warmups 1 --output book.yagf-experiment.json
```

A pipeline file contains a `pipelines` array. `dpi` is `0` for saved page DPI, `-1` for adaptive rendering, or 150/200/300/400. For example:

```json
{"pipelines":[
  {"id":"local","label":"Tesseract 300 DPI","provider":"tesseract","language":"eng","dpi":300,"binarize":true},
  {"id":"jetson","label":"PaddleOCR-VL","provider":"paddleocr","endpoint":"http://127.0.0.1:8765","language":"en","dpi":300,"token_env":"YAGF_BENCHMARK_TOKEN"}
]}
```

Supported provider names are `tesseract`, `paddleocr`, `doctr`, `rapidocr`, `surya`, and `kraken`. `auto_deskew` adds automatic orientation and deskew. `saved_filters: false` clears saved contrast/denoise/binarization; `contrast` and `denoise` enable those filters for a candidate. Use `token_env` for a remote credential; literal `token` fields are rejected. Endpoint credentials, query strings, and fragments are excluded from reports. Exit status is 0 for a complete run, 1 for failed/incomplete pipelines, 2 for invalid input, and 130 for cancellation.

Run the read-only hardware collector **on the Jetson**, alongside inference:

```sh
python3 tools/jetson_metrics.py --duration 90 --output jetson-hardware.json
```

It records device/L4T/kernel identity, inference-process RSS, available `tegrastats` RAM/GPU/temperature samples, and timestamps. No model or application is stopped. Without `tegrastats`, GPU observations stay unknown. Supply an observation to the CLI with `--hardware-report jetson-hardware.json`. The file is read when inference finishes, allowing a concurrent collector to finish first. Choose an observation duration shorter than the benchmark, or attach observations separately afterward. The CLI checks timestamp overlap; absent, incompatible, or non-overlapping observations are marked explicitly and return status 1 while preserving recognition results. Resource use by other processes is part of system-wide samples.

## Real-document regression

The supplied `ocr-examples/Scanned Document.pdf` has a blank first page, two sideways German book spreads, and two photographed English scientific pages with shadows and curvature. Its scans are natively 300 DPI. The previous global-threshold skew detector returned 0° for every text page because paper/background shadows dominated its ink mask.

At 300 DPI, automatic orientation plus the new locally binarized detector gives:

| PDF page | Clockwise orientation correction | Fine skew | Tesseract OCR items | Items at confidence ≥70 |
| --- | ---: | ---: | ---: | ---: |
| 2 | 270° | −4.9° | 701 | 579 |
| 3 | 90° | +1.5° | 699 | 651 |
| 4 | 0° | +3.0° | 637 | 522 |
| 5 | 0° | +4.4° | 376 | 272 |

All processed pages reassess at 0° global skew. The old 150-DPI pipeline produced 485/675/518/296 items, with only 364/81/468/228 at confidence ≥70. These counts and confidence values show recovered coverage; they are **not CER/WER or proof of correctness**. The scan has no fully verified reference transcript. Curved lines, obscured/cropped text at the spine, and equations still need review. PaddleOCR-VL can repeat or truncate mathematical text; bounded section retries do not guarantee recovery. No partial page is silently accepted after a failed retry.

For this document, choose 300 DPI, **Improve page… → Detect orientation + skew**, and enable **Adaptive binarization**. Use `deu` for pages 2–3 and `eng` for pages 4–5 with Tesseract. Re-render existing pages explicitly when changing their DPI. Source images are unchanged; rotation changes use the existing OCR-reset warning and undo history.

`scanneddocument_tests` checks orientation, residual skew, word coverage on both halves of the German spreads, serialization, and identical processed pixels for OCR/export. The fixture is user-provided and not redistributed; the test skips when it is absent or has a different SHA-256. Set `YAGF_SCAN_REGRESSION_PDF` to its location. Its expected SHA-256 is `6ee9337949b87bd6bf81f157e81944979ee31fe3a74b9b0be5ff91700ef7db22`.

The small generated reference project in `tests/data/benchmark` has known text and is suitable for checking the experiment machinery. It is not representative evidence for choosing an engine for photographed scientific books. See `docs/benchmarks` for the actual Jetson smoke-run observations.
