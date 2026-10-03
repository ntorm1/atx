// spo-v3 (platform v8 R-6): target tracking toward the aim through the spo engine and the v7
// hook -- the aim itself without costs or limits (a direct Engine::plan), the gross sanity
// bound slack on the fixture and its breach voiding the run, the tracking error, trade-limit
// share and aim correlation with their per-book report, gamma on the first scored decision
// under a warm start (review A-2), the CLI refusals of the registered
// constants, and (v8 E-26) the aim shaped by --hold-band / --adv-hold-q exactly as the
// aim-partial-v5 path shapes desired. Ruling E-31a outside the engine (review R6B-S-1): the
// tiered run's primary book taken from its own matrix, and the void through the CLI (exit 3,
// the extras only); Ruling E-14a's back-fill per book on two cadences (review R6B-S-2). The
// spo-v1 / spo-v2 digest guard is strategy_spo_v3_pin_test.cpp.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "../src/strategy_cost_v2.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_spo.hpp"
#include "../src/strategy_spo_v3.hpp"
#include "../src/strategy_target_replay.hpp"
#include "../src/strategy_target_replay_detail.hpp"
#include "strategy_spo_cli_fixture.hpp"
#include "strategy_spo_fixture.hpp"

namespace {
using namespace atx;
using namespace atx::impl::strategy::spo::fixture;
namespace co = atx::core;
namespace st = atx::impl::strategy;
namespace sp = atx::impl::strategy::spo;
namespace v7 = atx::impl::strategy::v7;
using Json = nlohmann::json;

std::shared_ptr<const sp::RiskStore> clean_model(const Directory& dir, const Role& role,
                                                 u64 seed) {
  const std::vector<u8> forecast(role.d, u8{1});
  const auto sha = write_risk_model(dir.path, role.sessions, role.n, forecast, "role-sha", seed);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  EXPECT_TRUE(store) << store.error().to_string();
  return store ? std::make_shared<const sp::RiskStore>(std::move(*store)) : nullptr;
}

// The observed book's rebalance decisions (a NavHoldingsSink): per session index, every
// reported name's shared desired target (NaN: not reported), every name's held weight (what
// its DECIDE read; 0: not reported, as the stream declares) and, given the spo engine, the aim
// its tracker received at that decision (Engine::last_aim, read right after the book's DECIDE).
class Recorder final : public st::NavHoldingsSink {
public:
  explicit Recorder(usize names) : n_(names) {}
  const sp::Engine* engine{};
  std::map<usize, std::vector<f64>> desired, held, aim;
  [[nodiscard]] co::Status session(const st::NavReplayDay& day,
                                   std::span<const st::NavHolding> names) override {
    if (!day.decision || !day.rebalance) return co::Ok();
    auto& row = desired[day.session_index];
    row.assign(n_, std::numeric_limits<f64>::quiet_NaN());
    for (const auto& h : names) row[h.index] = h.desired;
    auto& book = held[day.session_index];
    book.assign(n_, 0.0);
    for (const auto& h : names) book[h.index] = h.held_weight;
    if (engine != nullptr) {
      const auto received = engine->last_aim();
      aim[day.session_index].assign(received.begin(), received.end());
    }
    return co::Ok();
  }

private:
  usize n_{};
};

// A replay of the fixture (nav_config: S1, S2, S3, cadence 1, theta .25, L 1.2, NAV 1e8, or
// `cfg`) under spo-v3 through the v7 hook, with what the extension would publish; with a
// recorder, its primary book alone (replay_nav_scenarios, bit-identical) observed. The role's
// first scored row is `decision_begin` (0: the fixture's own).
struct Replay {
  std::vector<sp::TrackingRow> rows;
  usize legacy_rows{}; // spo-v1/v2 rows (none under spo-v3)
  sp::Calibration calibration;
  f64 horizon{}, bound{};
  Json parameters, summary, tripwire, calibration_json;
  std::string csv, declaration;
  bool captured{};
  usize primary_unmet{}; // Ruling E-31a: the primary book's scored rows with limits_met false
  st::NavReplayResult result; // the (first) book's
};
// A book's label "<trading id>+<financing id>", as the v7 hook and the engine write it.
std::string label_of(const st::NavScenario& s) { return s.id + "+" + s.financing.id; }
// The fixture's primary book (nav_config's S2), the book whose limits_unmet voids a run.
std::string primary_label() { return label_of(nav_config().scenario); }
usize count_primary_unmet(std::span<const sp::TrackingRow> rows) {
  const std::string primary = primary_label();
  usize n = 0;
  for (const auto& r : rows) n += !r.limits_met && r.book == primary ? 1U : 0U;
  return n;
}
Replay replay_v3(const Role& role, std::shared_ptr<const sp::RiskStore> risk,
                 const sp::SpoParams& params, const st::NavReplayConfig& cfg = nav_config(),
                 Recorder* recorder = nullptr, usize decision_begin = 0) {
  auto input = role.nav();
  input.target.decision_begin = decision_begin;
  v7::NavV7Options o;
  o.spo_v1 = true;
  o.spo_params = params;
  o.spo_risk = std::move(risk);
  const v7::ScopedNavExtension extension(o);
  Replay out;
  const auto* engine = extension.spo_engine();
  if (engine == nullptr) {
    ADD_FAILURE() << "no spo engine";
    return out;
  }
  if (recorder == nullptr) {
    auto result = st::replay_nav(input, cfg);
    EXPECT_TRUE(result) << result.error().to_string();
    if (result) out.result = std::move(*result);
  } else {
    recorder->engine = engine;
    const std::array<st::NavScenario, 1> primary{cfg.scenario};
    auto results = st::replay_nav_scenarios(input, cfg, primary, *recorder, 0);
    EXPECT_TRUE(results) << results.error().to_string();
    if (results && results->size() == 1) out.result = std::move(results->front());
  }
  const auto rows = engine->tracking_rows();
  out.rows.assign(rows.begin(), rows.end());
  out.legacy_rows = engine->rows().size();
  out.calibration = engine->calibration();
  out.horizon = engine->horizon();
  out.bound = engine->gross_budget();
  out.parameters = engine->rule_parameters_json();
  out.summary = engine->rows_summary_json();
  out.tripwire = engine->rows_tripwire_json();
  out.csv = engine->rows_csv();
  out.declaration = engine->rule_declaration();
  out.calibration_json = engine->rule_calibration_json();
  out.captured = static_cast<bool>(v7::capture({}, {}, {})); // the seam before publication
  out.primary_unmet = count_primary_unmet(out.rows);
  EXPECT_EQ(engine->primary_book(), primary_label()); // the default: the untiered S2
  return out;
}

// Ruling E-31a on a replay of the fixture (no clamp and no breach there): the run is void
// exactly when a scored decision of the primary book did not meet its net or beta limit (the
// fixture redraws every style exposure daily, so a book's beta can jump by more than one
// session's trade limits repair), and clear otherwise.
void expect_clear_or_limits_void(const Replay& run) {
  EXPECT_EQ(run.captured, run.primary_unmet == 0) << run.primary_unmet;
  EXPECT_EQ(run.tripwire.at("primary_book"), primary_label());
  EXPECT_EQ(run.tripwire.at("limits_unmet_primary").at("count"), run.primary_unmet);
  if (run.primary_unmet == 0) {
    EXPECT_EQ(run.tripwire.at("status"), "clear");
    EXPECT_FALSE(run.tripwire.contains("voided"));
  } else {
    EXPECT_EQ(run.tripwire.at("status"), "void");
    EXPECT_EQ(run.tripwire.at("voided"), "limits_unmet");
  }
}

// nav_config without aim-partial-v5's dust band (dust 0, hence exit rate 1), which the desired
// target never reads: the plain aim-partial-v5 replay then plans every member whose desired
// target is nonzero, so its holdings carry every member's desired at the first decision too
// (after it, a declared hold band reports every ranked name).
st::NavReplayConfig undusted_config() {
  auto cfg = nav_config();
  cfg.target.dust_multiple = 0.0;
  cfg.target.exit_rate = 1.0;
  return cfg;
}
u64 bits(f64 x) { return std::bit_cast<u64>(x); }
// Pearson correlation as the engine computes it (NaN below 3 pairs or without dispersion).
f64 pearson(std::span<const f64> a, std::span<const f64> b) {
  const usize n = std::min(a.size(), b.size());
  if (n < 3) return std::numeric_limits<f64>::quiet_NaN();
  f64 ma = 0, mb = 0;
  for (usize i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
  ma /= static_cast<f64>(n); mb /= static_cast<f64>(n);
  f64 ab = 0, aa = 0, bb = 0;
  for (usize i = 0; i < n; ++i) {
    const f64 x = a[i] - ma, y = b[i] - mb;
    ab += x * y; aa += x * x; bb += y * y;
  }
  return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : std::numeric_limits<f64>::quiet_NaN();
}
// Pearson correlation of a held book and an aim over every name either holds, as the engine's
// traded_correlation (review A-4, Ruling E-14a).
f64 traded_pearson(std::span<const f64> held, std::span<const f64> aim) {
  std::vector<f64> book, aimed;
  for (usize i = 0; i < aim.size(); ++i) {
    if (aim[i] == 0 && held[i] == 0) continue;
    book.push_back(held[i]);
    aimed.push_back(aim[i]);
  }
  return pearson(book, aimed);
}
// The fixture's desired target at a decision d where every name is a member: the signal row
// centred and scaled to gross 1, as every desired target (detail::desired_target).
std::vector<f64> gross_one_desired(const Role& role, usize d) {
  std::vector<f64> desired(role.n);
  f64 mean = 0;
  for (usize i = 0; i < role.n; ++i) {
    desired[i] = role.signal[d * role.n + i];
    mean += desired[i];
  }
  for (f64& v : desired) v -= mean / static_cast<f64>(role.n);
  f64 gross = 0;
  for (const f64 v : desired) gross += std::abs(v);
  for (f64& v : desired) v /= gross;
  return desired;
}
// The rows of a published spo_diagnostics.csv of `book` with limits_met 0: their count and the
// first one's session (in row order; -1 when none).
std::pair<usize, i64> unmet_rows_in_csv(const std::filesystem::path& csv,
                                        const std::string& book) {
  const auto split = [](const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    for (std::string field; std::getline(stream, field, ',');) fields.push_back(field);
    return fields;
  };
  std::ifstream in(csv);
  std::string line;
  if (!std::getline(in, line)) {
    ADD_FAILURE() << "no header in " << csv.string();
    return {0, -1};
  }
  const auto header = split(line);
  const auto column = [&](const std::string& name) {
    return static_cast<usize>(std::find(header.begin(), header.end(), name) - header.begin());
  };
  const usize session_at = column("session"), book_at = column("book");
  const usize met_at = column("limits_met");
  if (std::max({session_at, book_at, met_at}) >= header.size()) {
    ADD_FAILURE() << csv.string() << " lacks a session, book or limits_met column";
    return {0, -1};
  }
  usize count = 0;
  i64 first = -1;
  while (std::getline(in, line)) {
    const auto row = split(line);
    if (row.size() != header.size() || row[book_at] != book || row[met_at] != "0") continue;
    if (count == 0) first = static_cast<i64>(std::stoll(row[session_at]));
    ++count;
  }
  return {count, first};
}

// No trading cost, no borrow and no binding limit (ADV 1e15, beta band +-1, every name a
// member with a risk row, from flat, no fixed position): the tracking optimum is the aim
// L x desired itself, reached to 1e-8 with the registered solver constants.
TEST(SpoV3, ZeroCostNoLimitsReturnsAimTo1e8) {
  const Directory dir;
  const Role role(20, 12, 83);
  const auto risk = clean_model(dir, role, 13);
  ASSERT_NE(risk, nullptr);
  auto params = sp::v3_params();
  params.beta_max = 1.0;
  sp::Engine engine(params, risk);
  auto cfg = nav_config();
  cfg.scenario.financing.flat_short_bps = 0.0; // no borrow
  auto law = cfg.scenario;                      // the primary S2 law without its costs
  law.half_spread_bps = 0.0; law.commission_bps = 0.0; law.impact_y = 0.0;
  const auto x = role.target();
  const usize n = role.n, d = 2; // every name a member (name n - 1 leaves on sessions 6..9)
  std::vector<f64> desired(n);
  f64 mean = 0;
  for (usize i = 0; i < n; ++i) {
    desired[i] = role.signal[d * n + i];
    mean += desired[i];
  }
  for (f64& v : desired) v -= mean / static_cast<f64>(n);
  // Gross 1, as every desired target (detail::desired_target), so L x desired is inside 2 x L.
  f64 gross = 0;
  for (const f64 v : desired) gross += std::abs(v);
  for (f64& v : desired) v /= gross;
  const st::cost_v2::DecisionLiquidity liquidity{std::vector<f64>(n, 1e15),
                                                 std::vector<f64>(n, 0.02)};
  const sp::BookDecision in{x, cfg, law, d, 1e8, desired, {}, {}, liquidity, "S2"};
  std::vector<f64> planned(n, 0.0);
  st::TargetReplayDay day;
  const auto status = engine.plan(in, planned, day);
  ASSERT_TRUE(status) << status.error().to_string();
  f64 worst = 0;
  for (usize i = 0; i < n; ++i)
    worst = std::max(worst, std::abs(planned[i] - cfg.target.aim_leverage * desired[i]));
  EXPECT_LT(worst, 1e-8);
  EXPECT_TRUE(engine.rows().empty()); // spo-v3 writes its own rows only
  const auto rows = engine.tracking_rows();
  ASSERT_EQ(rows.size(), 1U);
  const auto& r = rows.front();
  EXPECT_TRUE(r.converged) << r.iterations;
  EXPECT_TRUE(r.limits_met) << r.limit_violation;
  EXPECT_EQ(r.optimized, n);
  EXPECT_EQ(r.at_trade_limit, 0U);
  EXPECT_EQ(r.trade_cost, 0.0);
  EXPECT_EQ(r.borrow, 0.0);
  EXPECT_LT(r.tracking_error, 1e-6);
  EXPECT_NEAR(r.aim_correlation, 1.0, 1e-9);
  EXPECT_FALSE(r.gross_bound_breached);
  const auto& c = engine.calibration();
  ASSERT_TRUE(c.done);
  EXPECT_GT(c.aim_vol, 0.0);
  EXPECT_DOUBLE_EQ(c.gamma, sp::v3_sharpe_prior / c.aim_vol);
  EXPECT_DOUBLE_EQ(r.gamma, c.gamma);
  EXPECT_DOUBLE_EQ(r.tracking_error_current, c.aim_vol); // from flat: the aim's own vol
  EXPECT_EQ(engine.horizon(), sp::v3_horizon);            // fixed, not 1 / theta (4 here)
  EXPECT_DOUBLE_EQ(engine.gross_budget(), sp::v3_gross_bound_multiple * cfg.target.aim_leverage);
}

// A payload the fixture's risk model wrote, read back as raw values (not through RiskStore).
template <class T>
std::vector<T> read_values(const std::filesystem::path& file) {
  std::vector<T> out(static_cast<usize>(std::filesystem::file_size(file) / sizeof(T)));
  std::ifstream in(file, std::ios::binary);
  // SAFETY: char reads into the object representation of trivially copyable fixture values.
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size() * sizeof(T)));
  if (!in) {
    ADD_FAILURE() << "short read of " << file.string();
  }
  return out;
}

