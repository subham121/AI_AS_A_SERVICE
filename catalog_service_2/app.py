from __future__ import annotations

import argparse
import json
import logging
import time
from copy import deepcopy
from pathlib import Path
from typing import Any

from flask import Flask, abort, g, jsonify, request, send_file


APP_ROOT = Path(__file__).resolve().parent
MANIFEST_PATH = APP_ROOT / "capability_metadata.json"
BUNDLE_DIR = APP_ROOT / "bundles"
API_PREFIX = "/apiv1"

logger = logging.getLogger("catalog_service_2")
app = Flask(__name__)


def normalize_text(text: str) -> str:
    return "".join(ch.lower() if ch.isalnum() else " " for ch in text).strip()


def derive_device_capability(entry: dict[str, Any]) -> dict[str, Any]:
    explicit = entry.get("device_capability")
    if isinstance(explicit, dict) and explicit:
        return deepcopy(explicit)

    runtime = entry.get("runtime_descriptor", {})
    derived: dict[str, Any] = {
        "accelerators": [],
    }
    memory_required = runtime.get("memory_required_mb")
    if isinstance(memory_required, int):
        derived["min_ram_mb"] = memory_required
    cpu_required = runtime.get("cpu_cores_recommended")
    if isinstance(cpu_required, int):
        derived["min_cpu_cores"] = cpu_required
    if runtime.get("gpu_required") is True:
        derived["accelerators"] = ["gpu"]
    return derived


def capability_matches(required: dict[str, Any], actual: dict[str, Any]) -> bool:
    architecture = required.get("architecture")
    if isinstance(architecture, list) and actual.get("architecture") not in architecture:
        return False
    if isinstance(architecture, str) and actual.get("architecture") != architecture:
        return False

    os_family = required.get("os_family")
    if isinstance(os_family, list) and actual.get("os_family") not in os_family:
        return False
    if isinstance(os_family, str) and actual.get("os_family") != os_family:
        return False

    if actual.get("ram_mb", 0) < required.get("min_ram_mb", 0):
        return False
    if actual.get("cpu_cores", 0) < required.get("min_cpu_cores", 0):
        return False

    actual_accelerators = actual.get("accelerators", [])
    for accelerator in required.get("accelerators", []):
        if accelerator not in actual_accelerators:
            return False

    return True


class CatalogIndex:
    def __init__(self, manifest_path: Path, bundle_dir: Path, log: logging.Logger) -> None:
        self.manifest_path = manifest_path
        self.bundle_dir = bundle_dir
        self.log = log
        self.public_base_url = ""
        self._entries: list[dict[str, Any]] = []
        self._pack_by_id: dict[str, dict[str, Any]] = {}

    def set_public_base_url(self, base_url: str) -> None:
        self.public_base_url = base_url.rstrip("/")
        self.log.info("Configured public base URL: %s", self.public_base_url)

    def reload(self) -> None:
        if not self.manifest_path.exists():
            raise FileNotFoundError(f"Manifest not found: {self.manifest_path}")

        payload = json.loads(self.manifest_path.read_text())
        raw_entries = payload.get("capabilities", [])
        if not isinstance(raw_entries, list):
            raise ValueError("Manifest field 'capabilities' must be a list")

        entries: list[dict[str, Any]] = []
        pack_by_id: dict[str, dict[str, Any]] = {}
        self.log.info("Loading capability manifest from %s", self.manifest_path)
        for raw_entry in raw_entries:
            normalized = self._normalize_entry(raw_entry)
            pack_id = normalized["package"]["pack_id"]
            entries.append(normalized)
            pack_by_id[pack_id] = normalized
            bundle_file = normalized["package"].get("bundle_file", "")
            bundle_path = self.bundle_dir / bundle_file if bundle_file else None
            if bundle_path and bundle_path.exists():
                self.log.info("Indexed pack_id=%s capability=%s bundle=%s",
                              pack_id,
                              normalized["capability"]["slug"],
                              bundle_path.name)
            else:
                self.log.warning("Indexed pack_id=%s capability=%s with missing bundle=%s",
                                 pack_id,
                                 normalized["capability"]["slug"],
                                 bundle_file or "(none)")

        entries.sort(key=lambda item: (item["capability"]["slug"], item["package"]["pack_id"]))
        self._entries = entries
        self._pack_by_id = pack_by_id
        self.log.info("Catalog loaded entries=%s capabilities=%s",
                      len(self._entries),
                      len(self.get_capability_list()))

    def _normalize_entry(self, raw_entry: dict[str, Any]) -> dict[str, Any]:
        entry = deepcopy(raw_entry)
        capability = entry.setdefault("capability", {})
        package = entry.setdefault("package", {})
        bundle_file = package.get("bundle_file", "")
        if bundle_file:
            package["bundle_path"] = str((self.bundle_dir / bundle_file).resolve())
            if self.public_base_url:
                package["pack_url"] = f"{self.public_base_url}/bundles/{bundle_file}"
        elif "bundle_path" not in package:
            package["bundle_path"] = ""

        entry["device_capability"] = derive_device_capability(entry)
        return entry

    def get_capability_list(self) -> list[str]:
        return [entry["capability"]["slug"] for entry in self._entries]

    def get_pack_details(self, pack_id: str) -> dict[str, Any] | None:
        entry = self._pack_by_id.get(pack_id)
        return deepcopy(entry) if entry else None

    def get_compatible_pack_list(self, capability: str, device_capability: dict[str, Any]) -> list[dict[str, Any]]:
        compatible: list[dict[str, Any]] = []
        for entry in self._entries:
            if capability and entry["capability"]["slug"] != capability:
                continue
            required = entry.get("device_capability", {})
            if capability_matches(required, device_capability):
                package = entry["package"]
                compatible.append(
                    {
                        "pack_id": package["pack_id"],
                        "pack_name": package.get("pack_name", entry["capability"].get("name", "")),
                        "pack_url": package.get("pack_url", package.get("bundle_path", "")),
                        "pack_description": entry["capability"].get("description", ""),
                        "pack_monetization": entry.get("monetization", {}),
                    }
                )
        self.log.info("Compatible pack query capability=%s device_capability=%s matched=%s",
                      capability,
                      json.dumps(device_capability, sort_keys=True),
                      len(compatible))
        return compatible

    def bundle_path_for(self, bundle_file: str) -> Path | None:
        if not bundle_file:
            return None
        path = self.bundle_dir / bundle_file
        return path if path.exists() else None


