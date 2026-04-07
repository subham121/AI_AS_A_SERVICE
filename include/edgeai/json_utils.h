#pragma once

#include <json/json.h>

#include <string>

namespace edgeai {

Json::Value parseJson(const std::string& text);
std::string toJsonString(const Json::Value& value, bool styled = false);
Json::Value makeStatus(const std::string& status, const std::string& message);

}  // namespace edgeai
