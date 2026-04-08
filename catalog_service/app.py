from __future__ import annotations

import argparse
import json
import logging
import sqlite3
from copy import deepcopy
from pathlib import Path
from typing import Any

from flask import Flask, abort, jsonify, request, send_file


ROOT = Path(__file__).resolve().parent.parent
DB_PATH = ROOT / "catalog_service" / "data" / "packs.db"
CAPABILITY_MANIFEST_PATH = ROOT / "capability_metadata.json"
PACK_SERVER_BASE_URL = "http://10.221.31.77:5000"
API_PREFIX = "/apiv1"
BUNDLE_DIR = ROOT / "catalog_service" / "bundles"
ARTIFACT_DIR = ROOT / "artifacts"

logging.basicConfig(level=logging.INFO, format="[CatalogService] %(message)s")
app = Flask(__name__)


def connect_db() -> sqlite3.Connection:
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def ensure_schema() -> None:
    conn = connect_db()
    conn.execute("DROP TABLE IF EXISTS packs")
    conn.execute(
        """
        CREATE TABLE packs (
            pack_id TEXT PRIMARY KEY,
            capability_slug TEXT NOT NULL,
            capability_name TEXT NOT NULL,
            pack_name TEXT NOT NULL,
            bundle_file TEXT NOT NULL,
            pack_url TEXT NOT NULL,
            pack_description TEXT NOT NULL,
            pack_monetization_json TEXT NOT NULL,
            device_capability_json TEXT NOT NULL,
            capability_json TEXT NOT NULL
        )
        """
    )
    conn.commit()
    conn.close()


def load_manifest() -> dict[str, Any]:
    return json.loads(CAPABILITY_MANIFEST_PATH.read_text())


def normalize_text(text: str) -> str:
    return "".join(ch.lower() if ch.isalnum() else " " for ch in text).strip()


def identify_capability_from_skill(skill: str, capability_list: list[str]) -> str | None:
    normalized_skill = normalize_text(skill)
    if not normalized_skill:
        return None
    skill_terms = set(normalized_skill.split())
    ranked: list[tuple[int, int, str]] = []
    for capability in capability_list:
        normalized_capability = normalize_text(capability)
        capability_terms = set(normalized_capability.split())
        score = 0
        if normalized_capability == normalized_skill:
            score += 100
        if normalized_capability in normalized_skill:
            score += 20
        if skill_terms & capability_terms:
            score += len(skill_terms & capability_terms) * 5
        if score > 0:
            ranked.append((score, len(normalized_capability), capability))
    if not ranked:
        return None
    ranked.sort(key=lambda item: (-item[0], item[1], item[2]))
    return ranked[0][2]


def derive_pack_url(package: dict[str, Any]) -> str:
    bundle_file = package.get("bundle_file", "")
    if not bundle_file:
        pack_url = package.get("pack_url") or package.get("bundle_path")
        return pack_url if isinstance(pack_url, str) else ""
    return f"{PACK_SERVER_BASE_URL.rstrip('/')}/bundles/{bundle_file}"


def resolve_bundle_path(bundle_file: str) -> Path | None:
    if not bundle_file:
        return None
    for base_dir in (BUNDLE_DIR, ARTIFACT_DIR):
        candidate = base_dir / bundle_file
        if candidate.exists():
            return candidate
    return None


def derive_device_capability(entry: dict[str, Any]) -> dict[str, Any]:
    explicit = entry.get("device_capability")
    if isinstance(explicit, dict) and explicit:
        return deepcopy(explicit)
    runtime = entry.get("runtime_descriptor", {})
    device_capability: dict[str, Any] = {
        "accelerators": [],
    }
    memory_required = runtime.get("memory_required_mb")
    if isinstance(memory_required, int):
        device_capability["min_ram_mb"] = memory_required
    cpu_cores = runtime.get("cpu_cores_recommended")
    if isinstance(cpu_cores, int):
        device_capability["min_cpu_cores"] = cpu_cores
    if runtime.get("gpu_required") is True:
        device_capability["accelerators"] = ["gpu"]
    return device_capability


