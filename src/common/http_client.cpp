#include <edgeai/fs_utils.h>
#include <edgeai/http_client.h>
#include <edgeai/json_utils.h>

#include <curl/curl.h>

#include <iostream>
#include <stdexcept>

namespace edgeai {

namespace {

std::string previewBody(const std::string& text, std::size_t max_length = 320) {
    if (text.size() <= max_length) {
        return text;
    }
    return text.substr(0, max_length) + "...";
}

void logHttp(const std::string& message) {
    std::cerr << "[HttpClient] " << message << std::endl;
}

size_t writeToString(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* output = static_cast<std::string*>(userp);
    output->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

size_t writeToFile(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* output = static_cast<FILE*>(userp);
    return fwrite(contents, size, nmemb, output);
}

Json::Value performJsonRequest(const std::string& url, const char* method, const std::string* payload) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize curl");
    }

    logHttp(std::string("Sending ") + method + " " + url +
            (payload ? " payload=" + previewBody(*payload) : ""));

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    if (payload) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload->c_str());
    }

    CURLcode code = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        logHttp("Request failed for " + url + ": " + curl_easy_strerror(code));
        throw std::runtime_error("HTTP request failed for " + url + ": " + curl_easy_strerror(code));
    }
    if (http_code < 200 || http_code >= 300) {
        logHttp("Unexpected HTTP status " + std::to_string(http_code) + " for " + url +
                " body=" + previewBody(response));
        throw std::runtime_error("Unexpected HTTP status " + std::to_string(http_code) + " for " + url + ": " + response);
    }

    logHttp(std::string("Received ") + std::to_string(http_code) + " from " + url +
            " body=" + previewBody(response));

    return parseJson(response);
}

}  // namespace

Json::Value HttpClient::getJson(const std::string& url) const {
    return performJsonRequest(url, "GET", nullptr);
}

Json::Value HttpClient::postJson(const std::string& url, const Json::Value& payload) const {
    const auto body = toJsonString(payload);
    return performJsonRequest(url, "POST", &body);
}

void HttpClient::downloadToFile(const std::string& url, const std::filesystem::path& destination) const {
    ensureDirectory(destination.parent_path());
    logHttp("Downloading " + url + " -> " + destination.string());
    FILE* file = fopen(destination.string().c_str(), "wb");
    if (!file) {
        throw std::runtime_error("Unable to open destination file for download: " + destination.string());
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(file);
        throw std::runtime_error("Failed to initialize curl");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    CURLcode code = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(file);

    if (code != CURLE_OK) {
        logHttp("Download failed for " + url + ": " + curl_easy_strerror(code));
        throw std::runtime_error("Download failed for " + url + ": " + curl_easy_strerror(code));
    }
    logHttp("Download complete for " + url + " -> " + destination.string());
}

std::string HttpClient::urlEncode(const std::string& value) const {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize curl for URL encoding");
    }
    char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    if (!encoded) {
        curl_easy_cleanup(curl);
        throw std::runtime_error("Failed to URL encode value");
    }
    const std::string result(encoded);
    curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

}  // namespace edgeai
