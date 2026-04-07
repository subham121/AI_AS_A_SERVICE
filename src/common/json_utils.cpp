#include <edgeai/json_utils.h>

#include <stdexcept>

namespace edgeai {

Json::Value parseJson(const std::string& text) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::string errors;
    Json::Value value;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(text.data(), text.data() + text.size(), &value, &errors)) {
        throw std::runtime_error("Failed to parse JSON: " + errors);
    }
    return value;
}

std::string toJsonString(const Json::Value& value, bool styled) {
    if (styled) {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        return Json::writeString(builder, value);
    }
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["commentStyle"] = "None";
    return Json::writeString(builder, value);
}

Json::Value makeStatus(const std::string& status, const std::string& message) {
    Json::Value value(Json::objectValue);
    value["status"] = status;
    value["message"] = message;
    return value;
}

}  // namespace edgeai
