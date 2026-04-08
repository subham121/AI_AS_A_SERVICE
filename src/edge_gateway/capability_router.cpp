#include <edgeai/capability_router.h>
#include <edgeai/json_utils.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace edgeai {

namespace {

Json::Value parseJsonObjectOrEmpty(const std::string& text) {
    if (text.empty()) {
        return Json::Value(Json::objectValue);
    }
    try {
        Json::Value value = parseJson(text);
        return value.isObject() ? value : Json::Value(Json::objectValue);
    } catch (const std::exception&) {
        return Json::Value(Json::objectValue);
    }
}

std::string normalizeText(const std::string& text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (char ch : text) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        } else {
            normalized.push_back(' ');
        }
    }

    std::string collapsed;
    collapsed.reserve(normalized.size());
    bool previous_space = true;
    for (char ch : normalized) {
        if (ch == ' ') {
            if (!previous_space) {
                collapsed.push_back(ch);
            }
            previous_space = true;
        } else {
            collapsed.push_back(ch);
            previous_space = false;
        }
    }
    while (!collapsed.empty() && collapsed.front() == ' ') {
        collapsed.erase(collapsed.begin());
    }
    while (!collapsed.empty() && collapsed.back() == ' ') {
        collapsed.pop_back();
    }
    return collapsed;
}

