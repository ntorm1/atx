#include "strategy_nav_v7.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/book/risk_target.hpp"
#include "atx/engine/book/two_speed.hpp"
#include "strategy_risk_target.hpp"
#include "strategy_spo_v3.hpp"
#include "strategy_target_replay_detail.hpp"
#include "strategy_vol_target.hpp"

namespace atx::impl::strategy::v7 {
namespace {
using namespace atx;
namespace co = atx::core;
namespace bk = atx::engine::book;
using Json = nlohmann::json;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr usize no_decision = std::numeric_limits<usize>::max();
std::string book_label(const NavScenario& s) { return s.id + "+" + s.financing.id; }
// spo-v3's capacity engine (Ruling E-37): the rule's parameters with the void off -- a
// report-only pass records its tripwire and never voids the run (the void flag is read by the
// tripwire only, never by the plan).
spo::SpoParams capacity_params(spo::SpoParams p) {
  p.void_on_capped = false;
  return p;
}
} // namespace

// Per-thread extension state. The decision-liquidity cache is keyed by the input's price
// buffer and the decision, so every book of one lockstep decision shares one computation.
struct ScopedNavExtension::State {
  explicit State(const NavV7Options& o)
      : options(o), s2(fixed_nav_scenarios()[nav_primary_scenario_index]),
        engine(o.spo_v1 ? std::make_unique<spo::Engine>(o.spo_params, o.spo_risk) : nullptr),
        capacity_engine(o.spo_v1 && o.capacity
                            ? std::make_unique<spo::Engine>(capacity_params(o.spo_params),
                                                            o.spo_risk)
                            : nullptr),
        scaler(o.risk_target.on
                   ? std::make_unique<risk_target::Scaler>(o.risk_target, o.spo_risk)
                   : nullptr) {
    // Review SPO-4: the capacity engine has no primary book, so Ruling E-31a never reads a
    // capacity book's limits; it binds the main pass, whose rows hold no capacity book.
    if (capacity_engine) capacity_engine->set_primary_book({});
  }
  NavV7Options options;
  NavScenario s2; // the primary S2 law: the v6 marginal cost c_i of every book
  NavV7Pass pass{NavV7Pass::Main};
  const f64* liquidity_key{};
  usize liquidity_decision{no_decision};
  cost_v2::DecisionLiquidity liquidity;
  std::map<std::string, std::vector<f64>> c_history; // per book: c_bar of recent decisions
  std::vector<TcRecord> tc;
  std::vector<BookRecord> books;
  std::vector<f64> costs;
  cost_v2::AimV6Decision decision;
  std::unique_ptr<spo::Engine> engine; // --rule spo-v1
  // spo-v3 --capacity-curve (Ruling E-37): the capacity pass plans on this engine (its own
  // rows, duals and timing), so `engine` keeps the main pass's rows, tripwire and summary.
  std::unique_ptr<spo::Engine> capacity_engine;
  // v8 R-8 (--risk-target): the scaler, and the book's config at its L_t (scratch, per plan).
  std::unique_ptr<risk_target::Scaler> scaler;
  NavReplayConfig scaled;
  // v8 Y-5 two-speed-v1 under the scaler (Ruling PM8-16 #10): per book, lambda = L_t / L at its
  // previous two-speed rebalance, and the carried desired target (scratch, per plan).
  std::map<std::string, f64, std::less<>> two_speed_lambda;
  std::vector<f64> carried;
  std::string void_reason;             // capture(): the tripwire voided the run
  [[nodiscard]] co::Status plan(const TargetReplayInput& x, const NavReplayConfig& cfg, usize d,
                                bool rebalance, f64 spent, f64 nav_post,
                                const std::vector<f64>& desired, std::vector<f64>& planned,
                                TargetReplayDay& out, std::span<const f64> rates,
                                std::span<const u8> tier, std::span<const u8> no_locate,
                                std::span<const f64> two_speed_fast);
  // The book's rule under `cfg` (base_leverage: the run's L when cfg carries L_t; NaN without
  // the risk target).
  [[nodiscard]] co::Status plan_rule(const TargetReplayInput& x, const NavReplayConfig& cfg,
                                     f64 base_leverage, usize d, bool rebalance, f64 spent,
                                     f64 nav_post, const std::vector<f64>& desired,
                                     std::vector<f64>& planned, TargetReplayDay& out,
                                     std::span<const f64> rates, std::span<const u8> tier,
                                     std::span<const u8> no_locate);
};

namespace {
thread_local ScopedNavExtension::State* active_state = nullptr;

f64 reference_cost(const std::vector<f64>& history) {
  if (history.empty()) return nan;
  f64 sum = 0;
  for (const f64 c : history) sum += c;
  return sum / static_cast<f64>(history.size());
}
std::string relabel_v6(const std::string& rule, const char* id = "aim-partial-v6") {
  constexpr std::string_view v5 = "aim-partial-v5";
  if (rule.compare(0, v5.size(), v5) != 0) return rule;
  return id + rule.substr(v5.size());
}
// The construction rule the extension runs instead of aim-partial-v5 (nullptr: v5 itself).
const char* rule_id(const NavV7Options& o) {
  return o.aim_v6 ? "aim-partial-v6" : o.spo_v1 ? spo::rule_name(o.spo_params) : nullptr;
}
// The spo value flags (each takes one value).
bool spo_flag(std::string_view key) {
  return key == "--risk-model" || key == "--risk-model-sha256" || key == "--gamma" ||
         key == "--ic-book" || key == "--w-max" || key == "--adv-cap-q" ||
         key == "--adv-trade-p" || key == "--spo-iters" || key == "--spo-tol" ||
         key == "--target-vol" || key == "--spo-horizon" || key == "--spo-books" ||
         key == "--alpha-horizon" || key == "--specific-ceiling" ||
         key == "--specific-ceiling-void" || key == "--spo-gross" || key == "--spo-alpha";
}
bool spo_rule(std::string_view rule) {
  return rule == "spo-v1" || rule == "spo-v2" || rule == "spo-v3";
}
// The risk target's value flags (v8 R-8; each takes one value).
bool risk_target_flag(std::string_view key) {
  return key == "--risk-target" || key == "--risk-target-bias" || key == "--risk-target-cadence";
}
// The store flags the risk target shares with the spo rules.
bool risk_model_flag(std::string_view key) {
  return key == "--risk-model" || key == "--risk-model-sha256";
}
// The spo flags that would move a registered constant of spo-v3 (refused with it). Ruling
// E-31a: the solver's iteration cap and tolerance are registered too (review SPO-5: a looser
// tolerance would redefine "converged" on the cell's argv).
bool refused_with_v3(std::string_view key) {
  return key == "--gamma" || key == "--ic-book" || key == "--w-max" || key == "--adv-cap-q" ||
         key == "--adv-trade-p" || key == "--target-vol" || key == "--spo-horizon" ||
         key == "--alpha-horizon" || key == "--spo-gross" || key == "--spo-iters" ||
         key == "--spo-tol";
}
const char* pass_name(NavV7Pass pass) { return pass == NavV7Pass::Main ? "main" : "capacity"; }
Json finite_or_null(f64 x) { return std::isfinite(x) ? Json(x) : Json(nullptr); }
f64 quantile(std::vector<f64> values, f64 q) {
  values.erase(std::remove_if(values.begin(), values.end(), [](f64 v) { return !std::isfinite(v); }),
               values.end());
  if (values.empty()) return nan;
  std::sort(values.begin(), values.end());
  const f64 h = static_cast<f64>(values.size() - 1) * q;
  const auto lo = static_cast<usize>(std::floor(h));
  const usize hi = std::min(lo + 1, values.size() - 1);
  return values[lo] + (h - static_cast<f64>(lo)) * (values[hi] - values[lo]);
}
Json distribution(const std::vector<f64>& values) {
  f64 sum = 0; usize n = 0;
  for (const f64 v : values)
    if (std::isfinite(v)) { sum += v; ++n; }
  return Json{{"n", n}, {"mean", finite_or_null(n ? sum / static_cast<f64>(n) : nan)},
              {"p05", finite_or_null(quantile(values, .05))},
              {"median", finite_or_null(quantile(values, .5))},
              {"p95", finite_or_null(quantile(values, .95))}};
}

constexpr const char* ko_declaration =
    "S2-KO (trading id modeled-1bn-ko-v1): S2 with the sqrt law replaced by the Kyle-Obizhaeva "
    "(2016) invariance square-root fit, cost/$ = commission + (sigma/.02)(W/W*)^(-1/3) [2.08 + "
    "12.08 sqrt(x (W/W*)^(2/3) / .01)] bps, W = sigma * ADV$ (execution liquidity row: 63-session "
    "raw-dollar ADV and daily return SD, fallback .05), W* = .02 * $40 * 1e6, x = |fill| / ADV; "
    "commission 1 bps (S2's; KO shortfall excludes commissions), no separate half spread (the "
    "kappa0 term is the spread-like part; quotes unavailable), 1% ADV cap, S2 financing";
constexpr const char* fim_declaration =
    "S2-FIM (trading id modeled-1bn-fim-v1): cost/$ = max(0, a + b y + c sqrt(y)) bps, y = 100 "
    "|fill| / ADV (percent of daily volume), b = -0.53 and c = 11.21 (FIM 2018 Table VII col 10, "
    "United States), a = 14.55 - b - c = 3.87 (Table XI cross-sectional median at 1% DTV); FIM "
    "market impact includes commissions, so no separate commission or half spread; 1% ADV cap, "
    "S2 financing; FIM uses one-year average dollar volume, this replay the 63-session ADV";
constexpr const char* capacity_declaration =
    "capacity by replay (R4.2): S2 at NAV multiples m in {.5,1,2,4,8} of the initial NAV, run as "
    "S2 books at the initial NAV with impact_y * m^0.5 and max_participation / m (the replay is "
    "scale invariant in every dollar except the impact law and the participation cap, so each "
    "book is the NAV-m book divided by m: returns, Sharpe, cost per traded dollar and cap binding "
    "are the NAV-m book's; dollar columns are scaled back by m; participation p95 is the "
    "histogram bound times m); fixed rate only; x1 is S2 bit for bit";
constexpr const char* capacity_v6_declaration =
    "aim-partial-v6 in the capacity pass: each capacity book prices its c_i with its own law (S2 "
    "with impact_y * m^0.5), which at the base-scale reference trade q equals the S2 law at the "
    "NAV-m trade m q, so c_i/c_bar, band_i, theta_t and target_i are the NAV-m book's (the main "
    "pass prices every book with the primary S2 law); x1 is the main pass's S2 v6 book bit for bit";
constexpr const char* capacity_spo_v3_declaration =
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
constexpr const char* v6_declaration =
    "aim-partial-v6 (R2.2 + R2.3): on a rebalance decision d, c_i = marginal cost per dollar of "
    "the primary S2 law (half spread 5 + commission 1 bps + 1.5 * 0.6 * sigma_i * (q/ADV_i)^0.5) "
    "at the reference trade q = theta * aim_leverage * NAV_post / N_d on the decision liquidity "
    "window [d-63, d) (sigma fallback .05), c_bar the members' median; target_i = desired_i / "
    "(1 + kappa c_i/c_bar), then longs and shorts each rescaled to the desired side gross "
    "(kappa 0: target = desired); band_i = (b / N_d)(c_i/c_bar)^(1/3) replaces the uniform dust "
    "band; theta_t = theta * clip((c_ref/c_bar)^(1/2), lo, hi), c_ref = mean c_bar of the book's "
    "last 252 rebalance decisions (today included); a member without ADV gets target 0 (kappa > "
    "0) and the median band; everything else (aim = aim_leverage * target, dusted members keep "
    "their weight, nonmember exits and exit_rate decay inside dust_multiple / N_d, non-rebalance "
    "decisions) is aim-partial-v5";
constexpr const char* tc_declaration =
    "transfer coefficient (R2.5, Clarke-de Silva-Thorley 2002) per rebalance decision and book: "
    "corr over members with a finite decision-window sigma_i > 0 of desired_i / sigma_i (= "
    "alpha_i / sigma_i^2 with Grinold-Kahn alpha_i = IC sigma_i z_i, z = the neutralized rank "
    "aim) and the planned weight w_i after the rule";

Json declarations(const ScopedNavExtension::State& s) {
  const auto& v6 = s.options.v6;
  Json j{{"hook", "platform-v7 lane L4 NAV extension (strategy_nav_v7)"},
         {"pass", pass_name(s.pass)},
         {"stress_books", s.options.stress},
         {"capacity_curve", s.options.capacity},
         {"transfer_coefficient", tc_declaration}};
  if (s.options.stress) {
    j["s2_ko"] = ko_declaration;
    j["s2_fim"] = fim_declaration;
  }
  if (s.options.capacity) {
    j["capacity"] = capacity_declaration;
    j["capacity_multiples"] = Json(std::vector<f64>(cost_v2::capacity_multiples.begin(),
                                                    cost_v2::capacity_multiples.end()));
    if (s.options.aim_v6) j["capacity_aim_partial_v6"] = capacity_v6_declaration;
    if (s.capacity_engine) j["capacity_spo_v3"] = capacity_spo_v3_declaration;
  }
  if (s.options.aim_v6)
    j["aim_partial_v6"] = Json{{"rule", v6_declaration}, {"kappa", v6.kappa},
        {"band_b", v6.band_b}, {"band_exponent", v6.band_exponent},
        {"rate_clip", Json::array({v6.clip_lo, v6.clip_hi})},
        {"reference_decisions", v6.reference_decisions}};
  if (s.engine) {
    Json block{{"rule", s.engine->rule_declaration()},
               {"parameters", s.engine->rule_parameters_json()},
               {"calibration", s.engine->rule_calibration_json()}};
    if (s.options.spo_risk)
      block["risk_model"] = Json{{"directory", s.options.spo_risk->directory()},
                                 {"manifest_sha256", s.options.spo_risk->manifest_sha256()}};
    j[spo::json_key(s.options.spo_params)] = std::move(block);
  }
  return j;
}
// v8 R-8: the "risk_target" block (with the per-book summary when `books`) and the rule id
// suffix of recipe.json, summary.json and the holdings manifest; nothing without --risk-target.
// v8 Y (vol-target-v1, the same scaler under Law::vol_target_v1): the "vol_target" block and the
// rule id suffix +vol-target-v1 instead.
void add_risk_target(const ScopedNavExtension::State& s, Json& doc, bool books) {
  if (!s.scaler || !doc.is_object()) return;
  const auto& o = s.options.risk_target;
  auto block = risk_target::parameters_json(o);
  if (books) block["books"] = risk_target::summary_json(s.scaler->records(), o.law);
  doc[risk_target::block_key(o)] = std::move(block);
  if (doc.contains("rule") && doc.at("rule").is_string())
    doc["rule"] =
        doc.at("rule").get<std::string>() + risk_target::rule_suffix(s.options.risk_target);
}

co::Status write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "nav v7: cannot write " + path.string());
  file << text;
  file.close();
  return file ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "nav v7: close " + path.string()));
}
std::string csv_number(f64 x) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << x;
  return out.str();
}
bool same_bits(const std::vector<f64>& a, const std::vector<f64>& b) {
  if (a.size() != b.size()) return false;
  for (usize k = 0; k < a.size(); ++k)
    if (std::bit_cast<u64>(a[k]) != std::bit_cast<u64>(b[k])) return false;
  return true;
}
std::string tc_csv(std::span<const TcRecord> records) {
  std::string text = "session,book,transfer_coefficient,members,costed,banded,c_bar,c_ref,theta\n";
  for (const auto& r : records)
    text += std::to_string(r.session) + ',' + r.book + ',' + csv_number(r.tc) + ',' +
            std::to_string(r.members) + ',' + std::to_string(r.costed) + ',' +
            std::to_string(r.banded) + ',' + csv_number(r.c_bar) + ',' + csv_number(r.c_ref) +
            ',' + csv_number(r.theta) + '\n';
  return text;
}
// One capacity row: NAV-m book quantities (dollars scaled back by m).
Json capacity_row(const BookRecord& b) {
  const f64 m = b.multiple;
  const auto& s = b.summary;
  const f64 cost_bps = b.traded_dollars > 0 ? 1e4 * b.trade_cost_dollars / b.traded_dollars : nan;
  return Json{{"multiple", m}, {"book", b.book}, {"equivalent_initial_nav", m * b.initial_nav},
      {"net_sharpe", finite_or_null(s.net_sharpe)}, {"gross_sharpe", finite_or_null(s.gross_sharpe)},
      {"ann_mean_net", finite_or_null(s.ann_mean)}, {"ann_vol", finite_or_null(s.ann_vol)},
      {"max_drawdown", finite_or_null(s.max_drawdown)},
      {"cost_bps_per_traded_dollar", finite_or_null(cost_bps)},
      {"traded_dollars", m * b.traded_dollars}, {"trade_cost_dollars", m * b.trade_cost_dollars},
      {"fills", s.fills}, {"capped_fills", s.capped_fills},
      {"capped_share", finite_or_null(s.fills ? static_cast<f64>(s.capped_fills) /
                                                    static_cast<f64>(s.fills) : nan)},
      {"unfilled_dollars", m * s.unfilled_dollars},
      {"participation_p95_bound", finite_or_null(m * b.participation_p95)},
      {"participation_max", finite_or_null(m * b.participation_max)},
      {"daily_turnover_gmv_mean", finite_or_null(s.daily_turnover_mean)},
      {"financing_dollars", m * (s.long_financing_dollars + s.borrow_dollars)}};
}
std::string capacity_csv(const Json& rows) {
  static constexpr std::array<const char*, 19> columns{
      "multiple", "book", "equivalent_initial_nav", "net_sharpe", "gross_sharpe", "ann_mean_net",
      "ann_vol", "max_drawdown", "cost_bps_per_traded_dollar", "traded_dollars",
      "trade_cost_dollars", "fills", "capped_fills", "capped_share", "unfilled_dollars",
      "participation_p95_bound", "participation_max", "daily_turnover_gmv_mean",
      "financing_dollars"};
  std::string text;
  for (usize c = 0; c < columns.size(); ++c) text += std::string(c ? "," : "") + columns[c];
  text += '\n';
  for (const auto& row : rows) {
    for (usize c = 0; c < columns.size(); ++c) {
      const auto& v = row.at(columns[c]);
      text += c ? "," : "";
      if (v.is_string()) text += v.get<std::string>();
      else if (v.is_null()) text += "nan";
      else if (v.is_number_float()) text += csv_number(v.get<f64>());
      else text += v.dump();
    }
    text += '\n';
  }
  return text;
}
// A complete run's extras, or (void_reason non-empty) a voided run's: the spo diagnostics
// and the transfer coefficients only, with status "void" -- no NAV or return quantity.
co::Status write_extras(const std::filesystem::path& dir, const ScopedNavExtension& ext,
                        const NavV7Options& o, const std::string& void_reason) {
  Json files = Json::object();
  const auto tc_path = dir / "v7_transfer_coefficient.csv";
  ATX_TRY_VOID(write_text(tc_path, tc_csv(ext.tc_records())));
  ATX_TRY(auto tc_sha, co::sha256_file(tc_path.string()));
  files["v7_transfer_coefficient.csv"] = tc_sha;
  Json extras{{"schema", "atx.nav-v7-extras/v1"}, {"capacity_curve", o.capacity}};
  if (!void_reason.empty()) {
    extras["status"] = "void";
    extras["void_reason"] = void_reason;
    // Ruling E-31a: a run its primary book's limits_unmet voided says so, with the count and the
    // first session (spo-v3's tripwire record).
    if (const auto* engine = ext.spo_engine(); engine && engine->params().version == 3) {
      const auto trip = engine->rows_tripwire_json();
      if (trip.contains("voided")) {
        extras["voided"] = trip.at("voided");
        Json unmet = trip.at("limits_unmet_primary");
        unmet["book"] = trip.at("primary_book");
        unmet["rule"] = trip.at("limits_unmet_rule");
        extras["limits_unmet"] = std::move(unmet);
      }
    }
  }
  // v8 R-8: the risk target's series (no NAV or return quantity: also on a void run); v8 Y
  // vol-target-v1: vol_target.csv and the "vol_target" block.
  if (const auto* scaler = ext.risk_target_scaler()) {
    const auto& rt = o.risk_target;
    const auto path = dir / risk_target::series_file(rt);
    ATX_TRY_VOID(write_text(path, risk_target::records_csv(scaler->records(), rt.law)));
    ATX_TRY(auto sha, co::sha256_file(path.string()));
    files[risk_target::series_file(rt)] = sha;
    auto block = risk_target::parameters_json(rt);
    block["books"] = risk_target::summary_json(scaler->records(), rt.law);
    extras[risk_target::block_key(rt)] = std::move(block);
  }
  if (o.capacity && void_reason.empty()) { // a void run stops before the capacity pass
    Json rows = Json::array();
    const BookRecord* primary = nullptr;
    const BookRecord* unit = nullptr;
    for (const auto& b : ext.books()) {
      if (b.pass == NavV7Pass::Main && b.primary) primary = &b;
      if (b.pass != NavV7Pass::Capacity) continue;
      rows.push_back(capacity_row(b));
      if (b.multiple == 1.0) unit = &b;
    }
    const auto path = dir / "capacity_curve.csv";
    ATX_TRY_VOID(write_text(path, capacity_csv(rows)));
    ATX_TRY(auto sha, co::sha256_file(path.string()));
    files["capacity_curve.csv"] = sha;
    extras["capacity"] = std::move(rows);
    extras["capacity_rule"] = capacity_declaration;
    if (o.aim_v6) extras["capacity_aim_partial_v6"] = capacity_v6_declaration;
    extras["capacity_x1_equals_primary_bit_for_bit"] =
        primary && unit && same_bits(primary->net_returns, unit->net_returns);
  }
  if (const auto* engine = ext.spo_engine()) {
    const auto path = dir / "spo_diagnostics.csv";
    ATX_TRY_VOID(write_text(path, engine->rows_csv()));
    ATX_TRY(auto sha, co::sha256_file(path.string()));
    files["spo_diagnostics.csv"] = sha;
    extras[spo::json_key(engine->params())] =
        Json{{"parameters", engine->rule_parameters_json()},
             {"calibration", engine->rule_calibration_json()},
             {"tripwire", engine->rows_tripwire_json()},
             {"diagnostics_units", engine->rows_units_json()},
             {"books", engine->rows_summary_json()}};
  }
  // spo-v3's capacity pass (Ruling E-37): the capacity engine's report, beside (never in) the
  // main pass's spo block above.
  const auto* main_engine = ext.spo_engine();
  if (const auto* capacity = ext.spo_capacity_engine();
      capacity && main_engine && o.capacity && void_reason.empty()) {
    const u64 capacity_gamma = std::bit_cast<u64>(capacity->calibration().gamma);
    const u64 main_gamma = std::bit_cast<u64>(main_engine->calibration().gamma);
    extras["capacity_" + std::string(spo::json_key(capacity->params()))] =
        Json{{"rule", capacity_spo_v3_declaration},
             {"calibration", capacity->rule_calibration_json()},
             {"gamma_equals_main", capacity_gamma == main_gamma},
             {"tripwire", capacity->rows_tripwire_json()},
             {"books", capacity->rows_summary_json()}};
  }
  extras["files"] = std::move(files);
  return write_text(dir / "v7_extras.json", extras.dump(2) + "\n");
}
} // namespace

