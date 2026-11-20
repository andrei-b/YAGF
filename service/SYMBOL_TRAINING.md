# Symbol training on a Jetson

YAGF's symbol augmentation learns a compact prototype classifier from explicitly approved matching and non-matching crops. It runs alongside the selected OCR engine. Training and inference are inexpensive CPU operations; the worker requires Python 3.10 or newer and no GPU/model downloads. The existing inference services can continue running.

The desktop finds candidates, stores source crops, and computes normalized binary masks. **Train approved examples** sends the reviewed masks to the selected worker. Its response contains a positive prototype bank, negative prototype bank, calibrated acceptance radius, and training-data fingerprint. YAGF verifies the returned model against the submitted examples before accepting it. Inference then runs on the desktop, including while disconnected from the worker.

Run this on the training host:

```sh
python3 service/symbol_training.py --host 127.0.0.1 --port 8770
```

Forward that port from the desktop:

```sh
ssh -N -L 127.0.0.1:8770:127.0.0.1:8770 user@jetson-host
```

In **Tools → Symbol recognition rules…**, choose **Jetson / remote host** and URL `http://127.0.0.1:8770`. The address and backend preference are remembered when training starts. Any bearer token remains in the dialog session.

For a persistent user service, copy `symbol_training.py` to `~/.local/lib/yagf/` on the Jetson, install [yagf-symbol-training.service](yagf-symbol-training.service) under `~/.config/systemd/user/`, and run `systemctl --user enable --now yagf-symbol-training.service`. Enable user lingering with `loginctl enable-linger` when the service should survive logout.

The optional desktop [tunnel unit](yagf-symbol-tunnel.service) reads `~/.config/yagf/symbol-tunnel.env` containing `YAGF_JETSON_HOST=user@jetson-host`. It uses a dedicated key at `~/.ssh/yagf_jetson_symbols`. Its authorized-key entry on the Jetson can be restricted to forwarding this port:

```text
restrict,port-forwarding,permitopen="127.0.0.1:8770",command="/bin/false" ssh-ed25519 PUBLIC_KEY yagf-symbol-training
```

Install the tunnel unit under `~/.config/systemd/user/`, then run `systemctl --user enable --now yagf-symbol-tunnel.service`. Check either unit with `systemctl --user status UNIT` and logs with `journalctl --user -u UNIT`. SSH authentication passwords are used interactively for setup; neither the worker nor the tunnel stores them.

`GET /v1/health` reports `yagf-symbol-training/1` and readiness. `POST /v1/train` accepts JSON with that `protocol`, a SHA-256 `training_hash`, and 3–256 `examples`, each with a base64 32-byte `mask` and `decision` of `positive` or `negative`. It requires at least two positives and one negative and rejects near-identical contradictory labels. The body limit is 128 KiB. The service is stateless and stores no submitted samples. Non-loopback binding requires `YAGF_SYMBOL_TOKEN`; the default loopback service is intended for SSH forwarding. A token never enters an exported model or dataset.

Run the framework-free service tests with:

```sh
python3 -m unittest discover -s service/tests -p test_symbol_training.py
```
