from __future__ import annotations

import json
import subprocess
import sys


def main() -> int:
    device = {
        "architecture": "arm64",
        "ram_mb": 2048,
        "os_family": "linux",
        "accelerators": [],
    }
    command = [
        "gdbus",
        "call",
        "--session",
        "--dest",
        "com.example.EdgeAI",
        "--object-path",
        "/com/example/EdgeAI/Gateway",
        "--method",
        "com.example.EdgeAI.Gateway1.HandleUserRequest",
        "user-1",
        "predict next word",
        json.dumps(device),
    ]
    completed = subprocess.run(command, check=False, capture_output=True, text=True)
    sys.stdout.write(completed.stdout)
    sys.stderr.write(completed.stderr)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
