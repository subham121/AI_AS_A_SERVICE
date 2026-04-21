# Software Requirements Specification

## 1. Introduction

### 1.1 Purpose

This document specifies the software requirements for the Edge AI Service platform implemented in this repository. The platform enables an on-device AI Gateway to discover, install, manage, and execute AI packs based on user intent, device capability, and pack capability metadata provided by a catalog/pack server.

### 1.2 Scope

The system provides:

- A C++ DBus-based Edge AI Gateway for external applications.
- A `PackManager` that governs the full AI pack lifecycle.
- A `CapabilityRouter` that maps user input to capability-driven pack discovery.
- A standalone Python pack server (`catalog_service_2`) exposing capability and pack metadata APIs.
- A plugin-based AI pack model with ABI compatibility requirements.
- A sample AI pack for next-word prediction.

The scope includes local execution, pack discovery, compatibility filtering, artifact download, pack lifecycle state management, and event/log publishing. Governance and policy enforcement are explicitly out of scope for the current version.

### 1.3 Definitions and Acronyms

- AI Gateway: DBus service entry point exposed to external applications.
- Pack: Deployable AI capability bundle containing manifest, binaries, runtime assets, and metadata.
- PackManager: Runtime manager responsible for pack lifecycle operations.
- CapabilityRouter: Component that maps user intent to capability slug and routes discovery.
- Pack Server: HTTP service exposing capability list, compatible pack list, and pack details.
- ABI: Application Binary Interface used to validate plugin compatibility.
- DBus: Inter-process communication mechanism used by the edge gateway.
- HITL: Human-in-the-loop.
- ONNX: Open Neural Network Exchange model format.

### 1.4 References

- [README.md](/Users/subhasishmishra/Documents/DesignDiagrams/README.md)
- [packmanager_lifecycle_sequence.puml](/Users/subhasishmishra/Documents/DesignDiagrams/packmanager_lifecycle_sequence.puml)
- [catalog_service_2/README.md](/Users/subhasishmishra/Documents/DesignDiagrams/catalog_service_2/README.md)
- [include/edgeai/pack_manager.h](/Users/subhasishmishra/Documents/DesignDiagrams/include/edgeai/pack_manager.h)
- [include/edgeai/ai_gateway_service.h](/Users/subhasishmishra/Documents/DesignDiagrams/include/edgeai/ai_gateway_service.h)
- [include/edgeai/capability_router.h](/Users/subhasishmishra/Documents/DesignDiagrams/include/edgeai/capability_router.h)
- [include/edgeai/pack_abi.h](/Users/subhasishmishra/Documents/DesignDiagrams/include/edgeai/pack_abi.h)

## 2. Overall Description

### 2.1 Product Perspective

The platform is an edge-side AI-as-a-service runtime. External applications interact only with the AI Gateway over DBus. The gateway delegates discovery to the CapabilityRouter and lifecycle actions to the PackManager. Metadata and pack artifact discovery are provided by a standalone HTTP pack server. AI packs are installed locally and loaded as plugins when activated.

### 2.2 Product Functions

The system shall:

- Accept user requests from external applications through DBus.
- Identify user intent and map it to a capability slug.
- Query local registry state and remote pack metadata.
- Filter compatible packs based on device capability.
- Download and install selected packs.
- Enable, load, invoke, unload, disable, uninstall, and roll back packs.
- Validate plugin ABI compatibility before runtime activation.
- Persist pack and rollback state in local registry files.
- Publish lifecycle events and logs for observability.

### 2.3 User Classes

- End User: Selects or uses a pack to satisfy an AI need.
- External Application: Consumes DBus APIs exposed by the AI Gateway.
- Platform Operator: Deploys the edge service and pack server.
- Pack Publisher: Produces pack bundles and capability metadata entries.

### 2.4 Operating Environment

- Edge runtime: C++17 application running on Linux or Darwin-class systems.
- IPC: DBus via GLib/GIO.
- Pack server: Python 3 with Flask.
- Model runtime: Sample pack uses Python helper with ONNX Runtime when available.
- Storage: Local filesystem-based staging, install, state, and rollback registries.

### 2.5 Constraints

- Current package installation is implemented through archive extraction and an `opkg`-shaped adapter, not full system package manager integration.
- Plugin loading is in-process and depends on ABI compatibility.
- Pack server metadata contract is HTTP GET based and must expose the required `/apiv1` endpoints.
- Governance, policy, and advanced multi-tenant isolation are not included in this version.

### 2.6 Assumptions and Dependencies

- Pack bundles are available and reachable by the `pack_url` returned from the pack server.
- Every pack has a valid capability metadata entry and bundle artifact.
- Device capability input is available either from the caller or system defaults.
- External applications can access the DBus session/service bus configured for the gateway.

