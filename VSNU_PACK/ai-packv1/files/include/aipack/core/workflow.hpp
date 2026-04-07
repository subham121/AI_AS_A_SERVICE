// =============================================================================
// AI Packs System - Workflow Engine
// Executes multi-step workflows with branching, parallelism, and loops
// =============================================================================
#pragma once

#include "types.hpp"
#include "manifest.hpp"
#include "logger.hpp"
#include "event_bus.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <functional>
#include <queue>

namespace aipack {

static const char* TAG_WORKFLOW = "Workflow";

// =============================================================================
// Workflow Execution Context
// =============================================================================
struct WorkflowContext {
    std::string workflowName;
    std::string currentStepId;
    Properties variables;       // Shared variables across steps
    Properties inputs;          // Workflow inputs
    Properties outputs;         // Workflow outputs
    std::vector<std::string> executedSteps;
    bool cancelled = false;
    bool failed = false;
    std::string failureReason;
};

// =============================================================================
// Step Handler
// =============================================================================
using StepHandler = std::function<Error(WorkflowContext&,
                                         const WorkflowStep&)>;

// =============================================================================
// Workflow Engine
// =============================================================================
class WorkflowEngine {
public:
    /// Register a step handler for a step type
    void registerHandler(const std::string& type, StepHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers_[type] = std::move(handler);
    }

    /// Register a service invoker (called when step type is "invoke")
    void setServiceInvoker(
            std::function<Error(const std::string& target,
                                const Properties& config,
                                WorkflowContext& ctx)> invoker) {
        serviceInvoker_ = std::move(invoker);
    }

    /// Register a workflow definition
    void registerWorkflow(const WorkflowDef& workflow) {
        std::lock_guard<std::mutex> lock(mutex_);
        workflows_[workflow.name] = workflow;
        AIPACK_INFO(TAG_WORKFLOW, "Registered workflow: %s (%zu steps)",
            workflow.name.c_str(), workflow.steps.size());
    }

    /// Execute a workflow
    Result<Properties> execute(const std::string& workflowName,
                                const Properties& inputs = {}) {
        const WorkflowDef* wf = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = workflows_.find(workflowName);
            if (it == workflows_.end()) {
                return Result<Properties>::failure(
                    ErrorCode::AgentWorkflowFailed,
                    "Workflow not found: " + workflowName);
            }
            wf = &it->second;
        }

        WorkflowContext ctx;
        ctx.workflowName = workflowName;
        ctx.inputs = inputs;
        ctx.variables = inputs;  // Initialize variables with inputs

        AIPACK_INFO(TAG_WORKFLOW, "Starting workflow: %s", workflowName.c_str());
        EventBus::instance().publish(EventType::WorkflowStarted,
            "WorkflowEngine",
            "Starting workflow: " + workflowName);

        // Build step index
        std::unordered_map<std::string, const WorkflowStep*> stepIndex;
        for (auto& step : wf->steps) {
            stepIndex[step.id] = &step;
        }

        // Execute from first step
        if (wf->steps.empty()) {
            return Result<Properties>::success(ctx.outputs);
        }

        // BFS-like execution following step.next
        std::queue<std::string> pending;
        pending.push(wf->steps[0].id);

        while (!pending.empty() && !ctx.cancelled && !ctx.failed) {
            std::string stepId = pending.front();
            pending.pop();

            auto stepIt = stepIndex.find(stepId);
            if (stepIt == stepIndex.end()) {
                ctx.failed = true;
                ctx.failureReason = "Step not found: " + stepId;
                break;
            }

            const WorkflowStep& step = *stepIt->second;
            ctx.currentStepId = stepId;

            AIPACK_DEBUG(TAG_WORKFLOW, "Executing step: %s (%s)",
                step.name.c_str(), step.type.c_str());

            Error err = executeStep(ctx, step);
            if (err) {
                ctx.failed = true;
                ctx.failureReason = err.message;
                AIPACK_ERROR(TAG_WORKFLOW, "Step '%s' failed: %s",
                    step.name.c_str(), err.message.c_str());
                break;
            }

            ctx.executedSteps.push_back(stepId);

            // Enqueue next steps
            for (auto& nextId : step.next) {
                pending.push(nextId);
            }
        }

