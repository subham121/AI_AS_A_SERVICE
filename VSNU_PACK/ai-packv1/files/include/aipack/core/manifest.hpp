// =============================================================================
// AI Packs System - Pack Manifest (Metadata)
// Defines the complete metadata schema for an AI Pack
// =============================================================================
#pragma once

#include "types.hpp"
#include "json.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

namespace aipack {

// =============================================================================
// Dependency specification
// =============================================================================
struct Dependency {
    std::string packId;
    std::string versionRange;   // e.g., ">=1.0.0", "^2.1", "1.0.0-2.0.0"
    bool optional = false;

    bool satisfiedBy(const Version& v) const {
        if (versionRange.empty()) return true;

        if (versionRange[0] == '^') {
            // Compatible with: same major
            Version min = Version::parse(versionRange.substr(1));
            return v.major == min.major && v >= min;
        }
        if (versionRange[0] == '~') {
            // Approximately: same major.minor
            Version min = Version::parse(versionRange.substr(1));
            return v.major == min.major && v.minor == min.minor && v >= min;
        }
        if (versionRange.substr(0, 2) == ">=") {
            return v >= Version::parse(versionRange.substr(2));
        }
        if (versionRange.substr(0, 2) == "<=") {
            return v <= Version::parse(versionRange.substr(2));
        }
        if (versionRange.substr(0, 1) == ">") {
            return v > Version::parse(versionRange.substr(1));
        }
        if (versionRange.substr(0, 1) == "<") {
            return v < Version::parse(versionRange.substr(1));
        }
        // Exact match
        return v == Version::parse(versionRange);
    }
};

// =============================================================================
// Model metadata within a pack
// =============================================================================
struct ModelInfo {
    std::string name;
    std::string format;         // "onnx", "tflite", "custom"
    std::string path;           // Relative path within pack
    std::string quantization;   // "none", "int8", "fp16"
    uint64_t sizeBytes = 0;
    std::string checksum;       // SHA256 hash for integrity

    struct IOSpec {
        std::string name;
        DataType dtype = DataType::Float32;
        std::vector<int64_t> shape;     // -1 for dynamic dims
    };
    std::vector<IOSpec> inputs;
    std::vector<IOSpec> outputs;
};

// =============================================================================
// Pipeline stage definition
// =============================================================================
struct PipelineStage {
    std::string name;
    std::string type;           // "preprocess", "inference", "postprocess", "custom"
    std::string implementation; // Class or function name
    Properties config;
    std::vector<std::string> inputsFrom;  // Names of stages providing input
};

// =============================================================================
// Service endpoint definition
// =============================================================================
struct ServiceEndpoint {
    std::string name;
    std::string interface;      // Interface class name
    std::string description;
    Properties config;
};

// =============================================================================
// Agent tool definition
// =============================================================================
struct AgentToolDef {
    std::string name;
    std::string description;
    std::string implementation;
    struct Parameter {
        std::string name;
        std::string type;       // "string", "int", "float", "bool", "tensor"
        std::string description;
        bool required = true;
        std::string defaultValue;
    };
    std::vector<Parameter> parameters;
    std::string returnType;
};

// =============================================================================
// Workflow definition
// =============================================================================
struct WorkflowStep {
    std::string id;
    std::string name;
    std::string type;           // "invoke", "condition", "parallel", "loop"
    std::string target;         // Pack/service to invoke
    Properties config;
    std::vector<std::string> next;      // Next step IDs
    std::string condition;              // For conditional steps
};

struct WorkflowDef {
    std::string name;
    std::string description;
    std::string trigger;        // "manual", "event", "schedule"
    std::vector<WorkflowStep> steps;
};

// =============================================================================
// Governance policy
// =============================================================================
struct PolicyRule {
    std::string name;
    std::string type;       // "access", "resource", "safety", "audit", "rate_limit"
    std::string condition;  // Expression or rule
    std::string action;     // "allow", "deny", "log", "throttle"
    Properties parameters;
};

struct GovernancePolicy {
    std::string name;
    std::string description;
    std::string scope;      // "pack", "service", "model", "global"
    std::vector<PolicyRule> rules;
};

// =============================================================================
// Complete Pack Manifest
// =============================================================================
struct PackManifest {
    // --- Identity ---
    std::string id;             // Unique identifier, e.g., "com.acme.tts-english"
    std::string name;           // Human-readable name
    std::string description;
    Version version;
    std::string author;
    std::string license;
    std::string homepage;
    std::vector<std::string> tags;

    // --- Type & Classification ---
    PackType type = PackType::Service;
    std::string category;       // "speech", "vision", "nlp", "audio", etc.

    // --- Platform Requirements ---
    PlatformRequirements platform;

