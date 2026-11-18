# Remote text OCR services for YAGF

This optional Python service can run [PaddleOCR 3.x](https://www.paddleocr.ai/main/en/version3.x/pipeline_usage/OCR.html), [docTR](https://mindee.github.io/doctr/latest/), [RapidOCR](https://rapidai.github.io/RapidOCRDocs/main/en/quickstart/), [Surya](https://github.com/datalab-to/surya), or [Kraken](https://kraken.re/main/user_guide/api.html) on a separate inference computer. YAGF sends a processed page as PNG over HTTP. Choose the matching provider and URL in **Tools → OCR engine…**. Each engine can be installed in its own Python environment; none is required by the desktop application.

## PaddleOCR on a Jetson

### Using an already running PaddleOCR-VL vision server

For a PaddleOCR-VL model served by an OpenAI-compatible vision endpoint, run the lightweight bridge on the desktop:

```sh
python service/server.py --engine paddleocr-vl --port 8765 \
  --vl-endpoint http://127.0.0.1:8082/v1/chat/completions --vl-model PaddleOCR-VL-1.6
```

In YAGF, choose **Tools → OCR engine… → PaddleOCR (remote service)**, set the service URL to `http://127.0.0.1:8765`, and use language `auto`. The endpoint must be reachable, for example through an SSH forward to the inference computer. Set `YAGF_VL_TOKEN` if the upstream vision server requires a bearer token. The bridge needs only Python's standard library; it runs no local inference model.

You can also start the desktop app with the bridge already selected: `yagf-revival --paddleocr-vl-url http://127.0.0.1:8765 /path/to/scan.png`.

The VL model transcribes each page or drawn image region as one editable text block. Its source box covers the complete submitted image, and confidence is unknown. It provides no individual word boxes. Draw smaller OCR regions when you want separate blocks. The bridge rejects truncated model responses instead of saving incomplete text. This endpoint serves the YAGF page OCR protocol; the vision server's chat endpoint can separately be used for AI review.

### Running the standard PaddleOCR pipeline

Use a Python environment compatible with your JetPack and selected PaddleOCR inference engine. Install PaddleOCR following its [current installation guide](https://www.paddleocr.ai/main/en/version3.x/installation.html). For GPU use on Jetson, first confirm that the chosen PaddlePaddle or other PaddleOCR engine has a build compatible with your JetPack, Python, CUDA, and ARM64 environment; the general x86 CUDA wheel commands are not a Jetson installation recipe. Test `PaddleOCR(device="gpu", lang="en", use_doc_orientation_classify=False, use_doc_unwarping=False, use_textline_orientation=False).predict("sample.png")` on the device before starting this service. Use `--device cpu` if a GPU backend is unavailable.

From this repository on the inference computer:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install paddleocr
python service/server.py --engine paddleocr --host 127.0.0.1 --port 8765 --device gpu --language en
```

The service loads its model at startup; the first launch may download model files. The language is fixed for the service process. Use PaddleOCR language codes such as `en` in YAGF. If you need another language, restart with `--language CODE` and set the same code in YAGF.

## docTR on a desktop or inference computer

docTR currently [requires Python 3.11 or newer](https://mindee.github.io/doctr/getting_started/installing.html) and its PyTorch dependencies. Install it in a separate environment from PaddleOCR. The default docTR recognition model uses its trained vocabulary; YAGF uses `auto` as the language setting. For a CPU service:

```sh
python3 -m venv .venv-doctr
. .venv-doctr/bin/activate
python -m pip install python-doctr
python service/server.py --engine doctr --host 127.0.0.1 --port 8766 --device cpu
```

Use `--device gpu` where a compatible PyTorch CUDA build is available. The service moves the predictor to CUDA as described in the [docTR GPU guide](https://mindee.github.io/doctr/using_doctr/using_models.html). In YAGF, select **docTR (remote service)** and use `http://127.0.0.1:8766`. docTR returns individual word rectangles, so corrected text starts with more precise source mapping than PaddleOCR line output.

## RapidOCR

The adapter uses the [current RapidOCR Python API](https://rapidai.github.io/RapidOCRDocs/main/en/install_usage/rapidocr/usage/) and returns text-line rectangles. Its default ONNX Runtime pipeline runs on CPU:

```sh
python3 -m venv .venv-rapidocr
. .venv-rapidocr/bin/activate
python -m pip install rapidocr onnxruntime
python service/server.py --engine rapidocr --host 127.0.0.1 --port 8767 --device cpu
```

Select **RapidOCR (remote service)** with `http://127.0.0.1:8767` and language `auto`. The language is determined by the models configured in RapidOCR, not by YAGF's language field. The Python adapter is available now; a native C++ RapidOCR build and ARM64/TensorRT deployment are separate work and are not claimed by this integration.

## Surya

The adapter targets [Surya 2.x's current `RecognitionPredictor` API](https://github.com/datalab-to/surya#ocr-text-recognition). It converts the predictor’s ordered blocks to editable text, including LaTeX from math tags and tab-separated table cells. For example:

```sh
python3 -m venv .venv-surya
. .venv-surya/bin/activate
python -m pip install surya-ocr
python service/server.py --engine surya --host 127.0.0.1 --port 8768
```

Select **Surya (remote service)** with `http://127.0.0.1:8768` and language `auto`. Surya also needs its [inference backend](https://github.com/datalab-to/surya#inference-backend-prerequisites), such as vLLM or llama.cpp. The adapter retains header and span metadata for recognized tables, preserves picture boxes even when OCR skips them, and keeps captions in reading order. The desktop scientific-book export crops figures from the local page image. Surya's [code and model weights have different licenses](https://github.com/datalab-to/surya#commercial-usage). YAGF does not redistribute those weights.

## Kraken

The adapter uses Kraken's [task API](https://kraken.re/main/user_guide/api.html). Supply a recognition model suitable for the document; Kraken's default segmentation model finds lines. Character cuts are grouped into word rectangles, preserving line order and confidence:

```sh
python3 -m venv .venv-kraken
. .venv-kraken/bin/activate
python -m pip install kraken
python service/server.py --engine kraken --model /path/to/recognition-model.safetensors \
  --host 127.0.0.1 --port 8769 --device cpu
```

Select **Kraken (remote service)** with `http://127.0.0.1:8769` and language `auto`. The selected model controls script and language. Check that model's own license before redistribution. If Kraken returns character geometry that cannot be aligned with its transcription, the service retains that line as one text item with a line box instead of inventing word boxes.

For a Jetson reached over SSH, keep the service bound to loopback and forward its port from the desktop:

```sh
ssh -L 8765:127.0.0.1:8765 user@jetson-host
```

Forward the matching service port for other providers. Test a service with `curl http://127.0.0.1:8765/v1/health`; the response identifies its engine. For direct network binding, set `YAGF_OCR_TOKEN` before starting the service and enter that token in YAGF. Use HTTPS through a trusted reverse proxy when sending documents or tokens across an untrusted network; the built-in server speaks plain HTTP.

## Protocol

`POST /v1/recognize` takes `application/json` with `protocol: "yagf-ocr/1"`, `image_png` as base64, `language`, and `page_number`. The response carries the same protocol, `engine_id`, image `width` and `height`, and `language`. PaddleOCR, RapidOCR, and Surya responses contain `lines`; docTR and Kraken responses contain `words`. Each item has `text`, `confidence` from 0 to 1, and a pixel box `[x, y, width, height]`. Structured providers add one-based `block` and `line` numbers; Surya also adds `type` and may report unknown confidence as `null`. Tables use tab-separated cells and newline-separated rows in `text`. The optional `table_layout` array records `header`, `rowspan`, and `colspan` for each cell. The Qt client validates provider, schema, dimensions, confidence, bounds, and reading order before accepting a page. Requests are capped at 44 MiB encoded and 32 MiB PNG; responses are capped at 8 MiB. `GET /v1/health` reports readiness.

Run the protocol and adapter tests without installing inference frameworks:

```sh
python3 -m unittest discover -s service/tests
```

The service is not bundled into the desktop application or AppImage. The three new adapters have contract tests, but real model inference and Jetson performance have not yet been verified in this repository.
