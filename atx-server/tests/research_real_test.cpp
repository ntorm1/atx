#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/server/research.hpp"

namespace atx::server {
namespace {
using Json = nlohmann::json;
constexpr std::int64_t day_ns = 86'400'000'000'000LL;

class RealResearchTest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<unsigned> sequence{};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
        ("atx-server-real-test-" + std::to_string(tick) + "-" +
         std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(root_);
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  template<class T> Json payload(std::string_view name, const std::vector<T>& values) const {
    const auto path = root_ / name;
    std::ofstream output(path, std::ios::binary);
    const auto bytes = std::as_bytes(std::span(values));
    // SAFETY: char writes arithmetic fixture object representations to the binary contract.
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("fixture write failed");
    return {{"bytes", bytes.size()}, {"sha256", atx::core::sha256_file(path.string()).value()}};
  }
  std::string document(std::string_view name, const Json& value) const {
    const auto path = root_ / name;
    std::ofstream output(path, std::ios::binary);
    output << value.dump();
    output.close();
    if (!output) throw std::runtime_error("fixture document write failed");
    return atx::core::sha256_file(path.string()).value();
  }

  Json fixtures(int year = 2020, std::string_view role_name = "train") const {
    constexpr std::size_t dates = 12, names = 3, count = dates * names;
    std::vector<double> close(count), signal(count), volume(count, 1e6);
    const std::vector<std::uint8_t> support(count, 1);
    std::vector<std::int64_t> sessions;
    const auto first = std::chrono::sys_days{std::chrono::year{year} / 1 / 2};
    for (std::size_t day = 0; day < dates; ++day) {
      const auto date = first.time_since_epoch().count() + static_cast<std::int64_t>(day);
      sessions.push_back(date * day_ns);
      for (std::size_t name = 0; name < names; ++name) {
        close[day * names + name] = 100.0 + static_cast<double>(day * name);
        signal[day * names + name] = static_cast<double>(name);
      }
    }
    const std::vector<std::uint64_t> ids{100, 101, 102};
    Json files;
    files["train_combined.f64"] = payload("train_combined.f64", signal);
    files["train_combined_member.u8"] = payload("train_combined_member.u8", support);
    files["train_combined_finite.u8"] = payload("train_combined_finite.u8", support);
    files["train_combined_sessions.i64"] = payload("train_combined_sessions.i64", sessions);
    files["train_combined_ids.u64"] = payload("train_combined_ids.u64", ids);
    Json role_files;
    role_files["sessions.i64"] = payload("sessions.i64", sessions);
    role_files["ids.u64"] = payload("ids.u64", ids);
    role_files["close.f64"] = payload("close.f64", close);
    role_files["raw_close.f64"] = payload("raw_close.f64", close);
    role_files["present.u8"] = payload("present.u8", support);
    role_files["member.u8"] = payload("member.u8", support);
    role_files["volume.f64"] = payload("volume.f64", volume);
    const std::string pin(64, 'a');
    const Json role{{"schema", "atx.recent-research-role/v1"}, {"status", "complete"},
        {"source_sha256", pin}, {"instrument_namespace", "spiderrock.securityID"},
        {"close_basis", "f64(raw-f32-close)*f64-cumulReturnFactor"},
        {"volume_basis", "raw-share-volume"},
        {"clock_recipe", "modeled-session+22h-mark+23h-decision-v1"},
        {"common_stock_verified", false}, {"historical_vintage_verified", false},
        {"dates", dates}, {"instruments", names}, {"score_begin", 0}, {"score_end", dates},
        {"files", std::move(role_files)}};
    const auto role_pin = document("role.json", role);
    const Json blend{{"schema", "atx.dsl-combined-signal/v1"}, {"status", "complete"},
        {"role", role_name}, {"layout", "date-major-little-endian"}, {"dates", dates},
        {"instruments", names}, {"score_begin", 0}, {"score_end", dates},
        {"role_manifest_sha256", role_pin}, {"source_sha256", pin}, {"library_sha256", pin},
        {"train_manifest_sha256", pin}, {"run_recipe_sha256", pin},
        {"orientation_candidates_sha256", pin}, {"orientations_artifact_sha256", nullptr},
        {"role_window_required", true},
        {"signal_semantics", "exact-pre-target-composition;equal-family/equal-within;"
                             "missing-or-unoriented-neutral-fixed-denominator"},
        {"member_semantics", "decision-member-and-source-present-and-finite-positive-close;"
                             "independent-of-component-coverage"},
        {"finite_semantics",
         "one-iff-saved-f64-is-finite;nonmembers-NaN;zero-is-valid-neutral-signal"},
        {"actual_trades_or_returns", false}, {"finite_cells", count}, {"member_cells", count},
        {"files", std::move(files)}};
    return {{"mode", "real_nav"}, {"combined_path", "train_combined.json"},
        {"combined_sha256", document("train_combined.json", blend)},
        {"role_path", "role.json"}, {"role_sha256", role_pin}};
  }
  ResearchBackend backend() const {
    ResearchOptions options{root_, {root_}};
    options.output_root = root_ / "outputs";
    return ResearchBackend(std::move(options));
  }
  void expect_rejection(const Json& request, std::string_view reason) const {
    try {
      (void)backend().execute(request.dump(), {}, {});
    } catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find(reason), std::string::npos) << error.what();
      return;
    }
    FAIL() << "Expected rejection containing: " << reason;
  }
  Json edit_manifest(Json request, bool combined, std::string_view field, const Json& value) const {
    const auto name = combined ? "train_combined.json" : "role.json";
    std::ifstream input(root_ / name);
    Json manifest;
    input >> manifest;
    input.close();
    manifest[std::string(field)] = value;
    const auto pin = document(name, manifest);
    request[combined ? "combined_sha256" : "role_sha256"] = pin;
    if (!combined) {
      std::ifstream blend_input(root_ / "train_combined.json");
      Json blend;
      blend_input >> blend;
      blend_input.close();
      blend["role_manifest_sha256"] = pin;
      request["combined_sha256"] = document("train_combined.json", blend);
    }
    return request;
  }
  std::filesystem::path root_;
};

