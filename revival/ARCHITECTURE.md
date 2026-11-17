# Code organization

The historical YAGF application stays at the repository root. `revival/` is the Qt 6 application under active development.

```text
revival/
  CMakeLists.txt
  src/
    main.cpp                         application entry point
    core/ocrdocument.{h,cpp}         OCR data, TSV normalization, text/JSON export
    core/projectfile.{h,cpp}         versioned project save/load, atomic write
    core/preprocesssettings.h         per-page, persisted quality options
    processing/documentprocessor.{h,cpp}  subprocesses and temporary PDF rendering
    processing/imageprocessor.{h,cpp}     deskew detection, contrast, denoise, rotation
    ui/mainwindow.{h,cpp}            page view, selection, editing, user actions
    ui/preprocessdialog.{h,cpp}       before/after preview and quality controls
  tests/ocrdocument_tests.cpp
  tests/projectfile_tests.cpp
  tests/imageprocessor_tests.cpp
```

Types live in `yagf::core`, `yagf::processing`, and `yagf::ui`. `ui` depends on `core` and `processing`. `core` has no widget or process dependency. `processing` uses only core preprocessing settings, not the OCR document model. This boundary lets a future PaddleOCR or Jetson adapter provide the same normalized document data without changing the editor. The application uses C++23, including `std::expected` for image-processing errors and `std::span` for skew scoring.

## Data ownership

The `OcrDocument` is the current in-memory source of truth. It contains numbered pages with regions, paragraphs, lines, and words. Stable IDs connect each line to its words; the project file also records an explicit word-ID reading order for each page. Each word stores its source box, original reading, corrected reading, confidence, review state, and basic bold/italic/size styling. Plain text, layout HTML, JSON, and the project file read from this document. The UI draws the current page's boxes and word list from it. Tesseract TSV supplies the first hierarchy; editable layout geometry and order are future work.

## Processing boundary

`DocumentProcessor` owns the subprocesses and temporary directory for PDF rendering, background miniature generation, full-page OCR, and region OCR. It emits image paths or Tesseract TSV and reports failures. The UI turns TSV into `OcrDocument`, translating region coordinates back to the page. Later engine adapters should return normalized OCR results through a common interface and identify engine version and capabilities. External commands must receive argument lists, never a shell command string.

`ImageProcessor` estimates page skew and luminance range, then applies optional denoise, contrast stretching, and rotation. `MainWindow` keeps the original image in memory, writes the processed page to a temporary PNG for Tesseract, and displays that same processed image. Project files store settings and both source and processed dimensions; reopening a project recreates the temporary image from the source. Changing rotation invalidates OCR geometry for that page.

## Next implementation slices

1. Add batch recognition and editable layout geometry/reading order.
2. Add searchable PDF export through OCRmyPDF.
3. Add optional neural and Jetson adapters behind the same processing boundary, with measured comparison data.
