# Engine-native OCR training

Choose **Tools → Training & experiments → OCR training and adaptation…**. This shares one review,
training and activation workflow across Tesseract, Kraken and PaddleOCR.
Native training updates OCR weights; the existing **Symbol recognition rules**
prototype classifier and Tesseract book vocabulary remain available alongside it.

## Desktop workflow

1. Recognize the source pages first. Open the training window, connect to the host
   (default `http://127.0.0.1:8772`), choose an engine/base-model profile and a page
   range, then **Prepare samples**. The current page is prepared automatically
   when the window first connects.
2. Check each full-resolution line crop against its exact printed transcription.
   Edit the text, including new characters such as `ψ` or `🙂`, then approve or
   exclude it. Already-reviewed document lines can be approved together with
   **Approve already-reviewed lines**. Editing a transcription resets approval.
   Approved positive symbol-rule crops are included when their pages are selected.
3. Keep representative examples in **Validation**, including the new glyphs and
   ordinary text/lookalikes. These examples are never supplied as training labels.
   Multi-page sets keep entire pages in one partition; a single-page set splits
   individual samples. At least two approved training examples and one approved
   validation example are required. Larger, varied sets are needed for useful
   accuracy; three examples are only the submission minimum.
4. Choose **This book** or **Reusable across projects**, set a training budget,
   and **Train approved examples**. Tesseract counts iterations; Kraken and
   PaddleOCR count epochs. Training runs on the host while the existing OCR model
   stays selected. **Cancel operation** terminates the native training process.
   Closing the window also requests cancellation of an active job.
5. Compare original and candidate character error rates (CER) on the same held-out
   images and inspect their readings. Only a candidate with CER no worse than its
   base can be selected with **Use model**. This small validation set is evidence
   for that dataset, not a guarantee for the entire book. Existing recognized text
   changes only when you recognize it again. Model selection is undoable.

Validated models appear in the model library. Book models are filtered by the
source file's SHA-256; reusable models are offered across projects. Leave
**Continue training from the selected validated model** enabled to produce a new
version from those weights, or disable it to restart from the profile's stock
model. Versions must use the same profile. Validation images that appeared in an
ancestor's training set are rejected. Keep a consistent held-out set and include
ordinary text in training to check for regressions when teaching new glyphs.

Review decisions are cached for matching image crops in the prepared page range
under the desktop application's `native-training` data folder. **Export engine
training bundle…** writes approved PNG/`.gt.txt` pairs, train/validation manifests,
PaddleOCR TSV labels, a character list and a reimportable JSON dataset to an empty
folder. This allows external native-tool training too. Import preserves review
choices. Model weights stay on the host; projects save the model URL, so inference
needs the host/tunnel. Portable projects do not bundle these weights. Tokens are
kept only in memory and can be re-entered through **Tools → Recognition → OCR engine… → Adapted
native OCR model**.

Formula/table/picture regions, mixed LaTeX lines, uncertain paragraph alignment,
and words assembled across physical line breaks are excluded from automatic line
collection. Line images retain native pixels instead of 16×16 prototype masks.
Paragraph-only OCR uses local Tesseract geometry only when detected and saved line
counts agree, and requires fresh approval. This workflow trains text recognition,
not page layout or the mathematical formula model. The existing
`Use raw OCR output` option applies to the local Tesseract provider.

## Worker setup

The current Jetson setup uses `abagx@localhost-0`, loopback port **8772**, and the
Toshiba disk at `/mnt/toshiba/yagf-native` for environments, models, jobs, cache and
temporary files. Training uses CPU with two threads, separately from existing
GPU/VL and layout services. A job stores its reviewed dataset, profile, log,
validation readings and immutable candidate. Only one training job runs at once.
The service unit caps memory at 6 GiB and a command at one hour.

