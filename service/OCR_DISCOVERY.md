# OCR discovery and parallel recognition

**Tools → OCR services and parallel recognition…** checks `GET BASE/v1/health`
on named hosts and saved service URLs. A hostname probes ports 8765–8769 and
8772; a URL or `host:port` probes that exact endpoint. Up to 32 concurrent probes
have an approximately three-second deadline and cancellable, bounded JSON reads.
There is no subnet scan, redirect following or document upload during discovery.

Health uses `yagf-ocr/1`, `engine_id`, `status: ready`, and optional `language`,
`model`, `requires_token`. Providers are PaddleOCR (including its VL bridge),
docTR, RapidOCR, Surya, Kraken and native models. The updated `server.py` publishes
this metadata. Older workers with only protocol/engine/status remain compatible.
Tokens can be entered for a selected service and remain in memory.

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
  without requiring a training run. English/Russian Tesseract and Kraken are
  configured on the Jetson's Toshiba disk.
- `/models/UUID/v1/recognize` uses an eligible trained model. Only reusable models
  appear in the general catalog; a selected book model can be probed directly.
- `/services/PORT/v1/recognize` forwards to a discovered local YAGF listener on
  loopback ports 8765–8769. Health is cached for 15 seconds. HTTP requests cannot
  choose arbitrary upstream destinations or ports.
- A running OpenAI-compatible model on loopback 8082 whose ID contains
  `PaddleOCR-VL` is offered as `/services/8082`, using the existing VL bridge.
  Discovery never starts or downloads a GPU model. The current host advertises
  `PaddleOCR-VL-1.6`.

Catalog children also provide `GET .../v1/health`. Native model/profile inference
shares a two-slot semaphore to bound CPU model loads. The existing 6 GiB worker
limit and two-thread CPU settings remain. GPU/VL services retain their own limits.

`YAGF_NATIVE_TOKEN` authenticates the gateway when configured. Keep it on loopback
behind SSH, or put HTTPS in front of a directly reachable authenticated worker.
Configure upstreams requiring separate bearer tokens directly through their own
service URL in the desktop.

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
