// historical_var — historical-simulation VaR / ES of a reference book of RELATIVE option
// templates plus share legs, one (base -> shifted) explain per scenario on the risk engine
// (design §5.6, decision D13).
//
// Per scenario: resolve every template on the base state's surface (one resolve_theo_many
// batch per uid), hold the resolved contracts with the template quantities in a fresh
// RiskBook together with the share legs, evaluate both states with the configured kernel,
// explain base -> shifted. The quantiles are nearest-rank over the admitted scenarios.
//
// Threading: scenarios are independent, but a kernel that fans out over its groups already
// fills the pool, and the executor runs nested dispatches inline. So scenarios are fanned
// over the executor ONLY when the kernel's own fan-out is disabled (its n_threads == 1);
// otherwise they run serially and the kernel parallelizes. Either way each scenario writes
// only its own frame / leg slots and every reduction (explain totals, quantiles, means) is
// serial in a fixed order, so the result is bit-identical for any thread count.

#include "atx/vol/api/risk/historical_var.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/pricing/pricing_executor.hpp"
#include "risk/expiry_clock.hpp"
#include "risk/historical_var_detail.hpp"
#include "risk/risk_neumaier.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// The templates of one uid, resolved together against that uid's base surface. Built once
// per call; every scenario reads it.
struct UidGroup {
  std::uint32_t uid{0};
  std::vector<std::size_t> template_index; // into book.options, ascending
  std::vector<TheoContract> specs;         // aligned with template_index
};

[[nodiscard]] std::vector<UidGroup> group_by_uid(std::span<const TheoLeg> options) {
  std::vector<std::size_t> order(options.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(), [options](std::size_t a, std::size_t b) {
    return options[a].contract.uid < options[b].contract.uid;
  });
  std::vector<UidGroup> groups;
  for (const std::size_t j : order) {
    const std::uint32_t uid = options[j].contract.uid;
    if (groups.empty() || groups.back().uid != uid) {
      groups.push_back(UidGroup{uid, {}, {}});
    }
    groups.back().template_index.push_back(j);
    groups.back().specs.push_back(options[j].contract);
  }
  return groups;
}

// A template that never reached the resolver (no surface for its uid): the same row shape
// theo_contract.hpp gives every non-Ok resolution — identity kept, strike-level numbers NaN,
// fingerprint 0.
[[nodiscard]] ResolvedTheo unresolved_row(const TheoLeg &t, std::size_t j,
                                          std::int64_t valuation_ns) noexcept {
  ResolvedTheo r;
  r.leg.id = j;
  r.leg.uid = t.contract.uid;
  r.leg.K = kNaN;
  r.leg.side = t.contract.side;
  r.leg.qty = t.qty;
  r.leg.multiplier = t.contract.multiplier;
  r.valuation_ns = valuation_ns;
  r.T = kNaN;
  r.forward = kNaN;
  r.k_log = kNaN;
  r.standard_moneyness = kNaN;
  r.delta = kNaN;
  r.forward_delta = kNaN;
  r.iv = kNaN;
  r.mark = kNaN;
  r.greeks = AmericanGreeks{kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN};
  r.flags = static_cast<RiskFlags>(RiskFlag::SurfaceMissing);
  r.status = TheoStatus::SurfaceUnavailable;
  return r;
}

[[nodiscard]] bool has_any_surface(const MarketState &state) noexcept {
  const std::span<const UnderlierState> u = state.underliers();
  return std::any_of(u.begin(), u.end(),
                     [&state](const UnderlierState &x) { return state.surface(x.uid).valid(); });
}

[[nodiscard]] bool finite_unit_interval(double c) noexcept {
  return std::isfinite(c) && c > 0.0 && c < 1.0;
}

