// spo-v3 (platform v8 R-6): parameters, declaration, diagnostics, summary and tripwire, and
// the Engine's rule-level dispatch of its published blocks. The problem is stated in
// strategy_spo_v3.hpp; the per-decision assembly is Engine::Impl::plan_tracking
// (strategy_spo.cpp).
#include "strategy_spo_v3.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>
#include "atx/engine/book/target_tracking.hpp"

namespace atx::impl::strategy::spo {
namespace {
using namespace atx;
namespace co = atx::core;
namespace tt = atx::engine::book;
using Json = nlohmann::json;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 inf = std::numeric_limits<f64>::infinity();

std::string number(f64 x) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << x;
  return out.str();
}
Json finite_or_null(f64 x) { return std::isfinite(x) ? Json(x) : Json(nullptr); }
bool is_tracking(const SpoParams& p) { return p.version == 3; }

// Mean, min and max over the finite values added.
struct Stats {
  f64 sum{}, lo{inf}, hi{-inf};
  usize n{};
  void add(f64 v) {
    if (!std::isfinite(v)) return;
    sum += v; lo = std::min(lo, v); hi = std::max(hi, v); ++n;
  }
  [[nodiscard]] f64 mean() const { return n ? sum / static_cast<f64>(n) : nan; }
  [[nodiscard]] f64 largest() const { return n ? hi : nan; }
  [[nodiscard]] f64 smallest() const { return n ? lo : nan; }
  [[nodiscard]] Json mean_max() const {
    return Json{{"mean", finite_or_null(mean())}, {"max", finite_or_null(largest())}, {"n", n}};
  }
  [[nodiscard]] Json mean_min() const {
    return Json{{"mean", finite_or_null(mean())}, {"min", finite_or_null(smallest())}, {"n", n}};
  }
};
// One book's scored rows.
struct BookStats {
  usize decisions{}, unconverged{}, unmet{}, breaches{}, capped_decisions{}, capped_max{};
  Stats iterations, primal, dual, tracking_error, tracking_error_current, share, correlation;
  Stats correlation_traded; // review A-4: the E-14 criterion's input
  Stats trade_cost, gross, turnover;
  Stats te_shadow, correlation_shadow, cost_shadow, gross_shadow, turnover_shadow;
  void add(const TrackingRow& r) {
    ++decisions;
    unconverged += r.converged ? 0U : 1U;
    unmet += r.limits_met ? 0U : 1U;
    breaches += r.gross_bound_breached ? 1U : 0U;
    capped_decisions += r.capped_specific != 0 ? 1U : 0U;
    capped_max = std::max(capped_max, r.capped_specific);
    iterations.add(static_cast<f64>(r.iterations));
    primal.add(r.primal_residual); dual.add(r.dual_residual);
    tracking_error.add(r.tracking_error); tracking_error_current.add(r.tracking_error_current);
    share.add(r.trade_limit_share); correlation.add(r.aim_correlation);
    correlation_traded.add(r.aim_correlation_traded);
    trade_cost.add(r.trade_cost); gross.add(r.gross); turnover.add(r.turnover);
    te_shadow.add(r.tracking_error_shadow); correlation_shadow.add(r.aim_correlation_shadow);
    cost_shadow.add(r.trade_cost_shadow); gross_shadow.add(r.gross_shadow);
    turnover_shadow.add(r.turnover_shadow);
  }
};
std::map<std::string, BookStats> by_book(std::span<const TrackingRow> rows) {
  std::map<std::string, BookStats> books;
  for (const auto& r : rows) books[r.book].add(r);
  return books;
}
// Ruling E-14's criterion on one book (review A-4): the mean correlation of the traded book
// with the aim (aim_correlation_traded), not the plan's (aim_correlation), against .9.
Json e14_criterion(const BookStats& b) {
  const f64 value = b.correlation_traded.mean();
  return Json{{"rule", "Ruling E-14: mean correlation of the traded book with the aim >= .9 "
                       "(threshold = v3_aim_correlation_min)"},
              {"reads", "aim_correlation_traded.mean"},
              {"threshold", v3_aim_correlation_min}, {"value", finite_or_null(value)},
              {"met", std::isfinite(value) ? Json(value >= v3_aim_correlation_min)
                                           : Json(nullptr)}};
}
// The report-only block of one book (the tripwire record and the summary share it): the
// plan's and the traded book's aim correlation, the criterion reading the traded one.
Json report(const BookStats& b) {
  return Json{{"decisions", b.decisions},
              {"tracking_error", b.tracking_error.mean_max()},
              {"trade_limit_share", b.share.mean_max()},
              {"aim_correlation", b.correlation.mean_min()},
              {"aim_correlation_traded", b.correlation_traded.mean_min()},
              {"aim_correlation_criterion", e14_criterion(b)},
              {"unconverged", b.unconverged}, {"limits_unmet", b.unmet}};
}
// Holding period of a plan (gross / one-way turnover, sessions).
Json ratio(f64 a, f64 b) { return finite_or_null(b > 0 ? a / b : nan); }

struct Trip {
  usize capped_decisions{}, capped_max{}, breaches{};
  f64 max_gross{nan};
};
// Distinct decisions (sessions) whose risk slice had a clamped entry, the most entries
// clamped at one decision, book decisions above the gross bound and the largest gross.
Trip count_trips(std::span<const TrackingRow> rows) {
  std::set<i64> sessions;
  Trip t;
  for (const auto& r : rows) {
    t.breaches += r.gross_bound_breached ? 1U : 0U;
    if (!(r.gross <= t.max_gross)) t.max_gross = r.gross; // the first row replaces NaN
    if (r.capped_specific == 0) continue;
    sessions.insert(r.session);
    t.capped_max = std::max(t.capped_max, r.capped_specific);
  }
  t.capped_decisions = sessions.size();
  return t;
}
// Ruling E-31a (review SPO-1): the primary book's scored decisions whose net or beta limit was
// not met, and the first such session.
struct Unmet {
  usize count{};
  i64 first_session{};
};
Unmet primary_unmet(std::span<const TrackingRow> rows, std::string_view primary) {
  Unmet u;
  for (const auto& r : rows) {
    if (r.limits_met || std::string_view(r.book) != primary) continue;
    if (u.count == 0 || r.session < u.first_session) u.first_session = r.session;
    ++u.count;
  }
  return u;
}
constexpr const char* limits_unmet_rule =
    "Ruling E-31a: a scored decision of the primary book whose net or beta limit is not met "
    "(limits_met false) voids the run, whatever --specific-ceiling-void, before any NAV or "
    "return file is written or printed";
constexpr const char* correlation_unit =
    "Pearson correlation over the optimized names of the planned (shadow) and the aim weights";
constexpr const char* traded_correlation_unit =
    "Pearson correlation of the traded book (the holdings DECIDE read at d: fills, caps, "
    "blocks and drift of earlier decisions, nonmember exits and unpriced members included) and "
    "the aim, over every name either holds; Ruling E-14's criterion reads its mean";
} // namespace