        if (ctx.failed) {
            AIPACK_ERROR(TAG_WORKFLOW, "Workflow '%s' failed: %s",
                workflowName.c_str(), ctx.failureReason.c_str());
            return Result<Properties>::failure(
                ErrorCode::AgentWorkflowFailed,
                "Workflow failed: " + ctx.failureReason);
        }

        AIPACK_INFO(TAG_WORKFLOW, "Workflow '%s' completed (%zu steps executed)",
            workflowName.c_str(), ctx.executedSteps.size());
        EventBus::instance().publish(EventType::WorkflowCompleted,
            "WorkflowEngine",
            "Workflow completed: " + workflowName);

        return Result<Properties>::success(std::move(ctx.outputs));
    }

    /// List registered workflows
    std::vector<std::string> listWorkflows() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> names;
        for (auto& [name, _] : workflows_) {
            names.push_back(name);
        }
        return names;
    }

private:
    Error executeStep(WorkflowContext& ctx, const WorkflowStep& step) {
        // Check condition
        if (!step.condition.empty()) {
            if (!evaluateCondition(step.condition, ctx)) {
                AIPACK_DEBUG(TAG_WORKFLOW,
                    "Step '%s' condition not met, skipping",
                    step.name.c_str());
                return Error::success();
            }
        }

        if (step.type == "invoke") {
            return executeInvoke(ctx, step);
        } else if (step.type == "condition") {
            return executeCondition(ctx, step);
        } else if (step.type == "set_variable") {
            return executeSetVariable(ctx, step);
        } else {
            // Check registered handlers
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = handlers_.find(step.type);
            if (it != handlers_.end()) {
                return it->second(ctx, step);
            }
            return Error::make(ErrorCode::NotImplemented,
                "Unknown step type: " + step.type);
        }
    }

    Error executeInvoke(WorkflowContext& ctx, const WorkflowStep& step) {
        if (serviceInvoker_) {
            return serviceInvoker_(step.target, step.config, ctx);
        }
        AIPACK_WARN(TAG_WORKFLOW,
            "No service invoker set, skipping invoke: %s",
            step.target.c_str());
        return Error::success();
    }

    Error executeCondition(WorkflowContext& ctx, const WorkflowStep& step) {
        // Simple condition evaluation
        bool result = evaluateCondition(step.condition, ctx);
        ctx.variables["_condition_result"] = result ? "true" : "false";
        return Error::success();
    }

    Error executeSetVariable(WorkflowContext& ctx, const WorkflowStep& step) {
        for (auto& [k, v] : step.config) {
            ctx.variables[k] = v;
        }
        return Error::success();
    }

    bool evaluateCondition(const std::string& condition,
                           const WorkflowContext& ctx) {
        // Simple condition: check if variable exists and is "true"
        // Format: "variable_name == value" or just "variable_name"
        auto eqPos = condition.find("==");
        if (eqPos != std::string::npos) {
            std::string varName = condition.substr(0, eqPos);
            std::string value = condition.substr(eqPos + 2);
            // Trim
            while (!varName.empty() && varName.back() == ' ') varName.pop_back();
            while (!value.empty() && value.front() == ' ') value = value.substr(1);

            auto it = ctx.variables.find(varName);
            if (it != ctx.variables.end()) {
                return it->second == value;
            }
            return false;
        }

        // Just check if variable exists and is truthy
        auto it = ctx.variables.find(condition);
        return it != ctx.variables.end() &&
               !it->second.empty() &&
               it->second != "false" &&
               it->second != "0";
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, WorkflowDef> workflows_;
    std::unordered_map<std::string, StepHandler> handlers_;
    std::function<Error(const std::string&, const Properties&,
                         WorkflowContext&)> serviceInvoker_;
};

} // namespace aipack