// The kernel validates its own policy inside evaluate; an empty book over an empty state is
// the cheapest call that reaches that check, so the policy rules stay in one place.
[[nodiscard]] Status validate_kernel(const RiskKernel &kernel) {
  ATX_TRY(const RiskBook empty, RiskBook::create({}, {}));
  ATX_TRY(const PreparedRiskBook prepared, PreparedRiskBook::create(empty));
  ATX_TRY(const MarketState state, MarketState::create(0, TimeSpec{}, {}, {}, {}));
  RiskWorkspace ws;
  ws.reserve(prepared);
  RiskFrame frame;
  if (Status st = kernel.evaluate(prepared, state, RiskNeeds{}, frame, ws); !st.has_value()) {
    return Err(ErrorCode::InvalidArgument,
               "historical_var: kernel policy refused: " + std::string(st.error().message()));
  }
  return Ok();
}

[[nodiscard]] Status validate(const HistoricalVarBook &book,
                              std::span<const HistoricalVarScenario> scenarios,
                              const HistoricalVarConfig &config) {
  if (book.options.empty() && book.shares.empty()) {
    return Err(ErrorCode::InvalidArgument, "historical_var: empty book");
  }
  for (const TheoLeg &t : book.options) {
    if (!std::isfinite(t.qty)) {
      return Err(ErrorCode::InvalidArgument, "historical_var: non-finite template qty");
    }
  }
  for (const ShareLeg &s : book.shares) {
    if (!std::isfinite(s.qty)) {
      return Err(ErrorCode::InvalidArgument, "historical_var: non-finite share qty");
    }
  }
  for (const HistoricalVarScenario &s : scenarios) {
    if (s.base == nullptr || s.shifted == nullptr) {
      return Err(ErrorCode::InvalidArgument, "historical_var: null state pointer");
    }
  }
  for (const double c : config.confidences) {
    if (!finite_unit_interval(c)) {
      return Err(ErrorCode::InvalidArgument, "historical_var: confidence outside (0, 1)");
    }
  }
  if (!(std::isfinite(config.max_excluded_premium_frac) &&
        config.max_excluded_premium_frac >= 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "historical_var: max_excluded_premium_frac must be finite and >= 0");
  }
  if (!(std::isfinite(config.explain.z_ref) && config.explain.z_ref > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "historical_var: explain z_ref must be finite > 0");
  }
  return Ok();
}

[[nodiscard]] Result<RiskKernel> make_kernel(const HistoricalVarConfig &config) {
  switch (config.kernel) {
  case RiskKernelKind::Family:
    return RiskKernel::family(config.family);
  case RiskKernelKind::Cold:
    return RiskKernel::cold(config.cold);
  }
  return Err(ErrorCode::InvalidArgument, "historical_var: unknown kernel kind");
}

[[nodiscard]] unsigned kernel_threads(const HistoricalVarConfig &config) noexcept {
  switch (config.kernel) {
  case RiskKernelKind::Family:
    return config.family.n_threads;
  case RiskKernelKind::Cold:
    return config.cold.n_threads;
  }
  return 0u;
}

// The pair's scenario-level refusals, tested exactly as the kernel and explain test them:
// either state's clock refuses one of the book's expiries (RiskKernel::evaluate: the expiry's
// own clock, risk_detail::expiry_T — a finite ExpiryCarry::T_override, else the TimeSpec), or
// the base TimeSpec refuses the step (explain_into). Both report these as InvalidArgument,
// like a structural error, so they are told apart here, before either runs.
[[nodiscard]] bool time_refuses(const PreparedRiskBook &prepared, const MarketState &base,
                                const MarketState &shifted) {
  for (const RiskGroup &g : prepared.groups()) {
    for (const MarketState *s : {&base, &shifted}) {
      const ExpiryCarry *row = risk_detail::expiry_row(*s, g.uid, g.expiry_ns);
      if (!risk_detail::expiry_T(*s, row, g.expiry_ns).has_value()) {
        return true;
      }
    }
  }
  const Result<double> dt = time_to_expiry_years(base.ts_ns(), shifted.ts_ns(), base.time());
  return !dt.has_value() || !std::isfinite(*dt);
}

