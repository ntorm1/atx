#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "../src/strategy_ic_composition.hpp"
#include "../src/strategy_price_exposures.hpp"
#include "../src/strategy_target_replay.hpp"
#include "../src/strategy_target_replay_detail.hpp"

namespace {
using namespace atx;
namespace st = atx::impl::strategy;
namespace co = atx::core;
using Json = nlohmann::json;
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr f64 missing = std::numeric_limits<f64>::quiet_NaN();
i64 session(int year, unsigned month, unsigned day) {
  return std::chrono::sys_days{std::chrono::year{year} / month / day}
      .time_since_epoch().count() * day_ns;
}
struct Fixture {
  usize d{}, n{};
  std::vector<f64> signal, close, raw;
  std::vector<u8> member, present;
  std::vector<i64> sessions;
  std::vector<u64> ids;
  Fixture(usize dates, usize names) : d(dates), n(names), signal(dates * names),
      member(dates * names, 1), sessions(dates), ids(names) {
    for (usize t = 0; t < d; ++t) sessions[t] = session(2020, 1, 1) + static_cast<i64>(t) * day_ns;
    for (usize i = 0; i < n; ++i) ids[i] = 100 + i;
  }
  st::TargetReplayInput input() const {
    return {d, n, 0, d, signal, member, sessions, ids, close, raw, present};
  }
  [[nodiscard]] usize members_at(usize t) const {
    usize count = 0;
    for (usize i = 0; i < n; ++i) count += member[t * n + i] ? 1U : 0U;
    return count;
  }
};
struct Directory {
  std::filesystem::path path;
  Directory() {
    static std::atomic<u64> counter{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
        ("atx_target_replay_" + std::to_string(tick) + "_" + std::to_string(counter.fetch_add(1)));
    if (!std::filesystem::create_directory(path)) throw std::runtime_error("unique fixture directory");
  }
  ~Directory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
template<class T> Json write_payload(const std::filesystem::path& file, const std::vector<T>& values) {
  std::ofstream out(file, std::ios::binary);
  const auto bytes = std::as_bytes(std::span(values));
  // SAFETY: char writes the object representation of contiguous arithmetic fixture data.
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) throw std::runtime_error("fixture payload write");
  return Json{{"bytes", bytes.size()}, {"sha256", co::sha256_file(file.string()).value()}};
}
std::string write_json(const std::filesystem::path& path, const Json& value) {
  std::ofstream out(path, std::ios::binary); out << value.dump(2) << '\n'; out.close();
  if (!out) throw std::runtime_error("fixture JSON write");
  return co::sha256_file(path.string()).value();
}
st::TargetReplayRunConfig artifact(const std::filesystem::path& dir, const Fixture& f) {
  std::vector<u8> finite(f.signal.size()); u64 finite_count = 0, members = 0;
  for (usize k = 0; k < finite.size(); ++k) {
    finite[k] = static_cast<u8>(std::isfinite(f.signal[k])); finite_count += finite[k]; members += f.member[k];
  }
  Json files;
  files["train_combined.f64"] = write_payload(dir / "train_combined.f64", f.signal);
  files["train_combined_member.u8"] = write_payload(dir / "train_combined_member.u8", f.member);
  files["train_combined_finite.u8"] = write_payload(dir / "train_combined_finite.u8", finite);
  files["train_combined_sessions.i64"] = write_payload(dir / "train_combined_sessions.i64", f.sessions);
  files["train_combined_ids.u64"] = write_payload(dir / "train_combined_ids.u64", f.ids);
  const std::string pin(64, 'a');
  Json manifest{{"schema", "atx.dsl-combined-signal/v1"}, {"status", "complete"}, {"role", "train"},
      {"layout", "date-major-little-endian"}, {"dates", f.d}, {"instruments", f.n},
      {"score_begin", 0}, {"score_end", f.d}, {"role_manifest_sha256", pin},
      {"source_sha256", pin}, {"library_sha256", pin}, {"train_manifest_sha256", pin},
      {"run_recipe_sha256", pin}, {"orientation_candidates_sha256", pin},
      {"orientations_artifact_sha256", nullptr}, {"role_window_required", true},
      {"signal_semantics", "exact-pre-target-composition;equal-family/equal-within;missing-or-unoriented-neutral-fixed-denominator"},
      {"member_semantics", "decision-member-and-source-present-and-finite-positive-close;independent-of-component-coverage"},
      {"finite_semantics", "one-iff-saved-f64-is-finite;nonmembers-NaN;zero-is-valid-neutral-signal"},
      {"actual_trades_or_returns", false}, {"finite_cells", finite_count}, {"member_cells", members},
      {"files", std::move(files)}};
  st::TargetReplayRunConfig cfg;
  cfg.combined_path = (dir / "train_combined.json").string();
  cfg.combined_sha256 = write_json(cfg.combined_path, manifest);
  cfg.output_directory = (dir / "out").string(); return cfg;
}

// ---- construction options (T4) ----
constexpr const char* default_daily_header =
    "decision,session_ns,month,entry,endpoint,turnover,forced,discretionary,deployment,"
    "month_turnover,budget_excess,applied_fraction,gross,net,long_weight,short_weight,"
    "max_abs_weight,effective_names,held_names,return_mature,return_complete,"
    "observed_return_component,missing_long,missing_short,missing_gross,missing_names,"
    "guarded_names,modeled_trade_cost,modeled_borrow_cost,complete_gross_return,complete_net_return";
std::string first_line(const std::filesystem::path& path) {
  std::ifstream in(path); std::string line; std::getline(in, line); return line;
}
Json read_json(const std::filesystem::path& path) {
  std::ifstream in(path); Json j; in >> j; return j;
}
u64 bits(f64 x) { return std::bit_cast<u64>(x); }
struct Lcg {
  u64 state{};
  f64 uniform() { // [0, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11) * 0x1.0p-53;
  }
};
// The strategy_price_exposures_test noisy panel, bit for bit (common factor with
// per-name loadings, noise scales and dollar volumes on coprime strides), plus
// random member signals and consecutive daily sessions.
struct Role {
  usize d{}, n{};
  std::vector<f64> signal, close, raw, volume;
  std::vector<u8> member, present;
  std::vector<i64> sessions;
  std::vector<u64> ids;
  Role(usize dates, usize names, u64 seed)
      : d(dates), n(names), signal(dates * names), close(dates * names), raw(dates * names),
        volume(dates * names), member(dates * names, 1), present(dates * names, 1),
        sessions(dates), ids(names) {
    Lcg rng{seed};
    for (usize i = 0; i < n; ++i) close[i] = 20.0 + static_cast<f64>(i);
    for (usize t = 1; t < d; ++t) {
      const f64 common = 0.03 * (rng.uniform() - 0.5);
      for (usize i = 0; i < n; ++i) {
        const f64 loading = 0.4 + 0.15 * static_cast<f64>((i * 5) % n);
        const f64 idio = 0.004 + 0.003 * static_cast<f64>((i * 7) % n);
        close[t * n + i] =
            close[(t - 1) * n + i] * (1 + loading * common + idio * (rng.uniform() - 0.5));
      }
    }
    for (usize t = 0; t < d; ++t)
      for (usize i = 0; i < n; ++i) {
        raw[t * n + i] = close[t * n + i];
        volume[t * n + i] = 1e4 * static_cast<f64>(1 + (i * 11) % n) * (0.5 + rng.uniform());
      }
    Lcg draws{seed + 1000};
    for (auto& s : signal) s = draws.uniform() - 0.5;
    for (usize t = 0; t < d; ++t) sessions[t] = session(2020, 1, 1) + static_cast<i64>(t) * day_ns;
    for (usize i = 0; i < n; ++i) ids[i] = 100 + i;
  }
  void nonmember(usize t, usize i) { member[t * n + i] = 0; signal[t * n + i] = missing; }
  st::TargetReplayInput input() const {
    return {d, n, 0, d, signal, member, sessions, ids, close, raw, present, volume};
  }
  st::PriceExposureInput prices() const { return {d, n, close, raw, volume, present}; }
};
// Every name has a full-window beta (20 pairs) from decision 20 on; before that no
// name has exposures and the neutralization refuses with too few usable names.
st::TargetReplayConfig neutral_daily() {
  st::TargetReplayConfig c; c.cadence = 1; c.trade_fraction = 1;
  c.neutralize = st::TargetNeutralize::PriceRiskV1;
  c.price_risk.beta_window = 40; c.price_risk.vol_window = 20; c.price_risk.adv_window = 10;
  c.price_risk.min_return_pairs = 20; c.price_risk.min_names = 5;
  return c;
}
usize count_outcome(const st::TargetReplayResult& r, st::NeutralizeOutcome outcome) {
  usize count = 0;
  for (const auto& day : r.days) count += day.construction.neutralize == outcome ? 1U : 0U;
  return count;
}
} // namespace

TEST(StrategyTargetReplay, BaselineBitMatchesExistingCompositionIncludingTiesAndExits) {
  Fixture f(27, 7);
  for (usize d = 0; d < f.d; ++d) for (usize i = 0; i < f.n; ++i) {
    f.signal[d * f.n + i] = static_cast<f64>((i + d / 3) % 4);
    if ((i == 1 && d >= 7) || (i == 6 && d < 8)) {
      f.member[d * f.n + i] = 0; f.signal[d * f.n + i] = missing;
    }
  }
  const std::vector<st::IcCompositionCandidate> candidates{{"one", "family"}};
  st::IcCompositionConfig cc; cc.dates = f.d; cc.instruments = f.n; cc.decision_end = f.d;
  auto c = st::IcComposition::create(cc, candidates, f.member); ASSERT_TRUE(c);
  ASSERT_TRUE(c->add(0, f.signal, 1)); auto baseline = c->finish(); ASSERT_TRUE(baseline);
  f.signal = baseline->signal;
  auto replay = st::replay_targets(f.input(), {}); ASSERT_TRUE(replay);
  ASSERT_EQ(replay->days.size(), f.d);
  for (usize d = 0; d < f.d; ++d) {
    EXPECT_EQ(std::bit_cast<u64>(replay->days[d].turnover),
              std::bit_cast<u64>(baseline->planned_turnover[d])) << d;
    EXPECT_EQ(std::bit_cast<u64>(replay->days[d].gross),
              std::bit_cast<u64>(baseline->planned_gross[d])) << d;
    EXPECT_EQ(std::bit_cast<u64>(replay->days[d].net),
              std::bit_cast<u64>(baseline->planned_net[d])) << d;
  }
  EXPECT_EQ(replay->deployment_date, baseline->deployment_date);
  EXPECT_EQ(replay->deployment_turnover, baseline->deployment_turnover);
  EXPECT_EQ(replay->total_turnover, baseline->total_planned_turnover);
  EXPECT_GT(replay->forced_turnover, 0);
}

TEST(StrategyTargetReplay, MonthBudgetChargesDeploymentForcedExitAndResetsWithoutFutureKnowledge) {
  Fixture f(5, 2);
  f.sessions = {session(2020, 1, 29), session(2020, 1, 30), session(2020, 1, 31),
                session(2020, 2, 3), session(2020, 2, 4)};
  f.signal = {-1, 1, 1, -1, missing, -1, -1, 1, -1, 1}; f.member[4] = 0;
  st::TargetReplayConfig cfg; cfg.rule = st::TargetReplayRule::MonthlyTargetBudgetV2;
  cfg.cadence = 1; cfg.trade_fraction = 1; cfg.monthly_budget = .30;
  auto result = st::replay_targets(f.input(), cfg); ASSERT_TRUE(result);
  const auto& days = result->days;
  EXPECT_NEAR(days[0].turnover, .30, 1e-15);
  EXPECT_EQ(days[0].deployment_turnover, days[0].turnover);
  EXPECT_EQ(days[1].turnover, 0); // no remaining January discretionary budget
  EXPECT_NEAR(days[2].forced_turnover, .15, 1e-15);
  EXPECT_EQ(days[2].discretionary_turnover, 0);
  EXPECT_NEAR(days[2].budget_excess, .15, 1e-15);
  EXPECT_NEAR(days[2].net, .15, 1e-15); // no hidden survivor re-neutralization
  EXPECT_EQ(days[3].calendar_month, 202002U);
  EXPECT_NEAR(days[3].month_turnover, .30, 1e-15);
  EXPECT_LE(days[3].budget_excess, 1e-15);
  EXPECT_NEAR(result->total_turnover, .75, 1e-14);
  auto changed = f; changed.signal[6] = 100; changed.signal[7] = -100;
  auto future = st::replay_targets(changed.input(), cfg); ASSERT_TRUE(future);
  for (usize d = 0; d < 3; ++d) {
    EXPECT_EQ(result->days[d].turnover, future->days[d].turnover);
    EXPECT_EQ(result->days[d].gross, future->days[d].gross);
  }
}

TEST(StrategyTargetReplay, DelayedRoughReturnsExposeAbsentBackingAndGuardedLongShortRisk) {
  Fixture f(5, 2);
  f.signal = {-1, 1, -1, 1, -1, 1, -1, 1, -1, 1};
  f.close = {100, 100, 100, 100, 777, 110, 777, 121, 777, 121};
  f.raw = f.close; f.present.assign(10, 1); f.present[4] = 0;
  st::TargetReplayConfig cfg; cfg.cadence = 5; cfg.trade_fraction = 1;
  cfg.one_way_bps = 10; cfg.annual_borrow_bps = 252;
  auto result = st::replay_targets(f.input(), cfg); ASSERT_TRUE(result);
  const auto& first = result->days[0];
  EXPECT_EQ(first.entry, 1U); EXPECT_EQ(first.endpoint, 2U);
  EXPECT_TRUE(first.return_mature); EXPECT_FALSE(first.return_complete);
  EXPECT_NEAR(first.observed_return_component, .05, 1e-14);
  EXPECT_EQ(first.missing_long, 0); EXPECT_EQ(first.missing_short, .5);
  EXPECT_EQ(first.missing_gross, .5); EXPECT_EQ(first.missing_names, 1U);
  EXPECT_TRUE(std::isnan(first.complete_net_return));
  EXPECT_NEAR(first.modeled_trade_cost, .001, 1e-16); // deployment included
  EXPECT_NEAR(first.modeled_borrow_cost, .00005, 1e-16);
  EXPECT_FALSE(result->days[3].return_mature); // no invented tail return
  f.present[4] = 1; f.close[4] = 120; f.raw[4] = 100;
  auto guarded = st::replay_targets(f.input(), cfg); ASSERT_TRUE(guarded);
  EXPECT_EQ(guarded->days[0].guarded_names, 1U); // adjusted-only >.10 log jump
  EXPECT_EQ(guarded->days[0].missing_short, .5);
  f.close[4] = 90; f.raw[4] = 90;
  auto complete = st::replay_targets(f.input(), cfg); ASSERT_TRUE(complete);
  EXPECT_TRUE(complete->days[0].return_complete);
  EXPECT_NEAR(complete->days[0].complete_gross_return, .10, 1e-14);
  EXPECT_NEAR(complete->days[0].complete_net_return, .10 - .001 - .00005, 1e-14);
}

TEST(StrategyTargetReplay, PinnedArtifactRunsWithoutPricesAndRefusesTamperingBeforePublication) {
  Directory dir; Fixture f(6, 3);
  for (usize d = 0; d < f.d; ++d) for (usize i = 0; i < f.n; ++i)
    f.signal[d * f.n + i] = static_cast<f64>(i);
  auto cfg = artifact(dir.path, f); std::ostringstream progress;
  ASSERT_TRUE(st::run_target_replay(cfg, progress));
  std::ifstream input(dir.path / "out" / "summary.json"); Json summary; input >> summary;
  EXPECT_EQ(summary.at("status"), "complete"); EXPECT_TRUE(summary.at("net_sharpe").is_null());
  EXPECT_EQ(summary.at("complete_return_days"), 0);
  EXPECT_EQ(summary.at("daily_csv_sha256"),
            co::sha256_file((dir.path / "out" / "daily.csv").string()).value());
  EXPECT_DOUBLE_EQ(summary.at("monthly_reconciled_total").get<f64>(),
                   summary.at("total_turnover").get<f64>());
  EXPECT_FALSE(st::run_target_replay(cfg, progress)); // exclusive output
  cfg.output_directory = (dir.path / "tampered").string();
  std::fstream payload(dir.path / "train_combined.f64", std::ios::binary | std::ios::in | std::ios::out);
  const char changed = 1; payload.write(&changed, 1); payload.close();
  EXPECT_FALSE(st::run_target_replay(cfg, progress));
  EXPECT_FALSE(std::filesystem::exists(dir.path / "tampered"));
}

TEST(StrategyTargetReplay, AdmissionAndFlatSignalDoNotInventExposure) {
  Fixture f(5, 4); auto cfg = st::TargetReplayConfig{};
  auto flat = st::replay_targets(f.input(), cfg); ASSERT_TRUE(flat);
  for (const auto& d : flat->days) { EXPECT_EQ(d.gross, 0); EXPECT_EQ(d.turnover, 0); }
  cfg.max_working_bytes = 1; EXPECT_FALSE(st::replay_targets(f.input(), cfg));
  cfg = {}; f.member[0] = 2; EXPECT_FALSE(st::replay_targets(f.input(), cfg));
  f.member[0] = 1; f.signal[0] = missing; EXPECT_FALSE(st::replay_targets(f.input(), cfg));
  f.signal[0] = 0; f.sessions[1] = f.sessions[0]; EXPECT_FALSE(st::replay_targets(f.input(), cfg));
  Directory dir; Fixture small(3, 2); auto run = artifact(dir.path, small);
  run.target.max_working_bytes = 64ULL << 20;
  std::filesystem::remove(dir.path / "train_combined.f64");
  std::ostringstream progress; auto refused = st::run_target_replay(run, progress);
  ASSERT_FALSE(refused); EXPECT_EQ(refused.error().code(), co::ErrorCode::OutOfRange);
  EXPECT_FALSE(std::filesystem::exists(dir.path / "out")); // admitted before payload I/O
}

TEST(StrategyTargetReplay, RoughReturnsRespectDeclaredWindowEvenWithFuturePayload) {
  Fixture f(7, 2);
  for (usize d = 0; d < f.d; ++d) { f.signal[2 * d] = -1; f.signal[2 * d + 1] = 1; }
  f.close.assign(14, 100); f.raw = f.close; f.present.assign(14, 1);
  auto input = f.input(); input.decision_end = 4;
  auto before = st::replay_targets(input, {}); ASSERT_TRUE(before);
  ASSERT_EQ(before->days.size(), 4U);
  EXPECT_TRUE(before->days[0].return_mature); EXPECT_TRUE(before->days[1].return_mature);
  EXPECT_FALSE(before->days[2].return_mature); EXPECT_FALSE(before->days[3].return_mature);
  for (usize k = 8; k < f.close.size(); ++k) {
    f.close[k] = 1000 + static_cast<f64>(k); f.raw[k] = 1; f.present[k] = 0;
  }
  input = f.input(); input.decision_end = 4;
  auto after = st::replay_targets(input, {}); ASSERT_TRUE(after);
  for (usize d = 0; d < 4; ++d) {
    EXPECT_EQ(before->days[d].return_mature, after->days[d].return_mature);
    EXPECT_EQ(before->days[d].return_complete, after->days[d].return_complete);
    EXPECT_EQ(before->days[d].observed_return_component, after->days[d].observed_return_component);
    EXPECT_EQ(before->days[d].turnover, after->days[d].turnover);
  }
}

TEST(StrategyTargetReplay, OptionalPinnedPriceRolePreservesMissingExposureAndRequiresExactBinding) {
  Directory dir; Fixture f(5, 2);
  for (usize d = 0; d < f.d; ++d) { f.signal[2 * d] = -1; f.signal[2 * d + 1] = 1; }
  f.close.assign(10, 100); f.raw = f.close; f.present.assign(10, 1);
  // Missing future endpoint is absent in both saved decision support and source
  // prices; it must not alter the already chosen first decision's short holding.
  f.member[4] = 0; f.signal[4] = missing;
  f.present[4] = 0; f.close[4] = missing; f.raw[4] = missing;
  auto cfg = artifact(dir.path, f);
  Json files;
  files["sessions.i64"] = write_payload(dir.path / "sessions.i64", f.sessions);
  files["ids.u64"] = write_payload(dir.path / "ids.u64", f.ids);
  files["close.f64"] = write_payload(dir.path / "close.f64", f.close);
  files["raw_close.f64"] = write_payload(dir.path / "raw_close.f64", f.raw);
  files["present.u8"] = write_payload(dir.path / "present.u8", f.present);
  files["member.u8"] = write_payload(dir.path / "member.u8", f.member);
  Json role{{"schema", "atx.recent-research-role/v1"}, {"status", "complete"},
      {"source_sha256", std::string(64, 'a')}, {"instrument_namespace", "spiderrock.securityID"},
      {"close_basis", "f64(raw-f32-close)*f64-cumulReturnFactor"},
      {"clock_recipe", "modeled-session+22h-mark+23h-decision-v1"},
      {"common_stock_verified", false}, {"historical_vintage_verified", false},
      {"dates", f.d}, {"instruments", f.n}, {"score_begin", 0}, {"score_end", f.d},
      {"files", std::move(files)}};
  cfg.role_path = (dir.path / "role.json").string(); cfg.role_sha256 = write_json(cfg.role_path, role);
  std::ifstream saved(cfg.combined_path); Json manifest; saved >> manifest; saved.close();
  manifest["role_manifest_sha256"] = cfg.role_sha256;
  cfg.combined_sha256 = write_json(cfg.combined_path, manifest);
  std::ostringstream progress; ASSERT_TRUE(st::run_target_replay(cfg, progress));
  std::ifstream report(dir.path / "out" / "summary.json"); Json summary; report >> summary;
  EXPECT_GT(summary.at("incomplete_return_days").get<usize>(), 0U);
  EXPECT_GT(summary.at("summed_missing_gross_exposure").get<f64>(), 0);
  EXPECT_TRUE(summary.at("net_sharpe").is_null());
  role["source_sha256"] = std::string(64, 'b');
  cfg.role_sha256 = write_json(cfg.role_path, role);
  cfg.output_directory = (dir.path / "wrong_role").string();
  EXPECT_FALSE(st::run_target_replay(cfg, progress));
  EXPECT_FALSE(std::filesystem::exists(dir.path / "wrong_role"));
}

// T4 (b): at a rebalance decision the desired target is the tied-rank target
// neutralized against that decision's own exposures (computed from the role at d):
// ~0 intercept/beta/vol/log-ADV moments, gross preserved, the recorded
// amplification is entry/residual gross, and the replay carries that record.
TEST(StrategyTargetReplay, PriceRiskNeutralizedTargetHasZeroExposureAndRecordsAmplification) {
  const Role role(70, 12, 21);
  const auto cfg = neutral_daily();
  constexpr usize d = 60;
  const auto in = role.input();
  // Independent construction from the tied rank and the T3 primitives.
  std::vector<std::pair<f64, usize>> row;
  std::vector<f64> ranked(role.n);
  st::detail::desired_target(std::span<const f64>(role.signal).subspan(d * role.n, role.n),
                             std::span<const u8>(role.member).subspan(d * role.n, role.n), row,
                             ranked);
  constexpr usize cols = st::kPriceExposureCount;
  std::vector<f64> exposures(role.n * cols);
  std::vector<u8> ok(role.n);
  st::PriceExposureScratch exposure_scratch;
  ASSERT_TRUE(st::compute_price_exposures(role.prices(), cfg.price_risk, d, exposure_scratch,
                                          exposures, ok));
  for (const u8 flag : ok) ASSERT_EQ(flag, 1);
  auto expected = ranked;
  const std::vector<u8> members(role.n, 1);
  st::NeutralizeScratch neutralize_scratch;
  st::NeutralizeStats stats;
  ASSERT_TRUE(st::neutralize_target(expected, members, exposures, ok, cfg.price_risk,
                                    neutralize_scratch, stats));
  // The shared construction step both replays call.
  std::vector<f64> desired(role.n);
  st::PriceRiskScratch scratch;
  st::ConstructionDay record;
  const auto rebalance = st::detail::form_desired(in, cfg, d, row, desired, scratch, record);
  ASSERT_TRUE(rebalance);
  EXPECT_TRUE(*rebalance);
  for (usize i = 0; i < role.n; ++i) EXPECT_EQ(bits(desired[i]), bits(expected[i])) << i;
  EXPECT_EQ(record.neutralize, st::NeutralizeOutcome::Applied);
  EXPECT_EQ(record.neutralize_used, 12U);
  EXPECT_EQ(record.neutralize_excluded, 0U);
  EXPECT_EQ(record.neutralize_excluded_share, 0);
  EXPECT_EQ(bits(record.neutralize_amplification), bits(stats.gross / stats.residual_gross));
  EXPECT_GT(record.neutralize_amplification, 0);
  EXPECT_LE(record.neutralize_amplification, cfg.neutralize_max_amplification);
  // ~0 exposure on the clipped z design rebuilt from the T3 contract (mean and
  // sample SD over the used rows, clip +-clip_z).
  f64 gross = 0;
  std::array<f64, cols + 1> moment{};
  for (usize i = 0; i < role.n; ++i) { gross += std::abs(desired[i]); moment[0] += desired[i]; }
  for (usize k = 0; k < cols; ++k) {
    f64 sum = 0, squares = 0;
    for (usize i = 0; i < role.n; ++i) sum += exposures[i * cols + k];
    const f64 mean = sum / static_cast<f64>(role.n);
    for (usize i = 0; i < role.n; ++i)
      squares += (exposures[i * cols + k] - mean) * (exposures[i * cols + k] - mean);
    const f64 sd = std::sqrt(squares / static_cast<f64>(role.n - 1));
    for (usize i = 0; i < role.n; ++i)
      moment[k + 1] += desired[i] * std::clamp((exposures[i * cols + k] - mean) / sd,
                                               -cfg.price_risk.clip_z, cfg.price_risk.clip_z);
  }
  EXPECT_NEAR(gross, 1.0, 1e-12);
  for (usize k = 0; k < moment.size(); ++k) EXPECT_LE(std::abs(moment[k]), 1e-12) << k;
  // The replay records the same decision and trades to it in full (fraction 1).
  auto replay = st::replay_targets(in, cfg);
  ASSERT_TRUE(replay) << replay.error().to_string();
  const auto& day = replay->days[d];
  EXPECT_TRUE(day.construction.rebalance);
  EXPECT_EQ(day.construction.neutralize, st::NeutralizeOutcome::Applied);
  EXPECT_EQ(day.construction.neutralize_used, 12U);
  EXPECT_EQ(bits(day.construction.neutralize_amplification),
            bits(record.neutralize_amplification));
  EXPECT_NEAR(day.gross, 1.0, 1e-12);
  EXPECT_NEAR(day.net, 0.0, 1e-12);
  // Before a full beta window no name has exposures: too few names, no trade.
  for (usize t = 0; t < 20; ++t) {
    EXPECT_EQ(replay->days[t].construction.neutralize,
              st::NeutralizeOutcome::SkippedTooFewNames) << t;
    EXPECT_EQ(replay->days[t].construction.neutralize_used, 0U) << t;
    EXPECT_EQ(replay->days[t].gross, 0) << t;
  }
  EXPECT_EQ(count_outcome(*replay, st::NeutralizeOutcome::SkippedTooFewNames), 20U);
}

// T4 (c): a refused neutralization (here too few usable names while two held names
// sit out of membership) skips the rebalance: every member keeps its weight, the
// leavers are still exited, and the skip is counted by reason. An amplification
// cap below every entry/residual ratio skips every otherwise-applied rebalance.
TEST(StrategyTargetReplay, NeutralizeSkipKeepsWeightsAppliesForcedExitsAndCounts) {
  Role role(70, 12, 21);
  for (usize t = 40; t < 45; ++t) { role.nonmember(t, 0); role.nonmember(t, 1); }
  auto cfg = neutral_daily();
  cfg.price_risk.min_names = 11; // 10 members at decisions 40-44
  auto r = st::replay_targets(role.input(), cfg);
  ASSERT_TRUE(r) << r.error().to_string();
  EXPECT_EQ(count_outcome(*r, st::NeutralizeOutcome::SkippedTooFewNames), 25U);
  for (usize t = 0; t < 20; ++t) {
    EXPECT_FALSE(r->days[t].construction.rebalance) << t;
    EXPECT_EQ(r->days[t].turnover, 0) << t;
    EXPECT_EQ(r->days[t].gross, 0) << t;
  }
  ASSERT_TRUE(r->days[39].construction.rebalance); // neutralized and traded
  const auto& leave = r->days[40];
  EXPECT_EQ(leave.construction.neutralize, st::NeutralizeOutcome::SkippedTooFewNames);
  EXPECT_EQ(leave.construction.neutralize_used, 10U);
  EXPECT_FALSE(leave.construction.rebalance);
  EXPECT_GT(leave.forced_turnover, 0);        // the leavers are exited anyway
  EXPECT_EQ(leave.discretionary_turnover, 0); // every member keeps its weight
  EXPECT_EQ(leave.applied_fraction, 0);
  EXPECT_NEAR(leave.gross, r->days[39].gross - leave.forced_turnover, 1e-12);
  for (usize t = 41; t < 45; ++t) {
    EXPECT_EQ(r->days[t].construction.neutralize, st::NeutralizeOutcome::SkippedTooFewNames);
    EXPECT_EQ(r->days[t].turnover, 0) << t;
    EXPECT_EQ(bits(r->days[t].gross), bits(leave.gross)) << t;
  }
  const Role plain(70, 12, 21);
  auto capped = neutral_daily();
  capped.neutralize_max_amplification = 1e-6;
  auto none = st::replay_targets(plain.input(), capped);
  ASSERT_TRUE(none) << none.error().to_string();
  EXPECT_EQ(none->total_turnover, 0);
  EXPECT_EQ(count_outcome(*none, st::NeutralizeOutcome::Applied), 0U);
  EXPECT_GT(count_outcome(*none, st::NeutralizeOutcome::SkippedAmplification), 0U);
  for (const auto& day : none->days) {
    EXPECT_FALSE(day.construction.rebalance);
    if (day.construction.neutralize == st::NeutralizeOutcome::SkippedAmplification) {
      EXPECT_GT(day.construction.neutralize_amplification, capped.neutralize_max_amplification);
    }
  }
}

// T4 (d): the no-trade band keeps members whose move is within band_multiple/N_d
// and moves the others by the rule's fraction; monthly-budget-v2 leaves banded
// names out of its distance. Hand-computed on four names (band 0.6/4 = .15).
TEST(StrategyTargetReplay, NoTradeBandKeepsSmallMovesAndLeavesThemOutOfTheBudgetDistance) {
  Fixture f(3, 4);
  const f64 a[] = {1, 2, 3, 4}, e[] = {1, 2, 4, 3};
  for (usize i = 0; i < 4; ++i) {
    f.signal[i] = a[i]; f.signal[4 + i] = e[i]; f.signal[8 + i] = e[i];
  }
  st::TargetReplayConfig base;
  base.cadence = 1; base.trade_fraction = 1; base.band_multiple = 0.6;
  auto r = st::replay_targets(f.input(), base); ASSERT_TRUE(r);
  // desired [-.375 -.125 .125 .375] from flat: the two .125 moves stay banded at 0.
  EXPECT_EQ(r->days[0].construction.banded_names, 2U);
  EXPECT_EQ(r->days[0].turnover, 0.75); EXPECT_EQ(r->days[0].gross, 0.75);
  EXPECT_EQ(r->days[0].net, 0);
  // desired [-.375 -.125 .375 .125] from [-.375 0 0 .375]: gaps 0 and .125 are
  // banded; .375 and .25 move in full.
  EXPECT_TRUE(r->days[1].construction.rebalance);
  EXPECT_EQ(r->days[1].construction.banded_names, 2U);
  EXPECT_EQ(r->days[1].turnover, 0.625); EXPECT_EQ(r->days[1].gross, 0.875);
  EXPECT_EQ(r->days[1].net, 0.125);
  auto v2 = base; v2.rule = st::TargetReplayRule::MonthlyTargetBudgetV2; v2.monthly_budget = .5;
  auto budget = st::replay_targets(f.input(), v2); ASSERT_TRUE(budget);
  // distance .75 (banded names excluded): fraction .5/.75, the two tails move 2/3.
  EXPECT_EQ(budget->days[0].construction.banded_names, 2U);
  EXPECT_NEAR(budget->days[0].applied_fraction, 2.0 / 3.0, 1e-15);
  EXPECT_NEAR(budget->days[0].turnover, .5, 1e-15);
  EXPECT_NEAR(budget->days[0].gross, .5, 1e-15);
  auto unbanded = v2; unbanded.band_multiple = 0;
  auto plain = st::replay_targets(f.input(), unbanded); ASSERT_TRUE(plain);
  EXPECT_EQ(plain->days[0].applied_fraction, .5); // distance 1 over all four names
  EXPECT_EQ(plain->days[0].construction.banded_names, 0U);
  st::TargetReplayConfig bad; bad.band_multiple = -1;
  EXPECT_FALSE(st::replay_targets(f.input(), bad));
}

// T4 (a)/CLI: the default run keeps the pre-T4 recipe keys, rule and CSV columns
// byte-for-byte; a non-default option suffixes the rule id and adds its recipe
// keys, CSV columns and summary diagnostics; price-risk neutralization needs the
// role, and an unknown --neutralize spelling is a usage error.
TEST(StrategyTargetReplay, ConstructionOptionsAreRecordedOnlyWhenNonDefault) {
  Fixture f(6, 3);
  for (usize d = 0; d < f.d; ++d) for (usize i = 0; i < f.n; ++i)
    f.signal[d * f.n + i] = static_cast<f64>((i + d) % 3);
  std::ostringstream progress;
  {
    Directory dir; auto cfg = artifact(dir.path, f);
    ASSERT_TRUE(st::run_target_replay(cfg, progress));
    const auto recipe = read_json(dir.path / "out" / "recipe.json");
    EXPECT_EQ(recipe.at("rule"), "baseline-target-v1");
    for (const auto* key : {"neutralize", "band_multiple", "price_risk", "neutralize_guard",
                            "band", "desired_target_postprocess"})
      EXPECT_FALSE(recipe.contains(key)) << key;
    EXPECT_EQ(first_line(dir.path / "out" / "daily.csv"), default_daily_header);
    EXPECT_FALSE(read_json(dir.path / "out" / "summary.json").contains("construction"));
  }
  {
    Directory dir; auto cfg = artifact(dir.path, f); cfg.target.band_multiple = 0.5;
    ASSERT_TRUE(st::run_target_replay(cfg, progress));
    const auto recipe = read_json(dir.path / "out" / "recipe.json");
    EXPECT_EQ(recipe.at("rule"), "baseline-target-v1+band-0.5");
    EXPECT_EQ(recipe.at("band_multiple"), 0.5);
    EXPECT_FALSE(recipe.contains("neutralize"));
    EXPECT_EQ(first_line(dir.path / "out" / "daily.csv"),
              std::string(default_daily_header) +
                  ",rebalance,neutralize,neutralize_used,neutralize_excluded,"
                  "neutralize_excluded_share,neutralize_amplification,banded_names");
    const auto construction = read_json(dir.path / "out" / "summary.json").at("construction");
    EXPECT_EQ(construction.at("rule_id"), "baseline-target-v1+band-0.5");
    EXPECT_EQ(construction.at("decisions"), 6);
    EXPECT_EQ(construction.at("neutralize_attempted_decisions"), 0);
    EXPECT_EQ(construction.at("neutralize_skipped_decisions"), 0);
  }
  {
    Directory dir; auto cfg = artifact(dir.path, f);
    cfg.target.neutralize = st::TargetNeutralize::PriceRiskV1;
    const auto refused = st::run_target_replay(cfg, progress);
    ASSERT_FALSE(refused); EXPECT_EQ(refused.error().code(), co::ErrorCode::InvalidArgument);
    EXPECT_FALSE(std::filesystem::exists(dir.path / "out"));
  }
  std::ostringstream out, err;
  std::vector<std::string> args{"targets", "--neutralize", "bogus"};
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  EXPECT_EQ(st::dispatch_target_replay(static_cast<int>(argv.size()), argv.data(), out, err), 2);
}

// ---- aim-partial-v5 (T30) ----
namespace {
// Random member signals (distinct ranks) on consecutive daily sessions, no prices.
Fixture random_fixture(usize dates, usize names, u64 seed) {
  Fixture f(dates, names);
  Lcg rng{seed};
  for (auto& s : f.signal) s = rng.uniform() - 0.5;
  return f;
}
// A role whose signal never changes (distinct per name): the desired target, hence
// the aim, is the same at every decision.
Fixture constant_signal(usize dates, usize names) {
  Fixture f(dates, names);
  for (usize t = 0; t < dates; ++t)
    for (usize i = 0; i < names; ++i)
      f.signal[t * names + i] = static_cast<f64>((i * 7) % names);
  return f;
}
st::TargetReplayConfig aim_partial(f64 theta, f64 dust, f64 leverage = 1) {
  st::TargetReplayConfig c;
  c.rule = st::TargetReplayRule::AimPartialV5; c.cadence = 1;
  c.trade_fraction = theta; c.dust_multiple = dust; c.aim_leverage = leverage;
  return c;
}
void expect_same_target_day(const st::TargetReplayDay& a, const st::TargetReplayDay& b,
                            usize d) {
  EXPECT_EQ(bits(a.turnover), bits(b.turnover)) << d;
  EXPECT_EQ(bits(a.forced_turnover), bits(b.forced_turnover)) << d;
  EXPECT_EQ(bits(a.discretionary_turnover), bits(b.discretionary_turnover)) << d;
  EXPECT_EQ(bits(a.deployment_turnover), bits(b.deployment_turnover)) << d;
  EXPECT_EQ(bits(a.month_turnover), bits(b.month_turnover)) << d;
  EXPECT_EQ(bits(a.budget_excess), bits(b.budget_excess)) << d;
  EXPECT_EQ(bits(a.applied_fraction), bits(b.applied_fraction)) << d;
  EXPECT_EQ(bits(a.gross), bits(b.gross)) << d;
  EXPECT_EQ(bits(a.net), bits(b.net)) << d;
  EXPECT_EQ(bits(a.long_weight), bits(b.long_weight)) << d;
  EXPECT_EQ(bits(a.short_weight), bits(b.short_weight)) << d;
  EXPECT_EQ(bits(a.max_abs_weight), bits(b.max_abs_weight)) << d;
  EXPECT_EQ(bits(a.effective_names), bits(b.effective_names)) << d;
  EXPECT_EQ(a.held_names, b.held_names) << d;
  EXPECT_EQ(a.construction.rebalance, b.construction.rebalance) << d;
  EXPECT_EQ(a.construction.banded_names, b.construction.banded_names) << d;
}
} // namespace

// Review focus 1: theta 1, dust 0, aim_leverage 1 is baseline-v1 (band 0, fraction 1)
// bit for bit: every day field, and the weights themselves through the shared seam,
// with forced exits (random nonmember cells) and on non-rebalance days (cadence 3).
TEST(TargetReplayV5, AimPartialV5_ThetaOne_MatchesBaseline) {
  auto f = random_fixture(30, 50, 11);
  Lcg drop{12};
  for (usize k = 0; k < f.signal.size(); ++k)
    if (drop.uniform() < 0.1) { f.member[k] = 0; f.signal[k] = missing; }
  for (const usize cadence : {usize{1}, usize{3}}) {
    st::TargetReplayConfig base; base.cadence = cadence; base.trade_fraction = 1;
    auto v5 = aim_partial(1, 0, 1); v5.cadence = cadence;
    const auto a = st::replay_targets(f.input(), base);
    const auto b = st::replay_targets(f.input(), v5);
    ASSERT_TRUE(a) << a.error().to_string();
    ASSERT_TRUE(b) << b.error().to_string();
    ASSERT_EQ(a->days.size(), b->days.size());
    for (usize d = 0; d < a->days.size(); ++d) expect_same_target_day(a->days[d], b->days[d], d);
    EXPECT_EQ(bits(a->total_turnover), bits(b->total_turnover)) << cadence;
    EXPECT_EQ(bits(a->forced_turnover), bits(b->forced_turnover)) << cadence;
    EXPECT_EQ(bits(a->discretionary_turnover), bits(b->discretionary_turnover)) << cadence;
    EXPECT_EQ(bits(a->deployment_turnover), bits(b->deployment_turnover)) << cadence;
    EXPECT_EQ(a->deployment_date, b->deployment_date) << cadence;
    EXPECT_GT(a->forced_turnover, 0) << cadence; // exits are exercised
  }
  const auto in = f.input();
  const auto v5 = aim_partial(1, 0, 1);
  const st::TargetReplayConfig base = [] {
    st::TargetReplayConfig c; c.cadence = 1; c.trade_fraction = 1; return c;
  }();
  std::vector<std::pair<f64, usize>> row;
  std::vector<f64> desired(f.n), wa(f.n), wb(f.n);
  for (usize d = 0; d < f.d; ++d) {
    st::detail::desired_target(std::span<const f64>(f.signal).subspan(d * f.n, f.n),
                               std::span<const u8>(f.member).subspan(d * f.n, f.n), row,
                               desired);
    st::TargetReplayDay da, db;
    ASSERT_TRUE(st::detail::update_weights(in, base, d, true, 0, desired, wa, da));
    ASSERT_TRUE(st::detail::update_weights(in, v5, d, true, 0, desired, wb, db));
    for (usize i = 0; i < f.n; ++i) EXPECT_EQ(bits(wa[i]), bits(wb[i])) << d << ' ' << i;
  }
}

// Review focus 2: the dust band never blocks entry. From flat, every member whose
// |aim| exceeds dust_multiple / N_d trades on the first rebalance; only the members
// whose |desired| <= 0.1/N (tied ranks spread desired over [-2/N, 2/N]: here 4-6 of
// 100) stay flat. Gross lies in [0.9, 1] x theta x G, G the un-dusted aim's gross.
TEST(TargetReplayV5, AimPartialV5_DustDoesNotBlockEntry) {
  const auto f = random_fixture(10, 100, 21);
  const auto v5 = aim_partial(0.05, 0.1);
  const auto r = st::replay_targets(f.input(), v5);
  ASSERT_TRUE(r) << r.error().to_string();
  const auto u = st::replay_targets(f.input(), aim_partial(0.05, 0));
  ASSERT_TRUE(u) << u.error().to_string();
  const f64 capped = u->days[0].gross; // theta * G with no dust
  EXPECT_NEAR(capped, 0.05, 1e-12);    // G = 1: gross-1 desired, aim_leverage 1
  const auto& first = r->days[0];
  EXPECT_GE(first.held_names, f.members_at(0) * 9 / 10);
  EXPECT_EQ(first.held_names + first.construction.banded_names, f.members_at(0));
  EXPECT_GT(first.construction.banded_names, 0U);
  EXPECT_GE(first.gross, 0.9 * capped);
  EXPECT_LE(first.gross, capped);
  // Name by name: |aim| > dust / N_d  <=>  entered at theta * aim.
  const auto in = f.input();
  std::vector<std::pair<f64, usize>> row;
  std::vector<f64> desired(f.n), w(f.n);
  st::detail::desired_target(std::span<const f64>(f.signal).subspan(0, f.n),
                             std::span<const u8>(f.member).subspan(0, f.n), row, desired);
  st::TargetReplayDay day;
  ASSERT_TRUE(st::detail::update_weights(in, v5, 0, true, 0, desired, w, day));
  const f64 dust = 0.1 / static_cast<f64>(f.members_at(0));
  for (usize i = 0; i < f.n; ++i) {
    if (std::abs(desired[i]) > dust) {
      EXPECT_EQ(bits(w[i]), bits(0.05 * desired[i])) << i;
    } else {
      EXPECT_EQ(w[i], 0) << i;
    }
  }
}

// A constant aim is approached geometrically, gross_t = L (1 - (1 - theta)^(t+1)), and
// reached (80 steps at theta .25); the dust band stops the approach within
// dust_multiple of the aim's gross, after which nothing trades.
TEST(TargetReplayV5, AimPartialV5_ConvergesToAimGross) {
  const auto f = constant_signal(80, 40);
  const auto r = st::replay_targets(f.input(), aim_partial(0.25, 0));
  ASSERT_TRUE(r) << r.error().to_string();
  EXPECT_NEAR(r->days[0].gross, 0.25, 1e-12);
  EXPECT_NEAR(r->days[1].gross, 1 - 0.75 * 0.75, 1e-12);
  EXPECT_NEAR(r->days.back().gross, 1.0, 1e-6);
  EXPECT_NEAR(r->days.back().net, 0.0, 1e-12);
  EXPECT_EQ(r->days.back().held_names, 40U);
  const auto levered = st::replay_targets(f.input(), aim_partial(0.25, 0, 1.5));
  ASSERT_TRUE(levered) << levered.error().to_string();
  EXPECT_NEAR(levered->days[0].gross, 0.375, 1e-12);
  EXPECT_NEAR(levered->days.back().gross, 1.5, 1e-6);
  const auto dusted = st::replay_targets(f.input(), aim_partial(0.25, 0.1));
  ASSERT_TRUE(dusted) << dusted.error().to_string();
  const auto& last = dusted->days.back();
  EXPECT_GE(last.gross, 0.9);
  EXPECT_LE(last.gross, 1.0 + 1e-12);
  EXPECT_EQ(last.construction.banded_names, 40U);
  EXPECT_EQ(last.turnover, 0);
}

TEST(TargetReplayV5, RefusesBandUnderV5) {
  const auto f = random_fixture(5, 4, 3);
  auto bad = aim_partial(0.25, 0); bad.band_multiple = 1;
  const auto refused = st::replay_targets(f.input(), bad);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), co::ErrorCode::InvalidArgument);
  bad.band_multiple = 0;
  EXPECT_TRUE(st::replay_targets(f.input(), bad)); // control
}

