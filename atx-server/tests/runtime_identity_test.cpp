#include <atx/server/runtime_identity.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace atx::server {
namespace {
class RuntimeIdentityTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<unsigned> sequence{};
        root_ = std::filesystem::temp_directory_path() /
            ("atx-runtime-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(root_);
    }
    void TearDown() override { std::filesystem::remove_all(root_); }
    static void write(const std::filesystem::path& path, std::string_view bytes) {
        std::ofstream output(path, std::ios::binary);
        output << bytes;
        ASSERT_TRUE(output.good());
    }
    std::filesystem::path root_;
};

TEST_F(RuntimeIdentityTest, ContentReceiptSurvivesRelocationAndIgnoresUnshippedDocuments) {
    const auto first = root_ / "first";
    const auto second = root_ / std::filesystem::path(u"\u7814\u7a76");
    std::filesystem::create_directory(first);
    std::filesystem::create_directory(second);
    for (const auto& folder : {first, second}) {
        write(folder / "atx-server.exe", "executable bytes");
        write(folder / "duckdb.dll", "database bytes");
        write(folder / "alpha.DLL", "uppercase library");
    }
    write(first / "readme.txt", "not executable code");
    const auto one = identify_runtime(first / "atx-server.exe");
    const auto two = identify_runtime(second / "atx-server.exe");
    EXPECT_EQ(one.sha256, two.sha256);
    EXPECT_EQ(one.manifest_json, two.manifest_json);
    EXPECT_EQ(one.sha256.size(), 64U);
    const auto manifest = nlohmann::json::parse(one.manifest_json);
    ASSERT_EQ(manifest.at("artifacts").size(), 3U);
    EXPECT_EQ(manifest.at("artifacts").at(0).at("name"), "alpha.DLL");
    EXPECT_EQ(one.manifest_json.find(root_.string()), std::string::npos);
}

TEST_F(RuntimeIdentityTest, ExecutableAndLibraryBytesBothInvalidateThePin) {
    write(root_ / "atx-server.exe", "first");
    write(root_ / "duckdb.dll", "first");
    const auto original = identify_runtime(root_ / "atx-server.exe").sha256;
    write(root_ / "atx-server.exe", "other");
    const auto changed_executable = identify_runtime(root_ / "atx-server.exe").sha256;
    EXPECT_NE(original, changed_executable);
    write(root_ / "duckdb.dll", "other");
    EXPECT_NE(changed_executable, identify_runtime(root_ / "atx-server.exe").sha256);
}

TEST_F(RuntimeIdentityTest, MissingAndUnboundedDistributionsFailClosed) {
    EXPECT_THROW(static_cast<void>(identify_runtime(root_ / "missing.exe")), std::exception);
    write(root_ / "atx-server.exe", "first");
    for (unsigned index = 0; index < 64; ++index)
        write(root_ / ("library-" + std::to_string(index) + ".dll"), "x");
    EXPECT_THROW(static_cast<void>(identify_runtime(root_ / "atx-server.exe")),
                 std::runtime_error);
}

TEST_F(RuntimeIdentityTest, ProcessImageIsAbsoluteExistingAndIndependentOfWorkingDirectory) {
    const auto executable = current_executable_path();
    EXPECT_TRUE(executable.is_absolute());
    EXPECT_TRUE(std::filesystem::is_regular_file(executable));
}
} // namespace
} // namespace atx::server