// Review T-4: gamma's units against an independent dense Sigma. sigma_aim is the aim's ANNUAL
// ex-ante vol sqrt(252 a' Sigma a), Sigma = X F X' + D of the risk row at the decision, built
// here from the files the fixture's risk model wrote (intercept, the industry slot's column,
// the 11 styles), and gamma = S_prior / sigma_aim with S_prior = 20 (Ruling E-14). An
// unannualised vol (gamma sqrt(252) = 15.87 times too large), 252 without the root, or a
// dropped industry or style column fails here.
TEST(SpoV3, GammaIsSPriorOverTheAnnualisedAimVolOfADenseSigma) {
  const Directory dir;
  const Role role(20, 12, 83);
  const auto risk = clean_model(dir, role, 13);
  ASSERT_NE(risk, nullptr);
  sp::Engine engine(sp::v3_params(), risk);
  const auto cfg = nav_config();
  const auto x = role.target();
  const usize n = role.n, d = 2; // every name a member with a risk row at d
  std::vector<f64> desired(n);
  f64 mean = 0;
  for (usize i = 0; i < n; ++i) {
    desired[i] = role.signal[d * n + i];
    mean += desired[i];
  }
  for (f64& v : desired) v -= mean / static_cast<f64>(n);
  f64 gross = 0;
  for (const f64 v : desired) gross += std::abs(v);
  for (f64& v : desired) v /= gross;
  const st::cost_v2::DecisionLiquidity liquidity{std::vector<f64>(n, 1e15),
                                                 std::vector<f64>(n, 0.02)};
  const sp::BookDecision in{x, cfg, cfg.scenario, d, 1e8, desired, {}, {}, liquidity, "S2"};
  std::vector<f64> planned(n, 0.0);
  st::TargetReplayDay day;
  const auto status = engine.plan(in, planned, day);
  ASSERT_TRUE(status) << status.error().to_string();
  const auto& c = engine.calibration();
  ASSERT_TRUE(c.done);
  // The aim the tracker received: L x desired.
  std::vector<f64> aim(n);
  for (usize i = 0; i < n; ++i) aim[i] = cfg.target.aim_leverage * desired[i];
  const auto received = engine.last_aim();
  ASSERT_EQ(received.size(), n);
  for (usize i = 0; i < n; ++i) EXPECT_EQ(std::bit_cast<u64>(received[i]), std::bit_cast<u64>(aim[i])) << i;
  // Sigma at d, dense, from the store's files.
  constexpr usize k = sp::risk_factors, styles = sp::risk_styles, style_column = 1 + sp::risk_industry_slots;
  const auto covariance = read_values<f64>(dir.path / "factor_covariance.f64");
  const auto specific = read_values<f64>(dir.path / "specific_variance.f64");
  const auto exposure = read_values<f32>(dir.path / "style_exposures.f32");
  const auto slot = read_values<u8>(dir.path / "industry_slot.u8");
  ASSERT_EQ(covariance.size(), role.d * k * k);
  ASSERT_EQ(specific.size(), role.d * n);
  ASSERT_EQ(exposure.size(), role.d * n * styles);
  ASSERT_EQ(slot.size(), role.d * n);
  std::vector<f64> loading(n * k, 0.0); // X
  for (usize i = 0; i < n; ++i) {
    ASSERT_LT(static_cast<usize>(slot[d * n + i]), sp::risk_industry_slots);
    loading[i * k] = 1.0;
    loading[i * k + 1 + slot[d * n + i]] = 1.0;
    for (usize s = 0; s < styles; ++s)
      loading[i * k + style_column + s] = static_cast<f64>(exposure[(d * n + i) * styles + s]);
  }
  const usize f = d * k * k; // F at d
  f64 daily = 0.0;
  for (usize i = 0; i < n; ++i)
    for (usize j = 0; j < n; ++j) {
      f64 sigma = i == j ? specific[d * n + i] : 0.0;
      for (usize r = 0; r < k; ++r)
        for (usize q = 0; q < k; ++q)
          sigma += loading[i * k + r] * covariance[f + r * k + q] * loading[j * k + q];
      daily += aim[i] * sigma * aim[j];
    }
  ASSERT_GT(daily, 0.0);
  const f64 sigma_aim = std::sqrt(252.0 * daily);
  EXPECT_NEAR(c.aim_vol, sigma_aim, 1e-12 * sigma_aim);
  EXPECT_NEAR(c.aim_vol / std::sqrt(daily), 15.874507866387544, 1e-11); // sqrt(252), pinned
  EXPECT_EQ(sp::v3_sharpe_prior, 20.0);                                  // Ruling E-14
  EXPECT_NEAR(c.gamma, 20.0 / sigma_aim, 1e-12 * c.gamma);
  const auto record = engine.rule_calibration_json();
  EXPECT_EQ(record.at("sigma_aim").get<f64>(), c.aim_vol);
  EXPECT_EQ(record.at("gamma").get<f64>(), c.gamma);
  EXPECT_EQ(record.at("sharpe_prior").get<f64>(), 20.0);
}

// The gross cap is the sanity bound 2 x L, never a solver constraint: on the fixture's replay
// (every book from flat, the S2 law's costs and the 1% ADV trade limit) the planned gross stays
// well inside it on every scored decision and the tripwire is clear unless Ruling E-31a voids
// it. A breach (forced on the recorded rows, every limit met) voids the run with the void on and
// is recorded, not voiding, with it off.
TEST(SpoV3, GrossCapIsSlackOnFixture) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_model(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  const auto params = sp::v3_params();
  const Replay run = replay_v3(role, risk, params);
  ASSERT_FALSE(run.rows.empty());
  const f64 bound = sp::v3_gross_bound_multiple * nav_config().target.aim_leverage;
  EXPECT_DOUBLE_EQ(run.bound, bound);
  EXPECT_EQ(run.parameters.at("gross_bound"), bound);
  f64 largest = 0;
  for (const auto& r : run.rows) {
    EXPECT_FALSE(r.gross_bound_breached) << r.session << ' ' << r.book;
    EXPECT_LT(r.gross, bound) << r.session << ' ' << r.book;
    largest = std::max(largest, r.gross);
  }
  EXPECT_GT(largest, 0.0);
  EXPECT_LT(largest, 0.75 * bound) << "the bound is slack, not near binding";
  expect_clear_or_limits_void(run); // no breach: only Ruling E-31a can void it
  EXPECT_EQ(run.tripwire.at("gross_bound_breaches"), 0U);
  EXPECT_EQ(run.tripwire.at("capped_specific_decisions"), 0U);
  // The breach alone (every limit met, so Ruling E-31a stays silent).
  auto breached = run.rows;
  for (auto& r : breached) r.limits_met = true;
  breached[breached.size() / 2].gross_bound_breached = true;
  const std::string primary = primary_label();
  const auto status = sp::tracking_tripwire(params, breached, primary);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), co::ErrorCode::Unavailable);
  EXPECT_NE(status.error().message().find("VOID"), std::string::npos) << status.error().message();
  const auto record = sp::tracking_tripwire_json(params, breached, primary);
  EXPECT_EQ(record.at("status"), "void");
  EXPECT_EQ(record.at("gross_bound_breaches"), 1U);
  EXPECT_FALSE(record.contains("voided")); // the breach, not limits_unmet
  auto off = params;
  off.void_on_capped = false;
  EXPECT_TRUE(sp::tracking_tripwire(off, breached, primary));
  const auto tripped = sp::tracking_tripwire_json(off, breached, primary);
  EXPECT_EQ(tripped.at("status").get<std::string>().rfind("tripped", 0), 0U);
}