// R-e: aim_leverage in [1, 2], dust_multiple in [0, 0.5], theta in (0, 1] (NaN
// refused); the v5 parameters are refused under the other rules.
TEST(TargetReplayV5, RefusesOutOfRangeAndForeignParameters) {
  const auto f = random_fixture(5, 4, 3);
  const auto runs = [&](const st::TargetReplayConfig& c) {
    return static_cast<bool>(st::replay_targets(f.input(), c));
  };
  EXPECT_TRUE(runs(aim_partial(1, 0.5, 2)));
  EXPECT_TRUE(runs(aim_partial(0.01, 0, 1)));
  for (const f64 leverage : {0.99, 2.01, missing})
    EXPECT_FALSE(runs(aim_partial(0.5, 0, leverage)));
  for (const f64 dust : {-0.01, 0.51, missing}) EXPECT_FALSE(runs(aim_partial(0.5, dust, 1)));
  for (const f64 theta : {0.0, 1.01, missing}) EXPECT_FALSE(runs(aim_partial(theta, 0, 1)));
  st::TargetReplayConfig baseline; baseline.aim_leverage = 1.5;
  EXPECT_FALSE(runs(baseline));
  baseline.aim_leverage = 1; baseline.dust_multiple = 0.1;
  EXPECT_FALSE(runs(baseline));
  st::TargetReplayConfig v2; v2.rule = st::TargetReplayRule::MonthlyTargetBudgetV2;
  v2.aim_leverage = 1.2;
  EXPECT_FALSE(runs(v2));
  v2.aim_leverage = 1;
  EXPECT_TRUE(runs(v2));
}

