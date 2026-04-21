#include "test_support.h"

#include <edgeai/fs_utils.h>
#include <edgeai/http_client.h>
#include <edgeai/json_utils.h>
#include <edgeai/process_utils.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace edgeai::testsupport {

namespace {

int allocatePort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("Failed to create socket for test server port allocation");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        throw std::runtime_error("Failed to bind test server socket");
    }

    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(fd);
        throw std::runtime_error("Failed to inspect test server socket");
    }

    const int port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}

std::string uniqueSuffix() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(static_cast<long long>(::getpid())) + "-" + std::to_string(static_cast<long long>(now));
}

std::filesystem::path sourcePath(const std::string& relative_path) {
    return projectRoot() / relative_path;
}

}  // namespace

TempDir::TempDir()
    : path_(std::filesystem::temp_directory_path() / ("edgeai-unit-" + uniqueSuffix())) {
    ensureDirectory(path_);
}

TempDir::~TempDir() {
    removePath(path_);
}

TestCatalogServer::TestCatalogServer(std::filesystem::path root_dir)
    : root_dir_(std::move(root_dir)),
      script_path_(root_dir_ / "server.py"),
      data_path_(root_dir_ / "catalog_seed.json"),
      port_(allocatePort()) {
    ensureDirectory(root_dir_ / "bundles");
}

TestCatalogServer::~TestCatalogServer() {
    stop();
}

std::string TestCatalogServer::baseUrl() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/apiv1";
}

std::string TestCatalogServer::bundleUrl(const std::string& file_name) const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/bundles/" + file_name;
}

void TestCatalogServer::writeSeed(const Json::Value& seed) const {
    writeJsonFile(data_path_, seed);
}

void TestCatalogServer::writeServerScript() const {
    static const char* kServerScript = R"PY(
import json
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

DATA_PATH = Path(sys.argv[1])
ROOT_DIR = Path(sys.argv[2])
PORT = int(sys.argv[3])


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        return

    def _load_seed(self):
        if not DATA_PATH.exists():
            return {}
        return json.loads(DATA_PATH.read_text())

    def _send_json(self, payload, status=200):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_file(self, path):
        if not path.exists():
            self.send_error(404)
            return
        body = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        seed = self._load_seed()

        if parsed.path == "/_health":
            self._send_json({"status": "ok"})
            return

        if parsed.path == "/apiv1/getCapabilityList":
            capabilities = seed.get("capabilities", [])
            self._send_json({"capabilities": capabilities, "count": len(capabilities)})
            return

        if parsed.path == "/apiv1/getCompatiblePackList":
            query = urllib.parse.parse_qs(parsed.query)
            capability = query.get("capability", [""])[0]
            response = dict(seed.get("compatible", {}).get(capability, {}))
            response.setdefault("packs", [])
            response.setdefault("count", len(response["packs"]))
            self._send_json(response)
            return

        if parsed.path == "/apiv1/getPackDetails":
            query = urllib.parse.parse_qs(parsed.query)
            pack_id = query.get("pack_id", [""])[0]
            details = seed.get("pack_details", {}).get(pack_id)
            if details is None:
                self.send_error(404)
                return
            self._send_json(details)
            return

        if parsed.path.startswith("/bundles/"):
            self._send_file(ROOT_DIR / parsed.path.lstrip("/"))
            return

        self.send_error(404)

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        length = int(self.headers.get("Content-Length", "0"))
        payload = self.rfile.read(length).decode() if length else "{}"
        body = json.loads(payload or "{}")

        if parsed.path == "/apiv1/echo":
            self._send_json(body)
            return

        self.send_error(404)


if __name__ == "__main__":
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    server.serve_forever()
)PY";

    writeTextFile(script_path_, kServerScript);
}

