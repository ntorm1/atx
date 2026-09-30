// spo-v3 (platform v8 R-6): target tracking toward the aim through the spo engine and the v7
// hook -- the aim itself without costs or limits (a direct Engine::plan), the gross sanity
// bound slack on the fixture and its breach voiding the run, the tracking error, trade-limit
// share and aim correlation with their per-book report, and the CLI refusals of the
// registered constants. The spo-v1 / spo-v2 digest guard is strategy_spo_v3_pin_test.cpp.

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
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

// A replay of the fixture (nav_config: S1, S2, S3, cadence 1, theta .25, L 1.2, NAV 1e8) under
// spo-v3 through the v7 hook, with what the extension would publish.
struct Replay {
  std::vector<sp::TrackingRow> rows;
  usize legacy_rows{}; // spo-v1/v2 rows (none under spo-v3)
  sp::Calibration calibration;
  f64 horizon{}, bound{};
  Json parameters, summary, tripwire;
  std::string csv;
  bool captured{};
};
Replay replay_v3(const Role& role, std::shared_ptr<const sp::RiskStore> risk,
                 const sp::SpoParams& params) {
  v7::NavV7Options o;
  o.spo_v1 = true;
  o.spo_params = params;
  o.spo_risk = std::move(risk);
  const v7::ScopedNavExtension extension(o);
  const auto result = st::replay_nav(role.nav(), nav_config());
  EXPECT_TRUE(result) << result.error().to_string();
  Replay out;
  const auto* engine = extension.spo_engine();
  if (engine == nullptr) {
    ADD_FAILURE() << "no spo engine";
    return out;
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
  out.captured = static_cast<bool>(v7::capture({}, {}, {})); // the seam before publication
  return out;
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

// The gross cap is the sanity bound 2 x L, never a solver constraint: on the fixture's replay
// (every book from flat, the S2 law's costs and the 1% ADV trade limit) the planned gross stays
// well inside it on every scored decision and the tripwire is clear. A breach (forced on the
// recorded rows) voids the run with the void on and is recorded, not voiding, with it off.
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
  EXPECT_TRUE(run.captured);
  EXPECT_EQ(run.tripwire.at("status"), "clear");
  EXPECT_EQ(run.tripwire.at("gross_bound_breaches"), 0U);
  EXPECT_EQ(run.tripwire.at("capped_specific_decisions"), 0U);
  auto breached = run.rows;
  breached[breached.size() / 2].gross_bound_breached = true;
  const auto status = sp::tracking_tripwire(params, breached);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), co::ErrorCode::Unavailable);
  EXPECT_NE(status.error().message().find("VOID"), std::string::npos) << status.error().message();
  const auto record = sp::tracking_tripwire_json(params, breached);
  EXPECT_EQ(record.at("status"), "void");
  EXPECT_EQ(record.at("gross_bound_breaches"), 1U);
  auto off = params;
  off.void_on_capped = false;
  EXPECT_TRUE(sp::tracking_tripwire(off, breached));
  EXPECT_EQ(sp::tracking_tripwire_json(off, breached).at("status").get<std::string>().rfind(
                "tripped", 0),
            0U);
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
  // One CSV row per scored (decision, book); 41 columns.
  EXPECT_EQ(static_cast<usize>(std::count(run.csv.begin(), run.csv.end(), '\n')),
            run.rows.size() + 1);
  const std::string header = run.csv.substr(0, run.csv.find('\n'));
  EXPECT_EQ(std::count(header.begin(), header.end(), ','), 40);
  for (const char* column : {",tracking_error,", ",tracking_error_current,", ",aim_correlation,",
                             ",at_trade_limit,", ",trade_limit_share,", ",gross_bound_breached,",
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
  EXPECT_EQ(run.tripwire.at("status"), "clear");
}

// The CLI: --rule spo-v3 takes spo::v3_params (S_prior 20 by Ruling E-14, H 20, p .01, beta
// .02, ceiling 1 with the void on); --spo-alpha implied-aim is its only alpha (refused with
// spo-v1/v2 and for any other value); every spo flag that would move a registered constant is
// refused; the allowed flags override; as spo-v1/v2 it refuses the capacity curve, a per-name
// rate and the holdings stream with the void on; its blocks are keyed spo_v3.
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
  const auto allowed = parse({"nav", "--rule", "spo-v3", "--spo-iters", "3000", "--spo-tol",
                              "1e-10", "--spo-books", "primary", "--specific-ceiling", "2",
                              "--specific-ceiling-void", "off"});
  ASSERT_TRUE(allowed) << allowed.error().to_string();
  const auto& a = allowed->options.spo_params;
  EXPECT_EQ(a.max_iterations, 3000U);
  EXPECT_EQ(a.tolerance, 1e-10);
  EXPECT_FALSE(a.all_books);
  EXPECT_EQ(a.specific_ceiling, 2.0);
  EXPECT_FALSE(a.void_on_capped);
  EXPECT_EQ(a.sharpe_prior, sp::v3_sharpe_prior);
  // Refused with spo-v3: every flag that would move a registered constant (still spo-v2's).
  for (const char* flag : {"--gamma", "--ic-book", "--w-max", "--adv-cap-q", "--adv-trade-p",
                           "--target-vol", "--spo-horizon", "--alpha-horizon", "--spo-gross"}) {
    const auto refused = parse({"nav", "--rule", "spo-v3", flag, "0.5"});
    ASSERT_FALSE(refused) << flag;
    EXPECT_EQ(refused.error().code(), co::ErrorCode::InvalidArgument) << flag;
    EXPECT_NE(refused.error().message().find(flag), std::string::npos)
        << refused.error().to_string();
    const auto early = parse({"nav", flag, "0.5", "--rule", "spo-v3"}); // wherever it stands
    EXPECT_FALSE(early) << flag;
  }
  EXPECT_TRUE(parse({"nav", "--rule", "spo-v2", "--gamma", "5"}));
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
  // As spo-v1/v2: no capacity curve, the fixed rate, no holdings stream with the void on.
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v3", "--capacity-curve"}));
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v3", "--rate", "per-name-v1"}));
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v3", "--emit-holdings", "h"}));
  EXPECT_TRUE(parse({"nav", "--rule", "spo-v3", "--emit-holdings", "h",
                     "--specific-ceiling-void", "off"}));
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
} // namespace