// The pinned run and CLI: v5 records its recipe keys (rule aim-partial-v5, theta,
// dust_multiple, aim_leverage, rate fixed), the construction CSV columns (banded_names
// = dust count) and summary construction.v5; baseline carries none of them.
TEST(TargetReplayV5, PinnedRunRecordsRecipeSummaryAndCli) {
  const auto f = random_fixture(8, 6, 5);
  std::ostringstream progress;
  constexpr std::array<const char*, 5> v5_keys{"theta", "dust_multiple", "aim_leverage", "rate",
                                               "aim_partial"};
  {
    Directory dir; auto cfg = artifact(dir.path, f);
    ASSERT_TRUE(st::run_target_replay(cfg, progress));
    const auto recipe = read_json(dir.path / "out" / "recipe.json");
    for (const auto* key : v5_keys) EXPECT_FALSE(recipe.contains(key)) << key;
    EXPECT_EQ(recipe.at("rule"), "baseline-target-v1");
    EXPECT_EQ(first_line(dir.path / "out" / "daily.csv"), default_daily_header);
    EXPECT_FALSE(read_json(dir.path / "out" / "summary.json").contains("construction"));
  }
  Directory dir; const auto cfg = artifact(dir.path, f);
  std::ostringstream out, err;
  const auto dispatch = [&](std::vector<std::string> args) {
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    return st::dispatch_target_replay(static_cast<int>(argv.size()), argv.data(), out, err);
  };
  const auto base_args = [&](const std::string& output) {
    return std::vector<std::string>{"targets", "--combined", cfg.combined_path,
                                    "--combined-sha256", cfg.combined_sha256, "--output",
                                    (dir.path / output).string(), "--cadence", "1"};
  };
  auto args = base_args("v5");
  for (const auto* extra : {"--rule", "aim-partial-v5", "--trade-fraction", ".5",
                            "--dust-multiple", ".1", "--aim-leverage", "1.5"})
    args.emplace_back(extra);
  ASSERT_EQ(dispatch(args), 0) << err.str();
  const auto recipe = read_json(dir.path / "v5" / "recipe.json");
  EXPECT_EQ(recipe.at("rule"), "aim-partial-v5");
  EXPECT_EQ(recipe.at("theta"), 0.5);
  EXPECT_EQ(recipe.at("trade_fraction"), 0.5);
  EXPECT_EQ(recipe.at("dust_multiple"), 0.1);
  EXPECT_EQ(recipe.at("aim_leverage"), 1.5);
  EXPECT_EQ(recipe.at("rate"), "fixed");
  EXPECT_TRUE(recipe.at("aim_partial").is_string());
  EXPECT_FALSE(recipe.contains("band_multiple"));
  EXPECT_EQ(first_line(dir.path / "v5" / "daily.csv"),
            std::string(default_daily_header) +
                ",rebalance,neutralize,neutralize_used,neutralize_excluded,"
                "neutralize_excluded_share,neutralize_amplification,banded_names");
  const auto summary = read_json(dir.path / "v5" / "summary.json");
  const auto& construction = summary.at("construction");
  EXPECT_EQ(construction.at("rule_id"), "aim-partial-v5");
  const auto& v5 = construction.at("v5");
  for (const auto* key : {"theta", "dust_multiple", "aim_leverage", "rate", "mean_gross",
                          "mean_net", "mean_held_share"})
    EXPECT_TRUE(v5.contains(key)) << key;
  EXPECT_EQ(v5.at("theta"), 0.5);
  EXPECT_EQ(v5.at("rate"), "fixed");
  EXPECT_EQ(v5.at("decisions"), f.d);
  EXPECT_GT(v5.at("mean_gross").get<f64>(), 0);
  EXPECT_LE(v5.at("mean_gross").get<f64>(), 1.5 + 1e-12);
  EXPECT_GT(v5.at("mean_held_share").get<f64>(), 0);
  EXPECT_LE(v5.at("mean_held_share").get<f64>(), 1);
  // Refusals: band under v5, v5 parameters under baseline, out-of-range leverage;
  // an unknown rule spelling is a usage error.
  auto band = base_args("band");
  for (const auto* extra : {"--rule", "aim-partial-v5", "--band-multiple", "1"})
    band.emplace_back(extra);
  EXPECT_EQ(dispatch(band), 1);
  auto foreign = base_args("foreign");
  for (const auto* extra : {"--dust-multiple", ".1"}) foreign.emplace_back(extra);
  EXPECT_EQ(dispatch(foreign), 1);
  auto levered = base_args("levered");
  for (const auto* extra : {"--rule", "aim-partial-v5", "--aim-leverage", "2.5"})
    levered.emplace_back(extra);
  EXPECT_EQ(dispatch(levered), 1);
  auto unknown = base_args("unknown");
  for (const auto* extra : {"--rule", "aim-partial-v6"}) unknown.emplace_back(extra);
  EXPECT_EQ(dispatch(unknown), 2);
  for (const auto* name : {"band", "foreign", "levered", "unknown"})
    EXPECT_FALSE(std::filesystem::exists(dir.path / name)) << name;
}