// Per-worker scratch, reused across the scenarios a worker runs. Each scenario has its own
// RiskBook, so the workspace is re-bound (`reserve`) and the frames are unbound
// (`book_logical_id = 0`) before every evaluation; capacity is kept.
struct Scratch {
  std::vector<ResolvedTheo> resolved; // per template, book order
  std::vector<ResolvedTheo> batch;    // one uid group's resolve_theo_many output
  std::vector<OptionLeg> legs;
  RiskWorkspace ws;
  RiskFrame base;
  RiskFrame shifted;
  ExplainResult explained;
};

struct Context {
  const HistoricalVarBook &book;
  std::span<const HistoricalVarScenario> scenarios;
  const HistoricalVarConfig &config;
  const RiskKernel &kernel;
  std::span<const UidGroup> groups;
  std::span<HistoricalVarFrame> frames;
  std::span<ResolvedTheo> kept; // scenario-major; empty unless keep_resolved_legs
  std::span<Status> status;     // per scenario: a structural failure, else Ok
};

// Resolve every template on `base`. `refused` = the state pair cannot host templates at all
// (a marks-only base or shifted state, D13). A structural resolver error (an invalid
// projection policy) is the Status.
[[nodiscard]] Status resolve_templates(const Context &c, const HistoricalVarScenario &sc,
                                       Scratch &s, bool &refused) {
  const std::span<const TheoLeg> options = c.book.options;
  const std::int64_t valuation_ns = sc.base->ts_ns();
  s.resolved.resize(options.size());
  refused = !options.empty() && (!has_any_surface(*sc.base) || !has_any_surface(*sc.shifted));
  for (const UidGroup &g : c.groups) {
    const SurfaceRef surface = sc.base->surface(g.uid);
    if (refused || !surface.valid()) {
      for (const std::size_t j : g.template_index) {
        s.resolved[j] = unresolved_row(options[j], j, valuation_ns);
      }
      continue;
    }
    s.batch.resize(g.specs.size());
    ATX_TRY_VOID(resolve_theo_many(g.specs, surface, valuation_ns, c.config.projection, s.batch));
    for (std::size_t k = 0; k < g.template_index.size(); ++k) {
      const std::size_t j = g.template_index[k];
      s.resolved[j] = std::move(s.batch[k]);
      s.resolved[j].leg.id = j;
      s.resolved[j].leg.qty = options[j].qty;
    }
  }
  return Ok();
}