co::Status ScopedNavExtension::State::plan(const TargetReplayInput& x, const NavReplayConfig& cfg,
                                           usize d, bool rebalance, f64 spent, f64 nav_post,
                                           const std::vector<f64>& desired,
                                           std::vector<f64>& planned, TargetReplayDay& out,
                                           std::span<const f64> rates, std::span<const u8> tier,
                                           std::span<const u8> no_locate,
                                           std::span<const f64> two_speed_fast) {
  if (!scaler)
    return plan_rule(x, cfg, nan, d, rebalance, spent, nav_post, desired, planned, out, rates,
                     tier, no_locate);
  // v8 R-8 (risk-target-v1): the book's L_t, from its current weights (`planned` on entry) and
  // the risk row d, replaces --aim-leverage in the config its rule reads (every decision, the
  // warm-up included; the main pass's scored decisions are recorded). spo-v3 keeps the run's L
  // for its gross bound and its gamma (BookDecision::base_leverage).
  const bool scored = pass == NavV7Pass::Main && d >= x.decision_begin;
  // v8 Y-5 two-speed-v1 (Ruling PM8-16 #10): the book's fast holding follows its scale lambda =
  // L_t / L. Its rebalance plans at L_t the netted desired target plus the carry of F from the
  // book's lambda at its previous two-speed rebalance (its first: lambda itself, no carry), so
  // the remainder moves at theta_s toward lambda L m_s d_s and the fast part becomes lambda F_next.
  // Checked before the scaler moves.
  const bool carry = two_speed_on(cfg.target) && rebalance;
  const usize n = x.instruments;
  if (carry && (two_speed_fast.size() != n || desired.size() != n || d >= x.dates))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav v7: two-speed-v1 under --risk-target / --vol-target needs the fast "
                   "sleeve F entering the rebalance");
  const std::string book = book_label(cfg.scenario);
  ATX_TRY(const f64 leverage, scaler->leverage(x, d, rebalance, book, cfg.target.aim_leverage,
                                               planned, scored));
  scaled = cfg;
  scaled.target.aim_leverage = leverage;
  if (!carry)
    return plan_rule(x, scaled, cfg.target.aim_leverage, d, rebalance, spent, nav_post, desired,
                     planned, out, rates, tier, no_locate);
  const f64 lambda = leverage / cfg.target.aim_leverage;
  const auto held = two_speed_lambda.try_emplace(book, lambda).first;
  carried.assign(desired.begin(), desired.end());
  ATX_TRY_VOID(bk::two_speed_carry(x.member.subspan(d * n, n), two_speed_fast, held->second,
                                   lambda, cfg.target.aim_leverage, cfg.target.trade_fraction,
                                   carried));
  held->second = lambda;
  return plan_rule(x, scaled, cfg.target.aim_leverage, d, rebalance, spent, nav_post, carried,
                   planned, out, rates, tier, no_locate);
}