// Ruling E-31a (review SPO-1) on recorded rows: one scored decision of the primary book with its
// net or beta limit unmet voids the run (Unavailable: the replay returns before its output
// directory, so no NAV or return file and no Sharpe on the console), with the void flag on or
// off, and the record says "voided": "limits_unmet" with the count and the first session. The
// same rows with the unmet decision on another book (a capacity book, S1, S3) do not void.
TEST(SpoV3, PrimaryBookLimitsUnmetVoidsTheRunWhateverTheVoidFlag) {
  const std::string primary = primary_label();
  std::vector<sp::TrackingRow> rows(6);
  for (usize k = 0; k < rows.size(); ++k) {
    rows[k].session = 20200102 + static_cast<i64>(k);
    rows[k].book = k % 2 == 0 ? primary : std::string("capacity-x4-v1+flat-300-v0");
    rows[k].limits_met = true;
    rows[k].converged = true;
    rows[k].gross = 1.0;
  }
  auto params = sp::v3_params();
  for (const bool flag : {true, false}) {
    params.void_on_capped = flag;
    ASSERT_TRUE(sp::tracking_tripwire(params, rows, primary)) << flag; // every limit met
    const auto clear = sp::tracking_tripwire_json(params, rows, primary);
    EXPECT_EQ(clear.at("status"), "clear") << flag;
    EXPECT_EQ(clear.at("limits_unmet_primary").at("count"), 0U) << flag;
    EXPECT_TRUE(clear.at("limits_unmet_primary").at("first_session").is_null()) << flag;
    EXPECT_FALSE(clear.contains("voided")) << flag;
    auto other = rows; // unmet on another book only: reported, never voiding
    other[1].limits_met = false;
    other[3].limits_met = false;
    EXPECT_TRUE(sp::tracking_tripwire(params, other, primary)) << flag;
    EXPECT_EQ(sp::tracking_tripwire_json(params, other, primary).at("status"), "clear") << flag;
    auto unmet = rows; // the primary book, twice: sessions 2 and 4
    unmet[4].limits_met = false;
    unmet[2].limits_met = false;
    const auto status = sp::tracking_tripwire(params, unmet, primary);
    ASSERT_FALSE(status) << flag;
    EXPECT_EQ(status.error().code(), co::ErrorCode::Unavailable) << flag;
    const auto& message = status.error().message();
    EXPECT_NE(message.find("Ruling E-31a"), std::string::npos) << message;
    EXPECT_NE(message.find("VOID"), std::string::npos) << message;
    EXPECT_NE(message.find(primary), std::string::npos) << message;
    EXPECT_NE(message.find("on 2 scored decisions (first session 20200104)"), std::string::npos)
        << message;
    const auto record = sp::tracking_tripwire_json(params, unmet, primary);
    EXPECT_EQ(record.at("status"), "void") << flag;
    EXPECT_EQ(record.at("voided"), "limits_unmet") << flag;
    EXPECT_EQ(record.at("primary_book"), primary) << flag;
    EXPECT_EQ(record.at("limits_unmet_primary").at("count"), 2U) << flag;
    EXPECT_EQ(record.at("limits_unmet_primary").at("first_session"), 20200104) << flag;
    EXPECT_NE(record.at("limits_unmet_rule").get<std::string>().find("Ruling E-31a"),
              std::string::npos);
    EXPECT_EQ(record.at("report_only").at(primary).at("limits_unmet"), 2U) << flag;
  }
  EXPECT_EQ(sp::default_primary_book(), primary); // the untiered matrix's S2
}

// Ruling E-31a through the engine: a book whose net cannot be restored inside one session's
// trade limit (every name long .01 of NAV, ADV $1, so p ADV / NAV = 1e-10) plans with its limits
// unmet. Labelled as the engine's primary book its row voids the engine's tripwire, the void
// flag on or off; labelled as any other book it does not. The v7 hook's capture reads exactly
// this tripwire (spo::Engine::rows_tripwire) before anything is published.
TEST(SpoV3, EngineVoidsOnItsPrimaryBooksLimitsUnmet) {
  const Directory dir;
  const Role role(20, 12, 83);
  const auto risk = clean_model(dir, role, 13);
  ASSERT_NE(risk, nullptr);
  const auto cfg = nav_config();
  const auto x = role.target();
  const usize n = role.n, d = 2;
  std::vector<f64> desired(n);
  f64 mean = 0;
  for (usize i = 0; i < n; ++i) {
    desired[i] = role.signal[d * n + i];
    mean += desired[i];
  }
  for (f64& v : desired) v -= mean / static_cast<f64>(n);
  f64 gross = 0;
  for (const f64 v : desired) gross += std::abs(v);
  for (f64& v : desired) v /= gross;
  const st::cost_v2::DecisionLiquidity liquidity{std::vector<f64>(n, 1.0),
                                                 std::vector<f64>(n, 0.02)};
  const auto plan_one = [&](bool void_flag, std::string_view label, std::string_view primary) {
    auto params = sp::v3_params();
    params.void_on_capped = void_flag;
    sp::Engine engine(params, risk);
    if (!primary.empty()) engine.set_primary_book(std::string(primary));
    const sp::BookDecision in{x, cfg, cfg.scenario, d, 1e8, desired, {}, {}, liquidity, label};
    std::vector<f64> planned(n, 0.01); // net .01 n: the trade limit cannot restore it
    st::TargetReplayDay day;
    const auto status = engine.plan(in, planned, day);
    EXPECT_TRUE(status) << status.error().to_string();
    const auto rows = engine.tracking_rows();
    EXPECT_EQ(rows.size(), 1U);
    const bool unmet = rows.size() == 1 && !rows.front().limits_met;
    return std::make_tuple(unmet, static_cast<bool>(engine.rows_tripwire()),
                           engine.rows_tripwire_json());
  };
  const std::string primary = primary_label();
  for (const bool flag : {true, false}) {
    const auto [unmet, ok, record] = plan_one(flag, primary, {}); // the default primary
    ASSERT_TRUE(unmet) << "premise: the net is not restorable inside the trade limit";
    EXPECT_FALSE(ok) << flag;
    EXPECT_EQ(record.at("voided"), "limits_unmet") << flag;
    EXPECT_EQ(record.at("limits_unmet_primary").at("count"), 1U) << flag;
    EXPECT_EQ(record.at("limits_unmet_primary").at("first_session"), role.sessions[d]) << flag;
    const auto [unmet_s1, ok_s1, record_s1] = plan_one(flag, "S1", {}); // not the primary
    ASSERT_TRUE(unmet_s1);
    EXPECT_TRUE(ok_s1) << flag;
    EXPECT_FALSE(record_s1.contains("voided")) << flag;
    const auto [unmet_set, ok_set, record_set] = plan_one(flag, "S1", "S1"); // set_primary_book
    ASSERT_TRUE(unmet_set);
    EXPECT_FALSE(ok_set) << flag;
    EXPECT_EQ(record_set.at("primary_book"), "S1") << flag;
  }
}

// Review R6B-S-1 (a), Ruling E-31a on the registered tiered run: the v7 hook's main pass takes the
// primary book from the run's own scenario matrix (run_scenarios, before --spo-books primary
// resizes it). On the tiered matrix (S1/S2/S3 x swap-fin-v1, then S2 x flat-300-v0 and S2 x
// engine-tiers-v1) that is S2 x swap-fin-v1, not the engine's default, the untiered S2 x
// flat-300-v0 (the tiered matrix's stress book at index 3). Each book of the run in turn plans,
// through the hook, one decision it cannot restore (ADV 0: every trade limit p ADV / NAV is 0, so
// every name stays at its current .01 and the net at .12): capture() voids the run exactly when
// that book is S2 x swap-fin-v1, with --spo-books all and primary. The capacity pass (Ruling
// E-37) keeps the main engine's primary; its own engine has none.
TEST(SpoV3, TieredRunVoidsOnItsSwapFinancedPrimaryBookOnly) {
  const Directory dir;
  Role role(20, 12, 83);
  std::fill(role.volume.begin(), role.volume.end(), 0.0);
  const auto risk = clean_model(dir, role, 13);
  ASSERT_NE(risk, nullptr);
  const auto tiered = st::nav_scenario_matrix(true);
  ASSERT_EQ(tiered.size(), 5U);
  const std::string swap_primary = label_of(tiered[st::nav_primary_scenario_index]);
  EXPECT_EQ(swap_primary, "modeled-1bn-stale5-v1+swap-fin-v1");
  EXPECT_EQ(label_of(tiered[3]), sp::default_primary_book()); // the engine's default primary
  const auto x = role.target();
  const usize d = 2;
  const std::vector<f64> desired = gross_one_desired(role, d);
  struct Decided {
    std::string primary, book, reason;
    usize books{};
    bool unmet{}, voided{};
  };
  // A fresh run (--spo-books all or primary) whose book k alone plans decision d.
  const auto decide = [&](bool all_books, usize k) {
    v7::NavV7Options o;
    o.spo_v1 = true;
    o.spo_params = sp::v3_params();
    o.spo_params.all_books = all_books;
    o.spo_risk = risk;
    v7::ScopedNavExtension extension(o);
    extension.begin_run(v7::NavV7Pass::Main);
    const auto books = v7::run_scenarios(tiered);
    Decided out;
    out.books = books.size();
    const auto* engine = extension.spo_engine();
    if (engine == nullptr || k >= books.size()) {
      ADD_FAILURE() << "no spo engine or no book " << k;
      return out;
    }
    out.primary = engine->primary_book();
    auto cfg = nav_config();
    cfg.scenario = books[k];
    out.book = label_of(cfg.scenario);
    std::vector<f64> planned(role.n, 0.01); // net .12, which no trade can restore
    st::TargetReplayDay day;
    const auto status =
        v7::plan(x, cfg, d, true, 0.0, cfg.initial_nav, desired, planned, day, {});
    EXPECT_TRUE(status) << status.error().to_string();
    const auto rows = engine->tracking_rows();
    out.unmet = rows.size() == 1 && rows.front().book == out.book && !rows.front().limits_met;
    out.voided = !v7::capture({}, {}, {});
    out.reason = extension.void_reason();
    return out;
  };
  for (const bool all_books : {true, false}) {
    const usize count = all_books ? tiered.size() : st::nav_primary_scenario_index + 1;
    for (usize k = 0; k < count; ++k) {
      const Decided run = decide(all_books, k);
      ASSERT_TRUE(run.unmet) << "premise: " << run.book << " cannot restore its net";
      EXPECT_EQ(run.books, count) << all_books;
      EXPECT_EQ(run.primary, swap_primary) << run.book << ' ' << all_books;
      const bool primary = k == st::nav_primary_scenario_index;
      EXPECT_EQ(run.voided, primary) << run.book << ' ' << all_books;
      if (primary) {
        EXPECT_NE(run.reason.find("Ruling E-31a"), std::string::npos) << run.reason;
        EXPECT_NE(run.reason.find(swap_primary), std::string::npos) << run.reason;
      } else {
        EXPECT_TRUE(run.reason.empty()) << run.book << ": " << run.reason;
      }
    }
  }
  // The capacity pass on the same matrix: the primary's capacity books; the main engine keeps
  // the run's primary and the capacity engine has none.
  v7::NavV7Options curve_options;
  curve_options.spo_v1 = true;
  curve_options.capacity = true;
  curve_options.spo_params = sp::v3_params();
  curve_options.spo_risk = risk;
  v7::ScopedNavExtension curve(curve_options);
  curve.begin_run(v7::NavV7Pass::Main);
  EXPECT_EQ(v7::run_scenarios(tiered).size(), tiered.size());
  curve.begin_run(v7::NavV7Pass::Capacity);
  const auto capacity = v7::run_scenarios(tiered);
  const auto expected = st::cost_v2::capacity_scenarios(tiered[st::nav_primary_scenario_index]);
  ASSERT_EQ(capacity.size(), expected.size());
  for (usize k = 0; k < capacity.size(); ++k)
    EXPECT_EQ(label_of(capacity[k]), label_of(expected[k])) << k;
  ASSERT_NE(curve.spo_engine(), nullptr);
  ASSERT_NE(curve.spo_capacity_engine(), nullptr);
  EXPECT_EQ(curve.spo_engine()->primary_book(), swap_primary);
  EXPECT_TRUE(curve.spo_capacity_engine()->primary_book().empty());
}