void run_scenario(const Context &c, std::size_t i, Scratch &s) {
  const HistoricalVarScenario &sc = c.scenarios[i];
  HistoricalVarFrame &f = c.frames[i];
  f = HistoricalVarFrame{};
  f.base_ts_ns = sc.base->ts_ns();
  f.shifted_ts_ns = sc.shifted->ts_ns();

  bool refused = false;
  if (Status st = resolve_templates(c, sc, s, refused); !st.has_value()) {
    c.status[i] = std::move(st);
    return;
  }
  const std::size_t n_templates = c.book.options.size();
  s.legs.clear();
  for (const ResolvedTheo &r : s.resolved) {
    if (r.status == TheoStatus::Ok) {
      s.legs.push_back(r.leg);
    }
  }
  f.n_legs_resolved = static_cast<std::uint32_t>(s.legs.size());
  f.n_legs_unresolved = static_cast<std::uint32_t>(n_templates - s.legs.size());
  if (!c.kept.empty()) {
    std::copy(s.resolved.begin(), s.resolved.end(),
              c.kept.begin() + static_cast<std::ptrdiff_t>(i * n_templates));
  }
  if (refused || (s.legs.empty() && c.book.shares.empty())) {
    return; // not admitted: nothing to price
  }

  Result<RiskBook> book = RiskBook::create(s.legs, c.book.shares);
  if (!book.has_value()) {
    c.status[i] = Err(book.error());
    return;
  }
  Result<PreparedRiskBook> prepared = PreparedRiskBook::create(*book);
  if (!prepared.has_value()) {
    c.status[i] = Err(prepared.error());
    return;
  }
  // RE-BIND RELIANCE. The workspace and frames serve a new RiskBook every scenario. The kernel
  // refuses a workspace or frame bound to another book, so they are re-bound here: the frames
  // by clearing their logical id, the workspace by RiskWorkspace::reserve, which today calls
  // WorkspaceState::bind and so re-binds it (risk_kernel.cpp). The header promises only
  // "Pre-size for a prepared book. Idempotent, grow-only." (risk_kernel.hpp, RiskWorkspace),
  // not the re-bind. If that ever changes, evaluate_pair reports the kernel's "workspace
  // bound to a different book" refusal as the call's error (structural), never as a dropped
  // scenario (HistoricalVar.ForeignWorkspaceIsAnErrorNotADroppedScenario).
  s.ws.reserve(*prepared);
  s.base.book_logical_id = 0u;
  s.shifted.book_logical_id = 0u;
  Result<historical_var_detail::PairOutcome> outcome = historical_var_detail::evaluate_pair(
      c.kernel, *prepared, sc, c.config.needs, c.config.explain, s.ws, s.base, s.shifted,
      s.explained);
  if (!outcome.has_value()) {
    c.status[i] = Err(outcome.error());
    return;
  }
  if (*outcome == historical_var_detail::PairOutcome::Refused) {
    return; // not admitted
  }
  f.totals = s.explained.total;
  f.admitted = historical_var_detail::admit(s.explained, c.config.max_excluded_premium_frac);
}

// Neumaier field-wise mean over a fixed order (the tail, sorted by loss): a pure function of
// the frames, independent of how they were produced.
class TotalsMean {
public:
  void add(const ExplainTotals &t) noexcept {
    const std::array<double, kFields> v{
        t.pv_base,   t.pv_target, t.pnl_total, t.pnl_delta,       t.pnl_gamma, t.pnl_vega,
        t.pnl_volga, t.pnl_vanna, t.pnl_theta, t.pnl_rho,         t.pnl_charm, t.pnl_unexplained,
        t.vol_level, t.vol_skew,  t.vol_curv,  t.vol_resid,       t.shares_pnl,
        t.excluded_premium, t.pnl_carry};
    for (std::size_t k = 0; k < kFields; ++k) {
      sum_[k].add(v[k]);
    }
    n_ok_ += t.n_ok;
    n_excluded_ += t.n_excluded;
  }

  // Field-wise mean over `n > 0` added rows; the two counts round to the nearest integer.
  [[nodiscard]] ExplainTotals mean(std::size_t n) const noexcept {
    const double d = static_cast<double>(n);
    const auto m = [this, d](std::size_t k) { return sum_[k].value() / d; };
    ExplainTotals t;
    t.pv_base = m(0);
    t.pv_target = m(1);
    t.pnl_total = m(2);
    t.pnl_delta = m(3);
    t.pnl_gamma = m(4);
    t.pnl_vega = m(5);
    t.pnl_volga = m(6);
    t.pnl_vanna = m(7);
    t.pnl_theta = m(8);
    t.pnl_rho = m(9);
    t.pnl_charm = m(10);
    t.pnl_unexplained = m(11);
    t.vol_level = m(12);
    t.vol_skew = m(13);
    t.vol_curv = m(14);
    t.vol_resid = m(15);
    t.shares_pnl = m(16);
    t.excluded_premium = m(17);
    t.pnl_carry = m(18); // lane E: the averaged axes still sum to the averaged total
    const std::uint64_t n64 = n;
    t.n_ok = static_cast<std::uint32_t>((n_ok_ + n64 / 2u) / n64);
    t.n_excluded = static_cast<std::uint32_t>((n_excluded_ + n64 / 2u) / n64);
    return t;
  }

private:
  static constexpr std::size_t kFields = 19;
  std::array<risk_detail::NeumaierSum, kFields> sum_{};
  std::uint64_t n_ok_{0};
  std::uint64_t n_excluded_{0};
};