Tesseract 5.3.4 and Kraken 7.0.2 training and native-model inference were exercised on
small synthetic English line sets. Tesseract's Russian character-set expansion
with Greek/smiley labels was also exercised. These smoke tests establish that the
native paths work; they do not establish useful new-glyph accuracy after a short
run. The installed PaddleOCR-VL GGUF and ONNX inference weights are not trainable
checkpoints. The PaddleOCR profile stays **not ready** until a compatible native
training environment, configuration, dictionary, checkpoint and inference export
are supplied. Its training adapter has not been exercised on this Jetson.

### Tesseract

Install Tesseract 5 training tools and Pillow, for example with your distribution's
`tesseract-ocr` and `libtesseract-dev` packages. Verify `lstmtraining`,
`combine_tessdata`, `combine_lang_model`, `unicharset_extractor` and
`merge_unicharsets`. Start with floating-point **tessdata_best** weights, not the
integer-only `tessdata_fast` models. See the official
[Tesseract training guide](https://tesseract-ocr.github.io/tessdoc/tess5/TrainingTesseract-5.html)
and [tesstrain](https://github.com/tesseract-ocr/tesstrain). Set the profile's
`language` (for example `rus`) so recognized pages retain the right spelling
dictionaries; the selected trained model itself fixes the recognition vocabulary.

The Jetson's English and Russian base models come from `tesseract-ocr/tessdata_best`
revision `e12c65a915945e4c28e237a9b52bc4a8f39a0cec`. SHA-256:

- `eng.traineddata`: `8280aed0782fe27257a68ea10fe7ef324ca0f8d85bd2fd145d1c2b560bcb66ba`
- `rus.traineddata`: `b617eb6830ffabaaa795dd87ea7fd251adfe9cf0efe05eb9a2e8128b7728d6b6`

Place script character sets from `tesseract-ocr/langdata_lstm` in the profile's
`langdata` directory (Latin, Cyrillic and Greek for these profiles, plus
`radical-stroke.txt`). New training characters extend the original unicharset and
remap the LSTM output head. The original weights are never overwritten.

### Kraken

Create an isolated environment without system site packages. The tested CPU
versions are in [requirements-native-kraken.txt](requirements-native-kraken.txt):

```sh
python3 -m venv /mnt/toshiba/yagf-native/venv
/mnt/toshiba/yagf-native/venv/bin/pip install -r service/requirements-native-kraken.txt
```

Supply a recognition model suitable for the book; the current profile uses the
existing `McCATMuS_nfd_nofix_V1.mlmodel`. YAGF compiles separate native Arrow
train/validation datasets from the reviewed line strips, uses `ketos train
--resize union`, and exports safetensors weights. Line strips compile as baseline
data to avoid Kraken 7.0.2's raw-bbox loader dereferencing unset transforms. The
held-out evaluation uses those same line images before and after training. Native
Kraken page inference uses its conventional line segmentation and the selected
recognizer; it does not load an additional neural layout model.
See [Kraken recognition training](https://kraken.re/main/user_guide/training_recognition.html).
Models and their licenses are not included in this repository.

### PaddleOCR

Use a separate Python environment with a compatible PaddlePaddle/PaddleOCR build,
a checked-out PaddleOCR source tree (`tools/train.py` and `tools/export_model.py`),
a training YAML, its full base character dictionary, a `.pdparams` checkpoint and
a matching base inference export. The recipe keeps the base dictionary's ordering,
appends approved training characters, fine-tunes the recognizer and exports it for
native inference. Architecture/head compatibility must be checked for the chosen
checkpoint; failures remain failed jobs and cannot replace the active model.
See [PaddleOCR recognition training](https://www.paddleocr.ai/main/en/version2.x/ppocr/model_train/recognition.html).
Quantized GGUF and inference-only ONNX files cannot substitute for that checkpoint.

### Configuration and services

Copy [native-profiles.example.json](native-profiles.example.json) to a writable
location, adjust its absolute paths and choose a storage directory on a mounted
disk. Each profile's paths/commands come from this administrator-owned file, never
from an HTTP request. Copy `native_training_server.py`, `native_engines.py`,
`native_engine_runner.py`, `ocr_catalog.py` and `server.py` together to `~/.local/lib/yagf/`.

For a manual launch:

```sh
/mnt/toshiba/yagf-native/venv/bin/python service/native_training_server.py \
  --config /mnt/toshiba/yagf-native/profiles.json --host 127.0.0.1 --port 8772
```

Install [yagf-native-training.service](yagf-native-training.service) in
`~/.config/systemd/user/` on the host. Its environment file
`~/.config/yagf/native-training.env` sets:

```ini
YAGF_NATIVE_PYTHON=/mnt/toshiba/yagf-native/venv/bin/python
YAGF_NATIVE_CONFIG=/mnt/toshiba/yagf-native/profiles.json
XDG_CACHE_HOME=/mnt/toshiba/yagf-native/cache
TMPDIR=/mnt/toshiba/yagf-native/tmp
```

Create those cache/tmp directories first. On the desktop install
[yagf-native-tunnel.service](yagf-native-tunnel.service). Its environment file
`~/.config/yagf/native-host.env` sets `YAGF_JETSON_HOST=abagx@localhost-0`.
It uses a dedicated key at `~/.ssh/yagf_jetson_native`, restricted on the host to:

```text
restrict,port-forwarding,permitopen="127.0.0.1:8772",command="/bin/false" ssh-ed25519 PUBLIC_KEY yagf-native-training
```

Enable each unit on its respective computer with `systemctl --user enable --now
UNIT`. The mount must exist before the worker starts; systemd retries startup
when unavailable. No SSH password is stored. The symbol and layout tunnels remain
separate. For direct non-loopback binding set `YAGF_NATIVE_TOKEN` and provide HTTPS
through a trusted reverse proxy; the built-in server speaks HTTP.

## Protocol and limits

`GET /v1/capabilities` returns `yagf-native-training/1` and profile readiness.
`POST /v1/jobs` accepts that protocol, `profile_id`, source SHA-256 `source_id`,
`scope` (`project`/`shared`), `iterations`, optional `base_model_id`, and `samples`.
Each sample has an ID, source page, one-line text, `approved` decision,
`train`/`validation` partition and base64 PNG. `GET /v1/jobs/UUID` reports state
and before/after CER/readings. `POST /v1/jobs/UUID/cancel` cancels a job.
`GET /v1/models` lists eligible completed models. The model's
`POST /models/UUID/v1/recognize` endpoint implements `yagf-ocr/1` and returns words.
The native provider preserves model URLs when appending the OCR path.

Datasets are limited to 5,000 samples and 32 MiB decoded PNGs, with at most 2 MiB,
8 million pixels, 16,000×4,000 dimensions and 4,096 text code units per example.
Duplicate image bytes and repeated IDs are rejected. Requests are capped at
48 MiB and desktop responses at 8 MiB. Errors/cancellation do not activate a model.
A restarted host marks interrupted runs failed, so they can be restarted from
reviewed data. Delete unneeded job directories only while their models are unused.

Run framework-free contracts with:

```sh
python3 -m unittest discover -s service/tests -p test_native_training.py
ctest --test-dir build -R nativetraining_tests --output-on-failure
```

To exercise the Qt native provider against a real validated model:

```sh
QT_QPA_PLATFORM=offscreen \
YAGF_NATIVE_TEST_MODEL_URL=http://127.0.0.1:8772/models/UUID \
YAGF_NATIVE_TEST_IMAGE=/path/to/line.png \
YAGF_NATIVE_TEST_EXPECTED='Exact printed line' \
  build/nativetraining_tests liveNativeWorker
```

The worker also advertises configured base profiles, reusable models and local
OCR listeners through its existing tunnel. See [OCR_DISCOVERY.md](OCR_DISCOVERY.md)
for discovery and parallel recognition.