co::Status ScopedNavExtension::State::plan_rule(const TargetReplayInput& x,
                                                const NavReplayConfig& cfg, f64 base_leverage,
                                                usize d, bool rebalance, f64 spent, f64 nav_post,
                                                const std::vector<f64>& desired,
                                                std::vector<f64>& planned, TargetReplayDay& out,
                                                std::span<const f64> rates,
                                                std::span<const u8> tier,
                                                std::span<const u8> no_locate) {
  // A warm-start decision (v8 D-0: d before the role's decision_begin) plans the book but is
  // not scored: it leaves no transfer-coefficient record. Without a warm start every
  // decision the replay makes has d >= decision_begin, so nothing changes.
  const bool v6 = options.aim_v6;
  const bool observe = pass == NavV7Pass::Main && d >= x.decision_begin;
  if ((v6 || engine) && !rates.empty())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "aim-partial-v6 / spo-v1: the per-name rate is not part of the rule "
                   "(--rate fixed)");
  // The spo rule's engine and NAV. Capacity pass (Ruling E-37, spo-v3 only): a book here is the
  // NAV-m book at the initial NAV, so it plans on the capacity engine as the tracker of NAV m x
  // NAV_post under the primary S2 law: its trade limit p ADV / (m NAV) and its impact
  // impact_y sigma sqrt(m NAV / ADV) are the NAV-m book's (m = 1: the main pass bit for bit).
  spo::Engine* rule = engine.get();
  f64 rule_nav = nav_post;
  if (engine && pass == NavV7Pass::Capacity) {
    const f64 m = cost_v2::capacity_multiple(cfg.scenario.id);
    if (!capacity_engine || engine->params().version != 3 || !std::isfinite(m))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "spo capacity pass: spo-v3 only, on a capacity book (Ruling E-37); book " +
                         cfg.scenario.id);
    rule = capacity_engine.get();
    rule_nav = m * nav_post;
  }
  // Non-rebalance decisions keep every member and only trade exits: aim-partial-v5's move
  // (spo: its shadow book makes the same move).
  if (!rebalance || (!v6 && !engine && !observe)) {
    if (rule && !rebalance)
      ATX_TRY_VOID(rule->hold(x, cfg, d, desired, book_label(cfg.scenario)));
    return detail::update_weights(x, cfg.target, d, rebalance, spent, desired, planned, out, rates);
  }
  const usize n = x.instruments;
  if (d >= x.dates || desired.size() != n || planned.size() != n)
    return co::Err(co::ErrorCode::InvalidArgument, "nav v7: decision geometry");
  if (liquidity_key != x.close.data() || liquidity_decision != d) {
    cost_v2::decision_liquidity(x, d, cfg.liquidity_window, cfg.min_vol_pairs, liquidity);
    liquidity_key = x.close.data(); liquidity_decision = d;
  }
  const auto member = x.member.subspan(d * n, n);
  const usize members = detail::members_at(x, d);
  const std::string book = book_label(cfg.scenario);
  TcRecord record{x.session_keys[d], book, nan, members, 0, 0, nan, nan, nan};
  if (v6) {
    // Capacity pass (R1 I-1): a book here is the NAV-m book at the initial NAV. Its c_i is the
    // S2 law at the NAV-m trade m q, which is exactly the book's own law (impact_y * m^delta,
    // cost_v2::capacity_scenarios) at the base-scale q. The main pass prices every book with S2.
    const NavScenario* law = &s2;
    if (pass == NavV7Pass::Capacity) {
      if (!std::isfinite(cost_v2::capacity_multiple(cfg.scenario.id)))
        return co::Err(co::ErrorCode::InvalidArgument,
                       "aim-partial-v6: capacity pass book " + cfg.scenario.id +
                           " is not a capacity book");
      law = &cfg.scenario;
    }
    const f64 q = members ? cfg.target.trade_fraction * cfg.target.aim_leverage * nav_post /
                                static_cast<f64>(members)
                          : nan;
    costs.assign(n, nan);
    for (usize i = 0; i < n; ++i)
      if (member[i])
        costs[i] = cost_v2::marginal_cost_s2(*law, q, liquidity.adv[i], liquidity.sigma[i]);
    const f64 c_bar = cost_v2::finite_median(costs);
    auto& history = c_history[book];
    if (std::isfinite(c_bar)) {
      history.push_back(c_bar);
      if (history.size() > options.v6.reference_decisions) history.erase(history.begin());
    }
    const f64 c_ref = reference_cost(history);
    cost_v2::form_aim_v6(member, desired, costs, c_bar, c_ref, cfg.target.trade_fraction,
                         options.v6, true, decision);
    ATX_TRY_VOID(cost_v2::aim_partial_v6_weights(x, cfg.target, d, true, decision, planned, out));
    record.c_bar = c_bar; record.c_ref = c_ref; record.theta = decision.theta;
    record.costed = decision.costed;
  } else if (rule) {
    const spo::BookDecision in{x, cfg, s2, d, rule_nav, desired, tier, no_locate, liquidity,
                               book, base_leverage};
    ATX_TRY_VOID(rule->plan(in, planned, out));
  } else {
    ATX_TRY_VOID(detail::update_weights(x, cfg.target, d, true, spent, desired, planned, out, rates));
  }
  record.banded = out.construction.banded_names;
  if (observe) {
    record.tc = cost_v2::transfer_coefficient(member, desired, liquidity.sigma, planned);
    tc.push_back(std::move(record));
  }
  return co::Ok();
}