// T36 ruling R-c (T30 review Minor 2): a per-name rate span is used only under
// aim-partial-v5 and only with exactly one rate in [0, 1] per name (nonmember entries
// included); anything else is refused with InvalidArgument before a weight moves,
// never silently traded at the fixed theta. With a good span each member moves by its
// own rate and applied_fraction is the members' mean rate.
TEST(TargetReplayV5, PerNameRateSpanIsChecked) {
  auto f = random_fixture(3, 10, 41);
  f.member[3] = 0; f.signal[3] = missing; // one nonmember at decision 0
  const auto in = f.input();
  std::vector<std::pair<f64, usize>> row;
  std::vector<f64> desired(f.n);
  st::detail::desired_target(std::span<const f64>(f.signal).subspan(0, f.n),
                             std::span<const u8>(f.member).subspan(0, f.n), row, desired);
  const auto v5 = aim_partial(0.05, 0);
  std::vector<f64> rates(f.n);
  for (usize i = 0; i < f.n; ++i) rates[i] = 0.01 * static_cast<f64>(i + 1);
  std::vector<f64> w(f.n, 0.0);
  st::TargetReplayDay day;
  const auto ok = st::detail::update_weights(in, v5, 0, true, 0, desired, w, day, rates);
  ASSERT_TRUE(ok) << ok.error().to_string();
  f64 sum = 0;
  usize members = 0;
  for (usize i = 0; i < f.n; ++i) {
    if (!f.member[i]) { EXPECT_EQ(w[i], 0) << i; continue; }
    EXPECT_EQ(bits(w[i]), bits(rates[i] * (1.0 * desired[i]))) << i; // from flat: theta_i * aim
    sum += rates[i]; ++members;
  }
  EXPECT_EQ(members, f.n - 1);
  EXPECT_EQ(bits(day.applied_fraction), bits(sum / static_cast<f64>(members)));
  const auto refused = [&](const st::TargetReplayConfig& c, std::span<const f64> per_name) {
    std::vector<f64> weights(f.n, 0.25);
    st::TargetReplayDay record;
    const auto status = st::detail::update_weights(in, c, 0, true, 0, desired, weights, record,
                                                   per_name);
    const bool untouched = record.turnover == 0 &&
        std::all_of(weights.begin(), weights.end(), [](f64 x) { return x == 0.25; });
    return !status && status.error().code() == co::ErrorCode::InvalidArgument && untouched;
  };
  const std::vector<f64> short_span(f.n - 1, 0.05), long_span(f.n + 1, 0.05);
  EXPECT_TRUE(refused(v5, short_span));
  EXPECT_TRUE(refused(v5, long_span));
  st::TargetReplayConfig baseline; baseline.cadence = 1;
  EXPECT_TRUE(refused(baseline, rates)); // per-name rates are aim-partial-v5 only
  for (const f64 bad : {missing, -0.01, 1.01, std::numeric_limits<f64>::infinity()}) {
    auto broken = rates; broken[5] = bad;
    EXPECT_TRUE(refused(v5, broken)) << bad;
  }
  auto unused = rates; unused[3] = missing; // the nonmember's entry is checked too
  EXPECT_TRUE(refused(v5, unused));
  // Controls: an empty span is the fixed theta; the bounds 0 and 1 are admitted.
  std::vector<f64> fixed_weights(f.n, 0.0);
  st::TargetReplayDay fixed_day;
  ASSERT_TRUE(st::detail::update_weights(in, v5, 0, true, 0, desired, fixed_weights, fixed_day));
  EXPECT_EQ(fixed_day.applied_fraction, 0.05);
  auto bounds = rates; bounds[0] = 0; bounds[1] = 1;
  std::vector<f64> bound_weights(f.n, 0.0);
  st::TargetReplayDay bound_day;
  EXPECT_TRUE(st::detail::update_weights(in, v5, 0, true, 0, desired, bound_weights, bound_day,
                                         bounds));
}

