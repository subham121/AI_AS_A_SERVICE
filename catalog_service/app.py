from __future__ import annotations

import hashlib
import json
import os
import sqlite3
from pathlib import Path
from typing import Any

from flask import Flask, jsonify, request


ROOT = Path(__file__).resolve().parent.parent
DB_PATH = ROOT / "catalog_service" / "data" / "packs.db"
SEED_PATH = ROOT / "catalog_service" / "data" / "seed_packs.json"

app = Flask(__name__)


def connect_db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def ensure_schema() -> None:
    conn = connect_db()
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS packs (
            pack_id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            version TEXT NOT NULL,
            package_relpath TEXT NOT NULL,
            manifest_relpath TEXT NOT NULL,
            package_url TEXT,
            md5 TEXT,
            license TEXT,
            intent TEXT NOT NULL,
            metering_unit TEXT NOT NULL,
            dependencies_json TEXT NOT NULL,
            device_capability_json TEXT NOT NULL,
            ai_capability_json TEXT NOT NULL,
            services_json TEXT NOT NULL,
            tags_json TEXT NOT NULL
        )
        """
    )
    conn.commit()
    conn.close()


def compute_md5(path: Path) -> str | None:
    if not path.exists():
        return None
    digest = hashlib.md5()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(8192), b""):
            digest.update(chunk)
    return digest.hexdigest()


def as_file_url(path: Path) -> str:
    return path.resolve().as_uri()


def load_seed() -> list[dict[str, Any]]:
    return json.loads(SEED_PATH.read_text())


def reindex() -> dict[str, Any]:
    ensure_schema()
    packs = load_seed()
    conn = connect_db()
    for pack in packs:
        package_path = ROOT / pack["package_relpath"]
        manifest_path = ROOT / pack["manifest_relpath"]
        md5 = compute_md5(package_path)
        package_url = as_file_url(package_path)
        conn.execute(
            """
            INSERT INTO packs (
                pack_id, name, version, package_relpath, manifest_relpath, package_url, md5, license,
                intent, metering_unit, dependencies_json, device_capability_json,
                ai_capability_json, services_json, tags_json
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(pack_id) DO UPDATE SET
                name=excluded.name,
                version=excluded.version,
                package_relpath=excluded.package_relpath,
                manifest_relpath=excluded.manifest_relpath,
                package_url=excluded.package_url,
                md5=excluded.md5,
                license=excluded.license,
                intent=excluded.intent,
                metering_unit=excluded.metering_unit,
                dependencies_json=excluded.dependencies_json,
                device_capability_json=excluded.device_capability_json,
                ai_capability_json=excluded.ai_capability_json,
                services_json=excluded.services_json,
                tags_json=excluded.tags_json
            """,
            (
                pack["pack_id"],
                pack["name"],
                pack["version"],
                pack["package_relpath"],
                pack["manifest_relpath"],
                package_url,
                md5,
                pack["license"],
                pack["intent"],
                pack["metering_unit"],
                json.dumps(pack["dependencies"]),
                json.dumps(pack["device_capability"]),
                json.dumps(pack["ai_capability"]),
                json.dumps(pack["services"]),
                json.dumps(pack["tags"]),
            ),
        )
    conn.commit()
    conn.close()
    return {"status": "ok", "indexed": len(packs)}


def row_to_pack(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "pack_id": row["pack_id"],
        "name": row["name"],
        "version": row["version"],
        "package_url": row["package_url"],
        "md5": row["md5"],
        "license": row["license"],
        "intent": row["intent"],
        "metering_unit": row["metering_unit"],
        "dependencies": json.loads(row["dependencies_json"]),
        "device_capability": json.loads(row["device_capability_json"]),
        "ai_capability": json.loads(row["ai_capability_json"]),
        "services": json.loads(row["services_json"]),
        "tags": json.loads(row["tags_json"]),
        "manifest_path": str((ROOT / row["manifest_relpath"]).resolve()),
    }


def capability_matches(required: dict[str, Any], actual: dict[str, Any]) -> bool:
    architecture = required.get("architecture")
    if isinstance(architecture, list) and actual.get("architecture") not in architecture:
        return False
    if isinstance(architecture, str) and actual.get("architecture") != architecture:
        return False
    if actual.get("ram_mb", 0) < required.get("min_ram_mb", 0):
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


@app.get("/healthz")
def healthz():
    return jsonify({"status": "ok"})


@app.post("/packs/reindex")
def reindex_endpoint():
    return jsonify(reindex())


@app.get("/packs/<pack_id>")
def get_pack(pack_id: str):
    conn = connect_db()
    row = conn.execute("SELECT * FROM packs WHERE pack_id = ?", (pack_id,)).fetchone()
    conn.close()
    if row is None:
        return jsonify({"status": "error", "message": "pack not found"}), 404
    return jsonify(row_to_pack(row))


@app.post("/packs/query")
def query_packs():
    payload = request.get_json(force=True)
    intent = payload["intent"]
    device = payload["device_capability"]
    conn = connect_db()
    rows = conn.execute("SELECT * FROM packs WHERE intent = ?", (intent,)).fetchall()
    conn.close()

    compatible = []
    for row in rows:
        pack = row_to_pack(row)
        if capability_matches(pack["device_capability"], device):
            compatible.append(pack)
    return jsonify({"packs": compatible, "count": len(compatible)})


if __name__ == "__main__":
    ensure_schema()
    reindex()
    app.run(host="127.0.0.1", port=5001, debug=False)