std::vector<std::string> splitTerms(const std::string& normalized) {
    std::vector<std::string> terms;
    std::string current;
    for (char ch : normalized) {
        if (ch == ' ') {
            if (!current.empty()) {
                terms.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        terms.push_back(current);
    }
    return terms;
}

bool containsTerm(const std::vector<std::string>& haystack, const std::string& needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

int intersectionScore(const std::vector<std::string>& left, const std::vector<std::string>& right) {
    int matches = 0;
    for (const auto& term : left) {
        if (containsTerm(right, term)) {
            ++matches;
        }
    }
    return matches;
}

void logRouter(const std::string& message) {
    std::cerr << "[CapabilityRouter] " << message << std::endl;
}

}  // namespace

std::string IntentManager::identifyIntent(const std::string& input) const {
    const Json::Value payload = parseJsonObjectOrEmpty(input);
    for (const char* field : {"skill", "intent", "input", "query"}) {
        if (payload.isMember(field) && payload[field].isString() && !payload[field].asString().empty()) {
            return payload[field].asString();
        }
    }
    if (input.empty()) {
        throw std::runtime_error("Input is empty");
    }
    return input;
}

DeviceCapabilityProvider::DeviceCapabilityProvider(Json::Value default_device_capability)
    : default_device_capability_(std::move(default_device_capability)) {}

Json::Value DeviceCapabilityProvider::getDeviceCapability(const Json::Value& override_capability) const {
    if (override_capability.isObject() && !override_capability.empty()) {
        return override_capability;
    }
    return default_device_capability_;
}

CapabilityRouter::CapabilityRouter(PackManager& manager,
                                   std::string catalog_url,
                                   Json::Value default_device_capability)
    : manager_(manager),
      catalog_url_(std::move(catalog_url)),
      capability_provider_(std::move(default_device_capability)) {}

Json::Value CapabilityRouter::routeUserRequest(const std::string& user_id,
                                               const std::string& input,
                                               const Json::Value& device_capability_override) {
    logRouter("routeUserRequest user_id=" + user_id + " input=" + input);
    const std::string skill = intent_manager_.identifyIntent(input);
    Json::Value capability_response;
    try {
        capability_response = http_client_.getJson(catalog_url_ + "/getCapabilityList");
        manager_.cacheCapabilityList(capability_response);
    } catch (const std::exception& ex) {
        logRouter(std::string("Falling back to cached capability list after fetch failure: ") + ex.what());
        capability_response = manager_.getCapabilityList();
    }
    const Json::Value capability_list = capability_response.get("capabilities", Json::Value(Json::arrayValue));
    const std::string capability = normalizeCapability(skill, capability_list);
    logRouter("Resolved skill '" + skill + "' to capability '" + capability + "'");

    Json::Value response(Json::objectValue);
    response["status"] = "ok";
    response["skill"] = skill;
    response["capability"] = capability;

    const Json::Value local_packs = manager_.getLocalPacks(capability);
    response["local_packs"] = local_packs["packs"];
    response["local_count"] = local_packs.get("count", 0);
    if (local_packs.get("count", 0).asUInt() > 0U) {
        const Json::Value selected = local_packs["packs"][0];
        const std::string pack_id = selected.get("pack_id", "").asString();
        logRouter("Using local pack candidate pack_id=" + pack_id);
        response["source"] = "local";
        response["pack_id"] = pack_id;
        response["pack"] = selected;
        response["prepare_result"] = manager_.preparePackForUse(user_id, pack_id, true);
        return response;
    }

    const Json::Value device_capability = capability_provider_.getDeviceCapability(device_capability_override);
    response["device_capability"] = device_capability;
    const Json::Value cloud_response = queryCompatiblePacks(capability, device_capability);
    response["source"] = "cloud";
    response["packs"] = cloud_response.get("packs", Json::Value(Json::arrayValue));
    response["count"] = cloud_response.get("count", response["packs"].size());
    if (response["count"].asUInt() == 0U) {
        response["status"] = "error";
        response["message"] = "No compatible packs found for capability";
    } else {
        response["message"] = "Compatible cloud packs available";
    }
    logRouter("Cloud query completed capability=" + capability +
              " count=" + std::to_string(response["count"].asUInt()));
    return response;
}

Json::Value CapabilityRouter::queryCompatiblePacks(const std::string& capability,
                                                   const Json::Value& device_capability_override) const {
    const Json::Value device_capability = capability_provider_.getDeviceCapability(device_capability_override);
    logRouter("Querying compatible packs capability=" + capability +
              " device_capability=" + toJsonString(device_capability));
    return manager_.queryPacks(capability, device_capability);
}

Json::Value CapabilityRouter::usePack(const std::string& user_id, const std::string& pack_id, bool approve_dependencies) {
    logRouter("usePack user_id=" + user_id + " pack_id=" + pack_id +
              " approve_dependencies=" + std::string(approve_dependencies ? "true" : "false"));
    return manager_.installPack(user_id, pack_id, approve_dependencies);
}

Json::Value CapabilityRouter::invoke(const std::string& user_id,
                                     const std::string& pack_id,
                                     const std::string& prompt,
                                     const std::string& options_json) {
    logRouter("invoke user_id=" + user_id + " pack_id=" + pack_id + " prompt=" + prompt);
    return manager_.invoke(user_id, pack_id, prompt, options_json);
}

std::string CapabilityRouter::normalizeCapability(const std::string& skill, const Json::Value& capability_list) const {
    const std::string normalized_skill = normalizeText(skill);
    if (normalized_skill.empty()) {
        throw std::runtime_error("Capability could not be identified");
    }

    const auto skill_terms = splitTerms(normalized_skill);
    std::vector<std::tuple<int, std::size_t, std::string>> ranked;
    for (const auto& item : capability_list) {
        if (!item.isString()) {
            continue;
        }
        const std::string capability = item.asString();
        const std::string normalized_capability = normalizeText(capability);
        if (normalized_capability.empty()) {
            continue;
        }

        int score = 0;
        if (normalized_capability == normalized_skill) {
            score += 100;
        }
        if (normalized_skill.find(normalized_capability) != std::string::npos) {
            score += 20;
        }
        score += intersectionScore(skill_terms, splitTerms(normalized_capability)) * 5;
        if (score > 0) {
            ranked.emplace_back(score, normalized_capability.size(), capability);
        }
    }

    if (ranked.empty()) {
        throw std::runtime_error("Capability could not be identified");
    }

    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        if (std::get<0>(left) != std::get<0>(right)) {
            return std::get<0>(left) > std::get<0>(right);
        }
        if (std::get<1>(left) != std::get<1>(right)) {
            return std::get<1>(left) < std::get<1>(right);
        }
        return std::get<2>(left) < std::get<2>(right);
    });
    return std::get<2>(ranked.front());
}

}  // namespace edgeai