TEST_F(RealResearchTest, PinnedArtifactsExecuteExistingImplNavPipeline) {
  const auto request = fixtures();
  std::vector<double> progress;
  const auto result = Json::parse(backend().execute(request.dump(), {},
      [&](double value, std::string_view) { progress.push_back(value); }));
  EXPECT_TRUE(std::is_sorted(progress.begin(), progress.end()));
  EXPECT_EQ(result.at("mode"), "real_nav");
  EXPECT_EQ(result.at("metrics").at("observations"), 10);
  EXPECT_EQ(result.at("provenance").at("engine"), "atx::impl::strategy::run_nav_replay");
  EXPECT_EQ(result.at("provenance").at("combined_sha256"), request.at("combined_sha256"));
  EXPECT_FALSE(result.at("provenance").at("historical_vintage_verified").get<bool>());
  EXPECT_EQ(result.at("provenance").at("evaluation"), "research_unverified");
  EXPECT_TRUE(std::filesystem::is_directory(root_ / "outputs"));
  const auto repeat = Json::parse(backend().execute(request.dump(), {}, {}));
  EXPECT_EQ(result.at("metrics"), repeat.at("metrics"));
  EXPECT_NE(result.at("provenance").at("output_directory"),
            repeat.at("provenance").at("output_directory"));
}

TEST_F(RealResearchTest, WrongExternalManifestPinRejectedBeforePublication) {
  auto request = fixtures();
  request["combined_sha256"] = std::string(64, 'b');
  expect_rejection(request, "manifest SHA-256 pin mismatch");
  EXPECT_FALSE(std::filesystem::exists(root_ / "outputs"));
}

TEST_F(RealResearchTest, HiddenHoldoutSessionDatesRejectedBeforePublication) {
  const auto request = fixtures(2024);
  expect_rejection(request, "locked holdout session date");
  EXPECT_FALSE(std::filesystem::exists(root_ / "outputs"));
}

TEST_F(RealResearchTest, ValidationRoleRejectedBeforePublication) {
  const auto request = fixtures(2020, "validation");
  expect_rejection(request, "locked evaluation role forbidden");
  EXPECT_FALSE(std::filesystem::exists(root_ / "outputs"));
}

TEST_F(RealResearchTest, FractionalDimensionsRejectedAsManifestIntegerBeforePublication) {
  const auto request = edit_manifest(fixtures(), false, "dates", 1.5);
  expect_rejection(request, "manifest integer");
  EXPECT_FALSE(std::filesystem::exists(root_ / "outputs"));
}

TEST_F(RealResearchTest, NumericManifestTypesRejectedWithoutUnsafeIntegerConversions) {
  const std::array<Json, 6> invalid{Json(1.5), Json(1e100), Json(-1),
                                     Json(true), Json("12"), Json(nullptr)};
  for (const bool combined : {true, false}) {
    for (const auto* field : {"dates", "instruments", "score_begin", "score_end"}) {
      for (const auto& value : invalid) {
        SCOPED_TRACE(std::string(combined ? "combined " : "role ") + field + "=" + value.dump());
        expect_rejection(edit_manifest(fixtures(), combined, field, value), "manifest integer");
        EXPECT_FALSE(std::filesystem::exists(root_ / "outputs"));
      }
    }
  }
  for (const auto* field : {"finite_cells", "member_cells"}) {
    for (const auto& value : invalid)
      expect_rejection(edit_manifest(fixtures(), true, field, value), "manifest integer");
  }
}

TEST_F(RealResearchTest, ReceiptByteTypesRejectedBeforeLoaderConversions) {
  auto request = fixtures();
  std::ifstream input(root_ / "role.json");
  Json role;
  input >> role;
  input.close();
  role["files"]["sessions.i64"]["bytes"] = 1e100;
  request = edit_manifest(request, false, "files", role.at("files"));
  expect_rejection(request, "manifest integer");
  EXPECT_FALSE(std::filesystem::exists(root_ / "outputs"));
}

TEST_F(RealResearchTest, ReversedDateWindowRejectedBeforePublication) {
  auto request = fixtures();
  request["start_date"] = "2020-01-10";
  request["end_date"] = "2020-01-02";
  expect_rejection(request, "start_date after end_date");
  EXPECT_FALSE(std::filesystem::exists(root_ / "outputs"));
}
} // namespace
} // namespace atx::server