SpoParams v3_params() {
  SpoParams p;
  p.version = 3;
  p.horizon = v3_horizon;
  p.sharpe_prior = v3_sharpe_prior;
  p.adv_trade_p = v3_adv_trade_p;
  p.beta_max = v3_beta_max;
  p.specific_ceiling = v3_specific_ceiling;
  p.void_on_capped = true;
  p.max_iterations = tt::tracking_max_iterations;
  p.tolerance = tt::tracking_tolerance;
  return p;
}

std::string tracking_declaration(const SpoParams& p) {
  return std::string(rule_name(p)) +
         " (platform v8 R-6, review S-8: target tracking, spo trial 3; S_prior by Ruling E-14): "
         "on every rebalance decision d each book solves min_w (gamma/2) (w - w_aim)' Sigma "
         "(w - w_aim) + (1/H) sum_i [s_i |w_i - w0_i| + eta_i |w_i - w0_i|^1.5] + sum_i b_i "
         "max(-w_i, 0) s.t. book net 0, |book beta| <= " + number(p.beta_max) +
         " (spo-v1's beta), |w_i - w0_i| <= " + number(p.adv_trade_p) +
         " ADV_i / NAV (0 without ADV), w_i >= min(w0_i, 0) where the book's locate rule guards "
         "the name; w_aim = --aim-leverage x desired on the members (the accepted rule's aim; "
         "--spo-alpha implied-aim: no alpha vector is fitted, the tracker trades for the aim's "
         "implied alpha gamma Sigma w_aim); Sigma = X F X' + D of atx-risk-v1 at the close of d "
         "(NaN factor entries 0, negative factor eigenvalues set to 0 and counted; a daily "
         "specific variance above --specific-ceiling " + number(p.specific_ceiling) +
         " clamped); gamma = S_prior / sigma_aim with S_prior = " + number(p.sharpe_prior) +
         " (the aim's implied annual Sharpe) and sigma_aim = sqrt(252 w_aim' Sigma w_aim) of the "
         "whole aim at the first rebalance decision; H = " + number(p.horizon) +
         " sessions (fixed, not 1 / theta); s_i = half spread + commission and eta_i = impact_y "
         "sigma_i sqrt(NAV / ADV_i) of the primary S2 law on the decision liquidity window "
         "(sigma fallback .05); b_i = the book's short financing per session (annual x "
         "(365/252) / day count; tier fee at d); no holding cap and no gross constraint: a "
         "planned gross above " + number(v3_gross_bound_multiple) +
         " x --aim-leverage breaches a sanity bound; --specific-ceiling-void " +
         (p.void_on_capped ? "on" : "off") +
         " (on: a clamp or a breach at any scored decision voids the run, which exits non-zero "
         "after writing spo_diagnostics.csv and v7_extras.json and before any NAV or return "
         "file); a scored decision of the primary book that does not meet its net or beta limit "
         "voids the run the same way whatever --specific-ceiling-void (Ruling E-31a); "
         "optimized names = members with a risk row; nonmembers follow aim-partial-v5's "
         "exit rule and unpriced members keep their weight (fixed positions in the limits and, "
         "through their factor exposure, in the tracking term); a position outside its box by "
         "more than one session's trade limit moves by the limit toward it; solver: over-relaxed "
         "ADMM on the factor structure (atx::engine::book::solve_tracking), stop at primal and "
         "dual residual <= " + number(p.tolerance) + " or at " + std::to_string(p.max_iterations) +
         " iterations (registered: --spo-tol and --spo-iters are refused, Ruling E-31a), warm "
         "dual per book; non-rebalance decisions "
         "are aim-partial-v5's (exits only); diagnostics beside each book: a plan-level "
         "aim-partial-v5 shadow book (same aim, full fills, no drift) scored with the same risk "
         "model and S2 law; tracking_error columns are annualised, the other money columns per "
         "session or per decision";
}

