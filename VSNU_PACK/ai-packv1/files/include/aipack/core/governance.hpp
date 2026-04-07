// =============================================================================
// AI Packs System - Governance & Policy Engine
// Access control, resource limits, audit logging, and safety guardrails
// =============================================================================
#pragma once

#include "types.hpp"
#include "manifest.hpp"
#include "logger.hpp"
#include "event_bus.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <functional>
#include <chrono>
#include <deque>
#include <algorithm>

namespace aipack {

static const char* TAG_GOVERN = "Governance";

// =============================================================================
// Audit Log Entry
// =============================================================================
struct AuditEntry {
    std::string timestamp;
    std::string actor;          // Who performed the action
    std::string action;         // What was done
    std::string resource;       // What was affected
    std::string result;         // "allowed", "denied", "error"
    Properties details;
};

// =============================================================================
// Access Control
// =============================================================================
class AccessController {
public:
    /// Define a role
    void defineRole(const std::string& role,
                    const std::vector<std::string>& permissions) {
        std::lock_guard<std::mutex> lock(mutex_);
        roles_[role] = {permissions.begin(), permissions.end()};
    }

    /// Grant a role to an identity
    void grantRole(const std::string& identity, const std::string& role) {
        std::lock_guard<std::mutex> lock(mutex_);
        identityRoles_[identity].insert(role);
    }

    /// Revoke a role
    void revokeRole(const std::string& identity, const std::string& role) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = identityRoles_.find(identity);
        if (it != identityRoles_.end()) {
            it->second.erase(role);
        }
    }

    /// Check if identity has permission
    bool hasPermission(const std::string& identity,
                       const std::string& permission) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = identityRoles_.find(identity);
        if (it == identityRoles_.end()) return false;

        for (const auto& role : it->second) {
            auto roleIt = roles_.find(role);
            if (roleIt != roles_.end()) {
                if (roleIt->second.count(permission) > 0 ||
                    roleIt->second.count("*") > 0) {
                    return true;
                }
            }
        }
        return false;
    }

    /// Check pack-specific permission
    bool canAccessPack(const std::string& identity,
                       const std::string& packId,
                       const std::string& action) const {
        std::string perm = "pack." + packId + "." + action;
        std::string wildcardPerm = "pack.*." + action;
        return hasPermission(identity, perm) ||
               hasPermission(identity, wildcardPerm) ||
               hasPermission(identity, "*");
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_set<std::string>> roles_;
    std::unordered_map<std::string, std::unordered_set<std::string>> identityRoles_;
};

// =============================================================================
// Resource Limiter
// =============================================================================
class ResourceLimiter {
public:
    struct Limits {
        size_t maxMemoryBytes = 0;      // 0 = unlimited
        double maxCpuPercent = 0.0;     // 0 = unlimited
        uint32_t maxConcurrentOps = 0;  // 0 = unlimited
        uint32_t maxOpsPerMinute = 0;   // Rate limiting
        size_t maxStorageBytes = 0;
    };

    /// Set limits for a pack
    void setLimits(const std::string& packId, const Limits& limits) {
        std::lock_guard<std::mutex> lock(mutex_);
        limits_[packId] = limits;
    }

    /// Set global limits
    void setGlobalLimits(const Limits& limits) {
        std::lock_guard<std::mutex> lock(mutex_);
        globalLimits_ = limits;
    }

    /// Check if operation is within limits
    Error checkLimits(const std::string& packId,
                      const ResourceUsage& current) const {
        std::lock_guard<std::mutex> lock(mutex_);

        // Check pack-specific limits
        auto it = limits_.find(packId);
        if (it != limits_.end()) {
            auto err = checkAgainstLimits(it->second, current, packId);
            if (err) return err;
        }

        // Check global limits
        return checkAgainstLimits(globalLimits_, current, "global");
    }

    /// Record an operation for rate limiting
    void recordOperation(const std::string& packId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        auto& ops = operations_[packId];
        ops.push_back(now);

        // Clean old entries (older than 1 minute)
        auto cutoff = now - std::chrono::minutes(1);
        while (!ops.empty() && ops.front() < cutoff) {
            ops.pop_front();
        }
    }

    /// Check rate limit
    bool isRateLimited(const std::string& packId) const {
        std::lock_guard<std::mutex> lock(mutex_);

        auto limIt = limits_.find(packId);
        uint32_t maxOps = (limIt != limits_.end())
            ? limIt->second.maxOpsPerMinute
            : globalLimits_.maxOpsPerMinute;

        if (maxOps == 0) return false;

        auto opIt = operations_.find(packId);
        if (opIt == operations_.end()) return false;

        return opIt->second.size() >= maxOps;
    }