void TestCatalogServer::waitUntilReady() const {
    HttpClient client;
    const std::string url = "http://127.0.0.1:" + std::to_string(port_) + "/_health";
    for (int attempt = 0; attempt < 50; ++attempt) {
        try {
            const Json::Value response = client.getJson(url);
            if (response.get("status", "").asString() == "ok") {
                return;
            }
        } catch (const std::exception&) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("Timed out waiting for test catalog server to become ready");
}

void TestCatalogServer::start() {
    if (pid_ > 0) {
        return;
    }

    writeServerScript();
    if (!std::filesystem::exists(data_path_)) {
        Json::Value empty(Json::objectValue);
        empty["capabilities"] = Json::Value(Json::arrayValue);
        empty["compatible"] = Json::Value(Json::objectValue);
        empty["pack_details"] = Json::Value(Json::objectValue);
        writeSeed(empty);
    }

    const std::string port_string = std::to_string(port_);
    pid_ = ::fork();
    if (pid_ < 0) {
        throw std::runtime_error("Failed to fork test catalog server process");
    }

    if (pid_ == 0) {
        ::execl(pythonExecutable().c_str(),
                pythonExecutable().c_str(),
                script_path_.c_str(),
                data_path_.c_str(),
                root_dir_.c_str(),
                port_string.c_str(),
                static_cast<char*>(nullptr));
        _exit(127);
    }

    waitUntilReady();
}

void TestCatalogServer::stop() {
    if (pid_ <= 0) {
        return;
    }

    ::kill(pid_, SIGTERM);
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
}

std::filesystem::path projectRoot() {
#ifndef EDGEAI_TEST_SOURCE_DIR
#error "EDGEAI_TEST_SOURCE_DIR is required for unit tests"
#endif
    return EDGEAI_TEST_SOURCE_DIR;
}

std::filesystem::path nextWordPackLibrary() {
#ifndef EDGEAI_TEST_NEXT_WORD_PACK_LIB
#error "EDGEAI_TEST_NEXT_WORD_PACK_LIB is required for unit tests"
#endif
    return EDGEAI_TEST_NEXT_WORD_PACK_LIB;
}

std::string pythonExecutable() {
#ifndef EDGEAI_TEST_PYTHON_BIN
#error "EDGEAI_TEST_PYTHON_BIN is required for unit tests"
#endif
    return EDGEAI_TEST_PYTHON_BIN;
}

Json::Value makeDefaultDeviceCapability() {
    Json::Value capability(Json::objectValue);
    capability["architecture"] = "arm64";
    capability["ram_mb"] = 4096;
    capability["cpu_cores"] = 8;
    capability["os_family"] = "darwin";
    capability["accelerators"] = Json::Value(Json::arrayValue);
    return capability;
}

Json::Value readJsonFile(const std::filesystem::path& path) {
    return parseJson(readTextFile(path));
}

void writeJsonFile(const std::filesystem::path& path, const Json::Value& value) {
    writeTextFile(path, toJsonString(value, true));
}

std::filesystem::path createNextWordPackRoot(const std::filesystem::path& target_root) {
    ensureDirectory(target_root / "lib");
    ensureDirectory(target_root / "runtime");
    ensureDirectory(target_root / "model");
    ensureDirectory(target_root / "config");

    const auto library_path = nextWordPackLibrary();
    if (!std::filesystem::exists(library_path)) {
        throw std::runtime_error("Next word pack library was not built at " + library_path.string());
    }

    std::filesystem::copy_file(sourcePath("packs/next_word/manifest.json"),
                               target_root / "manifest.json",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(sourcePath("packs/next_word/runtime/next_word_helper.py"),
                               target_root / "runtime" / "next_word_helper.py",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(sourcePath("packs/next_word/model/next_word_bigram.onnx"),
                               target_root / "model" / "next_word_bigram.onnx",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(sourcePath("packs/next_word/model/vocab.json"),
                               target_root / "model" / "vocab.json",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(sourcePath("packs/next_word/model/transitions.json"),
                               target_root / "model" / "transitions.json",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(sourcePath("packs/next_word/config/default.json"),
                               target_root / "config" / "default.json",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(library_path,
                               target_root / "lib" / library_path.filename(),
                               std::filesystem::copy_options::overwrite_existing);
    return target_root;
}

std::filesystem::path createTarGzBundle(const std::filesystem::path& source_dir,
                                        const std::filesystem::path& archive_path) {
    ensureDirectory(archive_path.parent_path());
    removePath(archive_path);
    const auto result = runCommandCapture({
        "tar",
        "-czf",
        archive_path.string(),
        "-C",
        source_dir.string(),
        ".",
    });
    if (result.exit_code != 0) {
        throw std::runtime_error("Failed to create tar.gz archive for test bundle");
    }
    return archive_path;
}

Json::Value makePackServerDetails(const std::string& pack_url, const std::string& md5_checksum) {
    Json::Value details(Json::objectValue);
    details["capability"]["slug"] = "next_word_prediction";
    details["capability"]["name"] = "Next Word Prediction";
    details["capability"]["description"] = "Predicts the next likely token for a prompt.";
    details["capability"]["category"] = "Language";
    details["capability"]["tags"] = Json::Value(Json::arrayValue);
    details["capability"]["tags"].append("next_word_prediction");
    details["capability"]["tags"].append("autocomplete");
    details["capability"]["version"] = "1.0.0";
    details["capability"]["license"] = "MIT";

    details["package"]["pack_id"] = "next_word_pack";
    details["package"]["pack_name"] = "Next Word Predictor";
    details["package"]["pack_url"] = pack_url;
    details["package"]["bundle_path"] = pack_url;
    details["package"]["checksum"] = "md5:" + md5_checksum;

    details["monetization"]["model"] = "prediction";
    details["monetization"]["currency"] = "USD";

    details["runtime_descriptor"]["memory_required_mb"] = 128;
    details["runtime_descriptor"]["cpu_cores_recommended"] = 1;
    details["runtime_descriptor"]["gpu_required"] = false;
    details["runtime_descriptor"]["interface"] = "predict";
    details["runtime_descriptor"]["dependencies"] = Json::Value(Json::arrayValue);
    return details;
}

Json::Value makeCompatiblePackResponse(const std::string& pack_url) {
    Json::Value response(Json::objectValue);
    Json::Value pack(Json::objectValue);
    pack["pack_id"] = "next_word_pack";
    pack["pack_name"] = "Next Word Predictor";
    pack["pack_url"] = pack_url;
    pack["pack_description"] = "Predicts the next likely token for a prompt.";
    pack["pack_monetization"]["model"] = "prediction";
    pack["pack_monetization"]["currency"] = "USD";
    response["packs"] = Json::Value(Json::arrayValue);
    response["packs"].append(pack);
    response["count"] = 1;
    return response;
}

Json::Value makeCatalogSeed(const std::string& pack_url, const std::string& md5_checksum) {
    Json::Value seed(Json::objectValue);
    seed["capabilities"] = Json::Value(Json::arrayValue);
    seed["capabilities"].append("next_word_prediction");
    seed["compatible"]["next_word_prediction"] = makeCompatiblePackResponse(pack_url);
    seed["pack_details"]["next_word_pack"] = makePackServerDetails(pack_url, md5_checksum);
    return seed;
}

}  // namespace edgeai::testsupport