ScopedNavExtension::ScopedNavExtension(const NavV7Options& options)
    : state_(std::make_unique<State>(options)), previous_(active_state) {
  active_state = state_.get();
}
ScopedNavExtension::~ScopedNavExtension() { active_state = previous_; }
void ScopedNavExtension::begin_run(NavV7Pass pass) {
  state_->pass = pass;
  state_->liquidity_key = nullptr;
  state_->liquidity_decision = no_decision;
  state_->c_history.clear();
  state_->two_speed_lambda.clear();
  if (state_->engine) state_->engine->begin_run();
  if (state_->capacity_engine) state_->capacity_engine->begin_run();
  if (state_->scaler) state_->scaler->begin_run();
}
std::span<const TcRecord> ScopedNavExtension::tc_records() const noexcept { return state_->tc; }
std::span<const BookRecord> ScopedNavExtension::books() const noexcept { return state_->books; }
const spo::Engine* ScopedNavExtension::spo_engine() const noexcept { return state_->engine.get(); }
const spo::Engine* ScopedNavExtension::spo_capacity_engine() const noexcept {
  return state_->capacity_engine.get();
}
const risk_target::Scaler* ScopedNavExtension::risk_target_scaler() const noexcept {
  return state_->scaler.get();
}
const std::string& ScopedNavExtension::void_reason() const noexcept { return state_->void_reason; }