Json tracking_parameters_json(const SpoParams& p, f64 horizon, f64 gross_bound) {
  return Json{
      {"rule", rule_name(p)}, {"version", p.version},
      {"alpha", "implied-aim (no alpha vector: the aim's implied alpha gamma Sigma w_aim)"},
      {"sharpe_prior", finite_or_null(p.sharpe_prior)},
      {"sharpe_prior_rule", "Ruling E-14 (pre-read amendment of the registration: 20, not 1.0)"},
      {"gamma_rule", "S_prior / sigma_aim on the first rebalance decision"},
      {"horizon", finite_or_null(horizon)}, {"horizon_rule", "registered constant, not 1 / theta"},
      {"gross_bound", finite_or_null(gross_bound)},
      {"gross_bound_rule", "2 x --aim-leverage: a checked sanity bound, not a solver constraint"},
      {"adv_trade_p", p.adv_trade_p}, {"beta_max", p.beta_max},
      {"specific_ceiling", finite_or_null(p.specific_ceiling)},
      {"specific_ceiling_void", p.void_on_capped},
      {"spo_iters", p.max_iterations}, {"spo_tol", p.tolerance},
      {"spo_iters_rule", "registered constants (Ruling E-31a: --spo-iters and --spo-tol are "
                         "refused under spo-v3)"},
      {"limits_unmet_rule", limits_unmet_rule},
      {"books", p.all_books ? "all" : "primary (S1, S2)"},
      {"solver", Json{{"method", "atx::engine::book::solve_tracking (over-relaxed ADMM on the "
                                 "factor structure, warm dual per book)"},
                      {"penalty_scale", tt::tracking_penalty_scale},
                      {"relaxation", tt::tracking_relaxation},
                      {"restore_passes", tt::tracking_restore_passes},
                      {"limit_tolerance", tt::tracking_limit_tolerance}}}};
}

