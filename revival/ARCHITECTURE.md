# Code organization

The historical YAGF application stays at the repository root. `revival/` is the Qt 6 application under active development.

```text
revival/
  CMakeLists.txt
  src/
    main.cpp                         application entry point
    core/ocrdocument.{h,cpp}         OCR data, text/JSON export
    core/wordeditor.{h,cpp}          source-linked split, merge, insert, delete
    core/projectfile.{h,cpp}         versioned and portable project save/load
    core/preprocesssettings.h         per-page, persisted quality options
    processing/documentprocessor.{h,cpp}  async OCR jobs, PDF rendering and batching
    processing/iocrengine.h          engine contract and capability flags
    processing/tesseractengine.{h,cpp}    Tesseract subprocess and TSV adapter
    processing/remoteocrengine.{h,cpp}   HTTP client for PaddleOCR and docTR services
    processing/imageprocessor.{h,cpp}     deskew detection, contrast, denoise, rotation
    processing/pdfexporter.{h,cpp}       searchable PDF and optional PDF/A conversion
    ui/mainwindow.{h,cpp}            page view, selection, editing, user actions
    ui/preprocessdialog.{h,cpp}       before/after preview and quality controls
    ui/layoutdialog.{h,cpp}           region geometry, type, and reading order
    ui/documentcommands.{h,cpp}       undoable page and layout commands
  service/server.py                  optional PaddleOCR or docTR inference service
  service/tests/test_server.py       protocol tests with fake inference
  tests/ocrdocument_tests.cpp
  tests/projectfile_tests.cpp
  tests/imageprocessor_tests.cpp
  tests/pdfexporter_tests.cpp
```

Types live in `yagf::core`, `yagf::processing`, and `yagf::ui`. `ui` depends on `core` and `processing`. `core` has no widget or process dependency. OCR jobs and PDF export live in `processing`; the exporter reads an immutable `OcrDocument` snapshot. The optional Python service and Jetson runtime are separate from the desktop binary. The application uses C++23, including `std::expected` for image-processing errors and `std::span` for skew scoring.

## Data ownership

The `OcrDocument` is the current in-memory source of truth. It contains numbered pages with regions, paragraphs, lines, and words. Stable IDs connect each line to its words; the project file also records an explicit word-ID reading order for each page. Each word stores a union box and source fragments, original reading, corrected reading, source word IDs, confidence, review state, and basic bold/italic/size styling. Plain text, layout HTML, JSON, and the project file read from this document. The UI draws the current page's boxes and word list from it. `WordEditor` aligns unchanged words and maps split, merged, inserted, and deleted text onto page geometry. Qt undo commands keep before/after page snapshots and restore hierarchy and reading order together.

## Processing boundary

`IOcrEngine` accepts a `QImage`, options, and `std::stop_token`, and returns a normalized `OcrPage` or error. `TesseractEngine` owns its subprocess and TSV parser. `RemoteOcrEngine` encodes a PNG, calls the versioned Python HTTP service, and validates PaddleOCR line geometry or docTR word geometry and reading order. `DocumentProcessor` runs the engine on a Qt Concurrent worker, owns PDF rendering and thumbnail subprocesses, schedules batch pages, and propagates cancellation. The UI receives normalized pages and translates region coordinates back to the source page. The core library has no recognition provider, widget, process, Python, or Jetson dependency. External commands receive argument lists, never shell command strings.

`ImageProcessor` estimates page skew and luminance range, then applies optional denoise, contrast stretching, and rotation. `MainWindow` keeps the original image in memory, writes the processed page to a temporary PNG for recognition, and displays that same processed image. Project files store settings and both source and processed dimensions; reopening a project recreates the temporary image from the source. Changing rotation invalidates OCR geometry for that page.

`PdfExporter` renders each source page at the editor's 150 DPI, applies its saved image settings, and draws an invisible text layer from corrected words. It writes to a temporary file and atomically replaces the destination on success. The UI runs it through Qt Concurrent and can cancel between pages or during external commands. For PDF/A it calls OCRmyPDF in skip-text mode, preserving the edited text layer while converting the document.

## Next implementation slices

1. Add Flatpak packaging and test releases on supported Linux distributions.
2. Test PaddleOCR GPU inference and performance on the target Jetson hardware.