bool claims_nav_args(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view key = argv[i];
    if (key == "--cost-v2" || key == "--capacity-curve" || key == "--cost-shrink-kappa" ||
        key == "--band-b" || key == "--band-exponent" || key == "--rate-clip" || spo_flag(key) ||
        risk_target_flag(key) || key == "--vol-target")
      return true;
    if (key == "--rule" && i + 1 < argc &&
        (std::string_view(argv[i + 1]) == "aim-partial-v6" || spo_rule(argv[i + 1])))
      return true;
  }
  return false;
}

std::vector<NavScenario> run_scenarios(std::vector<NavScenario> matrix) {
  auto* s = active_state;
  if (!s || matrix.size() <= nav_primary_scenario_index) return matrix;
  const NavScenario primary = matrix[nav_primary_scenario_index];
  // Ruling E-31a: the main pass's primary book (this run's matrix, tiered or not) is the book
  // whose limits_unmet voids a spo-v3 run.
  if (s->pass == NavV7Pass::Main && s->engine) s->engine->set_primary_book(book_label(primary));
  if (s->pass == NavV7Pass::Capacity) return cost_v2::capacity_scenarios(primary);
  // spo-v1 --spo-books primary: S1 and the primary S2 only (the replay keeps S2 at index 1).
  if (s->options.spo_v1 && !s->options.spo_params.all_books)
    matrix.resize(nav_primary_scenario_index + 1);
  if (s->options.stress) {
    matrix.push_back(cost_v2::ko_scenario(primary));
    matrix.push_back(cost_v2::fim_scenario(primary));
  }
  return matrix;
}

co::Result<std::unique_ptr<const bk::ReplayCostModel>> extension_cost_model(const NavScenario& s) {
  return cost_v2::reserved_cost_model(s);
}

co::Status plan(const TargetReplayInput& x, const NavReplayConfig& cfg, usize d, bool rebalance,
                f64 spent, f64 nav_post, const std::vector<f64>& desired,
                std::vector<f64>& planned, TargetReplayDay& out, std::span<const f64> rates,
                std::span<const u8> tier, std::span<const u8> no_locate,
                std::span<const f64> two_speed_fast) {
  if (auto* s = active_state)
    return s->plan(x, cfg, d, rebalance, spent, nav_post, desired, planned, out, rates, tier,
                   no_locate, two_speed_fast);
  return detail::update_weights(x, cfg.target, d, rebalance, spent, desired, planned, out, rates);
}

co::Status capture(std::span<const NavScenario> scenarios,
                   std::span<const NavReplayResult> results,
                   std::span<const NavSummary> summaries) {
  auto* s = active_state;
  if (!s) return co::Ok();
  // The spo specific-ceiling tripwire (R2 M-1) and, under spo-v3, the primary book's
  // limits_unmet (Ruling E-31a, whatever the void flag), read before any byte is published: the
  // replay returns this error before its output directory exists, so no NAV or return file
  // (and no return statistic on the console) exists for a void run. It reads the main pass's
  // rows only: spo-v3's capacity pass (Ruling E-37, report only) plans on capacity_engine,
  // whose tripwire is recorded in v7_extras.json and never voids the run.
  if (s->engine) {
    auto tripwire = s->engine->rows_tripwire();
    if (!tripwire) {
      s->void_reason = tripwire.error().message();
      return tripwire;
    }
  }
  if (scenarios.size() != results.size() || results.size() != summaries.size()) return co::Ok();
  for (usize k = 0; k < scenarios.size(); ++k) {
    BookRecord b;
    b.pass = s->pass; b.book = book_label(scenarios[k]);
    b.primary = k == nav_primary_scenario_index;
    b.multiple = s->pass == NavV7Pass::Capacity ? cost_v2::capacity_multiple(scenarios[k].id) : nan;
    const auto& r = results[k];
    b.initial_nav = r.days.empty() ? nan : r.days.front().pretrade_nav;
    for (const auto& day : r.days) {
      b.traded_dollars += day.traded_dollars; b.trade_cost_dollars += day.trade_cost_dollars;
      if (day.return_observation) b.net_returns.push_back(day.net_return);
    }
    b.summary = summaries[k];
    b.participation_p95 = r.participation_p95; b.participation_max = r.participation_max;
    s->books.push_back(std::move(b));
  }
  return co::Ok();
}

void extend_recipe(Json& recipe) {
  if (recipe.is_object() && recipe.contains("scenarios") && recipe.at("scenarios").is_array())
    for (auto& entry : recipe.at("scenarios")) {
      if (!entry.is_object() || !entry.contains("trading_scenario") ||
          !entry.at("trading_scenario").is_string())
        continue;
      if (const char* rule =
              cost_v2::reserved_cost_rule(entry.at("trading_scenario").get<std::string>()))
        entry["cost_rule"] = rule;
    }
  const auto* s = active_state;
  if (!s || !recipe.is_object()) return;
  recipe["v7"] = declarations(*s);
  if (const char* id = rule_id(s->options);
      id && recipe.contains("rule") && recipe.at("rule").is_string())
    recipe["rule"] = relabel_v6(recipe.at("rule").get<std::string>(), id);
  add_risk_target(*s, recipe, false);
}

void extend_summary(Json& summary) {
  const auto* s = active_state;
  if (!s || !summary.is_object()) return;
  std::map<std::string, std::vector<const TcRecord*>> by_book;
  for (const auto& r : s->tc) by_book[r.book].push_back(&r);
  Json books = Json::object();
  for (const auto& [book, records] : by_book) {
    std::vector<f64> tc, theta, c_bar;
    usize at_lo = 0, at_hi = 0;
    for (const auto* r : records) {
      tc.push_back(r->tc); theta.push_back(r->theta); c_bar.push_back(r->c_bar);
      if (s->options.aim_v6 && std::isfinite(r->c_ref) && std::isfinite(r->c_bar) && r->c_bar > 0) {
        const f64 raw = std::sqrt(r->c_ref / r->c_bar);
        at_lo += raw <= s->options.v6.clip_lo ? 1U : 0U;
        at_hi += raw >= s->options.v6.clip_hi ? 1U : 0U;
      }
    }
    Json entry{{"transfer_coefficient", distribution(tc)}, {"rebalance_decisions", records.size()}};
    if (s->options.aim_v6)
      entry["aim_partial_v6"] = Json{{"theta", distribution(theta)},
          {"c_bar", distribution(c_bar)}, {"decisions_at_clip_lo", at_lo},
          {"decisions_at_clip_hi", at_hi}};
    books[book] = std::move(entry);
  }
  summary["v7"] = Json{{"declarations", declarations(*s)}, {"books", std::move(books)},
      {"extras", "v7_transfer_coefficient.csv, capacity_curve.csv (with --capacity-curve), "
                 "spo_diagnostics.csv (spo-v1/v2) and v7_extras.json are written after this "
                 "summary"}};
  // The pass's own engine: spo-v3's capacity pass (Ruling E-37) summarises its capacity books.
  const spo::Engine* engine =
      s->pass == NavV7Pass::Capacity && s->capacity_engine ? s->capacity_engine.get()
                                                           : s->engine.get();
  if (engine) {
    const std::string key = spo::json_key(engine->params());
    summary["v7"][key + "_books"] = engine->rows_summary_json();
    summary["v7"][key + "_tripwire"] = engine->rows_tripwire_json();
  }
  if (const char* id = rule_id(s->options);
      id && summary.contains("rule") && summary.at("rule").is_string())
    summary["rule"] = relabel_v6(summary.at("rule").get<std::string>(), id);
  // v8 R-8: the per-book block over the main pass's records (the capacity pass: parameters).
  add_risk_target(*s, summary, s->pass == NavV7Pass::Main);
}