// One confidence over the admitted scenarios `sorted` by ascending loss.
[[nodiscard]] HistoricalVarPoint point_at(double confidence, std::span<const std::size_t> sorted,
                                          std::span<const HistoricalVarFrame> frames) {
  HistoricalVarPoint p;
  p.confidence = confidence;
  const std::size_t n = sorted.size();
  if (n == 0u) {
    p.value_at_risk = kNaN;
    p.expected_shortfall = kNaN;
    return p;
  }
  // Nearest rank: the ceil(c n)-th smallest loss (1-based), clamped into the sample.
  const auto rank = static_cast<std::size_t>(std::ceil(confidence * static_cast<double>(n)));
  const std::size_t index = std::min(n - 1u, rank == 0u ? 0u : rank - 1u);
  risk_detail::NeumaierSum tail_loss;
  TotalsMean tail;
  for (std::size_t k = index; k < n; ++k) {
    const ExplainTotals &t = frames[sorted[k]].totals;
    tail_loss.add(-t.pnl_total);
    tail.add(t);
  }
  const std::size_t n_tail = n - index;
  p.value_at_risk = -frames[sorted[index]].totals.pnl_total;
  p.expected_shortfall = tail_loss.value() / static_cast<double>(n_tail);
  p.tail_scenarios = n_tail;
  p.tail_mean = tail.mean(n_tail);
  return p;
}

} // namespace

namespace historical_var_detail {

Result<PairOutcome> evaluate_pair(const RiskKernel &kernel, const PreparedRiskBook &prepared,
                                  const HistoricalVarScenario &scenario, const RiskNeeds &needs,
                                  const ExplainOptions &opts, RiskWorkspace &ws, RiskFrame &base,
                                  RiskFrame &shifted, ExplainResult &out) {
  const MarketState &b = *scenario.base;
  const MarketState &t = *scenario.shifted;
  if (time_refuses(prepared, b, t)) {
    return PairOutcome::Refused;
  }
  ATX_TRY_VOID(kernel.evaluate(prepared, b, needs, base, ws));
  ATX_TRY_VOID(kernel.evaluate(prepared, t, needs, shifted, ws));
  ATX_TRY_VOID(explain_into(prepared, b, base, t, shifted, opts, out));
  if (out.rate_move_without_rho && !needs.rho) {
    // The pair moves the rate but the base frame carries no rho: materialize it (on the
    // family kernel it costs the carry-bump sets, hence only when a rate actually moved) so
    // the move is attributed to pnl_rho instead of the unexplained remainder. pnl_total is
    // P_t - P_b either way.
    RiskNeeds with_rho = needs;
    with_rho.rho = true;
    ATX_TRY_VOID(kernel.evaluate(prepared, b, with_rho, base, ws));
    ATX_TRY_VOID(explain_into(prepared, b, base, t, shifted, opts, out));
  }
  return PairOutcome::Explained;
}

bool admit(const ExplainResult &explained, double max_excluded_premium_frac) noexcept {
  const ExplainTotals &t = explained.total;
  if (t.n_ok == 0u) {
    return false;
  }
  if (t.n_excluded == 0u) {
    return true;
  }
  // explain books an excluded leg whose base value is unavailable at premium 0 (pnl_explain.cpp
  // reduce_totals): that premium is unknown, so it can never be shown to be below the bound.
  for (std::size_t i = 0; i < explained.size(); ++i) {
    if (explained.status[i] != RiskLaneStatus::Ok && !std::isfinite(explained.pv_base[i])) {
      return false;
    }
  }
  // Strictly below (historical_var.hpp): at the default fraction 0 any exclusion drops the
  // scenario, even one of known zero premium.
  return t.excluded_premium < max_excluded_premium_frac * std::fabs(t.pv_base);
}

} // namespace historical_var_detail