// ---- v6 prereg C2: the nonmember exit rate ----
namespace {
st::TargetReplayConfig decaying(f64 theta, f64 dust, f64 exit_rate) {
  auto c = aim_partial(theta, dust);
  c.exit_rate = exit_rate;
  return c;
}
// Role(70, 20) with prices: name 0 always ranks top (a constant aim) and leaves
// membership while still present from decision 3 on; name 1 is absent (no price, not a
// member) at decision 3 only.
Role exit_role() {
  Role r(70, 20, 9);
  for (usize t = 0; t < r.d; ++t) r.signal[t * r.n] = 10.0;
  for (usize t = 3; t < r.d; ++t) r.nonmember(t, 0);
  const usize k = 3 * r.n + 1;
  r.present[k] = 0; r.member[k] = 0; r.signal[k] = missing;
  r.close[k] = missing; r.raw[k] = missing; r.volume[k] = missing;
  return r;
}
usize members_of(const Role& r, usize t) {
  usize count = 0;
  for (usize i = 0; i < r.n; ++i) count += r.member[t * r.n + i] ? 1U : 0U;
  return count;
}
} // namespace

// Exit rate .05 at theta 1, dust .1: name 0 (constant top aim, .5 / (100 / 19)) leaves
// membership while present at decision 3 and then moves current * (1 - .05) per decision,
// bit for bit, until |next| <= .1 / N_d sets it to exactly 0 (where it stays); name 1,
// absent at decision 3, exits at once. Forced turnover is exactly those moves.
TEST(TargetReplayV6, ExitRateDecaysPresentNonmemberAndSnapsInsideTheDustBand) {
  const auto r = exit_role();
  const auto in = r.input();
  const auto cfg = decaying(1, 0.1, 0.05);
  std::vector<std::pair<f64, usize>> row;
  std::vector<f64> desired(r.n), w(r.n, 0.0);
  usize snapped = 0;
  for (usize d = 0; d < r.d; ++d) {
    st::detail::desired_target(std::span<const f64>(r.signal).subspan(d * r.n, r.n),
                               std::span<const u8>(r.member).subspan(d * r.n, r.n), row,
                               desired);
    const f64 prev0 = w[0], prev1 = w[1];
    st::TargetReplayDay day;
    const auto ok = st::detail::update_weights(in, cfg, d, true, 0, desired, w, day);
    ASSERT_TRUE(ok) << ok.error().to_string();
    if (d < 3) {
      EXPECT_NEAR(w[0], 0.5 / (100.0 / 19.0), 1e-15) << d; // at its constant top aim
      EXPECT_EQ(day.forced_turnover, 0) << d;
      continue;
    }
    const f64 band = 0.1 / static_cast<f64>(members_of(r, d));
    f64 expected = prev0 * (1 - 0.05);
    if (std::abs(expected) <= band) expected = 0;
    EXPECT_EQ(bits(w[0]), bits(expected)) << d;
    f64 forced = std::abs(expected - prev0); // name order: name 0, then name 1
    if (d == 3) {
      EXPECT_EQ(w[1], 0); // absent: the immediate exit
      forced += std::abs(prev1);
    }
    EXPECT_EQ(bits(day.forced_turnover), bits(forced)) << d;
    if (w[0] == 0 && prev0 != 0) snapped = d;
  }
  // .095 x .95^k <= .1 / 19 first at k = 57: decision 59.
  EXPECT_EQ(snapped, 59U);
  EXPECT_EQ(w[0], 0);
  EXPECT_EQ(members_of(r, 3), 18U);
  EXPECT_EQ(members_of(r, 4), 19U);
}