void extend_holdings(Json& manifest) {
  const auto* s = active_state;
  if (!s || !manifest.is_object()) return;
  manifest["v7"] = declarations(*s);
  if (const char* id = rule_id(s->options);
      id && manifest.contains("rule") && manifest.at("rule").is_string())
    manifest["rule"] = relabel_v6(manifest.at("rule").get<std::string>(), id);
  add_risk_target(*s, manifest, false);
}

void append_help(std::ostream& out) {
  out << "v7 (lane L4; any of these adds <output>/v7_transfer_coefficient.csv and "
         "v7_extras.json after summary.json): [--cost-v2 (stress books S2-KO "
         "modeled-1bn-ko-v1 and S2-FIM modeled-1bn-fim-v1 beside S1/S2/S3; S2 stays PRIMARY)] "
         "[--capacity-curve (second pass: S2 at NAV x .5,1,2,4,8 into <output>/capacity, then "
         "<output>/capacity_curve.csv; fixed rate only; --emit-holdings observes the main "
         "pass only)] [--rule aim-partial-v6 (aim-partial-v5 flags plus a cost-scaled target, "
         "band and regime rate; fixed rate only) --cost-shrink-kappa 1 --band-b "
         "<--dust-multiple> --band-exponent .3333 (1/3; 0: the uniform dust band) "
         "--rate-clip .5,1.5; kappa 0, band exponent 0 and clip 1,1 = aim-partial-v5] "
         "[--rule spo-v1 (cost-aware single-period optimiser around the GP aim, aim-partial-v5 "
         "flags plus) --risk-model DIR (risk verb output, --emit-exposures all, same role) "
         "--risk-model-sha256 SHA [--gamma G (default: calibrated, --target-vol .05)] "
         "[--ic-book .02] [--w-max .01] [--adv-cap-q .05] [--adv-trade-p .01] "
         "[--spo-iters 500] [--spo-tol 1e-8] [--spo-horizon (default 1/theta)] "
         "[--spo-books all|primary] [--alpha-horizon (default 1)] [--specific-ceiling "
         "(default inf)] [--specific-ceiling-void on|off (default off)] [--spo-gross G (the "
         "hard cap on planned gross; default --aim-leverage; (0, 1.5 x --aim-leverage])] "
         "(adds <output>/spo_diagnostics.csv; fixed rate only)] [--rule spo-v2 (spo-v1 "
         "flags; defaults --alpha-horizon 21 (independent of --spo-horizon), --spo-gross 1, "
         "--specific-ceiling 1, --specific-ceiling-void on (a clamp voids the run: exit 3, "
         "diagnostics only, no NAV or return file; not with --emit-holdings), gamma = the "
         "vol-target gamma)] [--rule spo-v3 [--spo-alpha implied-aim] (target tracking toward "
         "the aim --aim-leverage x desired, no alpha vector; --risk-model DIR "
         "--risk-model-sha256 SHA [--spo-books all|primary] [--specific-ceiling 1] "
         "[--specific-ceiling-void on|off (default on: a clamp or a planned gross above 2 x "
         "--aim-leverage voids the run)]; a scored decision of the primary book that does not "
         "meet its net or beta limit voids the run whatever --specific-ceiling-void (Ruling "
         "E-31a: exit 3, v7_extras.json voided limits_unmet, no NAV or return file; "
         "--emit-holdings refused); registered S_prior 20, H 20, --adv-trade-p .01, beta .02, "
         "2000 iterations, tolerance 1e-9: --gamma, --ic-book, --w-max, --adv-cap-q, "
         "--adv-trade-p, --target-vol, --spo-horizon, --alpha-horizon, --spo-gross, --spo-iters "
         "and --spo-tol are refused; [--hold-band B] [--adv-hold-q Q] [--vol-scale inv-vol-v1] "
         "shape desired as aim-partial-v5 does (refused with spo-v1/v2); [--capacity-curve] "
         "runs report only "
         "(Ruling E-37: each capacity book is the NAV-m tracker, its trade limit and impact at "
         "m x NAV, on its own engine; v7_extras.json capacity_spo_v3; refused with spo-v1/v2); "
         "--trade-fraction (theta) has no effect on spo-v3 (H is the registered 20; theta moves "
         "only the warm-up decisions, which are aim-partial-v5's, and the shadow book's "
         "diagnostics), so R-9 is undefined on it (Ruling E-37); fixed rate only)]\n";
  out << "v8 R-8: [--risk-target S (risk-target-v1: each book's aim leverage becomes L_t = "
         "clip(S / (b sigma_hat), .8 L, 1.25 L), sigma_hat the annualised ex-ante vol of its "
         "current book at gross 1 on the atx-risk-v1 store, re-estimated every C sessions; "
         "aim-partial-v5 and spo-v3 only (spo-v3: its gross bound and gamma keep L); needs "
         "--risk-model DIR --risk-model-sha256 SHA; adds <output>/risk_target.csv, the "
         "risk_target blocks and the rule id +risk-target-S) [--risk-target-bias b (default "
         "1.15)] [--risk-target-cadence C (default 21)]]\n";
  out << "v8 Y: [--vol-target vol-target-v1 (each book's aim leverage becomes L_t = clip(L x "
         "sigma_ref / sigma_hat, 1, L), L = --aim-leverage (the cap), sigma_hat the risk "
         "target's forecast, sigma_ref the mean of the book's estimates so far, re-estimated "
         "every 21 sessions; aim-partial-v5 only; not with --risk-target; needs --risk-model DIR "
         "--risk-model-sha256 SHA; adds <output>/vol_target.csv, the vol_target blocks and the "
         "rule id +vol-target-v1)]\n";
}

