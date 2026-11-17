# Remote text OCR services for YAGF

This optional Python service runs either [PaddleOCR 3.x](https://www.paddleocr.ai/main/en/version3.x/pipeline_usage/OCR.html) or [docTR](https://mindee.github.io/doctr/latest/) on a separate inference computer. YAGF sends a processed page as PNG over HTTP. PaddleOCR returns text-line boxes; docTR returns word boxes with block and line order. Choose the matching provider and URL in **Tools → OCR engine…**. Neither provider currently returns table structure to YAGF.

## PaddleOCR on a Jetson

Use a Python environment compatible with your JetPack and selected PaddleOCR inference engine. Install PaddleOCR following its [current installation guide](https://www.paddleocr.ai/main/en/version3.x/installation.html). For GPU use on Jetson, first confirm that the chosen PaddlePaddle or other PaddleOCR engine has a build compatible with your JetPack, Python, CUDA, and ARM64 environment; the general x86 CUDA wheel commands are not a Jetson installation recipe. Test `PaddleOCR(device="gpu", lang="en", use_doc_orientation_classify=False, use_doc_unwarping=False, use_textline_orientation=False).predict("sample.png")` on the device before starting this service. Use `--device cpu` if a GPU backend is unavailable.

From this repository on the inference computer:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install paddleocr
python revival/service/server.py --engine paddleocr --host 127.0.0.1 --port 8765 --device gpu --language en
```

The service loads its model at startup; the first launch may download model files. The language is fixed for the service process. Use PaddleOCR language codes such as `en` in YAGF. If you need another language, restart with `--language CODE` and set the same code in YAGF.

## docTR on a desktop or inference computer

docTR currently [requires Python 3.11 or newer](https://mindee.github.io/doctr/getting_started/installing.html) and its PyTorch dependencies. Install it in a separate environment from PaddleOCR. The default docTR recognition model uses its trained vocabulary; YAGF uses `auto` as the language setting. For a CPU service:

```sh
python3 -m venv .venv-doctr
. .venv-doctr/bin/activate
python -m pip install python-doctr
python revival/service/server.py --engine doctr --host 127.0.0.1 --port 8766 --device cpu
```

Use `--device gpu` where a compatible PyTorch CUDA build is available. The service moves the predictor to CUDA as described in the [docTR GPU guide](https://mindee.github.io/doctr/using_doctr/using_models.html). In YAGF, select **docTR (remote service)** and use `http://127.0.0.1:8766`. docTR returns individual word rectangles, so corrected text starts with more precise source mapping than PaddleOCR line output.

For a Jetson reached over SSH, keep the service bound to loopback and forward its port from the desktop:

```sh
ssh -L 8765:127.0.0.1:8765 user@jetson-host
```

Forward port 8766 instead if docTR runs there. Test a service with `curl http://127.0.0.1:8765/v1/health`; the response identifies its engine. For direct network binding, set `YAGF_OCR_TOKEN` before starting the service and enter that token in YAGF. Use HTTPS through a trusted reverse proxy when sending documents or tokens across an untrusted network; the built-in server speaks plain HTTP.

## Protocol

`POST /v1/recognize` takes `application/json` with `protocol: "yagf-ocr/1"`, `image_png` as base64, `language`, and `page_number`. The response carries the same protocol, `engine_id`, image `width` and `height`, and `language`. PaddleOCR responses contain `lines`; docTR responses contain `words`. Each item has `text`, `confidence` from 0 to 1, and a pixel box `[x, y, width, height]`. docTR words also have one-based `block` and `line` numbers. The Qt client validates provider, schema, dimensions, confidence, and bounds before accepting a page. Requests are capped at 44 MiB encoded and 32 MiB PNG; responses are capped at 8 MiB. `GET /v1/health` reports readiness.

Run the protocol tests without PaddleOCR or Jetson hardware:

```sh
python3 -m unittest discover -s revival/service/tests
```

The service is not bundled into the desktop application or AppImage. GPU inference and performance have not yet been verified on physical Jetson hardware in this repository.