// Review R6B-S-1 (b), Ruling E-31a through the command line (dispatch_nav_replay ->
// dispatch_nav_v7 -> the NAV replay -> capture): a spo-v3 run whose primary book does not meet
// its net or beta limit on a scored decision exits 3 and leaves a fresh <output> holding exactly
// the v7 diagnostics -- spo_diagnostics.csv, v7_transfer_coefficient.csv and v7_extras.json with
// "status": "void", "voided": "limits_unmet" and the limits_unmet block (count, first session,
// book, rule), which agrees with the spo_v3 tripwire record and with the primary book's rows of
// spo_diagnostics.csv -- and no recipe, summary, NAV or return file and no Sharpe on the
// console, whatever --specific-ceiling-void. The premise: the role has no volume from session
// `dry` on, so from decision dry + 63 (the replay's 63-session liquidity window, which no flag
// moves) every name's ADV is 0: no book can plan a trade (trade limit .01 ADV / NAV = 0, every
// name pinned at its current weight) or fill one, and the primary book's drifted net cannot be
// restored to 0.
TEST(SpoV3, CliVoidOnPrimaryLimitsUnmetExitsThreeWithTheExtrasOnly) {
  constexpr usize dry = 30, window = 63;
  Role role(100, 12, 53);
  ASSERT_EQ(st::NavReplayConfig{}.liquidity_window, window);
  ASSERT_LT(dry + window + 2, role.d); // scored decisions at ADV 0 remain
  for (usize k = dry * role.n; k < role.volume.size(); ++k) role.volume[k] = 0.0;
  const Directory dir;
  const auto inputs = write_run_inputs(dir.path, role);
  const std::vector<u8> forecast(role.d, u8{1});
  ASSERT_TRUE(std::filesystem::create_directory(dir.path / "risk"));
  const auto sha = write_risk_model(dir.path / "risk", role.sessions, role.n, forecast,
                                    inputs.role_sha256, 3);
  const auto nav = [&](const std::string& output, const std::string& void_flag,
                       std::ostream& out, std::ostream& err) {
    std::vector<std::string> args{
        "nav", "--combined", inputs.combined, "--combined-sha256", inputs.combined_sha256,
        "--role", inputs.role, "--role-sha256", inputs.role_sha256,
        "--output", (dir.path / output).string(), "--cadence", "1", "--trade-fraction", ".25",
        "--dust-multiple", ".1", "--aim-leverage", "1.2", "--exit-rate", ".05",
        "--rule", "spo-v3", "--spo-alpha", "implied-aim", "--risk-model",
        (dir.path / "risk").string(), "--risk-model-sha256", sha, "--spo-books", "primary",
        "--specific-ceiling-void", void_flag};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    return st::dispatch_nav_replay(static_cast<int>(argv.size()), argv.data(), out, err);
  };
  const std::string primary = primary_label(); // no borrow fields: the untiered S2
  for (const char* void_flag : {"on", "off"}) {
    const std::string output = std::string("void-") + void_flag;
    std::ostringstream out, err;
    ASSERT_EQ(nav(output, void_flag, out, err), 3) << void_flag << '\n' << err.str() << out.str();
    const auto path = dir.path / output;
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(path))
      files.push_back(entry.path().filename().string());
    std::sort(files.begin(), files.end());
    EXPECT_EQ(files, (std::vector<std::string>{"spo_diagnostics.csv", "v7_extras.json",
                                               "v7_transfer_coefficient.csv"}))
        << void_flag;
    EXPECT_NE(out.str().find("run VOID"), std::string::npos) << out.str();
    EXPECT_EQ(out.str().find("net Sharpe"), std::string::npos) << out.str();
    EXPECT_EQ(err.str().find("net Sharpe"), std::string::npos) << err.str();
    const auto extras = read_json_file(path / "v7_extras.json");
    EXPECT_EQ(extras.at("status"), "void") << void_flag;
    EXPECT_EQ(extras.at("voided"), "limits_unmet") << void_flag;
    EXPECT_NE(extras.at("void_reason").get<std::string>().find("Ruling E-31a"),
              std::string::npos);
    EXPECT_FALSE(extras.contains("capacity"));
    ASSERT_TRUE(extras.contains("limits_unmet")) << void_flag;
    const auto& unmet = extras.at("limits_unmet");
    EXPECT_EQ(unmet.at("book"), primary) << void_flag;
    EXPECT_NE(unmet.at("rule").get<std::string>().find("Ruling E-31a"), std::string::npos);
    const auto& trip = extras.at("spo_v3").at("tripwire");
    EXPECT_EQ(trip.at("status"), "void") << void_flag;
    EXPECT_EQ(trip.at("voided"), "limits_unmet") << void_flag;
    EXPECT_EQ(trip.at("specific_ceiling_void"), std::string(void_flag) == "on");
    EXPECT_EQ(unmet.at("count"), trip.at("limits_unmet_primary").at("count"));
    EXPECT_EQ(unmet.at("first_session"), trip.at("limits_unmet_primary").at("first_session"));
    // The published rows of the primary book: the same count and first session.
    const auto [count, first] = unmet_rows_in_csv(path / "spo_diagnostics.csv", primary);
    EXPECT_GT(count, 0U) << void_flag;
    EXPECT_EQ(unmet.at("count").get<usize>(), count) << void_flag;
    EXPECT_EQ(unmet.at("first_session").get<i64>(), first) << void_flag;
  }
}

// The diagnostics: the annualised tracking error of the plan and of the current book to the
// aim, the share of optimized names at their 1% ADV trade limit and the aim correlation, one
// CSV row per scored (decision, book), and the per-book report of their mean / max (min for
// the correlation) in the summary and the tripwire record, where the cell's mechanical
// criterion is read.
TEST(SpoV3, ReportsTrackingErrorAndShareAtTradeLimit) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_model(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  const Replay run = replay_v3(role, risk, sp::v3_params());
  ASSERT_FALSE(run.rows.empty());
  EXPECT_EQ(run.legacy_rows, 0U);
  EXPECT_EQ(run.horizon, sp::v3_horizon);
  const auto& c = run.calibration;
  ASSERT_TRUE(c.done);
  EXPECT_GT(c.aim_vol, 0.0);
  EXPECT_DOUBLE_EQ(c.gamma, sp::v3_sharpe_prior / c.aim_vol);
  // The first scored row is the calibration decision, planned from flat: its current
  // distance to the aim is the aim's own vol.
  EXPECT_DOUBLE_EQ(run.rows.front().tracking_error_current, c.aim_vol);
  // A book's first trading decision starts from flat (every earlier plan was flat): trading
  // closes part of the gap, and the 1% ADV limit binds on some names.
  const std::string first_book = run.rows.front().book;
  const auto trading = std::find_if(run.rows.begin(), run.rows.end(), [&](const auto& r) {
    return r.book == first_book && r.turnover > 0;
  });
  ASSERT_NE(trading, run.rows.end());
  EXPECT_TRUE(trading->converged) << trading->iterations;
  EXPECT_TRUE(trading->limits_met) << trading->limit_violation;
  EXPECT_LT(trading->tracking_error, trading->tracking_error_current);
  EXPECT_GT(trading->at_trade_limit, 0U);
  EXPECT_GT(trading->trade_limit_share, 0.0);
  std::map<std::string, std::vector<const sp::TrackingRow*>> by_book;
  for (const auto& r : run.rows) {
    // The fixture redraws every style exposure daily, so a book's beta can jump by more than
    // one session's trade limits can repair: such a solve is reported (limits unmet, counted
    // in the report below), never refused. Where the limits were met they hold.
    if (r.limits_met) {
      EXPECT_LT(std::abs(r.net), 1e-9) << r.session << ' ' << r.book;
      EXPECT_LE(r.abs_beta, sp::v3_beta_max + 1e-9) << r.session << ' ' << r.book;
    }
    EXPECT_TRUE(std::isfinite(r.tracking_error) && r.tracking_error >= 0.0) << r.session;
    EXPECT_TRUE(std::isfinite(r.tracking_error_current)) << r.session;
    EXPECT_TRUE(std::isfinite(r.tracking_error_shadow)) << r.session;
    EXPECT_GE(r.trade_cost, 0.0) << r.session;
    EXPECT_DOUBLE_EQ(r.gamma, c.gamma) << r.session;
    EXPECT_GT(r.optimized, 0U) << r.session;
    if (r.optimized > 0) {
      EXPECT_DOUBLE_EQ(r.trade_limit_share,
                       static_cast<f64>(r.at_trade_limit) / static_cast<f64>(r.optimized))
          << r.session;
    }
    if (std::isfinite(r.aim_correlation)) { // NaN only for a flat plan (no dispersion)
      EXPECT_GE(r.aim_correlation, -1.0 - 1e-12) << r.session;
      EXPECT_LE(r.aim_correlation, 1.0 + 1e-12) << r.session;
    } else {
      EXPECT_EQ(r.turnover, 0.0) << r.session;
    }
    by_book[r.book].push_back(&r);
  }
  // One CSV row per scored (decision, book); 43 columns (review A-4 added
  // aim_correlation_traded after aim_correlation, Ruling E-14a aim_correlation_traded_after
  // after it).
  EXPECT_EQ(static_cast<usize>(std::count(run.csv.begin(), run.csv.end(), '\n')),
            run.rows.size() + 1);
  const std::string header = run.csv.substr(0, run.csv.find('\n'));
  EXPECT_EQ(std::count(header.begin(), header.end(), ','), 42);
  for (const char* column : {",tracking_error,", ",tracking_error_current,", ",aim_correlation,",
                             ",aim_correlation_traded,", ",aim_correlation_traded_after,",
                             ",at_trade_limit,",
                             ",trade_limit_share,", ",gross_bound_breached,",
                             ",tracking_error_shadow,", ",aim_correlation_shadow"})
    EXPECT_NE(header.find(column), std::string::npos) << column;
  // The per-book report: summary and tripwire record agree with the rows.
  ASSERT_EQ(run.summary.size(), by_book.size());
  const auto& report = run.tripwire.at("report_only");
  ASSERT_EQ(report.size(), by_book.size());
  for (const auto& [book, list] : by_book) {
    f64 te_sum = 0, te_max = 0, share_max = 0, corr_sum = 0, corr_min = 2.0;
    usize corr_n = 0, unconverged = 0, unmet = 0;
    for (const auto* r : list) {
      unconverged += r->converged ? 0U : 1U;
      unmet += r->limits_met ? 0U : 1U;
      te_sum += r->tracking_error;
      te_max = std::max(te_max, r->tracking_error);
      share_max = std::max(share_max, r->trade_limit_share);
      if (!std::isfinite(r->aim_correlation)) continue;
      corr_sum += r->aim_correlation;
      corr_min = std::min(corr_min, r->aim_correlation);
      ++corr_n;
    }
    ASSERT_GT(corr_n, 0U) << book;
    const auto& entry = run.summary.at(book);
    EXPECT_EQ(entry.at("decisions"), list.size()) << book;
    EXPECT_NEAR(entry.at("tracking_error").at("mean").get<f64>(),
                te_sum / static_cast<f64>(list.size()), 1e-12 * (1.0 + te_max))
        << book;
    EXPECT_EQ(entry.at("tracking_error").at("max").get<f64>(), te_max) << book;
    EXPECT_EQ(entry.at("trade_limit_share").at("max").get<f64>(), share_max) << book;
    EXPECT_EQ(entry.at("aim_correlation").at("min").get<f64>(), corr_min) << book;
    EXPECT_NEAR(entry.at("aim_correlation").at("mean").get<f64>(),
                corr_sum / static_cast<f64>(corr_n), 1e-12)
        << book;
    EXPECT_EQ(entry.at("aim_correlation").at("n"), corr_n) << book;
    EXPECT_EQ(entry.at("unconverged"), unconverged) << book;
    EXPECT_EQ(entry.at("limits_unmet"), unmet) << book;
    EXPECT_TRUE(entry.at("shadow").at("mean_tracking_error").is_number()) << book;
    EXPECT_EQ(report.at(book).at("aim_correlation"), entry.at("aim_correlation")) << book;
    EXPECT_EQ(report.at(book).at("tracking_error"), entry.at("tracking_error")) << book;
    EXPECT_EQ(report.at(book).at("trade_limit_share"), entry.at("trade_limit_share")) << book;
  }
  expect_clear_or_limits_void(run);
}