    // --- Dependencies ---
    std::vector<Dependency> dependencies;
    std::vector<std::string> conflicts;   // Pack IDs that conflict

    // --- Content ---
    std::vector<ModelInfo> models;
    std::vector<PipelineStage> pipeline;
    std::vector<ServiceEndpoint> services;

    // --- Plugin ---
    std::string libraryPath;    // Path to .so/.dll within pack
    std::string entryPoint;     // Symbol name for plugin factory

    // --- Agent Components ---
    std::vector<AgentToolDef> tools;
    std::vector<std::string> skills;        // Skill identifiers
    std::vector<std::string> knowledgeBases; // Knowledge base paths

    // --- Workflows ---
    std::vector<WorkflowDef> workflows;

    // --- Governance ---
    std::vector<GovernancePolicy> policies;

    // --- Configuration ---
    Properties defaultConfig;
    Properties metadata;        // Arbitrary extra metadata

    // --- Runtime Hints ---
    uint32_t maxConcurrentInferences = 1;
    uint32_t warmupTimeMs = 0;
    bool supportsHotReload = false;
    bool lazyLoad = true;       // Load models on first use
    uint32_t priority = 100;    // Lower = higher priority

    // ==========================================================================
    // Serialization / Deserialization
    // ==========================================================================

    json::Value toJson() const {
        json::Object root;
        root["id"] = json::Value(id);
        root["name"] = json::Value(name);
        root["description"] = json::Value(description);
        root["version"] = json::Value(version.toString());
        root["author"] = json::Value(author);
        root["license"] = json::Value(license);
        root["homepage"] = json::Value(homepage);
        root["type"] = json::Value(packTypeToString(type));
        root["category"] = json::Value(category);

        // Tags
        json::Array tagArr;
        for (auto& t : tags) tagArr.push_back(json::Value(t));
        root["tags"] = json::Value(std::move(tagArr));

        // Platform
        {
            json::Object plat;
            plat["arch"] = json::Value(platform.arch);
            plat["os"] = json::Value(platform.os);
            plat["min_memory_mb"] = json::Value(platform.minMemoryMB);
            plat["min_storage_mb"] = json::Value(platform.minStorageMB);
            plat["requires_gpu"] = json::Value(platform.requiresGPU);
            plat["requires_npu"] = json::Value(platform.requiresNPU);
            plat["requires_dsp"] = json::Value(platform.requiresDSP);
            json::Array feats;
            for (auto& f : platform.requiredFeatures)
                feats.push_back(json::Value(f));
            plat["required_features"] = json::Value(std::move(feats));
            root["platform"] = json::Value(std::move(plat));
        }

        // Dependencies
        {
            json::Array deps;
            for (auto& d : dependencies) {
                json::Object dep;
                dep["pack_id"] = json::Value(d.packId);
                dep["version"] = json::Value(d.versionRange);
                dep["optional"] = json::Value(d.optional);
                deps.push_back(json::Value(std::move(dep)));
            }
            root["dependencies"] = json::Value(std::move(deps));
        }

        // Models
        {
            json::Array mods;
            for (auto& m : models) {
                json::Object mod;
                mod["name"] = json::Value(m.name);
                mod["format"] = json::Value(m.format);
                mod["path"] = json::Value(m.path);
                mod["quantization"] = json::Value(m.quantization);
                mod["size_bytes"] = json::Value(static_cast<int64_t>(m.sizeBytes));
                mod["checksum"] = json::Value(m.checksum);
                mods.push_back(json::Value(std::move(mod)));
            }
            root["models"] = json::Value(std::move(mods));
        }

        // Services
        {
            json::Array svcs;
            for (auto& s : services) {
                json::Object svc;
                svc["name"] = json::Value(s.name);
                svc["interface"] = json::Value(s.interface);
                svc["description"] = json::Value(s.description);
                svcs.push_back(json::Value(std::move(svc)));
            }
            root["services"] = json::Value(std::move(svcs));
        }

        // Plugin
        root["library_path"] = json::Value(libraryPath);
        root["entry_point"] = json::Value(entryPoint);

        // Tools
        {
            json::Array toolArr;
            for (auto& t : tools) {
                json::Object tool;
                tool["name"] = json::Value(t.name);
                tool["description"] = json::Value(t.description);
                tool["implementation"] = json::Value(t.implementation);
                tool["return_type"] = json::Value(t.returnType);
                json::Array params;
                for (auto& p : t.parameters) {
                    json::Object param;
                    param["name"] = json::Value(p.name);
                    param["type"] = json::Value(p.type);
                    param["description"] = json::Value(p.description);
                    param["required"] = json::Value(p.required);
                    param["default"] = json::Value(p.defaultValue);
                    params.push_back(json::Value(std::move(param)));
                }
                tool["parameters"] = json::Value(std::move(params));
                toolArr.push_back(json::Value(std::move(tool)));
            }
            root["tools"] = json::Value(std::move(toolArr));
        }

        // Workflows
        {
            json::Array wfArr;
            for (auto& wf : workflows) {
                json::Object wfObj;
                wfObj["name"] = json::Value(wf.name);
                wfObj["description"] = json::Value(wf.description);
                wfObj["trigger"] = json::Value(wf.trigger);
                json::Array steps;
                for (auto& s : wf.steps) {
                    json::Object step;
                    step["id"] = json::Value(s.id);
                    step["name"] = json::Value(s.name);
                    step["type"] = json::Value(s.type);
                    step["target"] = json::Value(s.target);
                    step["condition"] = json::Value(s.condition);
                    json::Array nextArr;
                    for (auto& n : s.next)
                        nextArr.push_back(json::Value(n));
                    step["next"] = json::Value(std::move(nextArr));
                    steps.push_back(json::Value(std::move(step)));
                }
                wfObj["steps"] = json::Value(std::move(steps));
                wfArr.push_back(json::Value(std::move(wfObj)));
            }
            root["workflows"] = json::Value(std::move(wfArr));
        }

        // Policies
        {
            json::Array polArr;
            for (auto& pol : policies) {
                json::Object polObj;
                polObj["name"] = json::Value(pol.name);
                polObj["description"] = json::Value(pol.description);
                polObj["scope"] = json::Value(pol.scope);
                json::Array rules;
                for (auto& r : pol.rules) {
                    json::Object rule;
                    rule["name"] = json::Value(r.name);
                    rule["type"] = json::Value(r.type);
                    rule["condition"] = json::Value(r.condition);
                    rule["action"] = json::Value(r.action);
                    rules.push_back(json::Value(std::move(rule)));
                }
                polObj["rules"] = json::Value(std::move(rules));
                polArr.push_back(json::Value(std::move(polObj)));
            }
            root["policies"] = json::Value(std::move(polArr));
        }

        // Runtime hints
        {
            json::Object hints;
            hints["max_concurrent_inferences"] = json::Value(
                static_cast<int64_t>(maxConcurrentInferences));
            hints["warmup_time_ms"] = json::Value(
                static_cast<int64_t>(warmupTimeMs));
            hints["supports_hot_reload"] = json::Value(supportsHotReload);
            hints["lazy_load"] = json::Value(lazyLoad);
            hints["priority"] = json::Value(static_cast<int64_t>(priority));
            root["runtime_hints"] = json::Value(std::move(hints));
        }

        // Default config
        {
            json::Object cfg;
            for (auto& [k, v] : defaultConfig) {
                cfg[k] = json::Value(v);
            }
            root["default_config"] = json::Value(std::move(cfg));
        }

        return json::Value(std::move(root));
    }