co::Result<NavV7Command> parse_nav_v7_args(int argc, char** argv) {
  try {
    NavV7Options o;
    std::vector<std::string> args;
    args.emplace_back(argc > 0 ? argv[0] : "nav");
    std::set<std::string> seen;
    bool band_given = false, v6_params = false;
    bool risk_target_tuned = false; // --risk-target-bias or --risk-target-cadence given
    bool vol_target_given = false;  // --vol-target vol-target-v1 (v8 Y)
    u32 spo_version = 1;
    std::vector<std::pair<std::string, std::string>> spo_values; // applied after the scan
    std::string risk_model, risk_model_sha256;
    const auto number = [](const std::string& value) {
      usize used = 0;
      const f64 x = std::stod(value, &used);
      if (used != value.size()) throw std::invalid_argument("invalid number: " + value);
      return x;
    };
    const auto count = [](const std::string& value) {
      usize used = 0;
      const unsigned long long x = std::stoull(value, &used);
      if (used != value.size() || value.front() == '-')
        throw std::invalid_argument("invalid count: " + value);
      return static_cast<usize>(x);
    };
    // Token scan: only the v7 tokens (and --rule aim-partial-v6) are consumed; every other
    // token passes through in order, so the replay parses its own flags unchanged.
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      if (key == "--cost-v2" || key == "--capacity-curve") {
        if (!seen.insert(key).second) throw std::invalid_argument("duplicate flag " + key);
        (key == "--cost-v2" ? o.stress : o.capacity) = true;
        continue;
      }
      if (key == "--cost-shrink-kappa" || key == "--band-b" || key == "--band-exponent" ||
          key == "--rate-clip") {
        if (!seen.insert(key).second || i + 1 >= argc)
          throw std::invalid_argument("duplicate/missing value: " + key);
        const std::string value = argv[++i];
        v6_params = true;
        if (key == "--cost-shrink-kappa") {
          o.v6.kappa = number(value);
        } else if (key == "--band-b") {
          o.v6.band_b = number(value); band_given = true;
        } else if (key == "--band-exponent") {
          o.v6.band_exponent = number(value);
        } else {
          const auto comma = value.find(',');
          if (comma == std::string::npos) throw std::invalid_argument("--rate-clip LO,HI");
          o.v6.clip_lo = number(value.substr(0, comma));
          o.v6.clip_hi = number(value.substr(comma + 1));
        }
        continue;
      }
      if (risk_target_flag(key)) { // v8 R-8
        if (!seen.insert(key).second || i + 1 >= argc)
          throw std::invalid_argument("duplicate/missing value: " + key);
        const std::string value = argv[++i];
        auto& rt = o.risk_target;
        if (key == "--risk-target") {
          rt.on = true;
          rt.params.sigma_star = number(value);
        } else if (key == "--risk-target-bias") {
          rt.params.bias = number(value); risk_target_tuned = true;
        } else {
          rt.params.cadence = count(value); risk_target_tuned = true;
        }
        continue;
      }
      if (key == "--vol-target") { // v8 Y (lane YCOMB): vol-target-v1, the scaler's other law
        if (!seen.insert(key).second || i + 1 >= argc)
          throw std::invalid_argument("duplicate/missing value: " + key);
        const std::string value = argv[++i];
        if (value != vol_target::rule_id)
          throw std::invalid_argument("--vol-target vol-target-v1 (the only value)");
        vol_target_given = true;
        continue;
      }
      if (spo_flag(key)) {
        if (!seen.insert(key).second || i + 1 >= argc)
          throw std::invalid_argument("duplicate/missing value: " + key);
        spo_values.emplace_back(key, argv[++i]);
        continue;
      }
      if (key == "--rule" && i + 1 < argc &&
          (std::string_view(argv[i + 1]) == "aim-partial-v6" || spo_rule(argv[i + 1]))) {
        if (!seen.insert(key).second) throw std::invalid_argument("duplicate flag --rule");
        const std::string_view rule = argv[i + 1];
        if (rule == "aim-partial-v6") {
          o.aim_v6 = true;
        } else {
          o.spo_v1 = true;
          spo_version = rule == "spo-v3" ? 3U : rule == "spo-v2" ? 2U : 1U;
        }
        args.emplace_back("--rule"); args.emplace_back("aim-partial-v5");
        ++i;
        continue;
      }
      args.push_back(key);
    }
    const auto value_of = [&](std::string_view flag) -> const std::string* {
      for (usize k = 1; k + 1 < args.size(); ++k)
        if (args[k] == flag) return &args[k + 1];
      return nullptr;
    };
    const auto* output = value_of("--output");
    const auto* rate = value_of("--rate");
    const auto* theta = value_of("--trade-fraction");
    const auto* dust = value_of("--dust-multiple");
    if (v6_params && !o.aim_v6)
      throw std::invalid_argument(
          "--cost-shrink-kappa/--band-b/--band-exponent/--rate-clip need --rule aim-partial-v6");
    if (risk_target_tuned && !o.risk_target.on)
      throw std::invalid_argument("--risk-target-bias/--risk-target-cadence need --risk-target");
    if (vol_target_given) { // v8 Y: the scaler under Law::vol_target_v1 (registered constants only)
      if (o.risk_target.on)
        throw std::invalid_argument("--vol-target and --risk-target are exclusive (both set the "
                                    "aim leverage)");
      o.risk_target.on = true;
      o.risk_target.law = risk_target::Law::vol_target_v1;
    }
    // v8 Y-5 two-speed-v1: the replay's shared construction composes the sleeves into the netted
    // aim that aim-partial-v5 trades at its own theta; so no rule that replaces that plan, and the
    // fixed rate (validate_nav_config refuses a per-name rate too, review YCOMB #1). Composition
    // with --risk-target / --vol-target (Ruling on Y-5 / Y-1, registered order Y-5 -> X-10 ->
    // Y-1): the netted target is the book target; the scaler's L_t replaces the run's L in the
    // plan of that target, i.e. scales it by lambda = L_t / L, and its sigma reads the book's own
    // (net) weights, never the virtual fast sleeve F (built at the run's L). The book's fast
    // holding follows its scale, lambda F (Ruling PM8-16 #10: State::plan carries F from the
    // book's previous lambda, engine::book::two_speed_carry).
    if (std::find(args.begin(), args.end(), "--two-speed") != args.end() &&
        (o.spo_v1 || o.aim_v6 || (rate && *rate != "fixed")))
      throw std::invalid_argument("--two-speed two-speed-v1 needs aim-partial-v5 at the fixed "
                                  "rate, without spo or aim-partial-v6");
    // v8 Y norm-score-v1 is registered on aim-partial-v5 (task-YCOMB-report.md Y-3), whose
    // tied ranks it shapes; aim-partial-v6 is rewritten to v5 above, so it is refused here
    // (review YCOMB #11, Ruling PM8-16 #11).
    if (o.aim_v6 && std::find(args.begin(), args.end(), "--rank-shape") != args.end())
      throw std::invalid_argument("--rank-shape norm-score-v1 needs aim-partial-v5 (refused with "
                                  "--rule aim-partial-v6)");
    // --risk-model / --risk-model-sha256 also serve the risk target (v8 R-8); every other spo
    // value flag needs an spo rule.
    const bool spo_only = std::any_of(spo_values.begin(), spo_values.end(), [&](const auto& e) {
      return !(o.risk_target.on && risk_model_flag(e.first));
    });
    if (spo_only && !o.spo_v1)
      throw std::invalid_argument(
          "--risk-model, --gamma, ... --spo-books need --rule spo-v1|v2|v3");
    // The rule's defaults first (spo-v2: spo::v2_params; spo-v3: spo::v3_params, whose
    // registered constants no flag may move), then the flags given.
    if (o.spo_v1 && spo_version == 2) o.spo_params = spo::v2_params();
    if (o.spo_v1 && spo_version == 3) {
      o.spo_params = spo::v3_params();
      for (const auto& entry : spo_values)
        if (refused_with_v3(entry.first))
          throw std::invalid_argument(entry.first + " is refused with spo-v3 (a registered "
                                                    "constant of the rule)");
    }
    for (const auto& [key, value] : spo_values) {
      auto& sp = o.spo_params;
      if (key == "--risk-model") risk_model = value;
      else if (key == "--risk-model-sha256") risk_model_sha256 = value;
      else if (key == "--gamma") sp.gamma = number(value);
      else if (key == "--ic-book") sp.ic_book = number(value);
      else if (key == "--w-max") sp.w_max = number(value);
      else if (key == "--adv-cap-q") sp.adv_cap_q = number(value);
      else if (key == "--adv-trade-p") sp.adv_trade_p = number(value);
      else if (key == "--spo-iters") sp.max_iterations = count(value);
      else if (key == "--spo-tol") sp.tolerance = number(value);
      else if (key == "--target-vol") sp.target_vol = number(value);
      else if (key == "--spo-horizon") sp.horizon = number(value);
      else if (key == "--alpha-horizon") sp.alpha_horizon = number(value);
      else if (key == "--specific-ceiling") sp.specific_ceiling = number(value);
      else if (key == "--spo-gross" && !std::isnan(number(value))) // NaN would read as L
        sp.gross_budget = number(value);
      else if (key == "--spo-gross") throw std::invalid_argument("--spo-gross G, a number");
      else if (key == "--specific-ceiling-void" && (value == "on" || value == "off"))
        sp.void_on_capped = value == "on";
      else if (key == "--specific-ceiling-void")
        throw std::invalid_argument("--specific-ceiling-void on|off");
      else if (key == "--spo-alpha" && spo_version != 3)
        throw std::invalid_argument("--spo-alpha needs --rule spo-v3");
      else if (key == "--spo-alpha" && value != "implied-aim")
        throw std::invalid_argument("--spo-alpha implied-aim (the only value)");
      else if (key == "--spo-alpha")
        continue; // implied-aim: spo-v3 fits no alpha vector
      else if (key == "--spo-books" && (value == "all" || value == "primary"))
        sp.all_books = value == "all";
      else throw std::invalid_argument("--spo-books all|primary");
    }
    if (o.spo_v1) {
      // v8 E-26: the desired-target shaping (hold-band-v1, adv-hold-v1; v8 X inv-vol-v1) passes
      // through to the replay, whose shared construction forms spo-v3's aim with it; spo-v1/v2
      // refuse it.
      if (spo_version != 3)
        for (usize k = 1; k < args.size(); ++k)
          if (args[k] == "--hold-band" || args[k] == "--adv-hold-q" || args[k] == "--vol-scale" ||
              args[k] == "--rank-shape") // v8 Y norm-score-v1 shapes desired too
            throw std::invalid_argument(args[k] + " needs --rule spo-v3 (spo-v1/v2 refuse the "
                                                  "desired-target shaping)");
      // Ruling E-37: spo-v3 runs the capacity curve as a report-only pass (its capacity books
      // plan on their own engine as the NAV-m tracker); spo-v1/v2 do not run it.
      if (o.capacity && spo_version != 3)
        throw std::invalid_argument("spo-v1 does not run the capacity curve (--capacity-curve)");
      if (rate && *rate != "fixed") throw std::invalid_argument("spo-v1 needs the fixed rate");
      if (risk_model.empty() || risk_model_sha256.empty())
        throw std::invalid_argument("spo-v1 needs --risk-model and --risk-model-sha256");
      const auto valid = spo::validate_params(o.spo_params);
      if (!valid) throw std::invalid_argument(valid.error().to_string());
      // G against the replay's --aim-leverage (its default when absent). spo-v1 without
      // --spo-gross is G = L: nothing new to refuse.
      if (!std::isnan(o.spo_params.gross_budget)) {
        const auto* leverage = value_of("--aim-leverage");
        const f64 aim = leverage ? number(*leverage) : TargetReplayConfig{}.aim_leverage;
        const auto budget = spo::validate_gross_budget(o.spo_params.gross_budget, aim);
        if (!budget) throw std::invalid_argument(budget.error().to_string());
      }
      // The void promise (no NAV file before the tripwire is read) excludes the holdings
      // stream, which writes NAV during the replay. spo-v3 can always void (Ruling E-31a: its
      // primary book's limits_unmet, whatever --specific-ceiling-void), so it never streams.
      if (spo_version == 3 && value_of("--emit-holdings"))
        throw std::invalid_argument("spo-v3 and --emit-holdings: the holdings stream writes NAV "
                                    "before the tripwire is read, and a spo-v3 run voids on its "
                                    "primary book's limits_unmet whatever "
                                    "--specific-ceiling-void (Ruling E-31a)");
      if (o.spo_params.void_on_capped && value_of("--emit-holdings"))
        throw std::invalid_argument("--specific-ceiling-void on and --emit-holdings: the "
                                    "holdings stream writes NAV before the tripwire is read "
                                    "(pass --specific-ceiling-void off to emit holdings)");
    }
    if (o.risk_target.on && o.risk_target.law == risk_target::Law::vol_target_v1) {
      // v8 Y vol-target-v1: aim-partial-v5's aim only (the leverage cell's rule); the cap is the
      // run's --aim-leverage (the replay holds it in [1, 2]), the floor 1.
      const auto* rule = value_of("--rule");
      if (o.aim_v6 || o.spo_v1 || !rule || *rule != "aim-partial-v5")
        throw std::invalid_argument("--vol-target needs --rule aim-partial-v5 (its aim is "
                                    "--aim-leverage x desired)");
      if (risk_model.empty() || risk_model_sha256.empty())
        throw std::invalid_argument("--vol-target needs --risk-model and --risk-model-sha256 "
                                    "(the atx-risk-v1 store of the role)");
    } else if (o.risk_target.on) { // v8 R-8: risk-target-v1 scales aim-partial-v5's, spo-v3's aim
      const auto* rule = value_of("--rule"); // the spo rules and v6 read aim-partial-v5 here
      if (o.aim_v6 || (o.spo_v1 && spo_version != 3) || !rule || *rule != "aim-partial-v5")
        throw std::invalid_argument("--risk-target needs --rule aim-partial-v5 or spo-v3 (the "
                                    "rules whose aim is --aim-leverage x desired)");
      if (risk_model.empty() || risk_model_sha256.empty())
        throw std::invalid_argument("--risk-target needs --risk-model and --risk-model-sha256 "
                                    "(the atx-risk-v1 store of the role)");
      const auto valid = bk::validate_risk_target(o.risk_target.params);
      if (!valid) throw std::invalid_argument(valid.error().to_string());
    }
    if (!output || output->empty()) throw std::invalid_argument("--output is required");
    if ((o.aim_v6 || o.capacity) && rate && *rate != "fixed")
      throw std::invalid_argument("aim-partial-v6 and --capacity-curve need the fixed rate "
                                  "(per-name-v1 reads the NAV)");
    if (o.aim_v6) {
      if (!band_given) o.v6.band_b = dust ? number(*dust) : 0.0; // median band = the dust band
      const auto valid = cost_v2::validate_aim_v6(o.v6, theta ? number(*theta) : 0.25);
      if (!valid) throw std::invalid_argument(valid.error().to_string());
    }
    std::string output_dir = *output; // points into args: copy before the move
    NavV7Command command{o, std::move(args), std::move(output_dir), std::move(risk_model),
                         std::move(risk_model_sha256)};
    return co::Ok(std::move(command));
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("nav v7: ") + e.what());
  }
}

