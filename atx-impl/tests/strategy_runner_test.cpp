#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <span>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "strategy_runner.hpp"

namespace {
using namespace atx;
using Json = nlohmann::json;
constexpr usize D = 404, N = 4;
constexpr i64 day = 86'400'000'000'000LL;
struct Directory {
  std::filesystem::path path;
  Directory() {
    static std::atomic<unsigned> sequence{};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned a = 0; a < 32; ++a) {
      auto candidate = std::filesystem::temp_directory_path() /
          ("atx-strategy-runner-" + std::to_string(stamp) + "-" + std::to_string(sequence.fetch_add(1)));
      if (std::filesystem::create_directory(candidate)) { path = std::move(candidate); break; }
    }
  }
  ~Directory() { if (!path.empty()) { std::error_code ec; std::filesystem::remove_all(path, ec); } }
};
template<class T> bool payload(const std::filesystem::path& dir, Json& files, const char* name, const std::vector<T>& data) {
  std::ofstream f(dir / name, std::ios::binary); const auto bytes = std::as_bytes(std::span(data));
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); f.close();
  if (!f) return false;
  auto sha = core::sha256_file((dir / name).string()); if (!sha) return false;
  files[name] = {{"bytes", bytes.size()}, {"sha256", *sha}}; return true;
}
bool json_file(const std::filesystem::path& path, const Json& j, std::string& sha) {
  const auto text = j.dump(2) + "\n";
  std::ofstream out(path, std::ios::binary); out << text; out.close();
  auto digest = core::sha256_hex(text); if (!out || !digest) return false; sha = *digest; return true;
}
bool role(const std::filesystem::path& dir, i64 first_day, std::string& sha) {
  if (!std::filesystem::create_directory(dir)) return false;
  std::vector<i64> sessions(D); std::vector<u8> member(D * N, 0), present(D * N, 1);
  std::vector<f64> price(D * N), volume(D * N, 1e12);
  for (usize d = 0; d < D; ++d) {
    sessions[d] = (first_day + static_cast<i64>(d)) * day;
    for (usize i = 0; i < N; ++i) {
      const auto drift = (static_cast<f64>(i) - 1.5) * .0002;
      price[d * N + i] = 100 * std::exp(drift * static_cast<f64>(d) +
          .001 * std::sin(static_cast<f64>(d) * .21 + static_cast<f64>(i)));
      member[d * N + i] = static_cast<u8>(d >= 63);
    }
  }
  // Observed zero shares is valid zero dollar volume inside a complete positive
  // ADV window. It must not turn this source-present name into unknown liquidity.
  volume[360 * N] = 0;
  Json files;
  if (!payload(dir, files, "sessions.i64", sessions) || !payload(dir, files, "ids.u64", std::vector<u64>{10,20,30,40}) ||
      !payload(dir, files, "member.u8", member) || !payload(dir, files, "present.u8", present) ||
      !payload(dir, files, "close.f64", price) || !payload(dir, files, "raw_close.f64", price) ||
      !payload(dir, files, "volume.f64", volume)) return false;
  const Json membership{{"rule", "research-prior63-usd-adv-topn-v1"}, {"top_n", N},
      {"lookback_sessions",63}, {"lag_sessions",1}, {"min_raw_price_exclusive",5}, {"min_adv_exclusive",5000000},
      {"ties","securityID-ascending"}, {"missing","complete-prior-calendar-window-required"}, {"common_stock_verified",false}};
  return json_file(dir / "manifest.json", {{"schema","atx.recent-research-role/v1"}, {"status","complete"},
      {"instrument_namespace","spiderrock.securityID"}, {"dates",D}, {"instruments",N}, {"score_begin",383}, {"score_end",D},
      {"score_start_ns",sessions[383]}, {"score_end_ns",sessions.back()+day}, {"source_sha256",std::string(64,'a')},
      {"membership_recipe",membership.dump()}, {"clock_recipe","modeled-session+22h-mark+23h-decision-v1"},
      {"close_basis","f64(raw-f32-close)*f64-cumulReturnFactor"}, {"volume_basis","raw-share-volume"},
      {"common_stock_verified",false}, {"historical_vintage_verified",false},
      {"declared_output_bytes",D*N*26+D*8+N*8}, {"files",files}}, sha);
}
bool fixture(Directory& dir, atx::impl::strategy::RunnerConfig& cfg) {
  if (dir.path.empty()) return false;
  cfg.library_path = (dir.path / "library.json").string();
  cfg.train_manifest = (dir.path / "train" / "manifest.json").string();
  cfg.validation_manifest = (dir.path / "validation" / "manifest.json").string();
  cfg.output_directory = (dir.path / "output").string(); cfg.min_names = 2; cfg.max_working_bytes = 64ULL << 20;
  if (!role(dir.path / "train", 17683, cfg.train_sha256) || !role(dir.path / "validation", 18200, cfg.validation_sha256)) return false;
  return json_file(cfg.library_path, {{"schema","atx.dsl-strategy-library/v1"}, {"id","synthetic-two"},
      {"primary_variant","weekly_partial25"},
      {"fields",Json::array({{{"name","close"}},{{"name","raw_close"}},{{"name","volume"}}})},
      {"families",Json::array({{{"id","synthetic"}}})},
      {"candidates",Json::array({{{"id","slow"},{"family","synthetic"},{"dsl","close / delay(close, 21)"},{"sign_policy","train-net"}},
          {{"id","fast"},{"family","synthetic"},{"dsl","raw_close / delay(raw_close, 10)"},{"sign_policy","train-net"}}})},
      {"execution_variants",Json::array({{{"id","weekly_partial25"},{"rebalance_sessions",5},{"trade_fraction",.25},{"signal_smoothing_sessions",0}},
          {{"id","weekly_full"},{"rebalance_sessions",5},{"trade_fraction",1.0},{"signal_smoothing_sessions",0}}})}}, cfg.library_sha256);
}
Json read(const std::filesystem::path& path) { std::ifstream in(path); Json j; in >> j; return j; }
}