// Review A-2 (v8 D-0 warm start): the warm-up decisions move the book as aim-partial-v5 and read
// no risk row, and gamma is calibrated on the first scored decision. On a risk store with no
// forecast before decision_begin (every earlier row unforecast, where a read refuses) the warm
// start runs: its calibration is the flat start's at decision_begin bit for bit (the same aim
// and slice), its rows are exactly the flat start's scored decisions, the book it scores from
// is the plain aim-partial-v5 warm start's (not flat), and its calibration block names the
// warm-up (the flat start's has no such key).
TEST(SpoV3, WarmStartCalibratesOnTheFirstScoredDecision) {
  constexpr usize begin = 12, warm = 8;
  const Role role(40, 12, 53);
  std::vector<u8> forecast(role.d, u8{1});
  for (usize d = 0; d < begin; ++d) forecast[d] = 0;
  const Directory dir;
  const auto sha = write_risk_model(dir.path, role.sessions, role.n, forecast, "role-sha", 3);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  ASSERT_TRUE(store) << store.error().to_string();
  const auto risk = std::make_shared<const sp::RiskStore>(std::move(*store));
  const auto params = sp::v3_params();
  const auto flat_cfg = nav_config();
  auto warm_cfg = flat_cfg;
  warm_cfg.warm_start_sessions = warm;
  const Replay flat = replay_v3(role, risk, params, flat_cfg, nullptr, begin);
  const Replay warmed = replay_v3(role, risk, params, warm_cfg, nullptr, begin);
  ASSERT_FALSE(flat.rows.empty());
  ASSERT_FALSE(warmed.rows.empty());
  // gamma of the first scored decision, not of row begin - K (unforecast: it would refuse).
  const auto& c = warmed.calibration;
  ASSERT_TRUE(c.done);
  EXPECT_TRUE(c.warm_up);
  EXPECT_FALSE(flat.calibration.warm_up);
  EXPECT_EQ(c.session, role.sessions[begin]);
  EXPECT_EQ(flat.calibration.session, role.sessions[begin]);
  EXPECT_EQ(bits(c.gamma), bits(flat.calibration.gamma));
  EXPECT_EQ(bits(c.aim_vol), bits(flat.calibration.aim_vol));
  EXPECT_EQ(c.names, flat.calibration.names);
  // Rows: the scored decisions only, the flat start's.
  ASSERT_EQ(warmed.rows.size(), flat.rows.size());
  for (usize k = 0; k < warmed.rows.size(); ++k) {
    EXPECT_EQ(warmed.rows[k].session, flat.rows[k].session) << k;
    EXPECT_EQ(warmed.rows[k].book, flat.rows[k].book) << k;
    EXPECT_GE(warmed.rows[k].session, role.sessions[begin]) << k;
    EXPECT_EQ(bits(warmed.rows[k].gamma), bits(c.gamma)) << k;
  }
  EXPECT_EQ(warmed.rows.front().session, role.sessions[begin]);
  // The warm-up built the book the scored window starts from (the flat start's is empty).
  ASSERT_FALSE(warmed.result.days.empty());
  ASSERT_FALSE(flat.result.days.empty());
  EXPECT_EQ(warmed.result.days.front().session, role.sessions[begin]);
  EXPECT_GT(warmed.result.days.front().pretrade_gross_dollars, 0.0);
  EXPECT_EQ(flat.result.days.front().pretrade_gross_dollars, 0.0);
  EXPECT_NE(bits(warmed.rows.front().tracking_error_current),
            bits(flat.rows.front().tracking_error_current));
  // That book is the plain aim-partial-v5 warm start's (no extension), bit for bit: row
  // score_begin's book entering it, its EXECUTE of the last warm-up orders and its gross.
  auto input = role.nav();
  input.target.decision_begin = begin;
  const auto plain = st::replay_nav(input, warm_cfg);
  ASSERT_TRUE(plain) << plain.error().to_string();
  ASSERT_FALSE(plain->days.empty());
  const auto& a = plain->days.front();
  const auto& b = warmed.result.days.front();
  EXPECT_EQ(a.session_index, b.session_index);
  EXPECT_EQ(bits(a.pretrade_gross_dollars), bits(b.pretrade_gross_dollars));
  EXPECT_EQ(bits(a.traded_dollars), bits(b.traded_dollars));
  EXPECT_EQ(bits(a.gross_leverage), bits(b.gross_leverage));
  // The calibration block: the warm-up is named only when there was one.
  ASSERT_TRUE(warmed.calibration_json.contains("warm_up"));
  EXPECT_EQ(warmed.calibration_json.at("warm_up").get<std::string>(),
            std::string(sp::warm_up_calibration_text));
  EXPECT_FALSE(flat.calibration_json.contains("warm_up"));
  EXPECT_EQ(warmed.calibration_json.at("session"), role.sessions[begin]);
  expect_clear_or_limits_void(warmed);
}

// Review A-4 and Ruling E-14a (review SPO-2): Ruling E-14's criterion reads the traded book
// after decision d's trades against the aim at d. Each row's aim_correlation_traded_after is the
// correlation of the holdings the book's NEXT rebalance decision's DECIDE read (the observed
// book's held weights there, which the holdings stream reports as the plan's current weights
// bit for bit) with the aim the tracker received at d, over every name either holds: recomputed
// here from the stream; NaN on the book's last scored decision. Review A-4's
// aim_correlation_traded (the book DECIDE read at d against the aim at d, one decision behind
// the trades; NaN from flat) stays beside it, as does the plan's aim_correlation. The report
// carries all three and its criterion is the traded-after mean against .9.
TEST(SpoV3, CriterionReadsTheTradedBookAfterTheTrades) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_model(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  Recorder recorder(role.n);
  const Replay run = replay_v3(role, risk, sp::v3_params(), nav_config(), &recorder);
  ASSERT_GT(run.rows.size(), 2U);
  std::map<i64, usize> row_of;
  for (usize t = 0; t < role.d; ++t) row_of[role.sessions[t]] = t;
  usize compared = 0, compared_after = 0, differ = 0;
  f64 sum_after = 0, lowest_after = 2.0;
  for (usize k = 0; k < run.rows.size(); ++k) {
    const auto& r = run.rows[k];
    const usize t = row_of.at(r.session);
    ASSERT_EQ(recorder.held.count(t), 1U) << t;
    ASSERT_EQ(recorder.aim.count(t), 1U) << t;
    const auto& aim = recorder.aim.at(t);
    ASSERT_EQ(aim.size(), role.n) << t;
    // Review A-4: the book DECIDE read at d.
    const f64 at_d = traded_pearson(recorder.held.at(t), aim);
    if (std::isnan(at_d)) {
      EXPECT_TRUE(std::isnan(r.aim_correlation_traded)) << t;
    } else {
      EXPECT_NEAR(r.aim_correlation_traded, at_d, 1e-12) << t;
      ++compared;
    }
    // Ruling E-14a: the book the next decision read, after d's trades.
    if (k + 1 == run.rows.size()) {
      EXPECT_TRUE(std::isnan(r.aim_correlation_traded_after)) << "the last scored decision";
      continue;
    }
    const usize next = row_of.at(run.rows[k + 1].session);
    ASSERT_GT(next, t);
    ASSERT_EQ(recorder.held.count(next), 1U) << next;
    const f64 after = traded_pearson(recorder.held.at(next), aim);
    if (std::isnan(after)) {
      EXPECT_TRUE(std::isnan(r.aim_correlation_traded_after)) << t;
      continue;
    }
    EXPECT_NEAR(r.aim_correlation_traded_after, after, 1e-12) << t;
    ++compared_after;
    sum_after += r.aim_correlation_traded_after;
    lowest_after = std::min(lowest_after, r.aim_correlation_traded_after);
    if (std::isfinite(r.aim_correlation_traded) &&
        std::abs(r.aim_correlation_traded_after - r.aim_correlation_traded) > 1e-9)
      ++differ;
  }
  EXPECT_TRUE(std::isnan(run.rows.front().aim_correlation_traded)); // the book is flat
  EXPECT_TRUE(std::isnan(run.rows.back().aim_correlation_traded_after));
  ASSERT_GT(compared, 0U);
  ASSERT_GT(compared_after, 0U);
  EXPECT_GT(differ, 0U) << "the book after the trades never differs from the one DECIDE read";
  // The report (one book): the three correlations; the criterion reads the traded-after mean.
  ASSERT_EQ(run.summary.size(), 1U);
  const auto& entry = run.summary.begin().value();
  EXPECT_TRUE(entry.contains("aim_correlation")); // the plan's, as before
  EXPECT_EQ(entry.at("aim_correlation_traded").at("n"), compared);
  EXPECT_EQ(entry.at("aim_correlation_traded_after").at("n"), compared_after);
  EXPECT_NEAR(entry.at("aim_correlation_traded_after").at("mean").get<f64>(),
              sum_after / static_cast<f64>(compared_after), 1e-12);
  EXPECT_EQ(entry.at("aim_correlation_traded_after").at("min").get<f64>(), lowest_after);
  const auto& criterion = entry.at("aim_correlation_criterion");
  EXPECT_EQ(criterion.at("reads"), "aim_correlation_traded_after.mean");
  EXPECT_NE(criterion.at("rule").get<std::string>().find("E-14a"), std::string::npos);
  EXPECT_EQ(criterion.at("threshold").get<f64>(), sp::v3_aim_correlation_min);
  EXPECT_EQ(sp::v3_aim_correlation_min, 0.9);
  const f64 value = criterion.at("value").get<f64>();
  EXPECT_EQ(value, entry.at("aim_correlation_traded_after").at("mean").get<f64>());
  EXPECT_EQ(criterion.at("met").get<bool>(), value >= 0.9);
  EXPECT_EQ(run.tripwire.at("report_only").begin().value().at("aim_correlation_criterion"),
            criterion);
  EXPECT_EQ(run.tripwire.at("report_only").begin().value().at("aim_correlation_traded_after"),
            entry.at("aim_correlation_traded_after"));
}

// Ruling E-14a per book (review R6B-S-2): the back-fill pairs a book's row with that same book's
// next rebalance decision, whatever the other books on the engine do. Two books on one engine
// decide at different cadences -- A (S2 x flat-300-v0) every session 2..11, B (S2 x
// swap-fin-v1) every third, 2, 5, 8, 11; A first on a shared session, as the lockstep's book
// order -- each entering a decision with its own book, drifted since its last plan. Every row's
// aim_correlation_traded_after is the correlation of the book its own book's next decision
// received with the aim at the row's decision, over every name either holds, and NaN on each
// book's last row. An engine-wide back-fill (one aim and one row for every book) pairs B's rows
// with A's books and fills A's row of session 2 from B's flat book (NaN).
TEST(SpoV3, TradedAfterIsBackFilledPerBookAcrossCadences) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_model(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  sp::Engine engine(sp::v3_params(), risk);
  const auto x = role.target();
  const auto law = nav_config(); // A's book; S2's law is every book's cost model
  auto swap_fin = law; // B's book: the tiered primary's financing
  swap_fin.scenario = st::nav_scenario_matrix(true)[st::nav_primary_scenario_index];
  struct Book {
    std::string label;
    st::NavReplayConfig cfg;
    usize cadence{};
    std::vector<f64> weights; // the book its next decision receives
    std::vector<f64> aim;     // the aim at its last decision
    usize row{};              // that decision's tracking row
    bool decided{};
  };
  std::array<Book, 2> books{Book{"A", law, 1, std::vector<f64>(role.n, 0.0), {}, 0, false},
                            Book{"B", swap_fin, 3, std::vector<f64>(role.n, 0.0), {}, 0, false}};
  const st::cost_v2::DecisionLiquidity liquidity{std::vector<f64>(role.n, 1e9),
                                                 std::vector<f64>(role.n, 0.02)};
  std::vector<f64> expected;      // per row: its book's back-fill (NaN: not yet decided again)
  std::vector<std::string> owner; // per row: its book
  for (usize d = 2; d < 12; ++d) { // every name a member (name 11 leaves on sessions 13..19)
    const std::vector<f64> desired = gross_one_desired(role, d);
    for (auto& b : books) {
      if ((d - 2) % b.cadence != 0) continue;
      if (b.decided) expected[b.row] = traded_pearson(b.weights, b.aim);
      const sp::BookDecision in{x, b.cfg, law.scenario, d, 1e8, desired, {}, {}, liquidity,
                                b.label};
      st::TargetReplayDay day;
      const auto status = engine.plan(in, b.weights, day);
      ASSERT_TRUE(status) << status.error().to_string();
      ASSERT_EQ(engine.tracking_rows().size(), expected.size() + 1) << b.label << ' ' << d;
      b.row = expected.size();
      expected.push_back(std::numeric_limits<f64>::quiet_NaN());
      owner.push_back(b.label);
      const auto aim = engine.last_aim();
      b.aim.assign(aim.begin(), aim.end());
      b.decided = true;
      // The book drifts until its next decision (a fixed return per name and session).
      for (usize i = 0; i < role.n; ++i)
        b.weights[i] *= 1.0 + 0.01 * static_cast<f64>((i + d) % 5) - 0.02;
    }
  }
  const auto rows = engine.tracking_rows();
  ASSERT_EQ(rows.size(), 14U); // A: 10 decisions, B: 4
  usize filled = 0;
  for (usize k = 0; k < rows.size(); ++k) {
    EXPECT_EQ(rows[k].book, owner[k]) << k;
    if (k == books[0].row || k == books[1].row) { // each book's last decision
      EXPECT_TRUE(std::isnan(rows[k].aim_correlation_traded_after)) << k << ' ' << owner[k];
      continue;
    }
    ASSERT_TRUE(std::isfinite(expected[k])) << "premise: a held book at row " << k;
    EXPECT_NEAR(rows[k].aim_correlation_traded_after, expected[k], 1e-12) << k << ' ' << owner[k];
    ++filled;
  }
  EXPECT_EQ(filled, rows.size() - 2);
}