// exit_rate 1 is the immediate exit bit for bit (every day field and total), and the
// decaying exit trades the same total forced turnover (the moves telescope to the
// weight) spread over decisions: less at the exit decision, some after it.
TEST(TargetReplayV6, ExitRateOneIsTheImmediateExitAndDecaySpreadsTheSameTotal) {
  const auto r = exit_role();
  const auto base = aim_partial(0.25, 0.1);
  const auto a = st::replay_targets(r.input(), base);
  const auto b = st::replay_targets(r.input(), decaying(0.25, 0.1, 1));
  const auto slow = st::replay_targets(r.input(), decaying(0.25, 0.1, 0.05));
  ASSERT_TRUE(a) << a.error().to_string();
  ASSERT_TRUE(b) << b.error().to_string();
  ASSERT_TRUE(slow) << slow.error().to_string();
  ASSERT_EQ(a->days.size(), b->days.size());
  for (usize d = 0; d < a->days.size(); ++d) expect_same_target_day(a->days[d], b->days[d], d);
  EXPECT_EQ(bits(a->total_turnover), bits(b->total_turnover));
  EXPECT_EQ(bits(a->forced_turnover), bits(b->forced_turnover));
  EXPECT_GT(a->days[3].forced_turnover, 0);
  EXPECT_EQ(a->days[4].forced_turnover, 0); // gone at once
  EXPECT_LT(slow->days[3].forced_turnover, a->days[3].forced_turnover);
  EXPECT_GT(slow->days[4].forced_turnover, 0); // still decaying
  for (usize d = 0; d < 3; ++d) expect_same_target_day(a->days[d], slow->days[d], d);
  EXPECT_NEAR(slow->forced_turnover, a->forced_turnover, 1e-12);
}

