from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

import numpy as np

try:
    import onnxruntime as ort
except Exception:  # pragma: no cover - fallback path is intentional
    ort = None


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text())


def normalize_token(prompt: str) -> str:
    tokens = re.findall(r"[A-Za-z']+", prompt.lower())
    return tokens[-1] if tokens else "hello"


def predict_with_onnx(model_path: Path, token_id: int) -> int:
    if ort is None:
        raise RuntimeError("onnxruntime is not available")
    session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    output = session.run(None, {"input_token": np.array([token_id], dtype=np.int64)})[0]
    return int(output[0])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--request", required=True)
    parser.add_argument("--response", required=True)
    parser.add_argument("--pack-root", required=True)
    args = parser.parse_args()

    pack_root = Path(args.pack_root)
    manifest = load_json(pack_root / "manifest.json")
    config = load_json(pack_root / manifest["entrypoint"]["default_config"])
    runtime_meta = manifest["runtime"]
    vocab = load_json(pack_root / runtime_meta["vocab"])["tokens"]
    transitions = load_json(pack_root / runtime_meta["transitions"])
    request = load_json(Path(args.request))

    token = normalize_token(request["prompt"])
    token_id = vocab.index(token) if token in vocab else vocab.index(config.get("unknown_token", "world"))

    backend = "fallback"
    try:
        if config.get("backend", "auto") in {"auto", "onnx"}:
            next_id = predict_with_onnx(pack_root / runtime_meta["model"], token_id)
            backend = "onnxruntime"
        else:
            raise RuntimeError("forcing fallback")
    except Exception:
        next_token = transitions.get(token, config.get("unknown_token", "world"))
        next_id = vocab.index(next_token)

    response = {
        "result": vocab[next_id],
        "metadata": {
            "backend": backend,
            "token_id": token_id,
            "next_token_id": next_id,
            "model": runtime_meta["model"],
        },
    }
    Path(args.response).write_text(json.dumps(response))
    return 0


if __name__ == "__main__":
    sys.exit(main())