Json tracking_calibration_json(const SpoParams& p, const Calibration& c) {
  Json j{{"done", c.done}, {"session", c.session},
         {"rule", "gamma = S_prior / sigma_aim, sigma_aim = sqrt(252 w_aim' Sigma w_aim) of "
                  "the whole aim (its names with a risk row) at the first rebalance "
                  "decision"},
         {"sharpe_prior", finite_or_null(p.sharpe_prior)},
         {"sigma_aim", finite_or_null(c.aim_vol)}, {"aim_gross", finite_or_null(c.aim_gross)},
         {"gamma", finite_or_null(c.gamma)}, {"names", c.names}};
  if (c.warm_up) j["warm_up"] = warm_up_calibration_text; // review A-2; absent without one
  return j;
}

std::string tracking_csv(std::span<const TrackingRow> rows) {
  std::string text =
      "session,book,members,optimized,unpriced_members,fixed_nonmembers,gamma,iterations,"
      "converged,limits_met,primal_residual,dual_residual,limit_violation,clipped_eigenvalues,"
      "tracking_error,tracking_error_current,aim_correlation,aim_correlation_traded,objective,"
      "trade_cost,"
      "amortized_cost,borrow,gross,aim_gross,net,long,short,abs_beta,turnover,no_trade,"
      "at_trade_limit,trade_limit_share,at_locate_floor,gross_bound_breached,nu,rho,"
      "capped_specific,gross_shadow,turnover_shadow,trade_cost_shadow,tracking_error_shadow,"
      "aim_correlation_shadow\n";
  for (const auto& r : rows) {
    const auto u = [](usize v) { return std::to_string(v); };
    const auto b = [](bool v) { return std::string(v ? "1" : "0"); };
    text += std::to_string(r.session) + ',' + r.book + ',' + u(r.members) + ',' + u(r.optimized) +
            ',' + u(r.unpriced_members) + ',' + u(r.fixed_nonmembers) + ',' + number(r.gamma) +
            ',' + u(r.iterations) + ',' + b(r.converged) + ',' + b(r.limits_met) + ',' +
            number(r.primal_residual) + ',' + number(r.dual_residual) + ',' +
            number(r.limit_violation) + ',' + u(r.clipped_eigenvalues) + ',' +
            number(r.tracking_error) + ',' + number(r.tracking_error_current) + ',' +
            number(r.aim_correlation) + ',' + number(r.aim_correlation_traded) + ',' +
            number(r.objective) + ',' + number(r.trade_cost) +
            ',' + number(r.amortized_cost) + ',' + number(r.borrow) + ',' + number(r.gross) +
            ',' + number(r.aim_gross) + ',' + number(r.net) + ',' + number(r.long_weight) + ',' +
            number(r.short_weight) + ',' + number(r.abs_beta) + ',' + number(r.turnover) + ',' +
            u(r.no_trade) + ',' + u(r.at_trade_limit) + ',' + number(r.trade_limit_share) +
            ',' + u(r.at_locate_floor) + ',' + b(r.gross_bound_breached) + ',' + number(r.nu) +
            ',' + number(r.rho) + ',' + u(r.capped_specific) + ',' + number(r.gross_shadow) +
            ',' + number(r.turnover_shadow) + ',' + number(r.trade_cost_shadow) + ',' +
            number(r.tracking_error_shadow) + ',' + number(r.aim_correlation_shadow) + '\n';
  }
  return text;
}