// The CLI: --rule spo-v3 takes spo::v3_params (S_prior 20 by Ruling E-14, H 20, p .01, beta
// .02, ceiling 1 with the void on); --spo-alpha implied-aim is its only alpha (refused with
// spo-v1/v2 and for any other value); every spo flag that would move a registered constant is
// refused, the solver's --spo-iters and --spo-tol included (Ruling E-31a, review SPO-5); the
// allowed flags override; as spo-v1/v2 it refuses a per-name rate, and it refuses the holdings
// stream whatever the void flag (Ruling E-31a: its primary book's limits_unmet always voids);
// unlike them it accepts the capacity curve (Ruling E-37); its blocks are keyed spo_v3.
TEST(SpoV3, ParseRefusesTheRegisteredConstantsAndRoutesTheImpliedAim) {
  const std::vector<std::string> tail{"--risk-model", "risk", "--risk-model-sha256", "abc",
                                      "--output", "x"};
  const auto parse = [&](std::vector<std::string> head) {
    head.insert(head.end(), tail.begin(), tail.end());
    std::vector<char*> argv;
    for (auto& a : head) argv.push_back(a.data());
    return v7::parse_nav_v7_args(static_cast<int>(argv.size()), argv.data());
  };
  EXPECT_EQ(sp::v3_sharpe_prior, 20.0); // Ruling E-14 (not the 1.0 first declared)
  const auto v3 = parse({"nav", "--rule", "spo-v3", "--spo-alpha", "implied-aim"});
  ASSERT_TRUE(v3) << v3.error().to_string();
  const auto& p = v3->options.spo_params;
  const auto registered = sp::v3_params();
  EXPECT_TRUE(v3->options.spo_v1);
  EXPECT_FALSE(v3->options.aim_v6);
  EXPECT_EQ(p.version, 3U);
  EXPECT_EQ(p.sharpe_prior, sp::v3_sharpe_prior);
  EXPECT_EQ(p.horizon, sp::v3_horizon);
  EXPECT_EQ(p.adv_trade_p, 0.01);
  EXPECT_EQ(p.beta_max, 0.02);
  EXPECT_EQ(p.specific_ceiling, 1.0);
  EXPECT_TRUE(p.void_on_capped);
  EXPECT_EQ(p.max_iterations, registered.max_iterations);
  EXPECT_EQ(p.tolerance, registered.tolerance);
  EXPECT_TRUE(p.all_books);
  EXPECT_TRUE(sp::validate_params(p));
  EXPECT_STREQ(sp::rule_name(p), "spo-v3");
  EXPECT_STREQ(sp::json_key(p), "spo_v3");
  EXPECT_EQ(v3->args, (std::vector<std::string>{"nav", "--rule", "aim-partial-v5", "--output",
                                                "x"}));
  EXPECT_TRUE(parse({"nav", "--rule", "spo-v3"})); // --spo-alpha is optional
  // The allowed flags override the rule's defaults.
  const auto allowed = parse({"nav", "--rule", "spo-v3", "--spo-books", "primary",
                              "--specific-ceiling", "2", "--specific-ceiling-void", "off"});
  ASSERT_TRUE(allowed) << allowed.error().to_string();
  const auto& a = allowed->options.spo_params;
  EXPECT_EQ(a.max_iterations, registered.max_iterations); // registered (Ruling E-31a)
  EXPECT_EQ(a.tolerance, registered.tolerance);
  EXPECT_FALSE(a.all_books);
  EXPECT_EQ(a.specific_ceiling, 2.0);
  EXPECT_FALSE(a.void_on_capped);
  EXPECT_EQ(a.sharpe_prior, sp::v3_sharpe_prior);
  // Refused with spo-v3: every flag that would move a registered constant (still spo-v2's),
  // the solver's iteration cap and tolerance included (Ruling E-31a).
  for (const char* flag : {"--gamma", "--ic-book", "--w-max", "--adv-cap-q", "--adv-trade-p",
                           "--target-vol", "--spo-horizon", "--alpha-horizon", "--spo-gross",
                           "--spo-iters", "--spo-tol"}) {
    const auto refused = parse({"nav", "--rule", "spo-v3", flag, "0.5"});
    ASSERT_FALSE(refused) << flag;
    EXPECT_EQ(refused.error().code(), co::ErrorCode::InvalidArgument) << flag;
    EXPECT_NE(refused.error().message().find(flag), std::string::npos)
        << refused.error().to_string();
    const auto early = parse({"nav", flag, "0.5", "--rule", "spo-v3"}); // wherever it stands
    EXPECT_FALSE(early) << flag;
  }
  EXPECT_TRUE(parse({"nav", "--rule", "spo-v2", "--gamma", "5"}));
  EXPECT_TRUE(parse({"nav", "--rule", "spo-v2", "--spo-iters", "3000", "--spo-tol", "1e-10"}));
  // --spo-alpha: implied-aim only, spo-v3 only, once.
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v3", "--spo-alpha", "fitted"}));
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v1", "--spo-alpha", "implied-aim"}));
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v2", "--spo-alpha", "implied-aim"}));
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v3", "--spo-alpha", "implied-aim", "--spo-alpha",
                      "implied-aim"}));
  {
    std::vector<std::string> args{"nav", "--spo-alpha", "implied-aim", "--output", "x"};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    EXPECT_FALSE(v7::parse_nav_v7_args(static_cast<int>(argv.size()), argv.data())); // no rule
    EXPECT_TRUE(v7::claims_nav_args(static_cast<int>(argv.size()), argv.data()));
  }
  {
    std::vector<std::string> args{"nav", "--rule", "spo-v3", "--output", "x"};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    EXPECT_FALSE(v7::parse_nav_v7_args(static_cast<int>(argv.size()), argv.data())); // no model
    EXPECT_TRUE(v7::claims_nav_args(static_cast<int>(argv.size()), argv.data()));
  }
  // As spo-v1/v2: the fixed rate. Unlike them, no holdings stream even with the void off (Ruling
  // E-31a), and the capacity curve runs, as a report-only pass (Ruling E-37,
  // SpoV3.CapacityPass...).
  const auto curve = parse({"nav", "--rule", "spo-v3", "--capacity-curve"});
  ASSERT_TRUE(curve) << curve.error().to_string();
  EXPECT_TRUE(curve->options.capacity);
  EXPECT_EQ(curve->args, (std::vector<std::string>{"nav", "--rule", "aim-partial-v5",
                                                   "--output", "x"}));
  for (const char* spo : {"spo-v1", "spo-v2"}) {
    const auto refused = parse({"nav", "--rule", spo, "--capacity-curve"});
    ASSERT_FALSE(refused) << spo;
    EXPECT_NE(refused.error().message().find("capacity curve"), std::string::npos)
        << refused.error().to_string();
  }
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v3", "--capacity-curve", "--rate", "per-name-v1"}));
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v3", "--rate", "per-name-v1"}));
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v3", "--emit-holdings", "h"}));
  const auto holdings = parse({"nav", "--rule", "spo-v3", "--emit-holdings", "h",
                               "--specific-ceiling-void", "off"});
  ASSERT_FALSE(holdings);
  EXPECT_EQ(holdings.error().code(), co::ErrorCode::InvalidArgument);
  EXPECT_NE(holdings.error().message().find("Ruling E-31a"), std::string::npos)
      << holdings.error().to_string();
  EXPECT_TRUE(parse({"nav", "--rule", "spo-v2", "--emit-holdings", "h",
                     "--specific-ceiling-void", "off"})); // spo-v1/v2 unchanged
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v3", "--rule", "aim-partial-v6"}));
  // The blocks are keyed spo_v3 and the rule is relabelled.
  Json recipe{{"rule", "aim-partial-v5+neutral-price-risk-v1"}};
  {
    const v7::ScopedNavExtension extension(v3->options);
    v7::extend_recipe(recipe);
  }
  EXPECT_EQ(recipe["rule"], "spo-v3+neutral-price-risk-v1");
  ASSERT_TRUE(recipe.contains("v7"));
  ASSERT_TRUE(recipe["v7"].contains("spo_v3"));
  EXPECT_FALSE(recipe["v7"].contains("spo_v2"));
  EXPECT_EQ(recipe["v7"]["spo_v3"]["parameters"]["sharpe_prior"], 20.0);
  EXPECT_EQ(recipe["v7"]["spo_v3"]["parameters"]["alpha"].get<std::string>().rfind("implied-aim",
                                                                                   0),
            0U);
  const auto declared = recipe["v7"]["spo_v3"]["rule"].get<std::string>();
  EXPECT_NE(declared.find("S_prior = 20 "), std::string::npos) << declared;
  EXPECT_NE(declared.find("Ruling E-14"), std::string::npos) << declared;
  EXPECT_EQ(recipe["v7"]["spo_v3"]["calibration"]["rule"].get<std::string>().rfind(
                "gamma = S_prior / sigma_aim", 0),
            0U);
}