    Limits getLimits(const std::string& packId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = limits_.find(packId);
        return it != limits_.end() ? it->second : globalLimits_;
    }

private:
    Error checkAgainstLimits(const Limits& limits,
                             const ResourceUsage& current,
                             const std::string& context) const {
        if (limits.maxMemoryBytes > 0 &&
            current.memoryBytes > limits.maxMemoryBytes) {
            return Error::make(ErrorCode::ResourceLimitExceeded,
                "Memory limit exceeded for " + context,
                "Used: " + std::to_string(current.memoryBytes) +
                " Limit: " + std::to_string(limits.maxMemoryBytes));
        }
        if (limits.maxCpuPercent > 0.0 &&
            current.cpuPercent > limits.maxCpuPercent) {
            return Error::make(ErrorCode::ResourceLimitExceeded,
                "CPU limit exceeded for " + context);
        }
        if (limits.maxConcurrentOps > 0 &&
            current.activeInferences > limits.maxConcurrentOps) {
            return Error::make(ErrorCode::ResourceLimitExceeded,
                "Concurrent operation limit exceeded for " + context);
        }
        return Error::success();
    }

    mutable std::mutex mutex_;
    Limits globalLimits_;
    std::unordered_map<std::string, Limits> limits_;
    mutable std::unordered_map<std::string,
        std::deque<std::chrono::steady_clock::time_point>> operations_;
};

// =============================================================================
// Safety Guardrails
// =============================================================================
class SafetyGuardrails {
public:
    using GuardrailCheck = std::function<Error(const std::string& input,
                                                const Properties& context)>;

    /// Register a safety check
    void registerCheck(const std::string& name, GuardrailCheck check) {
        std::lock_guard<std::mutex> lock(mutex_);
        checks_[name] = std::move(check);
    }

    /// Remove a safety check
    void removeCheck(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        checks_.erase(name);
    }

    /// Run all safety checks on input
    Error validate(const std::string& input,
                   const Properties& context = {}) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, check] : checks_) {
            auto err = check(input, context);
            if (err) {
                AIPACK_WARN(TAG_GOVERN,
                    "Safety guardrail '%s' triggered: %s",
                    name.c_str(), err.message.c_str());
                EventBus::instance().publish(
                    EventType::PolicyViolation,
                    "SafetyGuardrails",
                    "Guardrail triggered: " + name,
                    {{"guardrail", name}, {"message", err.message}});
                return err;
            }
        }
        return Error::success();
    }

    /// Register default guardrails
    void registerDefaults() {
        // Content length check
        registerCheck("max_content_length", [](const std::string& input,
                                                const Properties&) -> Error {
            if (input.size() > 1024 * 1024) {  // 1MB
                return Error::make(ErrorCode::SafetyGuardrailTriggered,
                    "Input exceeds maximum content length (1MB)");
            }
            return Error::success();
        });

        // Empty input check
        registerCheck("non_empty_input", [](const std::string& input,
                                             const Properties&) -> Error {
            if (input.empty()) {
                return Error::make(ErrorCode::SafetyGuardrailTriggered,
                    "Input cannot be empty");
            }
            return Error::success();
        });
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, GuardrailCheck> checks_;
};

// =============================================================================
// Audit Logger
// =============================================================================
class AuditLogger {
public:
    AuditLogger(size_t maxEntries = 10000) : maxEntries_(maxEntries) {}

    /// Log an audit entry
    void log(const std::string& actor, const std::string& action,
             const std::string& resource, const std::string& result,
             Properties details = {}) {
        std::lock_guard<std::mutex> lock(mutex_);

        AuditEntry entry;
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::gmtime(&time_t));
        entry.timestamp = buf;
        entry.actor = actor;
        entry.action = action;
        entry.resource = resource;
        entry.result = result;
        entry.details = std::move(details);

        entries_.push_back(std::move(entry));

        // Trim old entries
        while (entries_.size() > maxEntries_) {
            entries_.pop_front();
        }

        AIPACK_TRACE(TAG_GOVERN,
            "AUDIT: %s %s %s -> %s",
            actor.c_str(), action.c_str(), resource.c_str(), result.c_str());
    }

    /// Get recent entries
    std::vector<AuditEntry> getRecent(size_t count = 100) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<AuditEntry> result;
        size_t start = entries_.size() > count ? entries_.size() - count : 0;
        for (size_t i = start; i < entries_.size(); ++i) {
            result.push_back(entries_[i]);
        }
        return result;
    }

    /// Get entries for a specific actor
    std::vector<AuditEntry> getByActor(const std::string& actor) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<AuditEntry> result;
        for (auto& e : entries_) {
            if (e.actor == actor) result.push_back(e);
        }
        return result;
    }

    /// Get entries for a specific resource
    std::vector<AuditEntry> getByResource(const std::string& resource) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<AuditEntry> result;
        for (auto& e : entries_) {
            if (e.resource == resource) result.push_back(e);
        }
        return result;
    }

    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

private:
    mutable std::mutex mutex_;
    size_t maxEntries_;
    std::deque<AuditEntry> entries_;
};

