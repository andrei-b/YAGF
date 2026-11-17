# YAGF Revival prototype

This is the first vertical slice of the YAGF 2.0 proposal. The Qt 4 application remains in the repository root. This prototype uses Qt 6 and C++23 and requires no cloud service.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the source layout, dependency boundaries, and next implementation slices.

![OCR studio with page miniatures, page preview, and rich text editor](docs/preview.png)

The screenshot uses a generated sample image.

## Build

```sh
cmake -S revival -B build-revival -G Ninja
cmake --build build-revival
ctest --test-dir build-revival --output-on-failure
./build-revival/yagf-revival
# Or open a source document immediately:
./build-revival/yagf-revival /path/to/scan.png
```

Qt 6 development packages are required. Install Tesseract and language data to recognize images. PDF import also requires Poppler's `pdftoppm` command. Missing tools are reported in the app.

## Current workflow

Open an image or PDF. The left navigation strip fills with page miniatures in the background; click one or use the arrow buttons to change pages. The center is a zoomable page preview, and the right pane is a rich text editor with the page's recognized words below it. Press **Recognize** for the full page. Switch to **Select region** and drag on the preview to select overlapping OCR words, or press **Recognize region** to OCR only the selected image area. Click a word box or a word in the list to edit just that word. Use **Bold** and **Italic** in the editor, then **Apply correction**. Edits are also applied when moving to another selection or page. The editor requires the same number of words as the selection so every correction keeps its source box.

Use **Improve page…** to inspect the original beside a processed preview. **Detect skew** estimates a correction angle, which you can adjust manually; **Auto contrast** stretches faded scans, and **Denoise (grayscale)** removes isolated specks. The settings apply to the current page and to its OCR input, including region OCR. They are saved in the project and reapplied when it opens. Changing the deskew angle moves the image, so the app asks before clearing that page's existing OCR boxes and corrections. Contrast and denoise keep the existing boxes. The source image is never overwritten.

**Save project** writes a versioned `.yagf.json` file with corrections for all recognized pages; **Open project** restores the pages, boxes, edits, and word formatting. The title shows `*` for unsaved work, and opening another document or closing the window prompts to save. The **File** menu exports plain text, structured JSON, or layout HTML with positioned words. Pages are rendered on demand at 150 DPI. OCR and thumbnail generation run in subprocesses so the interface stays responsive, and **Cancel** stops the current OCR or page rendering job.

The project and document JSON schemas are version 2. They record the source path, source and processed page dimensions, per-page quality settings, Tesseract language, stable page/region/paragraph/line/word IDs, reading order, each word's geometry, original reading, current reading, confidence, review state, and basic formatting. Version 1 project files still load. Coordinates are pixels in the processed page image. Corrections survive a rerun when the recognized word and box match exactly. Projects reference their original source file by absolute path, so that file must remain available. OCR does not recover exact fonts or complete column layout; the editor keeps detected line and paragraph breaks and estimates relative text size. Searchable PDF output, editable layout geometry, and batch recognition are next steps.

## Audit and direction

The original root CMake build requires Qt 4 and Aspell. The old UI is oriented around image regions and free text rather than persistent word geometry. Its sources are GPL-3.0-or-later (see `COPYING`). Reusing selected image tools may be useful later, but directly porting the full UI would delay the proposal's defining source-linked correction workflow. The new code therefore starts as an independent Qt 6 frontend in this directory.