// Refusals: exit_rate in (0, 1] (NaN refused); below 1 only under aim-partial-v5 with a
// dust band; without prices (no presence) the replay is refused and the seam moves nothing.
TEST(TargetReplayV6, ExitRateRefusals) {
  const auto r = exit_role();
  const auto runs = [&](const st::TargetReplayConfig& c) {
    return static_cast<bool>(st::replay_targets(r.input(), c));
  };
  EXPECT_TRUE(runs(decaying(0.25, 0.1, 0.05)));
  EXPECT_TRUE(runs(decaying(0.25, 0.1, 1)));
  EXPECT_TRUE(runs(decaying(0.25, 0, 1))); // the immediate exit needs no band
  for (const f64 bad : {0.0, -0.1, 1.01, missing})
    EXPECT_FALSE(runs(decaying(0.25, 0.1, bad))) << bad;
  EXPECT_FALSE(runs(decaying(0.25, 0, 0.05))); // no band: an exit would never end
  st::TargetReplayConfig baseline; baseline.cadence = 1; baseline.exit_rate = 0.05;
  EXPECT_FALSE(runs(baseline));
  const auto f = random_fixture(5, 6, 3); // no prices
  const auto refused = st::replay_targets(f.input(), decaying(0.25, 0.1, 0.05));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), co::ErrorCode::InvalidArgument);
  std::vector<std::pair<f64, usize>> row;
  std::vector<f64> desired(f.n), w(f.n, 0.25);
  st::detail::desired_target(std::span<const f64>(f.signal).subspan(0, f.n),
                             std::span<const u8>(f.member).subspan(0, f.n), row, desired);
  st::TargetReplayDay day;
  const auto status =
      st::detail::update_weights(f.input(), decaying(0.25, 0.1, 0.05), 0, true, 0, desired, w, day);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), co::ErrorCode::InvalidArgument);
  EXPECT_EQ(day.turnover, 0);
  for (const f64 x : w) EXPECT_EQ(x, 0.25);
}

// ---- v6 C2: industry neutralization ids ----
namespace {
// grp_ff12 of a Role on every row: names 0-5 id 4 (6 names), 6-9 id 7 (4 names, pooled
// into the fallback) and 10-11 unknown (2 names, pooled too).
std::vector<f64> role_industry(const Role& role) {
  std::vector<f64> ids(role.d * role.n);
  for (usize t = 0; t < role.d; ++t)
    for (usize i = 0; i < role.n; ++i)
      ids[t * role.n + i] = i < 6 ? 4.0 : i < 10 ? 7.0 : missing;
  return ids;
}
st::TargetReplayConfig industry_daily(st::TargetNeutralize id) {
  auto c = neutral_daily(); c.neutralize = id; return c;
}
} // namespace

