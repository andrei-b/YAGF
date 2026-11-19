# Jetson smoke observations, 19 November 2026

[Recorded report](jetson-smoke-2026-11-19.json) contains two explicitly generated ground-truth pages, one calibration and one held out, three pipelines, two measured repetitions, and one calibration warmup per pipeline. The exact source and reference project are in `tests/data/benchmark`; the PDF source hash matches the report. Re-run with:

```sh
yagf2-benchmark tests/data/benchmark/synthetic.yagf.json \
  --pipelines tests/data/benchmark/pipelines.json \
  --calibration 1 --held-out 2 --repetitions 2 --warmups 1 \
  --output /tmp/synthetic.yagf-experiment.json
```

The remote row needs the configured local bridge connected to the Jetson. Credentials, if needed, belong in an environment variable, not the pipeline file.

| Pipeline | Held-out CER / WER | Mean recognition ms | Measured peak RSS MiB |
| --- | ---: | ---: | ---: |
| Tesseract 150 DPI | 0 / 0 | 324.4 | 45.3, recognition child |
| Tesseract 300 DPI + adaptive binarization | 0 / 0 | 709.0 | 83.9, recognition child |
| Jetson PaddleOCR-VL 300 DPI | 0 / 0 | 1081.3 | 29.0, desktop bridge only |

These are end-to-end recognition times on a simple synthetic fixture, including encoding/HTTP but excluding preparation, model startup, and calibration warmups. Repeated images can benefit from model-server caches. The observed recommendation was the first Tesseract pipeline; it also has the lowest preparation-plus-recognition cost here. This result does not select an engine for the supplied photographed/scientific scan.

The remote machine identifies as **NVIDIA Jetson AGX Orin Developer Kit**, AArch64, with L4T **R39 revision 2.0** reported by its installed image. The running server uses `PaddleOCR-VL-1.6-F16.gguf`. Its final sampled process RSS was about **4.0 GiB**. This is resident process memory, not a separately measured GPU allocation. The model was already loaded, and unrelated inference services were left running.

The attached hardware observation covers the benchmark interval and contains 89 `tegrastats` samples. System GPU activity reached **97%** during the sampled interval. GPU utilization and RAM samples include other processes; the report does not claim exclusive attribution, energy measurement, or per-request VRAM usage. Unrelated model process names and all process IDs were removed from this repository copy. The desktop bridge explicitly reports that its own RSS excludes the upstream model. Unknown upstream runtime/package versions remain unreported.

The photographed PDF has a separate coverage/orientation regression described in [the experiment guide](../experiments.md). Its OCR transcript is not a verified CER/WER reference, and the private source/transcription is not redistributed here.