    static PackManifest fromJson(const json::Value& j) {
        PackManifest m;
        m.id = j.getString("id");
        m.name = j.getString("name");
        m.description = j.getString("description");
        m.version = Version::parse(j.getString("version"));
        m.author = j.getString("author");
        m.license = j.getString("license");
        m.homepage = j.getString("homepage");
        m.type = packTypeFromString(j.getString("type", "service"));
        m.category = j.getString("category");
        m.tags = j.getStringArray("tags");

        // Platform
        if (j.has("platform")) {
            auto& p = j["platform"];
            m.platform.arch = p.getString("arch", "any");
            m.platform.os = p.getString("os", "any");
            m.platform.minMemoryMB = static_cast<uint32_t>(
                p.getInt("min_memory_mb"));
            m.platform.minStorageMB = static_cast<uint32_t>(
                p.getInt("min_storage_mb"));
            m.platform.requiresGPU = p.getBool("requires_gpu");
            m.platform.requiresNPU = p.getBool("requires_npu");
            m.platform.requiresDSP = p.getBool("requires_dsp");
            m.platform.requiredFeatures = p.getStringArray("required_features");
        }

        // Dependencies
        if (j.has("dependencies") && j["dependencies"].isArray()) {
            for (auto& d : j["dependencies"].asArray()) {
                Dependency dep;
                dep.packId = d.getString("pack_id");
                dep.versionRange = d.getString("version");
                dep.optional = d.getBool("optional");
                m.dependencies.push_back(std::move(dep));
            }
        }

        // Models
        if (j.has("models") && j["models"].isArray()) {
            for (auto& mi : j["models"].asArray()) {
                ModelInfo model;
                model.name = mi.getString("name");
                model.format = mi.getString("format");
                model.path = mi.getString("path");
                model.quantization = mi.getString("quantization", "none");
                model.sizeBytes = static_cast<uint64_t>(mi.getInt("size_bytes"));
                model.checksum = mi.getString("checksum");
                m.models.push_back(std::move(model));
            }
        }

        // Services
        if (j.has("services") && j["services"].isArray()) {
            for (auto& si : j["services"].asArray()) {
                ServiceEndpoint svc;
                svc.name = si.getString("name");
                svc.interface = si.getString("interface");
                svc.description = si.getString("description");
                m.services.push_back(std::move(svc));
            }
        }

        // Plugin
        m.libraryPath = j.getString("library_path");
        m.entryPoint = j.getString("entry_point", "aipack_create_plugin");

        // Tools
        if (j.has("tools") && j["tools"].isArray()) {
            for (auto& ti : j["tools"].asArray()) {
                AgentToolDef tool;
                tool.name = ti.getString("name");
                tool.description = ti.getString("description");
                tool.implementation = ti.getString("implementation");
                tool.returnType = ti.getString("return_type");
                if (ti.has("parameters") && ti["parameters"].isArray()) {
                    for (auto& pi : ti["parameters"].asArray()) {
                        AgentToolDef::Parameter param;
                        param.name = pi.getString("name");
                        param.type = pi.getString("type");
                        param.description = pi.getString("description");
                        param.required = pi.getBool("required", true);
                        param.defaultValue = pi.getString("default");
                        tool.parameters.push_back(std::move(param));
                    }
                }
                m.tools.push_back(std::move(tool));
            }
        }

        // Workflows
        if (j.has("workflows") && j["workflows"].isArray()) {
            for (auto& wi : j["workflows"].asArray()) {
                WorkflowDef wf;
                wf.name = wi.getString("name");
                wf.description = wi.getString("description");
                wf.trigger = wi.getString("trigger", "manual");
                if (wi.has("steps") && wi["steps"].isArray()) {
                    for (auto& si : wi["steps"].asArray()) {
                        WorkflowStep step;
                        step.id = si.getString("id");
                        step.name = si.getString("name");
                        step.type = si.getString("type", "invoke");
                        step.target = si.getString("target");
                        step.condition = si.getString("condition");
                        step.next = si.getStringArray("next");
                        wf.steps.push_back(std::move(step));
                    }
                }
                m.workflows.push_back(std::move(wf));
            }
        }

        // Policies
        if (j.has("policies") && j["policies"].isArray()) {
            for (auto& pi : j["policies"].asArray()) {
                GovernancePolicy pol;
                pol.name = pi.getString("name");
                pol.description = pi.getString("description");
                pol.scope = pi.getString("scope", "pack");
                if (pi.has("rules") && pi["rules"].isArray()) {
                    for (auto& ri : pi["rules"].asArray()) {
                        PolicyRule rule;
                        rule.name = ri.getString("name");
                        rule.type = ri.getString("type");
                        rule.condition = ri.getString("condition");
                        rule.action = ri.getString("action");
                        pol.rules.push_back(std::move(rule));
                    }
                }
                m.policies.push_back(std::move(pol));
            }
        }

        // Runtime hints
        if (j.has("runtime_hints")) {
            auto& h = j["runtime_hints"];
            m.maxConcurrentInferences = static_cast<uint32_t>(
                h.getInt("max_concurrent_inferences", 1));
            m.warmupTimeMs = static_cast<uint32_t>(
                h.getInt("warmup_time_ms"));
            m.supportsHotReload = h.getBool("supports_hot_reload");
            m.lazyLoad = h.getBool("lazy_load", true);
            m.priority = static_cast<uint32_t>(h.getInt("priority", 100));
        }

        // Default config
        if (j.has("default_config") && j["default_config"].isObject()) {
            for (auto& [k, v] : j["default_config"].asObject()) {
                m.defaultConfig[k] = v.asString();
            }
        }

        return m;
    }

    // Load from file
    static Result<PackManifest> loadFromFile(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            return Result<PackManifest>::failure(
                ErrorCode::IOError,
                "Cannot open manifest file: " + path);
        }
        std::ostringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();

        try {
            auto j = json::parse(content);
            auto manifest = fromJson(j);
            if (manifest.id.empty()) {
                return Result<PackManifest>::failure(
                    ErrorCode::PackInvalidManifest,
                    "Manifest missing required 'id' field");
            }
            return Result<PackManifest>::success(std::move(manifest));
        } catch (const std::exception& e) {
            return Result<PackManifest>::failure(
                ErrorCode::PackInvalidManifest,
                std::string("Failed to parse manifest: ") + e.what());
        }
    }

    // Save to file
    Error saveToFile(const std::string& path) const {
        std::ofstream file(path);
        if (!file.is_open()) {
            return Error::make(ErrorCode::IOError,
                "Cannot write manifest file: " + path);
        }
        file << toJson().dump(2);
        return Error::success();
    }
};

} // namespace aipack