// v8 E-26: spo-v3's aim is L x the desired target the aim-partial-v5 NAV path forms, its
// shaping included. On the fixture with the hold band b = .1 and the ADV cap Q = .05, both
// binding (members keep their band value, names are clipped), the aim the tracker receives at
// every rebalance decision equals L x the plain aim-partial-v5 replay's desired target bit for
// bit on every member (each one reported), and 0 off the members; the construction record of
// every decision agrees.
TEST(SpoV3, AimIncludesHoldBandAndAdvCapWhenDeclared) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_model(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  auto cfg = undusted_config();
  cfg.target.hold_band = 0.1;
  cfg.target.adv_hold_q = 0.05;
  Recorder reference(role.n); // the aim-partial-v5 path itself: no extension installed
  const std::array<st::NavScenario, 1> primary{cfg.scenario};
  const auto plain = st::replay_nav_scenarios(role.nav(), cfg, primary, reference, 0);
  ASSERT_TRUE(plain) << plain.error().to_string();
  ASSERT_EQ(plain->size(), 1U);
  usize kept = 0, clipped = 0;
  for (const auto& day : plain->front().days) {
    kept += day.construction.hold_kept;
    clipped += day.construction.adv_clipped;
  }
  EXPECT_GT(kept, 0U) << "the hold band keeps no member";
  EXPECT_GT(clipped, 0U) << "the ADV cap clips no name";
  Recorder tracked(role.n);
  const Replay run = replay_v3(role, risk, sp::v3_params(), cfg, &tracked);
  ASSERT_FALSE(run.rows.empty());
  ASSERT_FALSE(tracked.aim.empty());
  ASSERT_EQ(tracked.aim.size(), reference.desired.size());
  const f64 leverage = cfg.target.aim_leverage;
  usize compared = 0;
  for (const auto& [t, aim] : tracked.aim) {
    const auto found = reference.desired.find(t);
    ASSERT_NE(found, reference.desired.end()) << "session " << t;
    const auto& desired = found->second;
    ASSERT_EQ(aim.size(), role.n) << "session " << t;
    for (usize i = 0; i < role.n; ++i) {
      if (role.member[t * role.n + i] == 0) {
        EXPECT_EQ(bits(aim[i]), bits(0.0)) << "session " << t << " name " << i;
        continue;
      }
      ASSERT_FALSE(std::isnan(desired[i])) << "member not reported: " << t << ' ' << i;
      EXPECT_EQ(bits(aim[i]), bits(leverage * desired[i])) << "session " << t << " name " << i;
      ++compared;
    }
  }
  EXPECT_GT(compared, 0U);
  const auto& a = plain->front().days;
  const auto& b = run.result.days;
  ASSERT_EQ(a.size(), b.size());
  for (usize k = 0; k < a.size(); ++k) {
    EXPECT_EQ(a[k].rebalance, b[k].rebalance) << k;
    EXPECT_EQ(a[k].construction.hold_kept, b[k].construction.hold_kept) << k;
    EXPECT_EQ(a[k].construction.hold_moved, b[k].construction.hold_moved) << k;
    EXPECT_EQ(a[k].construction.adv_clipped, b[k].construction.adv_clipped) << k;
    EXPECT_EQ(bits(a[k].construction.adv_clipped_mass), bits(b[k].construction.adv_clipped_mass))
        << k;
  }
}

// v8 E-26, both flags absent: spo-v3 publishes what it did. The shaping reaches the rule only
// through the shared desired target and the rule's own blocks carry no shaping key, so the
// band declared at its identity b = 0 (the kernel runs and carries its state; no key) with
// Q = 0 (off) gives every published block and every received aim bit for bit, and the rule id
// stays "spo-v3"; declaring b = .1 and Q = .05 changes the diagnostics and extends the rule id
// exactly as the aim-partial path's.
TEST(SpoV3, ShapingFlagsAbsentIsByteIdentical) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_model(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  const auto params = sp::v3_params();
  const auto absent_cfg = nav_config();
  ASSERT_FALSE(absent_cfg.target.hold_band.has_value());
  ASSERT_EQ(absent_cfg.target.adv_hold_q, 0.0);
  auto identity_cfg = absent_cfg;
  identity_cfg.target.hold_band = 0.0;
  auto shaped_cfg = absent_cfg;
  shaped_cfg.target.hold_band = 0.1;
  shaped_cfg.target.adv_hold_q = 0.05;
  Recorder absent_aims(role.n), identity_aims(role.n);
  const Replay absent = replay_v3(role, risk, params, absent_cfg, &absent_aims);
  const Replay identity = replay_v3(role, risk, params, identity_cfg, &identity_aims);
  const Replay shaped = replay_v3(role, risk, params, shaped_cfg);
  ASSERT_FALSE(absent.rows.empty());
  EXPECT_EQ(identity.csv, absent.csv);
  EXPECT_EQ(identity.parameters.dump(), absent.parameters.dump());
  EXPECT_EQ(identity.summary.dump(), absent.summary.dump());
  EXPECT_EQ(identity.tripwire.dump(), absent.tripwire.dump());
  EXPECT_EQ(identity.calibration_json.dump(), absent.calibration_json.dump());
  EXPECT_EQ(identity.declaration, absent.declaration);
  ASSERT_FALSE(absent_aims.aim.empty());
  ASSERT_EQ(identity_aims.aim.size(), absent_aims.aim.size());
  for (const auto& [t, aim] : absent_aims.aim) {
    const auto found = identity_aims.aim.find(t);
    ASSERT_NE(found, identity_aims.aim.end()) << "session " << t;
    ASSERT_EQ(found->second.size(), aim.size()) << "session " << t;
    for (usize i = 0; i < aim.size(); ++i)
      EXPECT_EQ(bits(found->second[i]), bits(aim[i])) << "session " << t << " name " << i;
  }
  // The rule's blocks name no shaping, declared or not.
  for (const Replay* run : {&absent, &shaped}) {
    for (const char* key : {"hold_band", "hold-band", "adv_hold", "adv-hold"}) {
      EXPECT_EQ(run->parameters.dump().find(key), std::string::npos) << key;
      EXPECT_EQ(run->declaration.find(key), std::string::npos) << key;
    }
  }
  ASSERT_FALSE(shaped.rows.empty());
  EXPECT_NE(shaped.csv, absent.csv); // the shaped aim reached the tracker
  // The rule id: the NAV replay's, relabelled by the extension as every spo-v3 id.
  const auto rule = [&](const st::NavReplayConfig& cfg, bool v3) {
    Json recipe{{"rule", st::detail::construction_rule_id(cfg.target)}};
    if (v3) {
      v7::NavV7Options o;
      o.spo_v1 = true;
      o.spo_params = params;
      const v7::ScopedNavExtension extension(o);
      v7::extend_recipe(recipe);
    }
    return recipe.at("rule").get<std::string>();
  };
  EXPECT_EQ(rule(absent_cfg, true), "spo-v3");
  EXPECT_EQ(rule(identity_cfg, true), "spo-v3");
  EXPECT_EQ(rule(shaped_cfg, false), "aim-partial-v5+hold-band-0.1+adv-hold-0.05");
  EXPECT_EQ(rule(shaped_cfg, true), "spo-v3+hold-band-0.1+adv-hold-0.05");
}

// v8 E-26 CLI: --hold-band and --adv-hold-q pass through to the replay under spo-v3 (in the
// order given; the replay's own parser and validation read them) and are refused with spo-v1
// and spo-v2 whatever their value or place.
TEST(SpoV3, ShapingFlagsPassThroughAndSpoV1V2RefuseThem) {
  const std::vector<std::string> tail{"--risk-model", "risk", "--risk-model-sha256", "abc",
                                      "--output", "x"};
  const auto parse = [&](std::vector<std::string> head) {
    head.insert(head.end(), tail.begin(), tail.end());
    std::vector<char*> argv;
    for (auto& a : head) argv.push_back(a.data());
    return v7::parse_nav_v7_args(static_cast<int>(argv.size()), argv.data());
  };
  const auto v3 =
      parse({"nav", "--hold-band", ".1", "--rule", "spo-v3", "--adv-hold-q", ".05"});
  ASSERT_TRUE(v3) << v3.error().to_string();
  EXPECT_EQ(v3->options.spo_params.version, 3U);
  EXPECT_EQ(v3->args, (std::vector<std::string>{"nav", "--hold-band", ".1", "--rule",
                                                "aim-partial-v5", "--adv-hold-q", ".05",
                                                "--output", "x"}));
  EXPECT_TRUE(parse({"nav", "--rule", "spo-v3", "--hold-band", "0"}));
  for (const char* spo : {"spo-v1", "spo-v2"}) {
    EXPECT_TRUE(parse({"nav", "--rule", spo})) << spo; // without the flags, as before
    for (const char* flag : {"--hold-band", "--adv-hold-q"}) {
      for (const char* value : {"0", ".1"}) {
        const auto refused = parse({"nav", "--rule", spo, flag, value});
        ASSERT_FALSE(refused) << spo << ' ' << flag << ' ' << value;
        EXPECT_EQ(refused.error().code(), co::ErrorCode::InvalidArgument) << spo << ' ' << flag;
        EXPECT_NE(refused.error().message().find(flag), std::string::npos)
            << refused.error().to_string();
        EXPECT_NE(refused.error().message().find("spo-v3"), std::string::npos)
            << refused.error().to_string();
        EXPECT_FALSE(parse({"nav", flag, value, "--rule", spo})) << spo << ' ' << flag;
      }
    }
  }
}

