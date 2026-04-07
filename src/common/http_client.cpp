#include <edgeai/fs_utils.h>
#include <edgeai/http_client.h>
#include <edgeai/json_utils.h>

#include <curl/curl.h>

#include <stdexcept>

namespace edgeai {

namespace {

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
        throw std::runtime_error("HTTP request failed for " + url + ": " + curl_easy_strerror(code));
    }
    if (http_code < 200 || http_code >= 300) {
        throw std::runtime_error("Unexpected HTTP status " + std::to_string(http_code) + " for " + url + ": " + response);
    }

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
        throw std::runtime_error("Download failed for " + url + ": " + curl_easy_strerror(code));
    }
}

}  // namespace edgeai