TEST(StrategyRunner, FixedCostedPortfoliosAcceptObservedZeroVolumeAndFreezeTrainSigns) {
  Directory dir; atx::impl::strategy::RunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  std::ostringstream progress; auto result = atx::impl::strategy::run(cfg,progress);
  ASSERT_TRUE(result) << (result ? "" : result.error().to_string());
  const auto report = read(dir.path / "output" / "summary.json");
  EXPECT_EQ(report.at("status"), "complete");
  EXPECT_EQ(report.at("train_hypotheses_planned"), 6);
  ASSERT_EQ(report.at("roles").size(), 2U);
  EXPECT_EQ(report["roles"][0]["completed_trials"], 6);
  EXPECT_EQ(report["roles"][1]["completed_trials"], 2);
  EXPECT_EQ(report["roles"][0]["orientations"].size(), 2U);
  EXPECT_TRUE(report["roles"][1]["orientations"].empty());
  EXPECT_EQ(report["frozen_signs"].size(), 2U);
  EXPECT_EQ(report["fitted_strategy_sha256"].get<std::string>().size(), 64U);
  for (const auto& r : report["roles"]) {
    ASSERT_EQ(r["family_contribution_coverage"].size(), 1U);
    EXPECT_DOUBLE_EQ(r["family_contribution_coverage"][0]["fixed_denominator_contribution_fraction"].get<f64>(), 1);
    for (const auto& c : r["combined"]) {
    EXPECT_EQ(c["observations"], 19);
    EXPECT_GE(c["mean_held_names"].get<f64>(), 2);
    EXPECT_GT(c["summed_execution_cost_returns"].get<f64>(), 0);
    EXPECT_GT(c["summed_borrow_cost_returns"].get<f64>(), 0);
    EXPECT_EQ(c["execution_context_sha256"].get<std::string>().size(), 64U);
    EXPECT_EQ(c["hac_lag_requested"], 5);
    EXPECT_EQ(c["hac_lag"], 5);
    EXPECT_GE(c["maximum_net_nav_drawdown"].get<f64>(), 0);
    EXPECT_LT(c["maximum_net_nav_drawdown"].get<f64>(), 1);
    }
  }
  EXPECT_LT(report["roles"][0]["combined"][0]["total_one_way_turnover"].get<f64>(),
            report["roles"][0]["combined"][1]["total_one_way_turnover"].get<f64>());
  EXPECT_FALSE(atx::impl::strategy::run(cfg,progress)); // no overwrite/reuse of prior outputs
}

TEST(StrategyRunner, ShortMaturePrefixReportsActualClampedHacLag) {
  Directory dir; atx::impl::strategy::RunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  for (const auto& pair : {std::pair{cfg.train_manifest, &cfg.train_sha256},
                           std::pair{cfg.validation_manifest, &cfg.validation_sha256}}) {
    auto j = read(pair.first);
    const auto old_begin = j["score_begin"].get<i64>();
    j["score_start_ns"] = j["score_start_ns"].get<i64>() + (static_cast<i64>(D - 4) - old_begin) * day;
    j["score_begin"] = D - 4;
    ASSERT_TRUE(json_file(pair.first, j, *pair.second));
  }
  std::ostringstream progress; auto result = atx::impl::strategy::run(cfg,progress);
  ASSERT_TRUE(result) << (result ? "" : result.error().to_string());
  const auto report = read(dir.path / "output" / "summary.json");
  for (const auto& r : report["roles"]) for (const auto& c : r["combined"]) {
    EXPECT_EQ(c["observations"], 2);
    EXPECT_EQ(c["hac_lag_requested"], 5);
    EXPECT_EQ(c["hac_lag"], 1);
  }
}

TEST(StrategyRunner, RefusesExternalPinOrCombinedBudgetBeforePayloadEvaluation) {
  Directory dir; atx::impl::strategy::RunnerConfig cfg; ASSERT_TRUE(fixture(dir,cfg));
  std::ostringstream progress;
  cfg.train_sha256 = std::string(64,'f');
  auto mismatch = atx::impl::strategy::run(cfg,progress); ASSERT_FALSE(mismatch);
  EXPECT_EQ(mismatch.error().code(), core::ErrorCode::InvalidArgument);
  EXPECT_FALSE(std::filesystem::exists(cfg.output_directory));
  auto original = core::sha256_file(cfg.train_manifest); ASSERT_TRUE(original); cfg.train_sha256 = *original;
  cfg.max_working_bytes = 32ULL << 20;
  auto budget = atx::impl::strategy::run(cfg,progress); ASSERT_FALSE(budget);
  EXPECT_EQ(budget.error().code(), core::ErrorCode::Unavailable);
  EXPECT_FALSE(std::filesystem::exists(cfg.output_directory));
  EXPECT_TRUE(progress.str().empty());
}
