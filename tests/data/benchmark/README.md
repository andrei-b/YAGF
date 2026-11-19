# Generated benchmark fixture

`synthetic-source.pdf` was generated with Qt 6 QPdfWriter at 150 DPI using Liberation Serif at 25 pixels. The two pages contain independently written test sentences. `synthetic.yagf.json` records that known source text and its drawing rectangles; its `synthetic_rendered_ground_truth` origin is explicit. It was not made by accepting an engine's transcription as its own reference.

The exact PDF and source-linked project are included so a report's source SHA-256 and reference hashes can be checked. The pipeline file compares local Tesseract at two resolutions and an optional local bridge to a Jetson PaddleOCR-VL service. No credential is included. The remote model is external and not redistributed.

The fixture validates scoring, geometry and protocol plumbing. Its simple type and layout do not establish accuracy on photographed, historical, multilingual or mathematical material. Use independent manually proofread calibration and held-out pages from the intended document before choosing a production pipeline.
