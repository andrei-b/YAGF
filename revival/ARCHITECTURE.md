# Code organization

The historical YAGF application stays at the repository root. `revival/` is the Qt 6 application under active development.

```text
revival/
  CMakeLists.txt
  src/
    main.cpp                         application entry point
    core/ocrdocument.{h,cpp}         OCR data, TSV normalization, text/JSON export
    core/projectfile.{h,cpp}         versioned project save/load, atomic write
    processing/documentprocessor.{h,cpp}  subprocesses and temporary PDF rendering
    ui/mainwindow.{h,cpp}            page view, selection, editing, user actions
  tests/ocrdocument_tests.cpp
  tests/projectfile_tests.cpp
```

`ui` depends on `core` and `processing`. `core` has no widget or process dependency. `processing` has no widget or document-model dependency. This boundary lets a future PaddleOCR or Jetson adapter provide the same normalized document data without changing the editor.

## Data ownership

The `OcrDocument` is the current in-memory source of truth. A word stores its source box, original reading, corrected reading, confidence, and review state. Both exports and the project file read from this document. The UI draws boxes and shows the word list from it. The next model change should introduce stable page/region/paragraph/line/word IDs and explicit reading order; the current flat `words` vector only carries grouping numbers from Tesseract TSV.

## Processing boundary

`DocumentProcessor` owns `QProcess` and the temporary directory for PDF rendering. It emits PDF image paths or Tesseract TSV and reports failures. The UI turns TSV into `OcrDocument`. Later engine adapters should return normalized OCR results through a common interface and identify engine version and capabilities. External commands must receive argument lists, never a shell command string.

## Next implementation slices

1. Replace the flat word vector with the source-linked hierarchy and stable IDs before adding layout edits or multiple engines.
2. Add all-page PDF import and page navigation, then searchable PDF export through OCRmyPDF.
3. Add optional neural and Jetson adapters behind the same processing boundary, with measured comparison data.
