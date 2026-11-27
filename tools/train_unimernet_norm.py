"""Adapt the last decoder layer normalization with the encoder and output embeddings frozen."""
import argparse
import json
import re
import sys
from pathlib import Path
import numpy as np
import torch
from PIL import Image
from safetensors import safe_open
from safetensors.torch import save_file
from transformers import VisionEncoderDecoderModel
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path, help="MinerU source tree")
parser.add_argument("weights", type=Path)
parser.add_argument("manifest", type=Path, help="Training rows with relative image paths and LaTeX")
parser.add_argument("output", type=Path)
args = parser.parse_args()
sys.path.insert(0, str(args.source.resolve()))
from mineru.model.mfr.unimernet.unimernet_hf import UnimernetModel
base = args.weights.resolve()
data = args.manifest.resolve().parent
torch.set_num_threads(1)
torch.set_num_interop_threads(1)
torch.manual_seed(29)
if args.output.exists() and any(args.output.iterdir()):
    parser.error("Output directory must be empty to avoid mixing checkpoints")
model = UnimernetModel.from_pretrained(str(base), local_files_only=True).eval()
model.requires_grad_(False)
layer = model.decoder.model.decoder.layers[-1].final_layer_norm
layer.requires_grad_(True)
optim = torch.optim.AdamW(layer.parameters(), lr=0.00015, weight_decay=0.01)
rows = json.loads(args.manifest.read_text())
np.random.default_rng(29).shuffle(rows)
losses = []
for row in rows:
    if row["split"] != "train":
        raise ValueError("Training manifest includes a non-training example")
    label = re.sub(r"\\(?:begin|end)\{(?:equation\*?|displaymath)\}", "", row["latex"]).strip()
    ids = model.tokenizer.tokenizer(label, return_tensors="pt").input_ids
    if ids.shape[1] > 256:
        continue
    with Image.open(data / row["image"]) as image:
        pixels = model.transform(np.asarray(image.convert("RGB")))[None]
    if pixels.shape[1] == 1:
        pixels = pixels.repeat(1, 3, 1, 1)
    outputs = VisionEncoderDecoderModel.forward(
        model, pixel_values=pixels, decoder_input_ids=ids[:, :-1], use_cache=False)
    loss = torch.nn.functional.cross_entropy(outputs.logits.flatten(0, 1), ids[:, 1:].flatten())
    optim.zero_grad()
    loss.backward()
    torch.nn.utils.clip_grad_norm_(layer.parameters(), 1.0)
    optim.step()
    losses.append((row["image"], float(loss.detach())))

if not losses:
    raise ValueError("No training example fits the token limit")
dest = args.output
dest.mkdir(parents=True, exist_ok=True)
trained = {k: p.detach().contiguous() for k, p in model.named_parameters() if p.requires_grad}
with safe_open(base / "model.safetensors", framework="pt") as old:
    if not trained.keys() <= set(old.keys()):
        raise ValueError("Trained parameters are missing from the original checkpoint")
    weights = {k: trained[k] if k in trained else old.get_tensor(k) for k in old.keys()}
    save_file(weights, str(dest / "model.safetensors.tmp"), metadata=old.metadata())
(dest / "model.safetensors.tmp").replace(dest / "model.safetensors")
for path in base.glob("*.json"):
    if not (dest / path.name).exists():
        (dest / path.name).symlink_to(path)
(dest / "fit.json").write_text(json.dumps(dict(losses=losses, parameters=list(trained)), indent=2))
print(f"Fitted {len(losses)} examples")