Result<HistoricalVarResult> summarize_historical_var(std::vector<HistoricalVarFrame> frames,
                                                     std::span<const double> confidences) {
  for (const double c : confidences) {
    if (!finite_unit_interval(c)) {
      return Err(ErrorCode::InvalidArgument,
                 "summarize_historical_var: confidence outside (0, 1)");
    }
  }
  // Admitted scenarios by ascending loss; ties keep scenario order, so the tail set is a pure
  // function of the frames.
  std::vector<std::size_t> sorted;
  sorted.reserve(frames.size());
  risk_detail::NeumaierSum pv;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    if (!frames[i].admitted) {
      continue; // not in the sample: its contents are never read
    }
    // A NaN loss has no rank (it would break the sort's strict weak ordering) and a
    // non-finite loss or base value has no mean. historical_var never admits one; a caller
    // summarizing frames of its own (a verifier reading them back) is told so.
    if (!std::isfinite(frames[i].totals.pnl_total) || !std::isfinite(frames[i].totals.pv_base)) {
      return Err(ErrorCode::InvalidArgument,
                 "summarize_historical_var: an admitted frame has a non-finite pnl_total or "
                 "pv_base (scenario " + std::to_string(i) + ")");
    }
    sorted.push_back(i);
    pv.add(frames[i].totals.pv_base);
  }
  std::stable_sort(sorted.begin(), sorted.end(), [&frames](std::size_t a, std::size_t b) {
    return -frames[a].totals.pnl_total < -frames[b].totals.pnl_total;
  });

  HistoricalVarResult out;
  out.n_admitted = sorted.size();
  out.reference_pv = sorted.empty() ? kNaN : pv.value() / static_cast<double>(sorted.size());
  out.points.reserve(confidences.size());
  for (const double c : confidences) {
    out.points.push_back(point_at(c, sorted, frames));
  }
  out.frames = std::move(frames);
  return out;
}

Result<HistoricalVarResult> historical_var(const HistoricalVarBook &book,
                                           std::span<const HistoricalVarScenario> scenarios,
                                           const HistoricalVarConfig &config) {
  ATX_TRY_VOID(validate(book, scenarios, config));
  ATX_TRY(const RiskKernel kernel, make_kernel(config));
  ATX_TRY_VOID(validate_kernel(kernel));

  const std::vector<UidGroup> groups = group_by_uid(book.options);
  std::vector<HistoricalVarFrame> frames(scenarios.size());
  std::vector<ResolvedTheo> kept;
  if (config.keep_resolved_legs) {
    kept.resize(scenarios.size() * book.options.size());
  }
  std::vector<Status> status(scenarios.size(), Status{Ok()});
  const Context ctx{book, scenarios, config, kernel, groups, frames, kept, status};

  if (kernel_threads(config) == 1u) {
    pricing_executor().run_ranges(scenarios.size(), config.n_threads,
                                  [&ctx](std::size_t lo, std::size_t hi) {
                                    Scratch s;
                                    for (std::size_t i = lo; i < hi; ++i) {
                                      run_scenario(ctx, i, s);
                                    }
                                  });
  } else {
    Scratch s;
    for (std::size_t i = 0; i < scenarios.size(); ++i) {
      run_scenario(ctx, i, s);
    }
  }
  // The first structural failure in scenario order, independent of the schedule.
  for (const Status &st : status) {
    if (!st.has_value()) {
      return Err(st.error());
    }
  }

  ATX_TRY(HistoricalVarResult out,
          summarize_historical_var(std::move(frames), config.confidences));
  out.resolved_legs = std::move(kept);
  return out;
}

} // namespace atx::vol