// The CLI ids: ind-v2 carries its declared windows (vol 126, log ADV 252; beta stays
// 252) and refuses any other pair; ind-v1 keeps price_risk as it is; an unknown id
// leaves the config untouched; a short industry span is refused.
TEST(TargetReplayV6, IndustryIdsParseWithTheirWindowsAndRefuseOtherPairs) {
  st::TargetReplayConfig v2;
  ASSERT_TRUE(st::detail::parse_neutralize("price-risk-ind-v2", v2));
  EXPECT_EQ(v2.neutralize, st::TargetNeutralize::PriceRiskIndV2);
  EXPECT_EQ(v2.price_risk.vol_window, 126U);
  EXPECT_EQ(v2.price_risk.adv_window, 252U);
  EXPECT_EQ(v2.price_risk.beta_window, 252U);
  st::TargetReplayConfig v1;
  ASSERT_TRUE(st::detail::parse_neutralize("price-risk-ind-v1", v1));
  EXPECT_EQ(v1.neutralize, st::TargetNeutralize::PriceRiskIndV1);
  EXPECT_EQ(v1.price_risk.vol_window, 63U);
  EXPECT_EQ(v1.price_risk.adv_window, 63U);
  ASSERT_TRUE(st::detail::parse_neutralize("price-risk-v1", v1));
  EXPECT_EQ(v1.neutralize, st::TargetNeutralize::PriceRiskV1);
  EXPECT_FALSE(st::detail::parse_neutralize("price-risk-ind-v3", v1));
  EXPECT_EQ(v1.neutralize, st::TargetNeutralize::PriceRiskV1);
  EXPECT_TRUE(st::neutralize_by_industry(st::TargetNeutralize::PriceRiskIndV1));
  EXPECT_TRUE(st::neutralize_by_industry(st::TargetNeutralize::PriceRiskIndV2));
  EXPECT_FALSE(st::neutralize_by_industry(st::TargetNeutralize::PriceRiskV1));
  EXPECT_FALSE(st::neutralize_by_industry(st::TargetNeutralize::None));
  const Role role(30, 12, 3);
  const auto industry = role_industry(role);
  auto in = role.input();
  in.industry = industry;
  const auto wrong = industry_daily(st::TargetNeutralize::PriceRiskIndV2); // windows 40/20/10
  const auto refused = st::replay_targets(in, wrong);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), co::ErrorCode::InvalidArgument);
  auto right = wrong;
  right.price_risk.vol_window = st::price_risk_ind_v2_vol_window;
  right.price_risk.adv_window = st::price_risk_ind_v2_adv_window;
  const auto early = st::replay_targets(in, right); // no full log-ADV window yet: all skipped
  ASSERT_TRUE(early) << early.error().to_string();
  EXPECT_EQ(count_outcome(*early, st::NeutralizeOutcome::SkippedTooFewNames), role.d);
  const std::vector<f64> short_industry(industry.begin(), industry.end() - 1);
  in.industry = short_industry;
  EXPECT_FALSE(st::replay_targets(in, industry_daily(st::TargetNeutralize::PriceRiskIndV1)));
}

// ind-v1 at a decision is the within-groups primitive on that decision's industry row,
// bit for bit, with its group record; every applied decision leaves both demeaning
// groups flat and (at fraction 1) a gross-1, net-0 plan; the replay and the shared
// construction refuse the id without the field; the pinned target replay (no fields)
// refuses it before output.
TEST(TargetReplayV6, IndustryConstructionIsTheWithinGroupsPrimitive) {
  const Role role(70, 12, 21);
  const auto cfg = industry_daily(st::TargetNeutralize::PriceRiskIndV1);
  constexpr usize d = 60;
  const auto industry = role_industry(role);
  auto in = role.input();
  in.industry = industry;
  std::vector<std::pair<f64, usize>> row;
  std::vector<f64> expected(role.n);
  st::detail::desired_target(std::span<const f64>(role.signal).subspan(d * role.n, role.n),
                             std::span<const u8>(role.member).subspan(d * role.n, role.n), row,
                             expected);
  const std::vector<u8> members(role.n, 1);
  st::PriceRiskScratch primitive;
  st::NeutralizeStats stats;
  const auto direct = st::neutralize_price_risk_within_groups(
      role.prices(), cfg.price_risk, d, expected, members,
      std::span<const f64>(industry).subspan(d * role.n, role.n), primitive, stats);
  ASSERT_TRUE(direct) << direct.error().to_string();
  EXPECT_EQ(stats.groups, 2U);
  EXPECT_EQ(stats.fallback_names, 6U);
  EXPECT_EQ(stats.unknown_group_names, 2U);
  std::vector<f64> desired(role.n);
  st::PriceRiskScratch scratch;
  st::ConstructionDay record;
  const auto rebalance = st::detail::form_desired(in, cfg, d, row, desired, scratch, record);
  ASSERT_TRUE(rebalance) << rebalance.error().to_string();
  EXPECT_TRUE(*rebalance);
  for (usize i = 0; i < role.n; ++i) EXPECT_EQ(bits(desired[i]), bits(expected[i])) << i;
  EXPECT_EQ(record.neutralize, st::NeutralizeOutcome::Applied);
  EXPECT_EQ(record.neutralize_used, 12U);
  EXPECT_EQ(record.neutralize_groups, 2U);
  EXPECT_EQ(record.neutralize_fallback_names, 6U);
  EXPECT_EQ(record.neutralize_unknown_group_names, 2U);
  EXPECT_EQ(bits(record.neutralize_amplification), bits(stats.gross / stats.residual_gross));
  f64 own = 0, pooled = 0, gross = 0;
  for (usize i = 0; i < role.n; ++i) {
    (i < 6 ? own : pooled) += desired[i];
    gross += std::abs(desired[i]);
  }
  EXPECT_LE(std::abs(own), 1e-12);
  EXPECT_LE(std::abs(pooled), 1e-12);
  EXPECT_NEAR(gross, 1.0, 1e-12);
  auto replay = st::replay_targets(in, cfg);
  ASSERT_TRUE(replay) << replay.error().to_string();
  usize applied = 0;
  for (const auto& day : replay->days) {
    if (day.construction.neutralize != st::NeutralizeOutcome::Applied) continue;
    ++applied;
    EXPECT_NEAR(day.gross, 1.0, 1e-12) << day.decision;
    EXPECT_NEAR(day.net, 0.0, 1e-12) << day.decision;
  }
  EXPECT_GT(applied, 0U);
  const auto without = st::replay_targets(role.input(), cfg);
  ASSERT_FALSE(without);
  EXPECT_EQ(without.error().code(), co::ErrorCode::InvalidArgument);
  const auto shared = st::detail::form_desired(role.input(), cfg, d, row, desired, scratch, record);
  ASSERT_FALSE(shared);
  EXPECT_EQ(shared.error().code(), co::ErrorCode::InvalidArgument);
  Fixture f(6, 3);
  for (usize t = 0; t < f.d; ++t)
    for (usize i = 0; i < f.n; ++i) f.signal[t * f.n + i] = static_cast<f64>((i + t) % 3);
  Directory dir; auto run = artifact(dir.path, f); run.target = cfg;
  std::ostringstream progress;
  const auto pinned = st::run_target_replay(run, progress);
  ASSERT_FALSE(pinned);
  EXPECT_EQ(pinned.error().code(), co::ErrorCode::InvalidArgument);
  EXPECT_FALSE(std::filesystem::exists(dir.path / "out"));
}

// Locate-in-aim under ind-v1 (review I3): form_desired zeroes the special-tier shorts and
// hands the no-short mask to the within-groups primitive as its hold mask, bit for bit, so
// the zeroed names (3, 4, 5 of group 4) are reset to 0 after the group demeaning instead
// of inheriting -(group mean): they end less short than unheld. price-risk-v1 with the
// same mask is the v1 primitive on the zeroed target (no hold; C1's path unchanged).
TEST(TargetReplayV6, LocateInAimHoldsZeroedShortsThroughTheIndustryDemeaning) {
  const Role role(70, 12, 21);
  const auto cfg = industry_daily(st::TargetNeutralize::PriceRiskIndV1);
  constexpr usize d = 60;
  const auto industry = role_industry(role);
  auto in = role.input();
  in.industry = industry;
  std::vector<std::pair<f64, usize>> row;
  std::vector<f64> zeroed(role.n);
  st::detail::desired_target(std::span<const f64>(role.signal).subspan(d * role.n, role.n),
                             std::span<const u8>(role.member).subspan(d * role.n, role.n), row,
                             zeroed);
  std::vector<u8> no_short(role.n, 0);
  usize shorts = 0;
  for (usize i = 0; i < 6; ++i) { // group 4 is in the special tier
    no_short[i] = 1;
    if (zeroed[i] < 0) { zeroed[i] = 0; ++shorts; }
  }
  ASSERT_EQ(shorts, 3U);
  const std::vector<u8> members(role.n, 1);
  const auto group = std::span<const f64>(industry).subspan(d * role.n, role.n);
  st::PriceRiskScratch primitive;
  st::NeutralizeStats stats;
  auto held = zeroed, unheld = zeroed;
  ASSERT_TRUE(st::neutralize_price_risk_within_groups(role.prices(), cfg.price_risk, d, held,
                                                      members, group, primitive, stats,
                                                      no_short));
  ASSERT_TRUE(st::neutralize_price_risk_within_groups(role.prices(), cfg.price_risk, d, unheld,
                                                      members, group, primitive, stats));
  std::vector<f64> desired(role.n);
  st::PriceRiskScratch scratch;
  st::ConstructionDay record;
  const auto rebalance =
      st::detail::form_desired(in, cfg, d, row, desired, scratch, record, no_short);
  ASSERT_TRUE(rebalance) << rebalance.error().to_string();
  EXPECT_TRUE(*rebalance);
  EXPECT_EQ(record.neutralize, st::NeutralizeOutcome::Applied);
  EXPECT_EQ(record.locate_zeroed, shorts);
  for (usize i = 0; i < role.n; ++i) EXPECT_EQ(bits(desired[i]), bits(held[i])) << i;
  f64 held_short = 0, unheld_short = 0;
  for (usize i = 3; i < 6; ++i) {
    held_short += held[i];
    unheld_short += unheld[i];
  }
  EXPECT_GT(held_short, unheld_short);
  const auto v1_cfg = neutral_daily();
  auto v1 = zeroed;
  ASSERT_TRUE(st::neutralize_price_risk(role.prices(), v1_cfg.price_risk, d, v1, members,
                                        primitive, stats));
  record = {};
  const auto v1_rebalance =
      st::detail::form_desired(role.input(), v1_cfg, d, row, desired, scratch, record, no_short);
  ASSERT_TRUE(v1_rebalance) << v1_rebalance.error().to_string();
  EXPECT_EQ(record.locate_zeroed, shorts);
  for (usize i = 0; i < role.n; ++i) EXPECT_EQ(bits(desired[i]), bits(v1[i])) << i;
}