Json tracking_units_json() {
  constexpr const char* annualised =
      "annualised: sqrt(252 x daily ex-ante variance of w - w_aim), whole book (priced names)";
  constexpr const char* session = "per session, NAV fraction";
  constexpr const char* decision = "per decision, NAV fraction (unamortized)";
  return Json{{"tracking_error", annualised}, {"tracking_error_current", annualised},
              {"tracking_error_shadow", annualised}, {"objective", session},
              {"amortized_cost", "trade_cost / H"}, {"borrow", session},
              {"trade_cost", decision}, {"trade_cost_shadow", decision},
              {"aim_correlation", correlation_unit}, {"aim_correlation_shadow", correlation_unit},
              {"aim_correlation_traded", traded_correlation_unit},
              {"primal_residual", "weight units"},
              {"dual_residual", "weight units (rho step over gamma d)"}};
}

Json tracking_summary_json(std::span<const TrackingRow> rows) {
  Json books = Json::object();
  for (const auto& [book, s] : by_book(rows)) {
    Json entry = report(s);
    entry["mean_iterations"] = finite_or_null(s.iterations.mean());
    entry["max_primal_residual"] = finite_or_null(s.primal.largest());
    entry["max_dual_residual"] = finite_or_null(s.dual.largest());
    entry["mean_tracking_error_current"] = finite_or_null(s.tracking_error_current.mean());
    entry["mean_trade_cost"] = finite_or_null(s.trade_cost.mean());
    entry["mean_gross"] = finite_or_null(s.gross.mean());
    entry["max_gross"] = finite_or_null(s.gross.largest());
    entry["mean_turnover"] = finite_or_null(s.turnover.mean());
    entry["holding_sessions"] = ratio(s.gross.sum, s.turnover.sum);
    entry["gross_bound_breaches"] = s.breaches;
    entry["capped_specific_decisions"] = s.capped_decisions;
    entry["capped_specific_names_max"] = s.capped_max;
    entry["shadow"] = Json{{"rule", "plan-level aim-partial-v5 (full fills, no drift)"},
                           {"mean_trade_cost", finite_or_null(s.cost_shadow.mean())},
                           {"mean_gross", finite_or_null(s.gross_shadow.mean())},
                           {"mean_turnover", finite_or_null(s.turnover_shadow.mean())},
                           {"mean_tracking_error", finite_or_null(s.te_shadow.mean())},
                           {"aim_correlation", s.correlation_shadow.mean_min()},
                           {"holding_sessions",
                            ratio(s.gross_shadow.sum, s.turnover_shadow.sum)}};
    entry["trade_cost_ratio"] = ratio(s.trade_cost.sum, s.cost_shadow.sum);
    books[book] = std::move(entry);
  }
  return books;
}

std::string default_primary_book() {
  const auto scenarios = fixed_nav_scenarios();
  const auto& s = scenarios[nav_primary_scenario_index];
  return s.id + "+" + s.financing.id;
}

