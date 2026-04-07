# Edge AI Service Scaffold

This workspace now contains a runnable scaffold for an on-edge AI service with:

- A C++ DBus daemon exposing an AI Gateway to external applications.
- A `PackManager` that stages, installs, enables, loads, invokes, unloads, disables, uninstalls, and rolls back AI packs.
- A Python catalog service that indexes available packs and answers compatibility queries.
- A sample ABI-compatible AI pack for next-word prediction backed by a tiny ONNX model.

## Project Layout

- `src/edge_gateway`: C++ DBus daemon, AI Gateway, and PackManager.
- `include/edgeai`: Shared headers, plugin ABI, and interfaces.
- `catalog_service`: Python Flask service with an SQLite-backed pack index.
- `packs/next_word`: Sample next-word plugin pack, manifest, ONNX helper, and model assets.
- `artifacts`: Generated pack bundle tarball after building.
- `var`: Local state, staging, and installed pack roots used by the sample daemon.

## Runtime Design

### AI Gateway

The AI Gateway is exposed over DBus at:

- Bus name: `com.example.EdgeAI`
- Object path: `/com/example/EdgeAI/Gateway`
- Interface: `com.example.EdgeAI.Gateway1`

Supported methods:

- `QueryPacks(userId, intent, deviceCapabilityJson)`
- `InstallPack(userId, packId, approveDependencies)`
- `EnablePack(userId, packId)`
- `LoadPack(userId, packId)`
- `Invoke(userId, packId, prompt, optionsJson)`
- `UnloadPack(userId, packId)`
- `DisablePack(userId, packId)`
- `UninstallPack(userId, packId, forceSharedUsers)`
- `RollbackPack(userId, packId)`

The service also emits a `PackStateChanged` signal with a JSON payload when the PackManager publishes lifecycle events.

### PackManager

The sample PackManager implements the lifecycle expressed in your PlantUML:

1. Query pack metadata from the catalog service.
2. Download the selected pack into staging.
3. Verify the downloaded artifact by MD5.
4. Validate device capability and AI capability constraints.
5. Resolve dependencies with explicit approval.
6. Install through an `opkg`-shaped adapter.
7. Register state in the intent and pack registry.
8. Load the plugin and validate ABI compatibility.
9. Activate, configure, invoke, unload, disable, uninstall, or roll back as required.

State is persisted in:

- `var/state/intent_and_pack_registry.json`
- `var/state/rollback_registry.json`

### Catalog Service

The catalog service keeps an SQLite index of:

- Pack URL
- MD5 checksum
- Device capability requirements
- AI capability metadata
- Dependencies
- Version
- License
- Intent
- Tags
- Metering unit

It exposes:

- `GET /healthz`
- `GET /packs/<pack_id>`
- `POST /packs/query`
- `POST /packs/reindex`

## Building

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

This builds:

- `build/edge_gatewayd`
- `build/packs/next_word/lib/libnext_word_pack.*`
- `artifacts/next_word_pack.tar.gz`

## Running the Catalog Service

Use Python 3.10+ if you want ONNX-backed inference in the sample helper:

```bash
/opt/homebrew/bin/python3.10 -m venv .venv
source .venv/bin/activate
pip install -r catalog_service/requirements.txt
python catalog_service/app.py
```

## Running the Edge Daemon

In another terminal:

```bash
./build/edge_gatewayd
```

## Example DBus Calls

If `gdbus` is available:

```bash
gdbus call --session \
  --dest com.example.EdgeAI \
  --object-path /com/example/EdgeAI/Gateway \
  --method com.example.EdgeAI.Gateway1.QueryPacks \
  "user-1" "next_word_prediction" '{"architecture":"arm64","ram_mb":2048,"os_family":"linux","accelerators":[]}'
```

## Sample Pack

The sample pack:

- Exports a stable C ABI defined in `include/edgeai/pack_abi.h`
- Loads metadata from `packs/next_word/manifest.json`
- Uses a tiny ONNX bigram model for next-word prediction
- Falls back to the embedded transition table if `onnxruntime` is unavailable in the helper environment

## Notes

- The `opkg` integration here is an adapter scaffold. For a production edge runtime, replace the shell-based bundle extraction with your real package manager workflow.
- The sample ONNX model is intentionally tiny and is meant to validate plugin/runtime wiring, not ML quality.
