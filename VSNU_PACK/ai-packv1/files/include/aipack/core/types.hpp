// =============================================================================
// AI Packs System - Core Types & Definitions
// Designed for embedded C++ devices with scalability and modifiability
// =============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <optional>
#include <variant>
#include <chrono>
#include <mutex>
#include <atomic>
#include <ostream>

namespace aipack {

// =============================================================================
// Version
// =============================================================================
struct Version {
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t patch = 0;
    std::string prerelease;

    bool operator==(const Version& o) const {
        return major == o.major && minor == o.minor && patch == o.patch;
    }
    bool operator<(const Version& o) const {
        if (major != o.major) return major < o.major;
        if (minor != o.minor) return minor < o.minor;
        return patch < o.patch;
    }
    bool operator<=(const Version& o) const { return *this == o || *this < o; }
    bool operator>(const Version& o) const { return !(*this <= o); }
    bool operator>=(const Version& o) const { return !(*this < o); }
    bool operator!=(const Version& o) const { return !(*this == o); }

    std::string toString() const {
        std::string s = std::to_string(major) + "." +
                        std::to_string(minor) + "." +
                        std::to_string(patch);
        if (!prerelease.empty()) s += "-" + prerelease;
        return s;
    }

    static Version parse(const std::string& str) {
        Version v;
        size_t pos1 = str.find('.');
        size_t pos2 = str.find('.', pos1 + 1);
        size_t posDash = str.find('-', pos2 != std::string::npos ? pos2 : 0);

        if (pos1 != std::string::npos) {
            v.major = static_cast<uint16_t>(std::stoi(str.substr(0, pos1)));
            if (pos2 != std::string::npos) {
                v.minor = static_cast<uint16_t>(
                    std::stoi(str.substr(pos1 + 1, pos2 - pos1 - 1)));
                std::string patchStr = (posDash != std::string::npos)
                    ? str.substr(pos2 + 1, posDash - pos2 - 1)
                    : str.substr(pos2 + 1);
                v.patch = static_cast<uint16_t>(std::stoi(patchStr));
                if (posDash != std::string::npos) {
                    v.prerelease = str.substr(posDash + 1);
                }
            }
        }
        return v;
    }
};

// =============================================================================
// Error Handling
// =============================================================================
enum class ErrorCode : uint32_t {
    OK = 0,
    // Pack errors
    PackNotFound = 100,
    PackAlreadyInstalled,
    PackInvalidManifest,
    PackDependencyMissing,
    PackVersionConflict,
    PackCorrupted,
    PackIncompatiblePlatform,
    // Runtime errors
    RuntimeNotInitialized = 200,
    RuntimeOutOfMemory,
    RuntimeModelLoadFailed,
    RuntimeInferenceFailed,
    RuntimePipelineError,
    // Plugin errors
    PluginLoadFailed = 300,
    PluginSymbolNotFound,
    PluginInterfaceMismatch,
    PluginInitFailed,
    // Agent errors
    AgentNotFound = 400,
    AgentSkillNotAvailable,
    AgentToolError,
    AgentWorkflowFailed,
    // Governance errors
    PolicyViolation = 500,
    AccessDenied,
    AuditLogFailed,
    ResourceLimitExceeded,
    SafetyGuardrailTriggered,
    // General errors
    InvalidArgument = 900,
    NotImplemented,
    InternalError,
    IOError,
    TimeoutError,
};

inline std::ostream& operator<<(std::ostream& os, ErrorCode code) {
    return os << static_cast<uint32_t>(code);
}

struct Error {
    ErrorCode code = ErrorCode::OK;
    std::string message;
    std::string context;

    bool ok() const { return code == ErrorCode::OK; }
    operator bool() const { return !ok(); }

    static Error success() { return {ErrorCode::OK, "", ""}; }
    static Error make(ErrorCode c, const std::string& msg,
                      const std::string& ctx = "") {
        return {c, msg, ctx};
    }
};

template<typename T>
struct Result {
    std::optional<T> value;
    Error error;

    bool ok() const { return error.ok() && value.has_value(); }
    operator bool() const { return ok(); }

    const T& operator*() const { return *value; }
    T& operator*() { return *value; }
    const T* operator->() const { return &(*value); }
    T* operator->() { return &(*value); }

    static Result success(T val) {
        return {std::move(val), Error::success()};
    }
    static Result failure(ErrorCode c, const std::string& msg,
                          const std::string& ctx = "") {
        return {std::nullopt, Error::make(c, msg, ctx)};
    }
};

// =============================================================================
// Data Types for AI Operations
// =============================================================================

// Tensor data types for model I/O
enum class DataType : uint8_t {
    Float32,
    Float16,
    Int32,
    Int16,
    Int8,
    UInt8,
    Bool,
    String,
    BFloat16,
};

inline size_t dataTypeSize(DataType dt) {
    switch (dt) {
        case DataType::Float32: case DataType::Int32: return 4;
        case DataType::Float16: case DataType::Int16: case DataType::BFloat16: return 2;
        case DataType::Int8: case DataType::UInt8: case DataType::Bool: return 1;
        case DataType::String: return 0; // variable
    }
    return 0;
}

// Generic tensor for model I/O
struct Tensor {
    std::string name;
    DataType dtype = DataType::Float32;
    std::vector<int64_t> shape;
    std::vector<uint8_t> data;

    size_t elementCount() const {
        size_t count = 1;
        for (auto s : shape) count *= static_cast<size_t>(s);
        return count;
    }

