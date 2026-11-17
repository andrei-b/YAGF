# YAGF Revival prototype

This is the first vertical slice of the YAGF 2.0 proposal. The Qt 4 application remains in the repository root. This prototype uses Qt 6 and C++23 and requires no cloud service.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the source layout, dependency boundaries, and next implementation slices.

![OCR prototype with linked word boxes](docs/preview.png)

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

Open an image or PDF, press **Recognize**, then click a blue source box or its word in the list. Double-click a word in the list to correct it. **Save project** writes a versioned `.yagf.json` file with corrections; **Open project** restores the image, boxes, and edits. The title shows `*` for unsaved work, and opening another document or closing the window prompts to save. Export text or structured JSON; both use the corrected word. PDF import currently renders its first page at 150 DPI. OCR runs in a subprocess so the interface stays responsive, and **Cancel** stops the subprocess.

The JSON schema is version 1. It records the source path, page dimensions, Tesseract language, each word's geometry, original reading, current reading, confidence, and review state. Coordinates are pixels in the rendered image. Corrections survive a rerun when the recognized word and box match exactly. Projects reference their original source file by absolute path, so that file must remain available. Multiple pages, PDF output, and layout editing are next steps.

## Audit and direction

The original root CMake build requires Qt 4 and Aspell. The old UI is oriented around image regions and free text rather than persistent word geometry. Its sources are GPL-3.0-or-later (see `COPYING`). Reusing selected image tools may be useful later, but directly porting the full UI would delay the proposal's defining source-linked correction workflow. The new code therefore starts as an independent Qt 6 frontend in this directory.