co::Status tracking_tripwire(const SpoParams& p, std::span<const TrackingRow> rows,
                             std::string_view primary_book) {
  if (const Unmet u = primary_unmet(rows, primary_book); u.count > 0)
    return co::Err(co::ErrorCode::Unavailable,
                   std::string(rule_name(p)) + ": tripwire: Ruling E-31a: the primary book " +
                       std::string(primary_book) + " did not meet its net or beta limit " +
                       "(limits_unmet) on " + std::to_string(u.count) +
                       " scored decisions (first session " + std::to_string(u.first_session) +
                       "); the run is VOID whatever --specific-ceiling-void: no NAV or return " +
                       "file is written");
  if (!p.void_on_capped) return co::Ok();
  const Trip t = count_trips(rows);
  if (t.capped_decisions == 0 && t.breaches == 0) return co::Ok();
  return co::Err(co::ErrorCode::Unavailable,
                 std::string(rule_name(p)) + ": tripwire: " +
                     std::to_string(t.capped_decisions) +
                     " decisions had a daily specific variance above " +
                     number(p.specific_ceiling) + " (at most " + std::to_string(t.capped_max) +
                     " entries at one decision) and " + std::to_string(t.breaches) +
                     " book decisions planned a gross above " +
                     number(v3_gross_bound_multiple) + " x --aim-leverage (largest " +
                     number(t.max_gross) + "); the run is VOID (--specific-ceiling-void on): no "
                     "NAV or return file is written");
}

Json tracking_tripwire_json(const SpoParams& p, std::span<const TrackingRow> rows,
                            std::string_view primary_book) {
  const Trip t = count_trips(rows);
  const Unmet u = primary_unmet(rows, primary_book);
  const bool tripped = t.capped_decisions > 0 || t.breaches > 0;
  const bool void_run = u.count > 0 || (tripped && p.void_on_capped);
  const char* status = void_run  ? "void"
                       : !tripped ? "clear"
                                  : "tripped (not voiding: --specific-ceiling-void off)";
  Json report_only = Json::object();
  for (const auto& [book, s] : by_book(rows)) report_only[book] = report(s);
  Json j{{"specific_ceiling", finite_or_null(p.specific_ceiling)},
         {"specific_ceiling_void", p.void_on_capped},
         {"capped_specific_decisions", t.capped_decisions},
         {"capped_specific_names_max", t.capped_max},
         {"gross_bound_multiple", v3_gross_bound_multiple},
         {"gross_bound_breaches", t.breaches}, {"max_gross", finite_or_null(t.max_gross)},
         {"primary_book", std::string(primary_book)},
         {"limits_unmet_rule", limits_unmet_rule},
         {"limits_unmet_primary",
          Json{{"count", u.count},
               {"first_session", u.count > 0 ? Json(u.first_session) : Json(nullptr)}}},
         {"status", status},
         {"report_only", std::move(report_only)}};
  if (u.count > 0) j["voided"] = "limits_unmet";
  return j;
}

// ---- rule-level dispatch (Engine): spo-v1/v2 unchanged, spo-v3 above ----------------------------
std::string Engine::rule_declaration() const {
  return is_tracking(params()) ? tracking_declaration(params()) : declaration(params());
}
Json Engine::rule_parameters_json() const {
  return is_tracking(params()) ? tracking_parameters_json(params(), horizon(), gross_budget())
                               : parameters_json(params(), horizon(), gross_budget());
}
Json Engine::rule_calibration_json() const {
  return is_tracking(params()) ? tracking_calibration_json(params(), calibration())
                               : calibration_json(calibration());
}
std::string Engine::rows_csv() const {
  return is_tracking(params()) ? tracking_csv(tracking_rows()) : diagnostics_csv(rows());
}
Json Engine::rows_units_json() const {
  return is_tracking(params()) ? tracking_units_json() : diagnostics_units_json();
}
Json Engine::rows_summary_json() const {
  return is_tracking(params()) ? tracking_summary_json(tracking_rows()) : summary_json(rows());
}
Json Engine::rows_tripwire_json() const {
  return is_tracking(params())
             ? tracking_tripwire_json(params(), tracking_rows(), primary_book())
             : tripwire_json(params(), rows());
}
co::Status Engine::rows_tripwire() const {
  return is_tracking(params()) ? tracking_tripwire(params(), tracking_rows(), primary_book())
                               : ceiling_tripwire(params(), rows());
}
} // namespace atx::impl::strategy::spo