    size_t byteSize() const {
        return elementCount() * dataTypeSize(dtype);
    }

    template<typename T>
    T* dataAs() { return reinterpret_cast<T*>(data.data()); }

    template<typename T>
    const T* dataAs() const { return reinterpret_cast<const T*>(data.data()); }
};

// Audio buffer for TTS/STT
struct AudioBuffer {
    std::vector<float> samples;
    uint32_t sampleRate = 16000;
    uint16_t channels = 1;
    uint16_t bitsPerSample = 16;

    float durationSeconds() const {
        if (sampleRate == 0 || channels == 0) return 0.0f;
        return static_cast<float>(samples.size()) /
               static_cast<float>(sampleRate * channels);
    }
};

// Key-value properties for flexible configuration
using Properties = std::unordered_map<std::string, std::string>;
using JsonValue = std::variant<std::string, int64_t, double, bool>;
using JsonMap = std::unordered_map<std::string, JsonValue>;

// =============================================================================
// Capability & Platform Descriptors
// =============================================================================
enum class PackType : uint8_t {
    Model,          // AI model (ONNX, TFLite, custom)
    Pipeline,       // Processing pipeline
    Service,        // Inference service (TTS, STT, etc.)
    Runtime,        // AI runtime/engine
    Agent,          // Autonomous agent
    AgentTool,      // Tool for agents
    Skill,          // Reusable skill
    Knowledge,      // Knowledge base
    Workflow,       // Workflow definition
    Governance,     // Governance policies
    Bundle,         // Bundle of multiple packs
};

inline std::string packTypeToString(PackType t) {
    switch (t) {
        case PackType::Model: return "model";
        case PackType::Pipeline: return "pipeline";
        case PackType::Service: return "service";
        case PackType::Runtime: return "runtime";
        case PackType::Agent: return "agent";
        case PackType::AgentTool: return "agent_tool";
        case PackType::Skill: return "skill";
        case PackType::Knowledge: return "knowledge";
        case PackType::Workflow: return "workflow";
        case PackType::Governance: return "governance";
        case PackType::Bundle: return "bundle";
    }
    return "unknown";
}

inline PackType packTypeFromString(const std::string& s) {
    if (s == "model") return PackType::Model;
    if (s == "pipeline") return PackType::Pipeline;
    if (s == "service") return PackType::Service;
    if (s == "runtime") return PackType::Runtime;
    if (s == "agent") return PackType::Agent;
    if (s == "agent_tool") return PackType::AgentTool;
    if (s == "skill") return PackType::Skill;
    if (s == "knowledge") return PackType::Knowledge;
    if (s == "workflow") return PackType::Workflow;
    if (s == "governance") return PackType::Governance;
    if (s == "bundle") return PackType::Bundle;
    return PackType::Service;
}

struct PlatformRequirements {
    std::string arch;           // "arm64", "armv7", "x86_64", "any"
    std::string os;             // "linux", "android", "rtos", "any"
    uint32_t minMemoryMB = 0;   // Minimum RAM required
    uint32_t minStorageMB = 0;  // Minimum storage required
    bool requiresGPU = false;
    bool requiresNPU = false;   // Neural Processing Unit
    bool requiresDSP = false;   // Digital Signal Processor
    std::vector<std::string> requiredFeatures;  // e.g., "neon", "avx2"
};

// =============================================================================
// Lifecycle & Event Types
// =============================================================================
enum class PackState : uint8_t {
    Unknown,
    Discovered,
    Validated,
    Installed,
    Loaded,
    Initialized,
    Running,
    Suspended,
    Stopped,
    Disabled,
    Uninstalled,
    Error,
};

inline std::string packStateToString(PackState s) {
    switch (s) {
        case PackState::Unknown: return "unknown";
        case PackState::Discovered: return "discovered";
        case PackState::Validated: return "validated";
        case PackState::Installed: return "installed";
        case PackState::Loaded: return "loaded";
        case PackState::Initialized: return "initialized";
        case PackState::Running: return "running";
        case PackState::Suspended: return "suspended";
        case PackState::Stopped: return "stopped";
        case PackState::Disabled: return "disabled";
        case PackState::Uninstalled: return "uninstalled";
        case PackState::Error: return "error";
    }
    return "unknown";
}

enum class EventType : uint16_t {
    PackDiscovered,
    PackInstalled,
    PackLoaded,
    PackUnloaded,
    PackUpdated,
    PackRemoved,
    PackEnabled,
    PackDisabled,
    PackError,
    ModelLoaded,
    ModelUnloaded,
    InferenceStarted,
    InferenceCompleted,
    InferenceFailed,
    AgentStarted,
    AgentStopped,
    WorkflowStarted,
    WorkflowCompleted,
    PolicyViolation,
    ResourceWarning,
    SystemShutdown,
};

struct Event {
    EventType type;
    std::string source;
    std::string message;
    Properties data;
    std::chrono::steady_clock::time_point timestamp =
        std::chrono::steady_clock::now();
};

using EventHandler = std::function<void(const Event&)>;

// =============================================================================
// Resource Tracking
// =============================================================================
struct ResourceUsage {
    size_t memoryBytes = 0;
    size_t peakMemoryBytes = 0;
    double cpuPercent = 0.0;
    size_t storageBytes = 0;
    uint32_t activeInferences = 0;
    std::chrono::steady_clock::time_point lastActivity;
};

} // namespace aipack
