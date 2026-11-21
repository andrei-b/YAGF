# Anonymized OCR smoke-run example

The [recorded report](ocr-smoke-example.json) uses two generated ground-truth
pages: one for calibration and one held out, with three pipelines, two measured
repetitions and one calibration warmup. Its source fixture and reference project
are in `tests/data/benchmark`.

Host identity, hardware/process observations, run timestamps and deployment
endpoints have been removed. Example service URLs use a reserved `.example`
hostname and must be configured for a local deployment.

```sh
yagf2-benchmark tests/data/benchmark/synthetic.yagf.json \
  --pipelines tests/data/benchmark/pipelines.json \
  --calibration 1 --held-out 2 --repetitions 2 --warmups 1 \
  --output /tmp/synthetic.yagf-experiment.json
```

The remote pipeline requires a configured OCR bridge. Credentials belong in a
private environment variable, rather than a published pipeline file.

| Pipeline | Held-out CER / WER | Mean recognition ms | Peak RSS MiB |
| --- | ---: | ---: | ---: |
| Tesseract 150 DPI | 0 / 0 | 324.4 | 45.3, recognition child |
| Tesseract 300 DPI + adaptive binarization | 0 / 0 | 709.0 | 83.9, recognition child |
| Remote PaddleOCR-VL 300 DPI | 0 / 0 | 1081.3 | 29.0, desktop bridge only |

Times include encoding and HTTP overhead, but exclude preparation, model startup
and calibration warmups. Repeated images can benefit from server caches. These
results check the experiment machinery; they do not select an engine for a
photographed scientific book or measure the upstream model's GPU memory.