index = CatalogIndex(MANIFEST_PATH, BUNDLE_DIR, logger)


def parse_device_capability_arg(value: str | None) -> dict[str, Any]:
    if not value:
        return {}
    parsed = json.loads(value)
    if not isinstance(parsed, dict):
        raise ValueError("device_capability must be a JSON object")
    return parsed


@app.before_request
def log_request() -> None:
    g.started_at = time.perf_counter()
    payload = request.get_data(as_text=True)
    logger.info("Request method=%s path=%s remote=%s args=%s body=%s",
                request.method,
                request.path,
                request.remote_addr,
                dict(request.args),
                payload or "{}")


@app.after_request
def log_response(response):
    elapsed_ms = (time.perf_counter() - g.started_at) * 1000.0
    logger.info("Response method=%s path=%s status=%s duration_ms=%.2f",
                request.method,
                request.path,
                response.status_code,
                elapsed_ms)
    return response


@app.get("/healthz")
def healthz():
    return jsonify(
        {
            "status": "ok",
            "manifest_path": str(MANIFEST_PATH),
            "bundle_dir": str(BUNDLE_DIR),
            "entries": len(index.get_capability_list()),
        }
    )


@app.get("/bundles/<path:bundle_file>")
def get_bundle(bundle_file: str):
    bundle_path = index.bundle_path_for(bundle_file)
    if bundle_path is None:
        logger.warning("Bundle not found bundle_file=%s", bundle_file)
        abort(404)
    logger.info("Serving bundle bundle_file=%s path=%s", bundle_file, bundle_path)
    return send_file(bundle_path, as_attachment=True, download_name=bundle_path.name)


@app.get(f"{API_PREFIX}/getCapabilityList")
def get_capability_list():
    capabilities = index.get_capability_list()
    return jsonify({"capabilities": capabilities, "count": len(capabilities)})


@app.get(f"{API_PREFIX}/getCompatiblePackList")
def get_compatible_pack_list():
    capability = request.args.get("capability", "")
    if not capability:
        return jsonify({"status": "error", "message": "capability is required"}), 400

    try:
        device_capability = parse_device_capability_arg(request.args.get("device_capability"))
    except ValueError as ex:
        return jsonify({"status": "error", "message": str(ex)}), 400

    packs = index.get_compatible_pack_list(capability, device_capability)
    return jsonify({"capability": capability, "packs": packs, "count": len(packs)})


@app.get(f"{API_PREFIX}/getPackDetails")
def get_pack_details():
    pack_id = request.args.get("pack_id", "")
    if not pack_id:
        return jsonify({"status": "error", "message": "pack_id is required"}), 400

    entry = index.get_pack_details(pack_id)
    if entry is None:
        logger.warning("Pack details not found pack_id=%s", pack_id)
        return jsonify({"status": "error", "message": "pack not found"}), 404

    logger.info("Returning pack details pack_id=%s capability=%s",
                pack_id,
                entry["capability"].get("slug", ""))
    return jsonify(entry)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the standalone catalog_service_2 pack server")
    parser.add_argument("--host", default="0.0.0.0", help="Bind host, for example 10.221.31.77")
    parser.add_argument("--port", type=int, default=5000, help="Bind port")
    parser.add_argument("--advertise-host", default="", help="Host/IP to publish in pack_url values")
    parser.add_argument("--log-level", default="INFO", help="Python log level")
    args = parser.parse_args()

    logging.basicConfig(level=getattr(logging, args.log_level.upper(), logging.INFO),
                        format="[CatalogService2] %(message)s")
    advertise_host = args.advertise_host or args.host or "127.0.0.1"
    index.set_public_base_url(f"http://{advertise_host}:{args.port}")
    index.reload()
    logger.info("Starting catalog_service_2 host=%s port=%s advertise_host=%s", args.host, args.port, advertise_host)
    app.run(host=args.host, port=args.port, debug=False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
