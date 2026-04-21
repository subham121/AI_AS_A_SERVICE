#include "test_support.h"

#include <edgeai/fs_utils.h>
#include <edgeai/json_utils.h>
#include <edgeai/process_utils.h>

#include <gtest/gtest.h>

namespace {

using edgeai::testsupport::TempDir;
using edgeai::testsupport::pythonExecutable;

TEST(JsonUtilsTest, ParseJsonRoundTripsCompactAndStyledOutput) {
    const auto value = edgeai::parseJson(R"({"name":"edge","count":2})");

    EXPECT_EQ(value["name"].asString(), "edge");
    EXPECT_EQ(value["count"].asInt(), 2);
    const auto compact = edgeai::toJsonString(value, false);
    EXPECT_EQ(edgeai::parseJson(compact), value);
    EXPECT_EQ(compact.find(' '), std::string::npos);
    EXPECT_NE(edgeai::toJsonString(value, true).find("\n"), std::string::npos);
}

TEST(JsonUtilsTest, ParseJsonThrowsForInvalidInput) {
    EXPECT_THROW(edgeai::parseJson("{invalid json"), std::runtime_error);
}

TEST(JsonUtilsTest, MakeStatusReturnsExpectedFields) {
    const auto status = edgeai::makeStatus("ok", "ready");

    EXPECT_EQ(status["status"].asString(), "ok");
    EXPECT_EQ(status["message"].asString(), "ready");
}

TEST(ProcessUtilsTest, ShellEscapeHandlesSingleQuotes) {
    EXPECT_EQ(edgeai::shellEscape("ab'cd"), R"('ab'\''cd')");
}

TEST(ProcessUtilsTest, RunCommandCaptureSupportsArgvEscapingAndExitCodes) {
    const auto echo = edgeai::runCommandCapture({
        pythonExecutable(),
        "-c",
        "import sys; print(sys.argv[1])",
        "hello world",
    });
    EXPECT_EQ(echo.exit_code, 0);
    EXPECT_EQ(echo.output, "hello world\n");

    const auto failure = edgeai::runCommandCapture({
        "sh",
        "-c",
        "printf failing; exit 7",
    });
    EXPECT_EQ(failure.exit_code, 7);
    EXPECT_EQ(failure.output, "failing");
}

TEST(FsUtilsTest, WriteReadAndRemovePathWorkAcrossNestedDirectories) {
    TempDir temp_dir;
    const auto nested_file = temp_dir.path() / "deep" / "path" / "sample.txt";

    edgeai::writeTextFile(nested_file, "payload");
    EXPECT_EQ(edgeai::readTextFile(nested_file), "payload");

    edgeai::removePath(temp_dir.path() / "deep");
    EXPECT_FALSE(std::filesystem::exists(nested_file));
}

TEST(FsUtilsTest, MakeTempJsonCreatesUniqueFilesAndExtractTarGzWorks) {
    TempDir temp_dir;
    const auto json_dir = temp_dir.path() / "json";
    const auto first = edgeai::makeTempJson(json_dir, "request", R"({"id":1})");
    const auto second = edgeai::makeTempJson(json_dir, "request", R"({"id":2})");

    EXPECT_NE(first, second);
    EXPECT_EQ(edgeai::readTextFile(first), R"({"id":1})");
    EXPECT_EQ(edgeai::readTextFile(second), R"({"id":2})");

    const auto source_dir = temp_dir.path() / "bundle_src";
    edgeai::ensureDirectory(source_dir);
    edgeai::writeTextFile(source_dir / "payload.txt", "bundle");
    const auto archive = temp_dir.path() / "bundle.tar.gz";
    const auto tar = edgeai::runCommandCapture({
        "tar",
        "-czf",
        archive.string(),
        "-C",
        source_dir.string(),
        ".",
    });
    ASSERT_EQ(tar.exit_code, 0);

    const auto extract_dir = temp_dir.path() / "bundle_out";
    ASSERT_TRUE(edgeai::extractTarGz(archive, extract_dir));
    EXPECT_EQ(edgeai::readTextFile(extract_dir / "payload.txt"), "bundle");
    EXPECT_FALSE(edgeai::computeMd5(archive).empty());
}

}  // namespace
