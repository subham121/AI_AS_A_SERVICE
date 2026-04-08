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
- `packs/next_word`: Sample next-word plugin pack, manifest, Python helper runtime, and model assets.
- `artifacts`: Generated pack bundle tarball after building.
- `var`: Local state, staging, and installed pack roots used by the sample daemon.

## Runtime Design

### AI Gateway

The AI Gateway is exposed over DBus at:

- Bus name: `com.example.EdgeAI`
- Object path: `/com/example/EdgeAI/Gateway`
- Interface: `com.example.EdgeAI.Gateway1`

Supported methods:

- `HandleUserRequest(userId, input, deviceCapabilityJson)`
- `QueryPacks(userId, capability, deviceCapabilityJson)`
- `UsePack(userId, packId, approveDependencies)`
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

1. Refresh the capability list from the catalog service during initialization and cache it in the registry.
2. Let the `CapabilityRouter` use the `IntentManager` to derive a skill from the user input.
3. Normalize the skill to a known capability and check local matching packs before querying cloud packs.
4. Ask the device capability provider for edge-device capabilities when cloud matching is required.
5. Download the selected pack into staging and verify it by MD5.
6. Resolve dependencies and install the bundle through an `opkg`-shaped adapter.
7. Register state in the pack registry.
8. Enable, load, invoke, unload, disable, and uninstall packs according to registry state.
9. Validate ABI compatibility before activation.

State is persisted in:

- `var/state/intent_and_pack_registry.json`
- `var/state/rollback_registry.json`

### Catalog Service

The catalog service keeps an SQLite index derived from `capability_metadata.json` and stores:

- Capability slug
- Pack ID and pack name
- Pack URL / bundle path
- Pack description
- Pack monetization metadata
- Derived device capability requirements
- Full raw pack capability JSON

It exposes:

- `GET /apiv1/getCapabilityList`
- `GET /apiv1/getCompatiblePackList`
- `GET /apiv1/getPackDetails`
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

Use Python 3.10+ for the catalog service and the sample pack helper:

```bash
/opt/homebrew/bin/python3.10 -m venv .venv
source .venv/bin/activate
pip install -r catalog_service/requirements.txt
python catalog_service/app.py --host 0.0.0.0 --port 5000
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
  --method com.example.EdgeAI.Gateway1.HandleUserRequest \
  "user-1" "predict next word" '{"architecture":"arm64","ram_mb":2048,"os_family":"linux","accelerators":[]}'
```

## Sample Pack

The sample pack:

- Exports a stable C ABI defined in `include/edgeai/pack_abi.h`
- Loads metadata from `packs/next_word/manifest.json`
- Uses a Python helper script to run model inference out of process
- Falls back to the embedded transition table when ONNX inference is unavailable or the pack is configured away from ONNX

## Notes

- The `opkg` integration here is an adapter scaffold. For a production edge runtime, replace the shell-based bundle extraction with your real package manager workflow.
- The sample ONNX model is intentionally tiny and is meant to validate plugin/runtime wiring, not ML quality.