## 3. System Architecture

### 3.1 High-Level Components

- AI Gateway Service: DBus interface handler and event publisher.
- CapabilityRouter: Intent extraction, capability normalization, discovery routing.
- PackManager: Registry persistence, pack lifecycle orchestration, pack server integration.
- PackRuntime: Plugin load/unload/invoke abstraction.
- Pack Server (`catalog_service_2`): Manifest-driven capability and bundle service.
- AI Pack: ABI-compatible plugin bundle with manifest and runtime assets.

### 3.2 Data Stores

- Intent and Pack Registry: `var/state/intent_and_pack_registry.json`
- Rollback Registry: `var/state/rollback_registry.json`
- Pack server manifest: `catalog_service_2/capability_metadata.json`
- Pack artifacts: `catalog_service_2/bundles/*`

### 3.3 Primary Runtime Flow

1. External application calls DBus method on AI Gateway.
2. Gateway delegates discovery or lifecycle action.
3. CapabilityRouter fetches capability list and resolves skill to capability.
4. PackManager checks local registry for compatible installed packs.
5. If none match, PackManager queries remote pack server for compatible packs and details.
6. User or caller initiates install/load/use flow for the chosen pack.
7. PackManager stages, verifies, installs, enables, and loads the pack.
8. Runtime invocation returns inference result and metadata.

## 4. External Interface Requirements

### 4.1 DBus Interface Requirements

The AI Gateway shall expose the following DBus methods on interface `com.example.EdgeAI.Gateway1`:

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

The AI Gateway shall emit a `PackStateChanged` signal containing JSON event payloads.

### 4.2 HTTP Interface Requirements

The pack server shall expose:

- `GET /apiv1/getCapabilityList`
  Response:
  `{ "capabilities": ["capability_slug", ...], "count": <n> }`
- `GET /apiv1/getCompatiblePackList?capability=<slug>&device_capability=<json>`
  Response:
  `{ "capability": "<slug>", "packs": [{ "pack_id", "pack_name", "pack_url", "pack_description", "pack_monetization" }], "count": <n> }`
- `GET /apiv1/getPackDetails?pack_id=<pack_id>`
  Response:
  complete capability metadata JSON object for the pack
- `GET /bundles/<bundle_file>`
  Response:
  bundle artifact binary payload

### 4.3 Pack Manifest Requirements

Each pack shall provide a manifest containing at minimum:

- `pack_id`
- `name`
- `version`
- `intent`
- `license`
- `metering_unit`
- `entrypoint.library`
- `entrypoint.default_config`
- `device_capability`
- `ai_capability`
- `dependencies`
- `services`
- `tags`

### 4.4 Plugin ABI Requirements

Each pack plugin shall implement the ABI defined in [pack_abi.h](/Users/subhasishmishra/Documents/DesignDiagrams/include/edgeai/pack_abi.h), including:

- ABI version query
- Manifest retrieval
- Instance creation
- Activation
- Configuration
- Prediction/inference handling
- Deactivation
- Instance destruction

## 5. Functional Requirements

### 5.1 Discovery and Capability Routing

- FR-1: The system shall accept user input and derive a normalized intent/skill string.
- FR-2: The system shall fetch and cache the capability list from the pack server.
- FR-3: The system shall normalize user skill to a capability slug using text matching.
- FR-4: The system shall check the local registry for packs supporting the resolved capability.
- FR-5: If no suitable local pack exists, the system shall query the pack server for compatible packs.
- FR-6: `QueryPacks` shall fetch compatible pack summaries and prefetch full pack details for each returned `pack_id`.
- FR-7: The system shall cache remote compatible pack summaries and full pack details in the local pack registry.

### 5.2 Device Compatibility

- FR-8: The system shall evaluate pack compatibility against device attributes including architecture, OS family, RAM, CPU cores, and accelerator requirements.
- FR-9: The system shall use explicit device capability input from the caller when available; otherwise it shall use default device capability metadata configured at runtime.

### 5.3 Installation and Staging

- FR-10: The system shall download the selected pack bundle into a staging location before installation.
- FR-11: The system shall verify the downloaded artifact against the declared checksum when an MD5 checksum is available.
- FR-12: The system shall reject installation if device capability requirements are not satisfied.
- FR-13: The system shall resolve pack dependencies before final installation.
- FR-14: The system shall persist previous installed versions in a rollback registry when upgrading a pack.
- FR-15: The system shall extract and install the bundle into a versioned local install root.
- FR-16: The system shall register installed pack metadata in the local pack registry.

### 5.4 Enable, Load, and Invoke

