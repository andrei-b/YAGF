# OCR discovery and parallel recognition

The **Recognition** tab opens first, with separate model selectors for **Text
recognition** and **Formula recognition**. Each shows model names and locations;
the details below report readiness. The text language can be edited for Tesseract
and PaddleOCR. **Refresh models** updates both lists without changing their
selections. Unavailable saved models remain visible and can be checked again.
Only **Save** applies model/language changes; **Cancel** retains the current setup.

**Page layout** contains the layout URL, minimum score and automatic detection.
**Models & services** contains discovery hosts, the editable service library,
session tokens, add/remove controls and optional companion text checkboxes.

**Add service…** offers optional templates for PP-OCRv6 small/medium, GLM-OCR,
LightOnOCR-3-0.8B, MinerU2.5-Pro, Chandra OCR 2 and docTR FormulaNet. Templates
are never inserted automatically, enabled for comparison or selected as active
engines. They require separately installed/started workers; see the
[optional worker instructions](README.md#optional-model-presets). No settings
change until the user adds a service and saves the settings window.

**Tools → Recognition settings…** checks `GET BASE/v1/health`
on named hosts and saved service URLs. A hostname probes ports 8765–8769,
8771 (page layout), 8772 and 8082; a URL or `host:port` probes that exact endpoint. Up to 32 endpoints
have an approximately three-second deadline and cancellable, bounded JSON reads.
There is no subnet scan, redirect following or document upload during discovery.

Health uses `yagf-ocr/1`, `engine_id`, `status: ready`, and optional `language`,
`model`, `requires_token`. Providers are PaddleOCR (including its VL bridge),
docTR, RapidOCR, Surya, Kraken and native models. The updated `server.py` publishes
this metadata. Older workers with only protocol/engine/status remain compatible.
Tokens can be entered for a selected service and remain in memory.

Discovery also checks `BASE/v1/models` for PaddleOCR-VL vision servers. It retains
the returned model name and supports base URLs ending in `/v1` or a path prefix.
Unrelated chat models are ignored. The service library stores the model name and
advertised tasks and capability profiles, while tokens remain session-only. Legacy
workers without task metadata remain usable for text; Surya retains formula support.

## Model capability profiles

Capability metadata extends discovery without changing the existing wire protocols.
An OCR worker may add `profile` to its `yagf-ocr/1` health response, and the gateway
preserves that object in each `services` entry and child health response:

```json
{
  "protocol": "yagf-ocr/1",
  "engine_id": "example-vlm",
  "status": "ready",
  "profile": {
    "model": "example-document-model",
    "tasks": ["text", "layout", "table", "formula"],
    "geometry": "block",
    "formats": ["markdown", "json"],
    "languages": ["auto"],
    "max_image_pixels": 12000000,
    "supports_batching": true
  }
}
```

Only the model name and a nonempty task list are required inside a profile. Other
missing fields mean unknown. `ocr` is accepted as a legacy alias for `text`.
Geometry is `none`, `page`, `block`, `line`, `word`, or `unknown`; it describes
actual model evidence. `none` is appropriate when a bridge knows only its input
crop extent. Language `auto` means automatic language selection, not a promise to
accept every explicit language code. Formats describe the evidence requested
inside the protocol's JSON response envelope.

A multi-model OCR endpoint can publish `profiles: [PROFILE, ...]` instead of one
`profile`. Discovery keeps separate identities for each provider, URL and model.
Requests to `/v1/recognize` include the chosen `model`. Alternatively,
`/v1/models` entries may include `id`, `protocol: "yagf-ocr/1"`, `engine_id` and
`profile`; the profile's model must match `id`. This explicitly identifies the
recognition protocol instead of guessing it from an unfamiliar model name.
Existing PaddleOCR-VL chat servers still use their existing adapter.

An advertised profile is authoritative, including for familiar providers. Invalid
profiles are rejected with discovery diagnostics rather than replaced with guessed
capabilities. Older workers retain legacy task defaults. Saved settings retain
profiles, but never bearer tokens. Settings show tasks, geometry, formats,
languages, image limits and batching status. Text/formula selectors use the profile
tasks; discovering metadata does not automatically replace a selected model.

`ModelRequirements` and `selectOcrModel()` match ready models by task, exact evidence
granularity, format, language, image size and batching. Explicit requirements need
advertised support; an unknown capability does not satisfy them. Among matching
models the caller's preferred model wins, otherwise discovery order is retained;
the matcher makes no unsupported quality ranking. Remote recognition validates
known task/language/image limits before uploading. `OcrOptions.task` and
`outputFormat` carry negotiated requests; `text` maps to the legacy `ocr` wire task.

The bundled workers advertise their implemented JSON output and single-image
requests. Batching metadata is available for matching; the current scheduler still
sends individual images. A layout capability on an OCR model describes its
evidence; the AI page-area window continues to require a `yagf-layout/1` service.
The layout worker advertises its own block-geometry profile.

## Recognition and layout selections

The same settings window also selects the **Text engine** and the **AI page layout service**. Layout workers publish `yagf-layout/1` health, appear as **Page layout**, and cannot be selected as primary or companion OCR engines. Select one and click **Use selected for layout**, or enter a URL directly. Layout URL and minimum score are shared with **AI page areas…**. Text and formula engine selections persist across restarts; bearer tokens remain in memory.

The **Use AI page layout before Tesseract page and batch OCR** checkbox controls automatic layout analysis during recognition. It refreshes automatic blocks, preserves manual blocks, and falls back to existing/local detection with a warning on layout-service failure. Selected-block OCR uses its existing selection.

The **Formula engine** selector is independent of the primary text engine and
companion checkboxes. The default is local Rapid LaTeX OCR. Only formula-capable
services appear as alternatives. Its selection is saved in `ocr/formulaService`.
Direct PaddleOCR-VL uses `Formula Recognition:` on `/v1/chat/completions`; a
compatible YAGF worker/gateway receives `task: "formula"` on `/v1/recognize`.
Formula recognition uses complete crops with their original page offsets, retains
prose and source IDs, and reports failures without replacing them with partial output.

Different text and formula engines run concurrently once manual or AI layout
blocks identify the formula crops. This applies to page, selected-block and batch
recognition without another setting. Formula candidates that depend on text OCR
are processed after the text reading. Selecting the same engine or service URL
for both roles keeps its requests serial, including different model names on one
endpoint. Cancelling recognition stops both engines; failed formula crops retain
their native text reading.

For a persistent direct vision-server connection, `yagf-formula-tunnel.service`
forwards desktop loopback **8082** to the Jetson's loopback **8082**, independently
of the layout and training tunnels. Its environment file is
`~/.config/yagf/formula-host.env` (`YAGF_JETSON_HOST=user@host`), and its dedicated
key is `~/.ssh/yagf_jetson_formulas`. Restrict that key on the Jetson to
`restrict,port-forwarding,permitopen="127.0.0.1:8082",command="/bin/false"`.
Configure jump hosts or nonstandard SSH ports in your private SSH configuration
or a local service override. The unit forwards an already-running model.

## Jetson catalog and gateway

The native worker advertises services through the existing **8772** restricted
SSH forward. Deploy `ocr_catalog.py` alongside `native_training_server.py`,
`native_engines.py`, `native_engine_runner.py` and `server.py` in
`~/.local/lib/yagf/`, then restart `yagf-native-training.service`. Existing training
and model endpoints continue to work. No additional SSH key/listener is required.

Its `/v1/health` has engine ID `native` and `services: [{path, provider, label,
language, ready, reason}]`. Paths stay on the catalog's origin; the Qt client
preserves their prefix when appending `/v1/recognize`.

- `/profiles/PROFILE/v1/recognize` uses the administrator-configured base profile
  without requiring a training run. Model locations are supplied by the worker's
  private profile configuration.
- `/models/UUID/v1/recognize` uses an eligible trained model. Only reusable models
  appear in the general catalog; a selected book model can be probed directly.
- `/services/PORT/v1/recognize` forwards to a discovered local YAGF listener on
  loopback ports 8765–8769. Health is cached for 15 seconds. HTTP requests cannot
  choose arbitrary upstream destinations or ports.
- A running OpenAI-compatible model on loopback 8082 whose ID contains
  `PaddleOCR-VL` is offered as `/services/8082`, using the existing VL bridge.
  Discovery never starts or downloads a GPU model.

Catalog children also provide `GET .../v1/health`. Native model/profile inference
shares a two-slot semaphore to bound CPU model loads. The existing 6 GiB worker
limit and two-thread CPU settings remain. GPU/VL services retain their own limits.

`YAGF_NATIVE_TOKEN` authenticates the gateway when configured. Keep it on loopback
behind SSH, or put HTTPS in front of a directly reachable authenticated worker.
Configure upstreams requiring separate bearer tokens directly through their own
service URL in the desktop.

## PP-FormulaNet_plus-L worker

The formula-only adapter uses the installed original Paddle model and reports
`engine_id: "paddleocr"`, `model: "PP-FormulaNet_plus-L"`, `tasks: ["formula"]`.
It appears in the Formula selector, and is excluded from Text and companions.
No synthetic recognition confidence is assigned. Crops retain their full bounds.
The catalog forwards model names and task metadata unchanged.

Install the original model in a writable directory on the inference host.
A compatible CPU environment uses PaddlePaddle 3.2.2, PaddleOCR 3.7.0,
tokenizers, ftfy and regex. The examples use `/srv/yagf/formulanet` as a
replaceable storage root and two CPU inference threads.

To run an already installed model:

```sh
/srv/yagf/formulanet/venv/bin/python service/server.py \
  --engine pp-formulanet --device cpu --host 127.0.0.1 --port 8769 \
  --model /srv/yagf/formulanet/models/PP-FormulaNet_plus-L
```

For a persistent worker, deploy `server.py` as
`~/.local/lib/yagf/formulanet_server.py` and install `yagf-formulanet.service`.
Its environment file `~/.config/yagf/formulanet-worker.env` defines
`YAGF_FORMULANET_PYTHON`, `YAGF_FORMULANET_MODEL`, `TMPDIR`,
`PADDLE_PDX_CACHE_HOME`, `XDG_CACHE_HOME` and `HF_HOME`. Keep caches and temporary
files in the chosen storage directory. The worker requires the existing model directory;
it does not download a model. Its HTTP handler serializes inference requests.

The desktop's `yagf-models-tunnel.service` forwards loopback **8769** (formulas)
and **8772** (model catalog), using `~/.ssh/yagf_jetson_models` and
`~/.config/yagf/models-host.env` (`YAGF_JETSON_HOST=user@host`). Authorize the
public key with
`restrict,port-forwarding,permitopen="127.0.0.1:8769",permitopen="127.0.0.1:8772",command="/bin/false"`.
Configure jump hosts and SSH ports privately on the deployment machine.
Existing layout and PaddleOCR-VL
tunnels remain independent. Enable units with `systemctl --user enable --now UNIT`.
Installing/discovering the worker leaves YAGF's current engine choices unchanged.

## Simultaneous recognition and review

A primary and up to three enabled companions start concurrently. Selected blocks
are sent individually, retaining crop offsets. Picture/excluded blocks are omitted;
whole-page companion images mask excluded pictures. Languages can be set per
service. Empty local Tesseract language follows the primary; remote services
normally advertise their fixed language or `auto`.

Companions never vote on or overwrite primary text. Optional `ocr_readings` page
records preserve labels, languages, source keys, geometry, text and errors.
Tokens are excluded from settings, projects and exports. The library and enabled
choices are in desktop setting `ocr/serviceLibrary`; primary selection changes
the current session, following existing engine-selection behavior.

Companion failure retains the primary result. Primary failure fails the job.
Cancellation discards the unfinished job, stops local children/network waits and
joins companion workers. A remote server may finish already-submitted computation.

The review window matches by source geometry; larger paragraph/page outputs stay
available for complete-text inspection without invented word positions. Only
**Use this reading** and **Save** change the document. Normal source-linked editing,
font normalization, provenance and Undo/Redo apply. Geometry changes clear old
comparison coordinates; Undo restores them. Exports contain reviewed primary text.

Storage is bounded to eight sources/page, 10,000 items/source, 65,536 characters/item
and 8 MiB total text/page. Each new companion run is capped at 1 MiB text. Existing
remote request/response limits and recognition deadlines apply. URLs with embedded
credentials, query strings or fragments are rejected.

## Tests

```sh
python3 -m unittest discover -s service/tests
ctest --test-dir build -R 'ocrservices_tests|recognitionliveness_tests|mainwindow_tests' --output-on-failure
```

Tests cover incompatible listeners, same-origin paths, gateway authentication,
profile inference, proxy destination limits, book-model visibility, actual
concurrent starts, failed/stalled companions, cancellation, crop offsets,
project validation, explicit review, Undo/Redo and geometry invalidation.
Separate liveness regressions cover stalled formula helpers, hard deadlines,
partial-result preservation, progress reporting and raw-Tesseract bypass.

A live smoke test exercises local Tesseract and three discovered Jetson services:

```sh
QT_QPA_PLATFORM=offscreen \
YAGF_LIVE_OCR_URL=http://127.0.0.1:8772 \
YAGF_LIVE_OCR_IMAGE=/path/to/line.png \
  build/ocrservices_tests liveDiscoveredEngines
```

The current setup passed with English Tesseract, Kraken and PaddleOCR-VL
companions. This verifies the integration and separate outputs; it does not imply
each model transcribes every example correctly.

Companion jobs sharing the selected formula service are deferred until primary formula recognition finishes. Other companions remain concurrent. The full-page companion checkbox is optional and independent of choosing the service for formulas.
