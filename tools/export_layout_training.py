#!/usr/bin/env python3
"""Collect corrected block splits from saved YAGF projects into a training dataset."""
import argparse
import base64
import hashlib
import json
import os
import struct
from pathlib import Path
import tempfile


def export(projects, destination):
    destination = Path(destination)
    if destination.exists():
        raise ValueError(f"Output already exists: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with tempfile.TemporaryDirectory(dir=destination.parent) as staging:
        root = Path(staging) / "dataset"
        images = root / "images"
        images.mkdir(parents=True)
        with (root / "examples.jsonl").open("w", encoding="utf-8") as manifest:
            for project in projects:
                project_name = project.name
                if project.is_dir():
                    project = project / "project.yagf.json"
                document = json.loads(project.read_text(encoding="utf-8"))["document"]
                for page in document["pages"]:
                    training = page.get("layout_training")
                    if training is None:
                        continue
                    if training["version"] != 1:
                        raise ValueError(f"Unknown layout training format in {project}")
                    assets = {}
                    for example in training["examples"]:
                        digest = example["image_sha256"]
                        if digest not in assets:
                            png = base64.b64decode(training["images"][digest], validate=True)
                            if hashlib.sha256(png).hexdigest() != digest or len(png) < 24 or not png.startswith(b"\x89PNG\r\n\x1a\n"):
                                raise ValueError(f"Invalid training image in {project}, page {page['number']}")
                            asset = images / f"{digest}.png"
                            if not asset.exists():
                                asset.write_bytes(png)
                            width, height = struct.unpack(">II", png[16:24])
                            assets[digest] = dict(image=asset.relative_to(root).as_posix(), width=width, height=height)
                        row = dict(example, **assets[digest], project=project_name, page=page["number"])
                        manifest.write(json.dumps(row, ensure_ascii=False) + "\n")
                        count += 1
        os.rename(root, destination)
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path, help="New dataset directory")
    parser.add_argument("projects", type=Path, nargs="+", help="Project files or portable project folders")
    args = parser.parse_args()
    try:
        count = export(args.projects, args.destination)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Exported {count} layout examples to {args.destination}")


if __name__ == "__main__":
    main()