// =============================================================================
// Policy Engine - Combines all governance components
// =============================================================================
class PolicyEngine {
public:
    PolicyEngine()
        : auditLogger_(10000) {
        guardrails_.registerDefaults();
        setupDefaultRoles();
    }

    AccessController& accessControl() { return accessController_; }
    const AccessController& accessControl() const { return accessController_; }

    ResourceLimiter& resourceLimiter() { return resourceLimiter_; }
    const ResourceLimiter& resourceLimiter() const { return resourceLimiter_; }

    SafetyGuardrails& guardrails() { return guardrails_; }
    const SafetyGuardrails& guardrails() const { return guardrails_; }

    AuditLogger& auditLogger() { return auditLogger_; }
    const AuditLogger& auditLogger() const { return auditLogger_; }

    /// Apply policies from a pack manifest
    void applyPolicies(const PackManifest& manifest) {
        for (auto& policy : manifest.policies) {
            for (auto& rule : policy.rules) {
                applyRule(manifest.id, policy.scope, rule);
            }
        }
        AIPACK_INFO(TAG_GOVERN,
            "Applied %zu policies from pack '%s'",
            manifest.policies.size(), manifest.id.c_str());
    }

    /// Full authorization check for a pack operation
    Error authorize(const std::string& identity,
                    const std::string& packId,
                    const std::string& action,
                    const std::string& input = "",
                    const ResourceUsage& resources = {}) {
        // 1. Access control
        if (!accessController_.canAccessPack(identity, packId, action)) {
            auditLogger_.log(identity, action, packId, "denied",
                {{"reason", "access_control"}});
            return Error::make(ErrorCode::AccessDenied,
                "Access denied for " + identity + " to " + packId + "." + action);
        }

        // 2. Resource limits
        auto resErr = resourceLimiter_.checkLimits(packId, resources);
        if (resErr) {
            auditLogger_.log(identity, action, packId, "denied",
                {{"reason", "resource_limit"}, {"details", resErr.message}});
            return resErr;
        }

        // 3. Rate limiting
        if (resourceLimiter_.isRateLimited(packId)) {
            auditLogger_.log(identity, action, packId, "denied",
                {{"reason", "rate_limited"}});
            return Error::make(ErrorCode::ResourceLimitExceeded,
                "Rate limit exceeded for " + packId);
        }

        // 4. Safety guardrails (for input-bearing operations)
        if (!input.empty()) {
            auto safetyErr = guardrails_.validate(input,
                {{"pack", packId}, {"action", action}});
            if (safetyErr) {
                auditLogger_.log(identity, action, packId, "denied",
                    {{"reason", "safety_guardrail"},
                     {"details", safetyErr.message}});
                return safetyErr;
            }
        }

        // Record operation for rate limiting
        resourceLimiter_.recordOperation(packId);

        // Audit log success
        auditLogger_.log(identity, action, packId, "allowed");
        return Error::success();
    }

private:
    void setupDefaultRoles() {
        // Admin role - full access
        accessController_.defineRole("admin", {"*"});


        // User role - basic pack usage
        accessController_.defineRole("user", {
            "pack.*.use", "pack.*.query", "pack.*.list"
        });

        // Developer role - install and configure packs
        accessController_.defineRole("developer", {
            "pack.*.use", "pack.*.query", "pack.*.list",
            "pack.*.install", "pack.*.configure", "pack.*.reload"
        });

          accessController_.defineRole("aipack_cli", {
            "pack.*.use", "pack.*.query", "pack.*.list",
            "pack.*.install","pack.*.uninstall", "pack.*.configure", "pack.*.reload"
        });
        // Service role - for inter-pack communication
        accessController_.defineRole("service", {
            "pack.*.use", "pack.*.query"
        });
    }

    void applyRule(const std::string& packId, const std::string& scope,
                   const PolicyRule& rule) {
        if (rule.type == "resource") {
            ResourceLimiter::Limits limits;
            auto it = rule.parameters.find("max_memory_mb");
            if (it != rule.parameters.end()) {
                limits.maxMemoryBytes =
                    static_cast<size_t>(std::stol(it->second)) * 1024 * 1024;
            }
            it = rule.parameters.find("max_ops_per_minute");
            if (it != rule.parameters.end()) {
                limits.maxOpsPerMinute =
                    static_cast<uint32_t>(std::stol(it->second));
            }
            it = rule.parameters.find("max_concurrent");
            if (it != rule.parameters.end()) {
                limits.maxConcurrentOps =
                    static_cast<uint32_t>(std::stol(it->second));
            }
            resourceLimiter_.setLimits(packId, limits);
        }
    }

    AccessController accessController_;
    ResourceLimiter resourceLimiter_;
    SafetyGuardrails guardrails_;
    AuditLogger auditLogger_;
};

} // namespace aipack