def normalize_manifest_entry(entry: dict[str, Any]) -> dict[str, Any]:
    normalized = deepcopy(entry)
    capability = normalized.get("capability", {})
    package = normalized.setdefault("package", {})
    package["pack_url"] = derive_pack_url(package)
    normalized["device_capability"] = derive_device_capability(normalized)
    return {
        "pack_id": package.get("pack_id", ""),
        "capability_slug": capability.get("slug", ""),
        "capability_name": capability.get("name", ""),
        "pack_name": package.get("pack_name", capability.get("name", "")),
        "bundle_file": package.get("bundle_file", ""),
        "pack_url": package.get("pack_url", ""),
        "pack_description": capability.get("description", ""),
        "pack_monetization": normalized.get("monetization", {}),
        "device_capability": normalized.get("device_capability", {}),
        "capability_json": normalized,
    }


def reindex() -> dict[str, Any]:
    ensure_schema()
    manifest = load_manifest()
    packs = [normalize_manifest_entry(entry) for entry in manifest.get("capabilities", [])]
    conn = connect_db()
    for pack in packs:
        conn.execute(
            """
            INSERT INTO packs (
                pack_id, capability_slug, capability_name, pack_name, bundle_file, pack_url,
                pack_description, pack_monetization_json, device_capability_json, capability_json
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                pack["pack_id"],
                pack["capability_slug"],
                pack["capability_name"],
                pack["pack_name"],
                pack["bundle_file"],
                pack["pack_url"],
                pack["pack_description"],
                json.dumps(pack["pack_monetization"]),
                json.dumps(pack["device_capability"]),
                json.dumps(pack["capability_json"]),
            ),
        )
    conn.commit()
    conn.close()
    app.logger.info("Indexed %s capability-pack entries from %s", len(packs), CAPABILITY_MANIFEST_PATH)
    return {"status": "ok", "indexed": len(packs)}


def row_to_summary(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "pack_id": row["pack_id"],
        "pack_name": row["pack_name"],
        "bundle_file": row["bundle_file"],
        "pack_url": row["pack_url"],
        "pack_description": row["pack_description"],
        "pack_monetization": json.loads(row["pack_monetization_json"]),
    }


def row_to_capability_json(row: sqlite3.Row) -> dict[str, Any]:
    return json.loads(row["capability_json"])


def capability_matches(required: dict[str, Any], actual: dict[str, Any]) -> bool:
    architecture = required.get("architecture")
    if isinstance(architecture, list) and actual.get("architecture") not in architecture:
        return False
    if isinstance(architecture, str) and actual.get("architecture") != architecture:
        return False
    if actual.get("ram_mb", 0) < required.get("min_ram_mb", 0):
        return False
    if actual.get("cpu_cores", 0) < required.get("min_cpu_cores", 0):
        return False
    os_family = required.get("os_family")
    if isinstance(os_family, list) and actual.get("os_family") not in os_family:
        return False
    if isinstance(os_family, str) and actual.get("os_family") != os_family:
        return False
    for accelerator in required.get("accelerators", []):
        if accelerator not in actual.get("accelerators", []):
            return False
    return True


def parse_device_capability_arg(value: str | None) -> dict[str, Any]:
    if not value:
        return {}
    parsed = json.loads(value)
    return parsed if isinstance(parsed, dict) else {}


@app.before_request
def log_request() -> None:
    payload = request.get_data(as_text=True)
    app.logger.info("Request %s %s args=%s body=%s", request.method, request.path, dict(request.args), payload or "{}")


@app.after_request
def log_response(response):
    app.logger.info("Response %s %s status=%s", request.method, request.path, response.status_code)
    return response


@app.get("/healthz")
def healthz():
    return jsonify({"status": "ok"})


@app.get("/bundles/<path:bundle_file>")
def get_bundle(bundle_file: str):
    bundle_path = resolve_bundle_path(bundle_file)
    if bundle_path is None:
        abort(404)
    return send_file(bundle_path, as_attachment=True, download_name=bundle_path.name)


@app.get(f"{API_PREFIX}/getCapabilityList")
def get_capability_list():
    conn = connect_db()
    rows = conn.execute("SELECT capability_slug FROM packs ORDER BY capability_slug ASC").fetchall()
    conn.close()
    capabilities = [row["capability_slug"] for row in rows]
    return jsonify({"capabilities": capabilities, "count": len(capabilities)})


@app.get(f"{API_PREFIX}/getCompatiblePackList")
def get_compatible_pack_list():
    capability = request.args.get("capability", "")
    device_capability = parse_device_capability_arg(request.args.get("device_capability"))

    conn = connect_db()
    rows = conn.execute(
        "SELECT pack_id, pack_name, bundle_file, pack_url, pack_description, pack_monetization_json, device_capability_json, capability_slug "
        "FROM packs ORDER BY pack_name ASC"
    ).fetchall()
    conn.close()

    compatible: list[dict[str, Any]] = []
    for row in rows:
        if capability and row["capability_slug"] != capability:
            continue
        required = json.loads(row["device_capability_json"])
        if capability_matches(required, device_capability):
            compatible.append(row_to_summary(row))

    return jsonify({"capability": capability, "packs": compatible, "count": len(compatible)})


@app.get(f"{API_PREFIX}/getPackDetails")
def get_pack_details():
    pack_id = request.args.get("pack_id", "")
    if not pack_id:
        return jsonify({"status": "error", "message": "pack_id is required"}), 400

    conn = connect_db()
    row = conn.execute("SELECT capability_json FROM packs WHERE pack_id = ?", (pack_id,)).fetchone()
    conn.close()
    if row is None:
        return jsonify({"status": "error", "message": "pack not found"}), 404
    return jsonify(row_to_capability_json(row))


@app.get("/capabilities")
def get_capabilities_legacy():
    return get_capability_list()


@app.post("/capabilities/identify")
def identify_capability():
    payload = request.get_json(force=True)
    skill = payload.get("skill", "")
    capability_list = payload.get("capability_list")
    if not isinstance(capability_list, list):
        conn = connect_db()
        rows = conn.execute("SELECT capability_slug FROM packs ORDER BY capability_slug ASC").fetchall()
        conn.close()
        capability_list = [row["capability_slug"] for row in rows]
    capability = identify_capability_from_skill(skill, capability_list)
    if capability is None:
        return jsonify({"status": "error", "message": "capability not identified"}), 404
    return jsonify({"status": "ok", "capability": capability})


@app.post("/packs/reindex")
def reindex_endpoint():
    return jsonify(reindex())


@app.get("/packs/<pack_id>")
def get_pack(pack_id: str):
    conn = connect_db()
    row = conn.execute("SELECT capability_json FROM packs WHERE pack_id = ?", (pack_id,)).fetchone()
    conn.close()
    if row is None:
        return jsonify({"status": "error", "message": "pack not found"}), 404
    return jsonify(row_to_capability_json(row))


@app.post("/packs/query")
def query_packs_legacy():
    payload = request.get_json(force=True)
    capability = payload.get("capability") or payload.get("intent") or ""
    device_capability = payload.get("device_capability", {})

    conn = connect_db()
    rows = conn.execute(
        "SELECT pack_id, pack_name, bundle_file, pack_url, pack_description, pack_monetization_json, device_capability_json, capability_slug "
        "FROM packs ORDER BY pack_name ASC"
    ).fetchall()
    conn.close()

    compatible: list[dict[str, Any]] = []
    for row in rows:
        if capability and row["capability_slug"] != capability:
            continue
        required = json.loads(row["device_capability_json"])
        if capability_matches(required, device_capability):
            compatible.append(row_to_summary(row))

    return jsonify({"capability": capability, "packs": compatible, "count": len(compatible)})


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run the Catalog Service")
    parser.add_argument("--host", default="0.0.0.0", help="Host IP address to bind to (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=5000, help="Port to bind to (default: 5000)")
    args = parser.parse_args()

    reindex()
    app.run(host=args.host, port=args.port, debug=False)
