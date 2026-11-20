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

You can also start the desktop app with the bridge already selected: `yagf2 --paddleocr-vl-url http://127.0.0.1:8765 /path/to/scan.png`.

The VL model transcribes each page or drawn image region as one editable text block. Its source box covers the complete submitted image, and confidence is unknown. It provides no individual word boxes. Draw smaller OCR regions when you want separate blocks. The bridge identifies truncated model responses with `code: transcription_truncated`. YAGF retries these on smaller non-overlapping image sections, preferring low-ink cut lines, with at most two split levels (four leaf sections). It keeps all results in page coordinates and publishes them only when every section succeeds. Repeated failure remains an error rather than saving an incomplete page. Other server failures are not retried this way. This endpoint serves the YAGF page OCR protocol; the vision server's chat endpoint can separately be used for AI review.

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

## AI page-area detection on the Jetson

Choose the magic-wand **Tools → AI page areas…** command, or the arrow beside
**Smart split**. The current page and existing blocks appear immediately, and
analysis starts automatically. Review the cropped block miniatures on the right;
click a miniature or an outline to focus that block on the page. Change its type
or inclusion checkbox, then **Apply areas**. **Fit page** restores the full-page
view. Service settings and the replacement policy are under **Options**;
**Analyze again** reruns detection. The four block
kinds are text, picture, formula and text with formulas. Pictures are skipped by
OCR and included in the scientific HTML and LaTeX exports. The detector's original
label (including tables, captions and headings) and score are saved in the project.
Tables currently use text OCR; this feature does not infer table cell structure.

When the same model query has a second strong class of a different block type,
its card offers **Also likely**, with the model score. Click it to switch types;
click again to return to the first choice. Alternatives require a score of at
least 40% and a gap of at most 20 percentage points from the first choice.
They come from the exported model's top-k class scores for exactly the same box
and reading-order value, following the
[official postprocessor](https://github.com/PaddlePaddle/PaddleDetection/blob/release/2.9/ppdet/modeling/post_process.py).
Nearby or overlapping detections from separate queries are not presented as
alternative classes. The selected candidate's label and score are saved with the
applied block. The type selector also permits manual choices when the model
offers no strong alternative. Older workers without alternatives still work.

Manual blocks are always preserved. By default the new selection replaces only
automatic blocks; uncheck that option to keep all existing blocks. Proposals that
overlap preserved blocks cannot be selected. Selecting an overlapping proposal
unchecks the conflicting AI proposal. Inline formulas inside a detected paragraph
make the paragraph a mixed block. Applying is undoable and leaves existing OCR
text untouched; recognize the page again to refresh its text using the new areas.

The dedicated worker uses the official Apache-2.0
[PP-DocLayoutV3 ONNX model](https://huggingface.co/PaddlePaddle/PP-DocLayoutV3_onnx),
pinned to revision `46bbdf188bb0a772c08aed74882ce7e51a8f1ea6`, with SHA256
`45bf71750b00739a41fc209f132eb104a4d6b5bb29483c9078164d8b87cf28ba`.
It uses ONNX Runtime's CPU provider with two threads; the existing GPU OCR services
can continue running. The 130,502,049-byte model is not included in this repository.

The current installation stores the model, isolated Python environment, download
cache and temporary files on the Toshiba disk at `/mnt/toshiba/yagf-layout`.
To reproduce it on another ARM64/Python 3.12 host (choose a writable storage path):

```sh
mkdir -p /mnt/toshiba/yagf-layout/tmp /mnt/toshiba/yagf-layout/cache /mnt/toshiba/yagf-layout/models
export TMPDIR=/mnt/toshiba/yagf-layout/tmp PIP_CACHE_DIR=/mnt/toshiba/yagf-layout/cache
python3 -m venv --system-site-packages /mnt/toshiba/yagf-layout/venv
/mnt/toshiba/yagf-layout/venv/bin/pip install -r service/requirements-layout.txt
curl -fL https://huggingface.co/PaddlePaddle/PP-DocLayoutV3_onnx/resolve/46bbdf188bb0a772c08aed74882ce7e51a8f1ea6/inference.onnx -o /mnt/toshiba/yagf-layout/models/inference.onnx
/mnt/toshiba/yagf-layout/venv/bin/python service/layout_server.py --model /mnt/toshiba/yagf-layout/models/inference.onnx
```

The worker verifies the model hash at startup. It listens on Jetson loopback port
8771. `yagf-layout.service` is the user-service template; its environment file
`~/.config/yagf/layout-worker.env` defines `YAGF_LAYOUT_PYTHON` and
`YAGF_LAYOUT_MODEL` with the paths above. Copy the worker to
`~/.local/lib/yagf/layout_server.py` and the unit to `~/.config/systemd/user/`.
The disk must be mounted before the service can start; a missing mount causes
startup failure and systemd retries every five seconds.

On the desktop, `yagf-layout-tunnel.service` forwards only local port 8771 using a
dedicated SSH key `~/.ssh/yagf_jetson_layout`. Its environment file
`~/.config/yagf/layout-host.env` defines `YAGF_JETSON_HOST` (SSH user and host).
Authorize that key on the worker with
`restrict,port-forwarding,permitopen="127.0.0.1:8771",command="/bin/false"`.
Enable each unit with `systemctl --user enable --now UNIT`. The existing symbol
training key and tunnel are separate.

The dialog defaults to `http://127.0.0.1:8771`. It saves the layout URL and minimum
score separately from OCR/AI-review settings. An optional bearer token stays in
memory for the dialog only. For non-loopback worker binding, set
`YAGF_LAYOUT_TOKEN` and use HTTPS or an SSH tunnel.
`GET /v1/health` reports readiness and the pinned model identity.
`POST /v1/layout` accepts protocol `yagf-layout/1`, `image_png` (base64 PNG) and
`threshold` (0.1–0.95). The response contains original page dimensions and ordered
areas with `id`, `label`, `type`, `score`, and native-pixel `box: [x,y,w,h]`.
Each area can also include `alternatives: [{label, type, score}, …]` (up to three
distinct alternative types).
Requests are limited to 32 MiB PNGs and 80 million pixels. The client validates
geometry, limits responses, supports cancellation, and never applies a partial
or failed analysis. Model scores are not guarantees; review is required.

## Native model training and adaptation

See [NATIVE_TRAINING.md](NATIVE_TRAINING.md) for the shared Tesseract/Kraken/PaddleOCR
review, training, held-out validation and explicit model activation workflow. The
dedicated worker runs on port 8772 and stores model versions separately from these
stock inference services.