- FR-17: The system shall support per-user pack enablement state.
- FR-18: The system shall validate plugin ABI version before runtime activation.
- FR-19: The system shall load a pack plugin into memory only after installation and enablement.
- FR-20: The system shall invoke the loaded pack and return inference result plus metadata.
- FR-21: The system shall reject invocation when the pack is not in a loaded state.

### 5.5 Unload, Disable, Uninstall, and Rollback

- FR-22: The system shall unload loaded packs and update their runtime state.
- FR-23: The system shall disable packs per user without deleting installation artifacts.
- FR-24: The system shall uninstall packs and remove local install/staging data.
- FR-25: The system shall support rollback to a previously recorded version when rollback data exists.

### 5.6 Logging and Events

- FR-26: The system shall log request and response flow for DBus gateway methods.
- FR-27: The system shall log HTTP request and response activity to the pack server.
- FR-28: The system shall log lifecycle state transitions for pack install, enable, load, invoke, unload, disable, uninstall, and rollback.
- FR-29: The system shall publish structured lifecycle events through the event sink used by the AI Gateway.

### 5.7 Pack Server Requirements

- FR-30: The standalone pack server shall load capability metadata from a local `capability_metadata.json`.
- FR-31: The standalone pack server shall serve local bundle artifacts from its own `bundles/` directory.
- FR-32: The standalone pack server shall return pack URLs pointing to its configured public host and port.
- FR-33: The standalone pack server shall support both real bundles and placeholder/dummy bundles used for compatibility and contract testing.

## 6. Non-Functional Requirements

### 6.1 Performance

- NFR-1: Capability list and compatible pack queries should complete fast enough for interactive user flows on a local network.
- NFR-2: Pack discovery should prefer locally installed packs over remote packs when capability matches exist.

### 6.2 Reliability

- NFR-3: Initialization shall remain resilient if the pack server is temporarily unavailable by falling back to cached registry data where possible.
- NFR-4: Registry writes shall be persisted to disk for lifecycle and rollback state.

### 6.3 Maintainability

- NFR-5: Component responsibilities shall remain separated between gateway, router, manager, runtime, and pack server.
- NFR-6: Metadata contracts shall be manifest-driven rather than hard-coded per pack.

### 6.4 Observability

- NFR-7: Logs shall include enough context to trace user request handling, remote metadata lookups, and pack state transitions.
- NFR-8: Event payloads shall be JSON serializable and consumable by external observers.

### 6.5 Portability

- NFR-9: The edge runtime shall support Linux and Darwin-class systems where the declared pack device capability allows.

## 7. Data Requirements

### 7.1 Registry Model

The local pack registry shall maintain:

- known packs and their installed metadata
- per-user pack states
- cached capability list
- cached compatible pack summaries
- cached full pack details from the pack server

### 7.2 Rollback Model

The rollback registry shall maintain:

- pack identifier
- previous version metadata
- prior manifest path
- prior install root

### 7.3 Pack Server Metadata Model

Each capability metadata entry shall include:

- `capability` object
- `package` object
- `owner` object
- `monetization` object
- `runtime_descriptor` object
- optional `device_capability` object

## 8. Security and Safety Requirements

- SEC-1: The system shall validate bundle integrity through checksum verification when checksum data is available.
- SEC-2: The system shall validate ABI compatibility before activating a plugin.
- SEC-3: The system shall not execute a pack plugin that fails ABI validation.

This version does not specify advanced sandboxing, code signing, tenant isolation, or governance policy enforcement.

## 9. Deployment Requirements

- The edge gateway shall be built from the C++ project via CMake.
- The pack server shall run as a standalone Python service.
- The pack server deployment shall include:
  - `app.py`
  - `capability_metadata.json`
  - `bundles/` directory
  - Python dependencies from `requirements.txt`

## 10. Acceptance Criteria

- AC-1: External clients can call DBus methods and receive structured JSON responses.
- AC-2: `HandleUserRequest` and `QueryPacks` can resolve `next_word_prediction` against the standalone pack server.
- AC-3: `getCapabilityList`, `getCompatiblePackList`, and `getPackDetails` return valid responses from the standalone pack server.
- AC-4: `QueryPacks` stores compatible pack summaries and pack details in the local registry cache.
- AC-5: The `next_word_pack` bundle is discoverable and downloadable from the standalone pack server.
- AC-6: The sample next-word pack can be installed, loaded, and invoked through the pack lifecycle flow.

## 11. Future Enhancements

- Full package-manager-backed install/uninstall integration.
- Richer dependency resolution and shared dependency reference tracking.
- Stronger pack signing and integrity verification beyond MD5.
- Process isolation for pack execution.
- Governance, policy enforcement, and audit controls.