int dispatch_nav_v7(int argc, char** argv, std::ostream& out, std::ostream& err) {
  try {
    auto parsed = parse_nav_v7_args(argc, argv);
    if (!parsed) { err << parsed.error().to_string() << '\n'; return 2; }
    NavV7Options& o = parsed->options;
    const std::vector<std::string>& args = parsed->args;
    // The pinned risk model of the same role (refused before any replay work): the spo rules'
    // and the risk target's (v8 R-8), one store.
    if (o.spo_v1 || o.risk_target.on) {
      std::string role_sha;
      for (usize k = 1; k + 1 < args.size(); ++k)
        if (args[k] == "--role-sha256") role_sha = args[k + 1];
      auto store = spo::RiskStore::open(parsed->risk_model, parsed->risk_model_sha256, role_sha);
      if (!store) { err << store.error().to_string() << '\n'; return 2; }
      o.spo_risk = std::make_shared<const spo::RiskStore>(std::move(*store));
    }
    const auto run = [&](std::vector<std::string> tokens) {
      std::vector<char*> pointers;
      pointers.reserve(tokens.size());
      for (auto& token : tokens) pointers.push_back(token.data());
      return dispatch_nav_replay(static_cast<int>(pointers.size()), pointers.data(), out, err);
    };
    const std::filesystem::path dir(parsed->output);
    ScopedNavExtension extension(o);
    extension.begin_run(NavV7Pass::Main);
    if (const int code = run(args); code != 0) {
      if (extension.void_reason().empty()) return code;
      // A void run (spo tripwire in capture()): the replay stopped before its output
      // directory existed; the diagnostics go to a fresh <output> with no NAV or return file.
      if (!std::filesystem::create_directory(dir)) {
        err << "nav v7: void run; " << dir.string() << " exists, diagnostics not written\n";
        return 1;
      }
      const auto status = write_extras(dir, extension, o, extension.void_reason());
      if (!status) { err << status.error().to_string() << '\n'; return 1; }
      out << "nav v7: run VOID (" << extension.void_reason() << "); spo diagnostics written to "
          << dir.string() << '\n';
      return 3;
    }
    if (o.capacity) {
      std::vector<std::string> tokens{args.front()};
      for (usize k = 1; k < args.size(); ++k) {
        if (args[k] == "--emit-holdings" && k + 1 < args.size()) { ++k; continue; } // main only
        tokens.push_back(args[k]);
        if (args[k] == "--output" && k + 1 < args.size()) {
          tokens.push_back((dir / "capacity").string());
          ++k;
        }
      }
      extension.begin_run(NavV7Pass::Capacity);
      if (const int code = run(std::move(tokens)); code != 0) return code;
    }
    const auto status = write_extras(dir, extension, o, std::string());
    if (!status) { err << status.error().to_string() << '\n'; return 1; }
    // Console only: solve timing is not a published byte.
    if (const auto* engine = extension.spo_engine()) {
      const auto t = engine->timing();
      out << "nav v7: " << spo::rule_name(engine->params()) << ' ' << t.solves << " solves, "
          << t.seconds << " s, mean "
          << (t.solves ? 1e3 * t.seconds / static_cast<f64>(t.solves) : 0.0) << " ms, max "
          << 1e3 * t.max_seconds << " ms; gamma " << engine->calibration().gamma;
      if (engine->params().version == 3) // the solves at the --spo-iters cap and their time
        out << "; " << t.unconverged << " unconverged, " << t.unconverged_seconds << " s";
      out << '\n';
    }
    out << "nav v7: extras written to " << dir.string() << '\n';
    return 0;
  } catch (const std::exception& e) {
    err << "nav v7: " << e.what() << '\n';
    return 2;
  }
}
} // namespace atx::impl::strategy::v7