// Ruling E-37 (review N-2): spo-v3 runs --capacity-curve as a report-only pass. A capacity book
// plans on the extension's capacity engine as the tracker of the NAV-m book (the primary S2 law
// at m x NAV_post: trade limit p ADV / (m NAV), impact at m NAV): x1 is the main pass's S2 book
// bit for bit (its tracking rows too, relabelled), x4 at NAV V is the spo-v3 book replayed at 4V
// divided by 4, and the tracker planned at the base NAV under the x4 fills is another book. The
// main engine keeps exactly the rows, summary and tripwire of a run without the capacity pass
// (review SPO-4: no capacity row in its rows or counts); the capacity engine's gamma is the main
// pass's bit for bit and its tripwire never voids (no primary book). x4's first trading row is
// the NAV-4V tracker's (its own trade limit). A capacity pass on a book that is not a capacity
// book is refused.
TEST(SpoV3, CapacityPassBooksAreTheNavMultipleTrackerAndLeaveTheMainPassAlone) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_model(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  const auto params = sp::v3_params();
  const auto base = nav_config();
  const auto books = st::cost_v2::capacity_scenarios(base.scenario);
  ASSERT_EQ(books[1].id, "capacity-x1-v1");
  ASSERT_EQ(books[3].id, "capacity-x4-v1");
  const Replay alone = replay_v3(role, risk, params); // no capacity pass
  auto genuine_cfg = base;
  genuine_cfg.initial_nav = 4.0 * base.initial_nav;
  const Replay genuine = replay_v3(role, risk, params, genuine_cfg); // the NAV-4 tracker
  auto shortcut_cfg = base;
  shortcut_cfg.scenario = books[3];
  const Replay shortcut = replay_v3(role, risk, params, shortcut_cfg); // planned at base NAV
  ASSERT_FALSE(alone.rows.empty());
  v7::NavV7Options o;
  o.spo_v1 = true;
  o.capacity = true;
  o.spo_params = params;
  o.spo_risk = risk;
  v7::ScopedNavExtension extension(o);
  const auto* engine = extension.spo_engine();
  const auto* capacity = extension.spo_capacity_engine();
  ASSERT_NE(engine, nullptr);
  ASSERT_NE(capacity, nullptr);
  EXPECT_TRUE(engine->params().void_on_capped);
  EXPECT_FALSE(capacity->params().void_on_capped); // report only: never voids
  const auto pass = [&](v7::NavV7Pass p, const st::NavScenario& s) {
    extension.begin_run(p);
    auto cfg = base;
    cfg.scenario = s;
    return st::replay_nav(role.nav(), cfg);
  };
  const auto primary = pass(v7::NavV7Pass::Main, base.scenario);
  const auto unit = pass(v7::NavV7Pass::Capacity, books[1]);
  const auto x4 = pass(v7::NavV7Pass::Capacity, books[3]);
  ASSERT_TRUE(primary) << primary.error().to_string();
  ASSERT_TRUE(unit) << unit.error().to_string();
  ASSERT_TRUE(x4) << x4.error().to_string();
  // The main pass: the engine's rows, summary and tripwire are a plain run's, after both passes.
  EXPECT_EQ(engine->rows_csv(), alone.csv);
  EXPECT_EQ(engine->rows_summary_json().dump(), alone.summary.dump());
  EXPECT_EQ(engine->rows_tripwire_json().dump(), alone.tripwire.dump());
  // The main tripwire reads the main rows alone: void exactly when a plain run's is (E-31a).
  EXPECT_EQ(static_cast<bool>(v7::capture({}, {}, {})), alone.primary_unmet == 0);
  // gamma = S_prior / sigma_aim is scale free: the capacity engine's is the main pass's.
  ASSERT_TRUE(capacity->calibration().done);
  EXPECT_EQ(bits(capacity->calibration().gamma), bits(engine->calibration().gamma));
  EXPECT_EQ(bits(genuine.calibration.gamma), bits(engine->calibration().gamma));
  // x1: the main pass's S2 book bit for bit, and its tracking rows the main rows.
  ASSERT_EQ(primary->days.size(), alone.result.days.size());
  ASSERT_EQ(unit->days.size(), primary->days.size());
  for (usize t = 0; t < primary->days.size(); ++t) {
    const auto& a = primary->days[t];
    const auto& u = unit->days[t];
    EXPECT_EQ(bits(a.net_return), bits(alone.result.days[t].net_return)) << t;
    EXPECT_EQ(bits(u.net_return), bits(a.net_return)) << t;
    EXPECT_EQ(bits(u.planned_gross), bits(a.planned_gross)) << t;
    EXPECT_EQ(bits(u.traded_dollars), bits(a.traded_dollars)) << t;
  }
  std::vector<sp::TrackingRow> unit_rows, x4_rows;
  for (const auto& r : capacity->tracking_rows()) {
    if (r.book.rfind(books[1].id + "+", 0) == 0) {
      auto row = r;
      row.book = alone.rows.front().book;
      unit_rows.push_back(std::move(row));
    } else if (r.book.rfind(books[3].id + "+", 0) == 0) {
      x4_rows.push_back(r);
    }
  }
  EXPECT_EQ(sp::tracking_csv(unit_rows), alone.csv);
  EXPECT_EQ(x4_rows.size(), alone.rows.size());
  EXPECT_EQ(capacity->rows_tripwire_json().at("status"), "clear");
  // Review SPO-4: no capacity row reaches the main pass's rows, tripwire or counts (Ruling
  // E-31a included), and the capacity engine, which has no primary book, never voids.
  for (const auto& r : engine->tracking_rows())
    EXPECT_NE(r.book.rfind("capacity-", 0), 0U) << r.book;
  EXPECT_EQ(engine->tracking_rows().size(), alone.rows.size());
  EXPECT_EQ(engine->rows_tripwire_json().at("limits_unmet_primary").at("count"),
            alone.primary_unmet);
  EXPECT_EQ(engine->primary_book(), primary_label());
  EXPECT_TRUE(capacity->primary_book().empty());
  EXPECT_TRUE(capacity->rows_tripwire());
  const auto capacity_trip = capacity->rows_tripwire_json();
  EXPECT_EQ(capacity_trip.at("limits_unmet_primary").at("count"), 0U);
  EXPECT_FALSE(capacity_trip.contains("voided"));
  // Review SPO-4: each capacity book plans with the trade limit of its own NAV. On x4's first
  // trading decision (every earlier plan flat, so NAV_post is exactly the initial NAV) its row is
  // the NAV-4V tracker's (the same names at the trade limit, turnover and cost), not the x1
  // book's (the base-NAV limit is 4 times looser).
  ASSERT_EQ(x4_rows.size(), genuine.rows.size());
  ASSERT_EQ(unit_rows.size(), genuine.rows.size());
  const auto first = std::find_if(genuine.rows.begin(), genuine.rows.end(),
                                  [](const sp::TrackingRow& r) { return r.turnover > 0; });
  ASSERT_NE(first, genuine.rows.end());
  const auto k = static_cast<usize>(first - genuine.rows.begin());
  const auto& nav4 = genuine.rows[k];
  const auto& x4_row = x4_rows[k];
  EXPECT_EQ(x4_row.session, nav4.session);
  EXPECT_GT(x4_row.at_trade_limit, 0U);
  EXPECT_EQ(x4_row.at_trade_limit, nav4.at_trade_limit);
  EXPECT_NEAR(x4_row.turnover, nav4.turnover, 1e-12 * nav4.turnover);
  EXPECT_NEAR(x4_row.trade_cost, nav4.trade_cost, 1e-12 * nav4.trade_cost);
  EXPECT_NEAR(x4_row.tracking_error, nav4.tracking_error, 1e-12 * nav4.tracking_error);
  EXPECT_NE(bits(x4_row.turnover), bits(unit_rows[k].turnover)) << "x4 planned at the base NAV";
  // x4 at NAV V is the NAV-4V tracker divided by 4; planned at the base NAV it is another book.
  const auto& g4 = genuine.result.days;
  ASSERT_EQ(x4->days.size(), g4.size());
  ASSERT_EQ(shortcut.result.days.size(), g4.size());
  usize trading = 0;
  f64 shortcut_gap = 0;
  for (usize t = 0; t < g4.size(); ++t) {
    const auto& g = g4[t];
    const auto& s = x4->days[t];
    EXPECT_NEAR(s.net_return, g.net_return, 1e-9 + 1e-6 * std::abs(g.net_return)) << t;
    EXPECT_NEAR(s.planned_gross, g.planned_gross, 1e-9 + 1e-6 * g.planned_gross) << t;
    EXPECT_NEAR(4.0 * s.traded_dollars, g.traded_dollars, 1.0 + 1e-5 * g.traded_dollars) << t;
    trading += g.traded_dollars > 0 ? 1U : 0U;
    shortcut_gap =
        std::max(shortcut_gap, std::abs(shortcut.result.days[t].planned_gross - g.planned_gross));
  }
  EXPECT_GT(trading, 0U);
  EXPECT_GT(shortcut_gap, 1e-4) << "the base-NAV plan is the NAV-4V tracker's plan";
  // A capacity pass on a book that is no capacity book: refused before any plan.
  extension.begin_run(v7::NavV7Pass::Capacity);
  const auto stray = st::replay_nav(role.nav(), base);
  ASSERT_FALSE(stray);
  EXPECT_NE(stray.error().message().find("Ruling E-37"), std::string::npos)
      << stray.error().to_string();
}

// P9 C1 fix 1 (Ruling C1-SPO): without --adv-hold-q the spo-v3 capacity declaration is base
// d7c1c520's sentence byte for byte: in the recipe (keyed off the recipe's own adv_hold_rule,
// so the decide path, which runs no replay, agrees) and in the holdings manifest (keyed off the
// replay's configs, v7::configure; off before any). With --adv-hold-q the sentence names the
// multiple's NAV and the recipe's adv_hold_rule says every capacity book's cap reads it.
TEST(SpoV3, CapacityDeclarationWithoutAdvHoldKeepsTheBaseBytes) {
  const std::string base_sentence =
      "spo-v3 in the capacity pass (Ruling E-37, report only; the primary series and the main "
      "pass's spo_diagnostics.csv, tripwire and summary are unchanged): each capacity book is the "
      "spo-v3 tracker of the NAV-m book, planned on its own engine under the primary S2 law at NAV "
      "m x NAV_post, so its trade limit p ADV_i / (m NAV) and its impact impact_y sigma_i sqrt(m "
      "NAV / ADV_i) are the NAV-m book's; the aim is the run's L x desired, whose ADV cap "
      "(--adv-hold-q) reads the initial NAV for every multiple (Ruling E-15); gamma is calibrated "
      "on the same first scored decision as the main pass (gamma_equals_main); the capacity "
      "engine's tripwire and per-book report are recorded (v7_extras.json capacity_spo_v3), never "
      "voiding the run: it has no primary book, and its rows never enter the main pass's rows, "
      "tripwire, counts or Ruling E-31a (review SPO-4); x1 is the main pass's S2 book bit for bit";
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_model(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  v7::NavV7Options o;
  o.spo_v1 = true;
  o.capacity = true;
  o.spo_params = sp::v3_params();
  o.spo_risk = risk;
  v7::ScopedNavExtension extension(o);
  ASSERT_NE(extension.spo_capacity_engine(), nullptr);
  // The recipe: its own adv_hold_rule key decides.
  Json plain = Json::object();
  v7::extend_recipe(plain);
  ASSERT_TRUE(plain.contains("v7"));
  ASSERT_TRUE(plain.at("v7").contains("capacity_spo_v3"));
  EXPECT_EQ(plain.at("v7").at("capacity_spo_v3").get<std::string>(), base_sentence);
  EXPECT_FALSE(plain.contains("adv_hold_rule"));
  Json capped{{"adv_hold_q", 0.1}, {"adv_hold_rule", "the target replay's sentence"}};
  v7::extend_recipe(capped);
  const auto capped_sentence = capped.at("v7").at("capacity_spo_v3").get<std::string>();
  EXPECT_NE(capped_sentence, base_sentence);
  EXPECT_NE(capped_sentence.find("reads the multiple's NAV m x NAV"), std::string::npos)
      << capped_sentence;
  EXPECT_NE(capped.at("adv_hold_rule").get<std::string>().find("every capacity book"),
            std::string::npos);
  // The holdings manifest: the replay's configs decide (v7::configure).
  const auto manifest_sentence = [] {
    Json manifest = Json::object();
    v7::extend_holdings(manifest);
    return manifest.at("v7").at("capacity_spo_v3").get<std::string>();
  };
  EXPECT_EQ(manifest_sentence(), base_sentence);
  auto cfg = nav_config();
  cfg.target.adv_hold_q = 0.1;
  v7::configure(cfg);
  EXPECT_EQ(manifest_sentence(), capped_sentence);
  cfg.target.adv_hold_q = 0;
  v7::configure(cfg);
  EXPECT_EQ(manifest_sentence(), base_sentence);
}

// Ruling E-37 (review N-2): theta (--trade-fraction) has no effect on spo-v3 -- H is the
// registered 20 and the tracker never reads theta -- so the book replayed at theta .25 and at
// theta .1 is the same bit for bit (every day's net return, plan and trades, every tracking row's
// plan columns); only the aim-partial-v5 shadow book's diagnostics move. The verb's help says so.
TEST(SpoV3, TradeFractionHasNoEffectOnTheBook) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_model(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  const auto params = sp::v3_params();
  auto slow = nav_config();
  ASSERT_EQ(slow.target.trade_fraction, 0.25);
  slow.target.trade_fraction = 0.1;
  const Replay fast_run = replay_v3(role, risk, params);
  const Replay slow_run = replay_v3(role, risk, params, slow);
  ASSERT_FALSE(fast_run.rows.empty());
  ASSERT_EQ(slow_run.rows.size(), fast_run.rows.size());
  ASSERT_EQ(slow_run.result.days.size(), fast_run.result.days.size());
  for (usize t = 0; t < fast_run.result.days.size(); ++t) {
    const auto& a = fast_run.result.days[t];
    const auto& b = slow_run.result.days[t];
    EXPECT_EQ(bits(a.net_return), bits(b.net_return)) << t;
    EXPECT_EQ(bits(a.planned_gross), bits(b.planned_gross)) << t;
    EXPECT_EQ(bits(a.traded_dollars), bits(b.traded_dollars)) << t;
  }
  bool shadow_moved = false;
  for (usize k = 0; k < fast_run.rows.size(); ++k) {
    const auto& a = fast_run.rows[k];
    const auto& b = slow_run.rows[k];
    EXPECT_EQ(bits(a.tracking_error), bits(b.tracking_error)) << k;
    EXPECT_EQ(bits(a.gross), bits(b.gross)) << k;
    EXPECT_EQ(bits(a.turnover), bits(b.turnover)) << k;
    EXPECT_EQ(bits(a.trade_cost), bits(b.trade_cost)) << k;
    EXPECT_EQ(a.iterations, b.iterations) << k;
    shadow_moved = shadow_moved || bits(a.turnover_shadow) != bits(b.turnover_shadow);
  }
  EXPECT_TRUE(shadow_moved) << "theta never reached the shadow book";
  EXPECT_EQ(fast_run.horizon, sp::v3_horizon);
  EXPECT_EQ(slow_run.horizon, sp::v3_horizon);
  std::ostringstream help;
  v7::append_help(help);
  EXPECT_NE(help.str().find("--trade-fraction (theta) has no effect on spo-v3"),
            std::string::npos);
  EXPECT_NE(help.str().find("R-9 is undefined on it (Ruling E-37)"), std::string::npos);
  EXPECT_NE(help.str().find("[--capacity-curve] runs report only"), std::string::npos);
}
} // namespace
