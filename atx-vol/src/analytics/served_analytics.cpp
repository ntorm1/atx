// Served analytics (lane A1, r6 2026-10-23): api/analytics/served_analytics.hpp is the
// contract, docs/analytics-contracts.md the narrative.
#include "atx/vol/api/analytics/served_analytics.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] bool finite_positive(double x) noexcept { return std::isfinite(x) && x > 0.0; }

[[nodiscard]] HedgeGreekReason from_serve(ServeGreekReason r) noexcept {
  return static_cast<HedgeGreekReason>(static_cast<std::uint8_t>(r));
}

[[nodiscard]] bool certifying_class(ServeBoundClass c) noexcept {
  return c == ServeBoundClass::Exact || c == ServeBoundClass::RefinementValidated ||
         c == ServeBoundClass::KernelTested;
}

[[nodiscard]] AnalyticsInputClass classify(ServeStatus status, bool sampled,
                                           ServeBoundClass certificate_class,
                                           double price) noexcept {
  switch (status) {
  case ServeStatus::Ok:
  case ServeStatus::Exercised:
    if (!std::isfinite(price)) return AnalyticsInputClass::Refused;
    if (sampled || !certifying_class(certificate_class)) return AnalyticsInputClass::Estimate;
    return AnalyticsInputClass::Certified;
  case ServeStatus::PriceOnly:
    return std::isfinite(price) ? AnalyticsInputClass::PriceOnly : AnalyticsInputClass::Refused;
  case ServeStatus::OutOfDomain:
  case ServeStatus::SigmaOutOfBox:
  case ServeStatus::TierUnavailable:
  case ServeStatus::Stale:
  case ServeStatus::NoConvergence:
  case ServeStatus::NonMonotoneBand:
  case ServeStatus::Unconfirmed:
  case ServeStatus::InvalidArgument:
  case ServeStatus::BudgetExhausted:
  case ServeStatus::Invalidated:
    return AnalyticsInputClass::Refused;
  }
  return AnalyticsInputClass::Refused;
}

[[nodiscard]] AnalyticsStatus status_of(AnalyticsInputClass c) noexcept {
  switch (c) {
  case AnalyticsInputClass::Certified: return AnalyticsStatus::Ok;
  case AnalyticsInputClass::Estimate: return AnalyticsStatus::Estimate;
  case AnalyticsInputClass::PriceOnly: return AnalyticsStatus::PriceOnly;
  case AnalyticsInputClass::Refused: return AnalyticsStatus::Refused;
  }
  return AnalyticsStatus::Refused;
}

// The declaration of an answer read from `version` (the reader's pinned version after the
// query that produced it).
[[nodiscard]] AnalyticsDeclaration declare(const ServeBookVersion &version,
                                           std::uint64_t expiry_id, std::int64_t now_ns,
                                           HedgeConvention convention,
                                           const ServeAnswer &answer) noexcept {
  AnalyticsDeclaration d;
  d.book_version = version.number;
  d.expiry_id = expiry_id;
  d.model = AnalyticsModel::QuotingRecord;
  d.greek_model = answer.convention.model;
  d.convention = convention;
  d.clock.now_ns = now_ns;
  d.price_map_delta = answer.price_map_delta;
  const ServeBookEntry *e = version.find(expiry_id);
  if (e == nullptr) return d;
  d.entry_tag = e->tag;
  d.clock.expiry_ns = e->expiry_ns;
  d.clock.next_event_ns = e->next_event_ns;
  if (e->record) {
    const ServeRecordVersion &v = e->record->version();
    d.record_fit_sha256 = v.fit_sha256;
    d.record_policy_sha256 = v.policy_sha256;
    d.record_input_sha256 = v.input_sha256;
    d.clock.as_of_ns = v.as_of_ns;
    d.clock.tau = e->record->tau();
  }
  return d;
}

[[nodiscard]] ServeRequest served_request(double strike, Side side, double spot,
                                          std::int64_t now_ns, ServeTier tier,
                                          ServeTier tier_max) noexcept {
  ServeRequest q;
  q.strike = strike;
  q.side = side;
  q.spot = spot;
  q.tier = tier;
  q.tier_max = tier_max;
  q.greeks = ServeGreekMask::four();
  q.now_ns = now_ns;
  q.family_bound = true;
  return q;
}

// The fitted curve's slope dsigma/dk at k = ln(K / F_fit) (central difference).
[[nodiscard]] double curve_slope(const SliceServeRecord &record, double strike) noexcept {
  const IVolCurve &curve = record.curve();
  const double k = std::log(strike / curve.F());
  const double up = curve.iv(k + kSmileSlopeStep);
  const double down = curve.iv(k - kSmileSlopeStep);
  return (up - down) / (2.0 * kSmileSlopeStep);
}

void apply_convention(HedgeGreeks &h, const SliceServeRecord *record, double strike,
                      double spot) noexcept {
  const bool numbers = std::isfinite(h.price) && h.input_class != AnalyticsInputClass::PriceOnly;
  switch (h.declaration.convention) {
  case HedgeConvention::ContractSigma:
  case HedgeConvention::StickyStrike:
    if (numbers) {
      h.dsigma_dspot = 0.0;
      h.slope_class = SmileSlopeClass::ZeroByConvention;
    }
    return;
  case HedgeConvention::RefittedSurface:
    h.delta = h.gamma = h.vega = h.theta = kNaN;
    h.reason.fill(HedgeGreekReason::RequiresRefit);
    return;
  case HedgeConvention::StickyDeltaMoneyness:
    break;
  }
  if (!numbers) return;
  const double slope = record != nullptr ? curve_slope(*record, strike) : kNaN;
  if (!std::isfinite(slope)) {
    h.delta = kNaN;
    h.reason[0] = HedgeGreekReason::SmileSlopeUnavailable;
  } else {
    h.curve_slope = slope;
    h.dsigma_dspot = -slope / spot;
    h.slope_class = SmileSlopeClass::FittedCurveCentralDifference;
    if (std::isfinite(h.delta) && std::isfinite(h.vega)) {
      h.delta = h.delta + h.vega * h.dsigma_dspot;
    } else if (std::isfinite(h.delta)) {
      h.delta = kNaN; // vega withheld: the smile term cannot be added
      h.reason[0] = h.reason[2];
    }
  }
  // Exercised: intrinsic does not move with the smile (gamma 0 stays). Otherwise the
  // convention's gamma needs vanna, volga and sigma'' terms no tier serves.
  if (h.reason[1] != HedgeGreekReason::Exercised) {
    h.gamma = kNaN;
    h.reason[1] = HedgeGreekReason::NeedsVannaVolga;
  }
}

[[nodiscard]] double payoff(Side side, double S, double K) noexcept {
  return side == Side::Call ? std::max(S - K, 0.0) : std::max(K - S, 0.0);
}

[[nodiscard]] int class_rank(ServeBoundClass c) noexcept {
  switch (c) {
  case ServeBoundClass::Exact: return 0;
  case ServeBoundClass::KernelTested: return 1;
  case ServeBoundClass::RefinementValidated: return 2;
  case ServeBoundClass::Sampled: return 3;
  case ServeBoundClass::None: return 4;
  }
  return 4;
}

[[nodiscard]] ServeBoundClass weaker(ServeBoundClass a, ServeBoundClass b) noexcept {
  return class_rank(a) >= class_rank(b) ? a : b;
}

// Does a roll of dt (variance years) cross the entry's next event? The event instant is
// located by mapping dt linearly onto [now, expiry]; without an expiry instant any roll
// with an event pending is treated as crossing (conservative).
[[nodiscard]] bool crosses_event(double dt, double tau, std::int64_t now_ns,
                                 std::int64_t expiry_ns, std::int64_t next_event_ns) noexcept {
  if (next_event_ns <= 0 || !(dt > 0.0)) return false;
  if (expiry_ns <= now_ns || !(tau > 0.0)) return true;
  const double span = static_cast<double>(expiry_ns - now_ns);
  const double at = static_cast<double>(now_ns) + (dt / tau) * span;
  return at >= static_cast<double>(next_event_ns);
}

// ── The scenario engine ─────────────────────────────────────────────────────────────────

struct Shock {
  double x = 0.0;  // spot_rel
  double y = 0.0;  // vol_abs
  double dt = 0.0; // variance-clock years
};

struct UniqueKey {
  std::uint64_t expiry_id = 0;
  double strike = 0.0;
  Side side = Side::Call;
};

// One unique contract's evaluation (a worker writes its own slot only).
struct UniqueEval {
  ServeAnswer base{};
  AnalyticsInputClass cls = AnalyticsInputClass::Refused;
  AnalyticsDeclaration declaration{};
  bool has_record = false;
  double q_node = kNaN;
  double rate = kNaN;
  double tau = kNaN;
  double cold_base = kNaN;
  double gamma_var = kNaN; // max |gamma(S (1 +/- b)) - gamma(S)|
  double vol_edge = kNaN;  // max |P(sigma +/- b) - P(sigma) - vega (+/- b)| (cold kernel)
  double time_edge = kNaN; // |P(T - b) - P(T) - theta b| (cold kernel)
  std::int64_t expiry_ns = 0;
  std::int64_t next_event_ns = 0;
  std::vector<ScenarioLegCell> cells;
};

struct WorkerTally {
  std::uint64_t lo = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t hi = 0;
  std::size_t base_serves = 0;
  std::size_t record_serves = 0;
  std::size_t cold_solves = 0;
  void saw(std::uint64_t v) noexcept {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
};

struct EngineContext {
  const ScenarioSpec &spec;
  std::span<const Shock> shocks;
  bool taylor = true;
  bool exact = true;
  bool any_spot = false; // a non-zero spot shock
  bool any_vol = false;
  bool any_time = false;
};

[[nodiscard]] double cold_price(const EngineContext &cx, const UniqueKey &key, double S,
                                double T, double sigma, const UniqueEval &u,
                                WorkerTally &tally) {
  ++tally.cold_solves;
  const Result<double> p =
      andersen_lake(S, key.strike, T, sigma, u.rate, u.q_node, key.side, cx.spec.cold_opts);
  return p ? *p : kNaN;
}

void remainder_setup(const EngineContext &cx, ServeBookReader &reader, const UniqueKey &key,
                     UniqueEval &u, WorkerTally &tally) {
  const ScenarioSpec &spec = cx.spec;
  const double S = spec.spot;
  const double sigma = u.base.sigma;
  if (cx.taylor && cx.any_spot) {
    const double b = spec.box.spot_rel;
    double worst = 0.0;
    bool ok = std::isfinite(u.base.gamma);
    for (const double sgn : {-1.0, 1.0}) {
      const ServeAnswer end = reader.serve(
          key.expiry_id, served_request(key.strike, key.side, S * (1.0 + sgn * b), spec.now_ns,
                                        spec.tier, spec.tier_max));
      ++tally.record_serves;
      tally.saw(reader.pinned().number);
      ok = ok && std::isfinite(end.gamma);
      if (ok) worst = std::max(worst, std::abs(end.gamma - u.base.gamma));
    }
    u.gamma_var = ok ? worst : kNaN;
  }
  if (!(cx.any_vol || cx.any_time) || !std::isfinite(u.q_node) || !(sigma > 0.0)) return;
  u.cold_base = cold_price(cx, key, S, u.tau, sigma, u, tally);
  if (!std::isfinite(u.cold_base) || !cx.taylor) return;
  if (cx.any_vol && std::isfinite(u.base.vega) && spec.box.vol_abs > 0.0) {
    const double b = spec.box.vol_abs;
    double worst = 0.0;
    bool ok = true;
    for (const double sgn : {-1.0, 1.0}) {
      const double s1 = std::max(sigma + sgn * b, kScenarioSigmaFloor);
      const double p = cold_price(cx, key, S, u.tau, s1, u, tally);
      ok = ok && std::isfinite(p);
      if (ok) worst = std::max(worst, std::abs(p - u.cold_base - u.base.vega * (s1 - sigma)));
    }
    u.vol_edge = ok ? worst : kNaN;
  }
  if (cx.any_time && std::isfinite(u.base.theta) && spec.box.time_years > 0.0 &&
      spec.box.time_years < u.tau) {
    const double b = spec.box.time_years;
    const double p = cold_price(cx, key, S, u.tau - b, sigma, u, tally);
    if (std::isfinite(p)) u.time_edge = std::abs(p - u.cold_base - u.base.theta * b);
  }
}

// The base answer, its provenance and the remainder estimates of one unique contract.
void evaluate_base(const EngineContext &cx, ServeBookReader &reader, const UniqueKey &key,
                   UniqueEval &u, WorkerTally &tally) {
  const ScenarioSpec &spec = cx.spec;
  u.base = reader.serve(key.expiry_id, served_request(key.strike, key.side, spec.spot,
                                                      spec.now_ns, spec.tier, spec.tier_max));
  ++tally.base_serves;
  const ServeBookVersion &version = reader.pinned();
  tally.saw(version.number);
  u.declaration = declare(version, key.expiry_id, spec.now_ns, spec.convention, u.base);
  u.cls = classify_served_answer(u.base);
  const ServeBookEntry *e = version.find(key.expiry_id);
  if (e == nullptr || !e->record) return;
  const SliceServeRecord &record = *e->record;
  u.has_record = true;
  u.tau = record.tau();
  u.rate = record.rate();
  u.expiry_ns = e->expiry_ns;
  u.next_event_ns = e->next_event_ns;
  for (const ServedNode &n : record.nodes()) {
    if (n.strike == key.strike) {
      u.q_node = key.side == Side::Call ? n.call.q : n.put.q;
      break;
    }
  }
  if (u.cls == AnalyticsInputClass::Certified || u.cls == AnalyticsInputClass::Estimate) {
    remainder_setup(cx, reader, key, u, tally);
  }
}

void taylor_cell(const EngineContext &cx, const UniqueEval &u, const Shock &s, bool event,
                 ScenarioLegCell &c) {
  const ScenarioSpec &spec = cx.spec;
  const TaylorBox &box = spec.box;
  if (event) {
    c.taylor_exclusion = ScenarioExclusion::EventOperation;
    return;
  }
  if (std::abs(s.x) > box.spot_rel || std::abs(s.y) > box.vol_abs || s.dt > box.time_years) {
    c.taylor_exclusion = ScenarioExclusion::OutsideTaylorBox;
    return;
  }
  const ServeAnswer &a = u.base;
  const bool spot = s.x != 0.0;
  const bool vol = s.y != 0.0;
  const bool time = s.dt != 0.0;
  if ((spot && !(std::isfinite(a.delta) && std::isfinite(a.gamma))) ||
      (vol && !std::isfinite(a.vega)) || (time && !std::isfinite(a.theta))) {
    c.taylor_exclusion = ScenarioExclusion::MissingGreek;
    return;
  }
  if ((spot && !std::isfinite(u.gamma_var)) || (vol && !std::isfinite(u.vol_edge)) ||
      (time && !std::isfinite(u.time_edge))) {
    c.taylor_exclusion = ScenarioExclusion::RemainderUnbounded;
    return;
  }
  const double dS = spec.spot * s.x;
  // Every term is gated on its own shock: a zero shock never reads its Greek.
  c.taylor_delta = spot ? a.delta * dS : 0.0;
  c.taylor_gamma = spot ? 0.5 * a.gamma * dS * dS : 0.0;
  c.taylor_vega = vol ? a.vega * s.y : 0.0;
  c.taylor_theta = time ? a.theta * s.dt : 0.0;
  c.taylor = c.taylor_delta + c.taylor_gamma + c.taylor_vega + c.taylor_theta;
  const double ry = vol ? s.y / box.vol_abs : 0.0;
  const double rt = time ? s.dt / box.time_years : 0.0;
  c.remainder = (spot ? 0.5 * dS * dS * u.gamma_var : 0.0) + (vol ? u.vol_edge * ry * ry : 0.0) +
                (time ? u.time_edge * rt * rt : 0.0);
  if (u.cls == AnalyticsInputClass::Estimate) c.taylor_exclusion = ScenarioExclusion::Estimate;
}

void exact_record_cell(const EngineContext &cx, ServeBookReader &reader, const UniqueKey &key,
                       const UniqueEval &u, const Shock &s, ScenarioLegCell &c,
                       WorkerTally &tally) {
  const ServeAnswer &a = u.base;
  c.exact_route = ScenarioExactRoute::RecordTier;
  if (s.x == 0.0) {
    c.exact = 0.0;
    c.exact_class = a.certificate_class;
    c.exact_certificate_usd = 0.0;
    return;
  }
  const ScenarioSpec &spec = cx.spec;
  const ServeAnswer b = reader.serve(
      key.expiry_id, served_request(key.strike, key.side, spec.spot * (1.0 + s.x), spec.now_ns,
                                    spec.tier, spec.tier_max));
  ++tally.record_serves;
  tally.saw(reader.pinned().number);
  const AnalyticsInputClass cls = classify_served_answer(b);
  if (cls == AnalyticsInputClass::Refused || cls == AnalyticsInputClass::PriceOnly) {
    c.exact_exclusion = ScenarioExclusion::ExactRefused;
    return;
  }
  c.exact = b.price - a.price;
  c.exact_class = weaker(a.certificate_class, b.certificate_class);
  c.exact_certificate_usd = a.certificate_usd + b.certificate_usd;
  if (cls == AnalyticsInputClass::Estimate) c.exact_exclusion = ScenarioExclusion::Estimate;
}

void exact_cell(const EngineContext &cx, ServeBookReader &reader, const UniqueKey &key,
                const UniqueEval &u, const Shock &s, bool crosses_next, bool crosses_expiry,
                ScenarioLegCell &c, WorkerTally &tally) {
  const ScenarioSpec &spec = cx.spec;
  const ServeAnswer &a = u.base;
  const double S1 = spec.spot * (1.0 + s.x);
  if (crosses_next) {
    c.exact_exclusion = ScenarioExclusion::EventRequiresRefit;
    return;
  }
  if (crosses_expiry) {
    c.exact_route = ScenarioExactRoute::EventSettlement;
    c.exact = payoff(key.side, S1, key.strike) - a.price;
    c.exact_class = a.certificate_class;
    c.exact_certificate_usd = a.certificate_usd;
  } else if (s.y == 0.0 && s.dt == 0.0) {
    exact_record_cell(cx, reader, key, u, s, c, tally);
  } else {
    c.exact_route = ScenarioExactRoute::ColdKernel;
    if (!std::isfinite(u.q_node)) {
      c.exact_exclusion = ScenarioExclusion::OffNode;
      return;
    }
    if (!std::isfinite(u.cold_base)) {
      c.exact_exclusion = ScenarioExclusion::ExactRefused;
      return;
    }
    const double sigma1 = std::max(a.sigma + s.y, kScenarioSigmaFloor);
    const double p = cold_price(cx, key, S1, u.tau - s.dt, sigma1, u, tally);
    if (!std::isfinite(p)) {
      c.exact_exclusion = ScenarioExclusion::ExactRefused;
      return;
    }
    c.exact = p - u.cold_base;
  }
  if (u.cls == AnalyticsInputClass::Estimate && c.exact_exclusion == ScenarioExclusion::None) {
    c.exact_exclusion = ScenarioExclusion::Estimate;
  }
}

void evaluate_unique(const EngineContext &cx, ServeBookReader &reader, const UniqueKey &key,
                     UniqueEval &u, WorkerTally &tally) {
  evaluate_base(cx, reader, key, u, tally);
  const std::size_t n = cx.shocks.size();
  u.cells.assign(n, ScenarioLegCell{});
  ScenarioExclusion leg_out = ScenarioExclusion::None;
  if (u.cls == AnalyticsInputClass::Refused) leg_out = ScenarioExclusion::Refused;
  if (u.cls == AnalyticsInputClass::PriceOnly) leg_out = ScenarioExclusion::PriceOnly;
  for (std::size_t c = 0; c < n; ++c) {
    ScenarioLegCell &cell = u.cells[c];
    if (leg_out != ScenarioExclusion::None) {
      cell.taylor_exclusion = leg_out;
      cell.exact_exclusion = leg_out;
      continue;
    }
    const Shock &s = cx.shocks[c];
    const bool next = crosses_event(s.dt, u.tau, cx.spec.now_ns, u.expiry_ns, u.next_event_ns);
    const bool expiry = s.dt >= u.tau;
    if (cx.taylor) taylor_cell(cx, u, s, next || expiry, cell);
    if (cx.exact) exact_cell(cx, reader, key, u, s, next, expiry, cell, tally);
  }
}

[[nodiscard]] Status validate(std::span<const ServedPosition> portfolio, const ScenarioSpec &s) {
  const auto finite_axis = [](const std::vector<double> &axis) {
    return !axis.empty() &&
           std::all_of(axis.begin(), axis.end(), [](double v) { return std::isfinite(v); });
  };
  if (!finite_axis(s.spot_rel) || !finite_axis(s.vol_abs) || !finite_axis(s.time_years)) {
    return Err(ErrorCode::InvalidArgument, "served_scenarios: an empty or non-finite axis");
  }
  if (std::any_of(s.time_years.begin(), s.time_years.end(), [](double v) { return v < 0.0; }) ||
      std::any_of(s.spot_rel.begin(), s.spot_rel.end(), [](double v) { return !(v > -1.0); })) {
    return Err(ErrorCode::InvalidArgument, "served_scenarios: a negative time or a spot <= 0");
  }
  if (!finite_positive(s.spot) ||
      !(s.box.spot_rel >= 0.0 && s.box.vol_abs >= 0.0 && s.box.time_years >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "served_scenarios: spot or Taylor box");
  }
  if (s.convention != HedgeConvention::ContractSigma &&
      s.convention != HedgeConvention::StickyStrike) {
    return Err(ErrorCode::InvalidArgument,
               "served_scenarios: scenarios hold the served volatility (contract sigma / sticky "
               "strike)");
  }
  for (const ServedPosition &p : portfolio) {
    if (!std::isfinite(p.quantity) || !std::isfinite(p.multiplier) || !std::isfinite(p.strike)) {
      return Err(ErrorCode::InvalidArgument, "served_scenarios: a non-finite position field");
    }
  }
  return Ok();
}

struct Uniques {
  std::vector<UniqueKey> keys;
  std::vector<std::size_t> of_leg; // leg -> unique
};

[[nodiscard]] Uniques dedupe(std::span<const ServedPosition> portfolio) {
  Uniques u;
  std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint8_t>, std::size_t> index;
  u.of_leg.reserve(portfolio.size());
  for (const ServedPosition &p : portfolio) {
    const auto key = std::make_tuple(p.expiry_id, std::bit_cast<std::uint64_t>(p.strike),
                                     static_cast<std::uint8_t>(p.side));
    const auto [it, inserted] = index.try_emplace(key, u.keys.size());
    if (inserted) u.keys.push_back({p.expiry_id, p.strike, p.side});
    u.of_leg.push_back(it->second);
  }
  return u;
}

[[nodiscard]] std::vector<Shock> shocks_of(const std::vector<double> &spot_rel,
                                           const std::vector<double> &vol_abs,
                                           const std::vector<double> &time_years) {
  std::vector<Shock> out;
  out.reserve(spot_rel.size() * vol_abs.size() * time_years.size());
  for (const double x : spot_rel) {
    for (const double y : vol_abs) {
      for (const double dt : time_years) out.push_back({x, y, dt});
    }
  }
  return out;
}

// Runs every unique on `workers` jthreads (contiguous chunks; one reader each). The
// workers write disjoint slots of `evals`; their tallies are merged after the join.
[[nodiscard]] WorkerTally run_uniques(const ServeBook &book, const EngineContext &cx,
                                      std::span<const UniqueKey> keys,
                                      std::vector<UniqueEval> &evals) {
  const std::size_t n = keys.size();
  const std::size_t asked = cx.spec.workers == 0 ? 1 : cx.spec.workers;
  const std::size_t workers =
      std::max<std::size_t>(1, std::min(asked, std::max<std::size_t>(n, 1)));
  std::vector<WorkerTally> tallies(workers);
  std::vector<std::exception_ptr> failures(workers);
  const auto body = [&](std::size_t w) {
    try {
      ServeBookReader reader = book.reader();
      const std::size_t lo = n * w / workers;
      const std::size_t hi = n * (w + 1) / workers;
      for (std::size_t i = lo; i < hi; ++i) {
        evaluate_unique(cx, reader, keys[i], evals[i], tallies[w]);
      }
    } catch (...) {
      failures[w] = std::current_exception(); // rethrown on the calling thread below
    }
  };
  if (workers == 1) {
    body(0);
  } else {
    std::vector<std::jthread> pool;
    pool.reserve(workers);
    for (std::size_t w = 0; w < workers; ++w) pool.emplace_back(body, w);
  } // joined here
  for (const std::exception_ptr &f : failures) {
    if (f) std::rethrow_exception(f);
  }
  WorkerTally total;
  for (const WorkerTally &t : tallies) {
    if (t.base_serves > 0) {
      total.lo = std::min(total.lo, t.lo);
      total.hi = std::max(total.hi, t.hi);
    }
    total.base_serves += t.base_serves;
    total.record_serves += t.record_serves;
    total.cold_solves += t.cold_solves;
  }
  return total;
}

struct CellSums {
  NeumaierSum taylor, delta, gamma, vega, theta, remainder, exact, certificate, est_taylor,
      est_exact;
  std::array<NeumaierSum, kScenarioExclusionCount> taylor_usd{}, exact_usd{};
};

// One leg's contribution to a cell (leg order, on the calling thread).
void accumulate(const ScenarioLegCell &lc, double scale, double premium, ScenarioCell &cell,
                CellSums &sum) {
  const double abs_scale = std::abs(scale);
  // One statement per scaled term, then the sum: no product is contracted into an FMA, so
  // the value equals the attribution's explained (the same four terms, summed alike).
  const double sd = scale * lc.taylor_delta;
  const double sg = scale * lc.taylor_gamma;
  const double sv = scale * lc.taylor_vega;
  const double st = scale * lc.taylor_theta;
  const double value = sd + sg + sv + st;
  const auto tx = static_cast<std::size_t>(lc.taylor_exclusion);
  if (lc.taylor_exclusion == ScenarioExclusion::None) {
    sum.taylor.add(value);
    sum.delta.add(sd);
    sum.gamma.add(sg);
    sum.vega.add(sv);
    sum.theta.add(st);
    sum.remainder.add(abs_scale * lc.remainder);
    ++cell.taylor_legs;
  } else {
    if (lc.taylor_exclusion == ScenarioExclusion::Estimate) sum.est_taylor.add(value);
    ++cell.taylor_excluded[tx];
    if (std::isfinite(premium)) sum.taylor_usd[tx].add(premium);
  }
  const auto ex = static_cast<std::size_t>(lc.exact_exclusion);
  if (lc.exact_exclusion == ScenarioExclusion::None) {
    sum.exact.add(scale * lc.exact);
    if (std::isfinite(lc.exact_certificate_usd)) {
      sum.certificate.add(abs_scale * lc.exact_certificate_usd);
    }
    ++cell.exact_legs;
    ++cell.exact_routes[static_cast<std::size_t>(lc.exact_route)];
  } else {
    if (lc.exact_exclusion == ScenarioExclusion::Estimate) sum.est_exact.add(scale * lc.exact);
    ++cell.exact_excluded[ex];
    if (std::isfinite(premium)) sum.exact_usd[ex].add(premium);
  }
}

void finish(const CellSums &sum, ScenarioCell &cell) {
  cell.taylor_pnl = sum.taylor.value();
  cell.taylor_delta = sum.delta.value();
  cell.taylor_gamma = sum.gamma.value();
  cell.taylor_vega = sum.vega.value();
  cell.taylor_theta = sum.theta.value();
  cell.taylor_remainder = sum.remainder.value();
  cell.exact_pnl = sum.exact.value();
  cell.exact_certificate_usd = sum.certificate.value();
  cell.estimate_taylor_pnl = sum.est_taylor.value();
  cell.estimate_exact_pnl = sum.est_exact.value();
  for (std::size_t k = 0; k < kScenarioExclusionCount; ++k) {
    cell.taylor_excluded_usd[k] = sum.taylor_usd[k].value();
    cell.exact_excluded_usd[k] = sum.exact_usd[k].value();
  }
}

[[nodiscard]] ServedLegProvenance provenance(const ServedPosition &p, std::size_t unique,
                                             const UniqueEval &u) {
  ServedLegProvenance leg;
  leg.leg_id = p.leg_id;
  leg.unique_index = unique;
  leg.base_price = u.base.price;
  leg.cold_base_price = u.cold_base;
  leg.delta = u.base.delta;
  leg.gamma = u.base.gamma;
  leg.vega = u.base.vega;
  leg.theta = u.base.theta;
  for (std::size_t k = 0; k < kHedgeGreekCount; ++k) leg.reason[k] = from_serve(u.base.reason[k]);
  leg.sigma = u.base.sigma;
  leg.edge_sd = u.base.edge_sd;
  leg.certificate_usd = u.base.certificate_usd;
  leg.certificate_class = u.base.certificate_class;
  leg.sampled = u.base.sampled;
  leg.price_only_reason = u.base.price_only_reason;
  leg.serve_status = u.base.status;
  leg.route = u.base.route;
  leg.tier = u.base.tier;
  leg.input_class = u.cls;
  leg.declaration = u.declaration;
  return leg;
}

void fill_workload(const ScenarioSpec &spec, std::span<const ServedPosition> portfolio,
                   const Uniques &uniq, std::span<const UniqueEval> evals, ScenarioResult &out) {
  ServedWorkload &w = out.workload;
  w.contracts = portfolio.size();
  w.unique_contracts = uniq.keys.size();
  w.n_spot = spec.spot_rel.size();
  w.n_vol = spec.vol_abs.size();
  w.n_time = spec.time_years.size();
  w.n_cells = w.n_spot * w.n_vol * w.n_time;
  const double max_dt = *std::max_element(spec.time_years.begin(), spec.time_years.end());
  std::set<std::uint64_t> expiries;
  std::set<std::uint64_t> events;
  for (std::size_t i = 0; i < uniq.keys.size(); ++i) {
    expiries.insert(uniq.keys[i].expiry_id);
    const UniqueEval &u = evals[i];
    if (!u.has_record) continue;
    if (max_dt >= u.tau ||
        crosses_event(max_dt, u.tau, spec.now_ns, u.expiry_ns, u.next_event_ns)) {
      events.insert(uniq.keys[i].expiry_id);
    }
  }
  w.expiries = expiries.size();
  w.events_in_horizon = events.size();
}

void assemble_cells(std::span<const ServedPosition> portfolio, const Uniques &uniq,
                    std::span<const UniqueEval> evals, std::span<const Shock> shocks,
                    ScenarioResult &out) {
  out.cell.assign(shocks.size(), ScenarioCell{});
  for (std::size_t c = 0; c < shocks.size(); ++c) {
    ScenarioCell &cell = out.cell[c];
    cell.spot_rel = shocks[c].x;
    cell.vol_abs = shocks[c].y;
    cell.time_years = shocks[c].dt;
    CellSums sum;
    for (std::size_t l = 0; l < portfolio.size(); ++l) {
      const ServedPosition &p = portfolio[l];
      const UniqueEval &u = evals[uniq.of_leg[l]];
      const double scale = p.quantity * p.multiplier;
      accumulate(u.cells[c], scale, std::abs(scale) * u.base.price, cell, sum);
    }
    finish(sum, cell);
  }
}

} // namespace

// ── Classification and hedge Greeks ─────────────────────────────────────────────────────

AnalyticsInputClass classify_served_answer(const ServeAnswer &answer) noexcept {
  return classify(answer.status, answer.sampled, answer.certificate_class, answer.price);
}

AnalyticsInputClass classify_served_statement(const ServedChainStatement &statement,
                                              double price) noexcept {
  return classify(statement.status, statement.sampled, statement.certificate_class, price);
}

HedgeGreeks hedge_greeks(ServeBookReader &reader, const HedgeGreeksRequest &request) {
  HedgeGreeks h;
  h.declaration.expiry_id = request.expiry_id;
  h.declaration.convention = request.convention;
  h.declaration.clock.now_ns = request.now_ns;
  h.reason.fill(HedgeGreekReason::Refused);
  if (!finite_positive(request.strike) || !finite_positive(request.spot)) {
    h.status = AnalyticsStatus::InvalidArgument;
    return h;
  }
  const ServeAnswer a =
      reader.serve(request.expiry_id, served_request(request.strike, request.side, request.spot,
                                                     request.now_ns, request.tier,
                                                     request.tier_max));
  h.price = a.price;
  h.delta = a.delta;
  h.gamma = a.gamma;
  h.vega = a.vega;
  h.theta = a.theta;
  for (std::size_t k = 0; k < kHedgeGreekCount; ++k) h.reason[k] = from_serve(a.reason[k]);
  h.sigma = a.sigma;
  h.certificate_usd = a.certificate_usd;
  h.certificate_class = a.certificate_class;
  h.sampled = a.sampled;
  h.price_only_reason = a.price_only_reason;
  h.iv_lower = a.iv_lower;
  h.iv_upper = a.iv_upper;
  h.edge_sd = a.edge_sd;
  h.near_edge = a.near_edge;
  h.serve_status = a.status;
  h.route = a.route;
  h.tier = a.tier;
  const ServeBookVersion &version = reader.pinned();
  h.declaration = declare(version, request.expiry_id, request.now_ns, request.convention, a);
  h.input_class = classify_served_answer(a);
  h.status = status_of(h.input_class);
  const ServeBookEntry *e = version.find(request.expiry_id);
  apply_convention(h, e != nullptr ? e->record.get() : nullptr, request.strike, request.spot);
  return h;
}

// ── Aggregation ─────────────────────────────────────────────────────────────────────────

void NeumaierSum::add(double value) noexcept {
  const double t = sum_ + value;
  if (std::abs(sum_) >= std::abs(value)) {
    compensation_ += (sum_ - t) + value;
  } else {
    compensation_ += (value - t) + sum_;
  }
  sum_ = t;
}

// ── Scenarios ───────────────────────────────────────────────────────────────────────────

Result<ScenarioResult> served_scenarios(const ServeBook &book,
                                        std::span<const ServedPosition> portfolio,
                                        const ScenarioSpec &spec) {
  if (Status ok = validate(portfolio, spec); !ok) return Err(ok.error());
  const std::vector<Shock> shocks = shocks_of(spec.spot_rel, spec.vol_abs, spec.time_years);
  const auto nonzero = [](const std::vector<double> &axis) {
    return std::any_of(axis.begin(), axis.end(), [](double v) { return v != 0.0; });
  };
  EngineContext cx{spec, shocks};
  cx.taylor = spec.method != ScenarioMethod::Exact;
  cx.exact = spec.method != ScenarioMethod::Taylor;
  cx.any_spot = nonzero(spec.spot_rel);
  cx.any_vol = nonzero(spec.vol_abs);
  cx.any_time = nonzero(spec.time_years);
  const Uniques uniq = dedupe(portfolio);
  ScenarioResult out;
  out.box = spec.box;
  out.convention = spec.convention;
  out.n_spot = spec.spot_rel.size();
  out.n_vol = spec.vol_abs.size();
  out.n_time = spec.time_years.size();
  std::vector<UniqueEval> evals;
  WorkerTally tally;
  // ONE version for every leg: a publication mid-run runs the whole matrix again once.
  for (std::uint32_t attempt = 1; attempt <= 2; ++attempt) {
    evals.assign(uniq.keys.size(), UniqueEval{});
    tally = run_uniques(book, cx, uniq.keys, evals);
    out.attempts = attempt;
    if (uniq.keys.empty() || tally.lo == tally.hi) break;
  }
  fill_workload(spec, portfolio, uniq, evals, out);
  out.base_serves = tally.base_serves;
  out.exact_record_serves = tally.record_serves;
  out.cold_solves = tally.cold_solves;
  if (!uniq.keys.empty() && tally.lo != tally.hi) {
    out.status = AnalyticsStatus::VersionMoved;
    return Ok(std::move(out));
  }
  out.book_version = uniq.keys.empty() ? book.version() : tally.lo;
  out.legs.reserve(portfolio.size());
  for (std::size_t l = 0; l < portfolio.size(); ++l) {
    out.legs.push_back(provenance(portfolio[l], uniq.of_leg[l], evals[uniq.of_leg[l]]));
  }
  assemble_cells(portfolio, uniq, evals, shocks, out);
  if (spec.keep_legs) {
    out.cells.reserve(evals.size() * shocks.size());
    for (const UniqueEval &u : evals) {
      out.cells.insert(out.cells.end(), u.cells.begin(), u.cells.end());
    }
  }
  return Ok(std::move(out));
}

// ── Attribution ─────────────────────────────────────────────────────────────────────────

Result<ServedAttribution> served_attribution(const ServeBook &book,
                                             std::span<const ServedPosition> portfolio,
                                             const ServedMove &move, const ScenarioSpec &spec) {
  ScenarioSpec one = spec;
  one.spot_rel = {move.spot_rel};
  one.vol_abs = {move.vol_abs};
  one.time_years = {move.time_years};
  one.method = ScenarioMethod::Both;
  one.keep_legs = true;
  Result<ScenarioResult> s = served_scenarios(book, portfolio, one);
  if (!s) return Err(s.error());
  ServedAttribution out;
  out.status = s->status;
  out.move = move;
  if (s->status != AnalyticsStatus::Ok) {
    out.scenario = std::move(*s);
    return Ok(std::move(out));
  }
  NeumaierSum delta, gamma, vega, theta, explained, actual, unexplained;
  out.legs.reserve(portfolio.size());
  for (std::size_t l = 0; l < portfolio.size(); ++l) {
    const ServedPosition &p = portfolio[l];
    const ScenarioLegCell &lc = s->cells[s->legs[l].unique_index];
    const double scale = p.quantity * p.multiplier;
    ServedLegAttribution leg;
    leg.leg_id = p.leg_id;
    leg.route = lc.exact_route;
    leg.taylor_exclusion = lc.taylor_exclusion;
    leg.exact_exclusion = lc.exact_exclusion;
    if (lc.taylor_exclusion == ScenarioExclusion::None &&
        lc.exact_exclusion == ScenarioExclusion::None) {
      leg.delta = scale * lc.taylor_delta;
      leg.gamma = scale * lc.taylor_gamma;
      leg.vega = scale * lc.taylor_vega;
      leg.theta = scale * lc.taylor_theta;
      leg.explained = leg.delta + leg.gamma + leg.vega + leg.theta;
      leg.actual = scale * lc.exact;
      leg.unexplained = leg.actual - leg.explained;
      delta.add(leg.delta);
      gamma.add(leg.gamma);
      vega.add(leg.vega);
      theta.add(leg.theta);
      explained.add(leg.explained);
      actual.add(leg.actual);
      unexplained.add(leg.unexplained);
      ++out.attributed_legs;
    }
    out.legs.push_back(leg);
  }
  out.delta = delta.value();
  out.gamma = gamma.value();
  out.vega = vega.value();
  out.theta = theta.value();
  out.explained = explained.value();
  out.actual = actual.value();
  out.unexplained = unexplained.value();
  out.cell = s->cell.front();
  out.scenario = std::move(*s);
  return Ok(std::move(out));
}

// ── Dated cash dividends (D1) ───────────────────────────────────────────────────────────

namespace {

struct CashRoll {
  int events = 0;
  double cash = 0.0;
  std::vector<CashDividend> rest; // later ex-dates, dt closer
};

[[nodiscard]] CashRoll roll_schedule(std::span<const CashDividend> dividends, double T,
                                     double dt) {
  CashRoll r;
  for (const CashDividend &d : dividends) {
    if (!(d.tau > 0.0) || d.tau > T * (1.0 + kTauAtExpiryRelTol)) continue;
    if (d.tau <= dt) {
      ++r.events;
      r.cash += d.amount;
    } else {
      r.rest.push_back({d.tau - dt, d.amount});
    }
  }
  return r;
}

[[nodiscard]] double cash_price(const CashDividendContract &c, double S, double T, double sigma,
                                std::span<const CashDividend> divs,
                                const DiscreteDivProductionOpts &opts, DiscreteDivWorkspace &ws,
                                std::size_t &solves) {
  ++solves;
  const Result<DiscreteDivProductionPrice> p = american_discrete_div_production_price(
      S, c.strike, T, sigma, c.rate, c.yield, c.side, divs, c.exercise, opts, ws);
  return p ? p->price : kNaN;
}

struct CashBase {
  double gamma_var = kNaN;
  double vol_edge = kNaN;
  double time_edge = kNaN;
  double time_h = 0.0; // the measured time edge (<= box), short of the first ex-date
};

[[nodiscard]] CashBase cash_remainders(const CashDividendContract &c,
                                       const DiscreteDivProductionGreeks &g,
                                       const CashDividendScenarioSpec &spec,
                                       DiscreteDivWorkspace &ws, std::size_t &solves) {
  CashBase b;
  const TaylorBox &box = spec.box;
  if (g.supported(DiscreteDivGreek::Gamma)) {
    double worst = 0.0;
    bool ok = true;
    for (const double sgn : {-1.0, 1.0}) {
      ++solves;
      const Result<DiscreteDivProductionGreeks> e = american_discrete_div_production_greeks(
          c.spot * (1.0 + sgn * box.spot_rel), c.strike, c.tau, c.sigma, c.rate, c.yield, c.side,
          c.dividends, c.exercise, {}, 0.0, spec.opts, ws);
      ok = ok && e && e->supported(DiscreteDivGreek::Gamma);
      if (ok) worst = std::max(worst, std::abs(e->gamma - g.gamma));
    }
    b.gamma_var = ok ? worst : kNaN;
  }
  if (g.supported(DiscreteDivGreek::Vega) && box.vol_abs > 0.0) {
    double worst = 0.0;
    bool ok = true;
    for (const double sgn : {-1.0, 1.0}) {
      const double s1 = std::max(c.sigma + sgn * box.vol_abs, kScenarioSigmaFloor);
      const double p = cash_price(c, c.spot, c.tau, s1, c.dividends, spec.opts, ws, solves);
      ok = ok && std::isfinite(p);
      if (ok) worst = std::max(worst, std::abs(p - g.price - g.vega * (s1 - c.sigma)));
    }
    b.vol_edge = ok ? worst : kNaN;
  }
  double first_ex = std::numeric_limits<double>::infinity();
  for (const CashDividend &d : c.dividends) {
    if (d.tau > 0.0 && d.tau <= c.tau) first_ex = std::min(first_ex, d.tau);
  }
  b.time_h = std::min(box.time_years, 0.5 * first_ex);
  if (g.supported(DiscreteDivGreek::Theta) && b.time_h > 0.0 && b.time_h < c.tau) {
    const CashRoll r = roll_schedule(c.dividends, c.tau, b.time_h);
    const double p =
        cash_price(c, c.spot, c.tau - b.time_h, c.sigma, r.rest, spec.opts, ws, solves);
    if (std::isfinite(p)) b.time_edge = std::abs(p - g.price - g.theta * b.time_h);
  }
  return b;
}

[[nodiscard]] ScenarioExclusion cash_taylor_gate(const DiscreteDivProductionGreeks &g,
                                                 const CashBase &b, const TaylorBox &box,
                                                 const Shock &s, bool event) noexcept {
  const bool spot = s.x != 0.0;
  const bool vol = s.y != 0.0;
  const bool time = s.dt != 0.0;
  if (event) return ScenarioExclusion::EventOperation;
  if (std::abs(s.x) > box.spot_rel || std::abs(s.y) > box.vol_abs || s.dt > box.time_years) {
    return ScenarioExclusion::OutsideTaylorBox;
  }
  if ((spot && !(g.supported(DiscreteDivGreek::Delta) && g.supported(DiscreteDivGreek::Gamma))) ||
      (vol && !g.supported(DiscreteDivGreek::Vega)) ||
      (time && !g.supported(DiscreteDivGreek::Theta))) {
    return ScenarioExclusion::MissingGreek;
  }
  if ((spot && !std::isfinite(b.gamma_var)) || (vol && !std::isfinite(b.vol_edge)) ||
      (time && (!std::isfinite(b.time_edge) || s.dt > b.time_h))) {
    return ScenarioExclusion::RemainderUnbounded;
  }
  return ScenarioExclusion::None;
}

void cash_cell(const CashDividendContract &c, const DiscreteDivProductionGreeks &g,
               const CashBase &b, const CashDividendScenarioSpec &spec, const Shock &s,
               DiscreteDivWorkspace &ws, std::size_t &solves, CashDividendLegCell &cell) {
  const TaylorBox &box = spec.box;
  const CashRoll roll = roll_schedule(c.dividends, c.tau, s.dt);
  cell.events_crossed = roll.events;
  cell.cash_crossed = roll.cash;
  const bool expiry = s.dt >= c.tau;
  // Taylor: never across an event.
  cell.taylor_exclusion = cash_taylor_gate(g, b, box, s, roll.events > 0 || expiry);
  if (cell.taylor_exclusion == ScenarioExclusion::None) {
    const double dS = c.spot * s.x;
    const double td = s.x != 0.0 ? g.delta * dS : 0.0;
    const double tg = s.x != 0.0 ? 0.5 * g.gamma * dS * dS : 0.0;
    const double tv = s.y != 0.0 ? g.vega * s.y : 0.0;
    const double tt = s.dt != 0.0 ? g.theta * s.dt : 0.0;
    cell.taylor = td + tg + tv + tt;
    const double ry = s.y != 0.0 ? s.y / box.vol_abs : 0.0;
    const double rt = s.dt != 0.0 ? s.dt / b.time_h : 0.0;
    cell.remainder = (s.x != 0.0 ? 0.5 * dS * dS * b.gamma_var : 0.0) +
                     (s.y != 0.0 ? b.vol_edge * ry * ry : 0.0) +
                     (s.dt != 0.0 ? b.time_edge * rt * rt : 0.0);
  }
  // Exact: D1's event roll - the crossed cash comes off the (shocked) stock and off the
  // schedule, later ex-dates move closer; through expiry, the payoff.
  const double S1 = c.spot * (1.0 + s.x) - roll.cash;
  if (!(S1 > 0.0)) {
    cell.exact_exclusion = ScenarioExclusion::ExactRefused;
    return;
  }
  if (expiry) {
    cell.exact_route = ScenarioExactRoute::EventSettlement;
    cell.exact = payoff(c.side, S1, c.strike) - g.price;
    return;
  }
  cell.exact_route = ScenarioExactRoute::DiscreteDividend;
  const double sigma1 = std::max(c.sigma + s.y, kScenarioSigmaFloor);
  const double p = cash_price(c, S1, c.tau - s.dt, sigma1, roll.rest, spec.opts, ws, solves);
  if (!std::isfinite(p)) {
    cell.exact_exclusion = ScenarioExclusion::ExactRefused;
    return;
  }
  cell.exact = p - g.price;
}

[[nodiscard]] CashDividendLeg cash_leg(const CashDividendContract &c,
                                       const CashDividendScenarioSpec &spec,
                                       std::span<const Shock> shocks, DiscreteDivWorkspace &ws,
                                       std::size_t &solves) {
  CashDividendLeg leg;
  leg.leg_id = c.leg_id;
  leg.declaration.model = AnalyticsModel::DiscreteDividend;
  leg.declaration.convention = HedgeConvention::ContractSigma;
  leg.declaration.clock.tau = c.tau;
  leg.declaration.price_map_delta = 0.0;
  leg.cells.assign(shocks.size(), CashDividendLegCell{});
  ++solves;
  const Result<DiscreteDivProductionGreeks> g = american_discrete_div_production_greeks(
      c.spot, c.strike, c.tau, c.sigma, c.rate, c.yield, c.side, c.dividends, c.exercise, {}, 0.0,
      spec.opts, ws);
  if (!g) {
    for (CashDividendLegCell &cell : leg.cells) {
      cell.taylor_exclusion = ScenarioExclusion::Refused;
      cell.exact_exclusion = ScenarioExclusion::Refused;
    }
    leg.base.price = kNaN;
    return leg;
  }
  leg.base = *g;
  const CashBase b = cash_remainders(c, *g, spec, ws, solves);
  for (std::size_t k = 0; k < shocks.size(); ++k) {
    cash_cell(c, *g, b, spec, shocks[k], ws, solves, leg.cells[k]);
  }
  return leg;
}

} // namespace

Result<CashDividendScenarioResult>
cash_dividend_scenarios(std::span<const CashDividendContract> contracts,
                        const CashDividendScenarioSpec &spec) {
  ScenarioSpec axes;
  axes.spot_rel = spec.spot_rel;
  axes.vol_abs = spec.vol_abs;
  axes.time_years = spec.time_years;
  axes.spot = 1.0;
  axes.box = spec.box;
  if (Status ok = validate({}, axes); !ok) return Err(ok.error());
  for (const CashDividendContract &c : contracts) {
    if (!finite_positive(c.spot) || !finite_positive(c.strike) || !finite_positive(c.tau) ||
        !finite_positive(c.sigma) || !std::isfinite(c.quantity) || !std::isfinite(c.multiplier)) {
      return Err(ErrorCode::InvalidArgument, "cash_dividend_scenarios: a contract field");
    }
  }
  const std::vector<Shock> shocks = shocks_of(spec.spot_rel, spec.vol_abs, spec.time_years);
  CashDividendScenarioResult out;
  out.n_spot = spec.spot_rel.size();
  out.n_vol = spec.vol_abs.size();
  out.n_time = spec.time_years.size();
  out.box = spec.box;
  DiscreteDivWorkspace ws(kDiscreteDivMaxRouteSteps, kDiscreteDivMaxPreSteps);
  std::size_t solves = 0;
  out.legs.reserve(contracts.size());
  for (const CashDividendContract &c : contracts) {
    out.legs.push_back(cash_leg(c, spec, shocks, ws, solves));
  }
  out.lattice_solves = solves;
  out.cell.assign(shocks.size(), ScenarioCell{});
  for (std::size_t k = 0; k < shocks.size(); ++k) {
    ScenarioCell &cell = out.cell[k];
    cell.spot_rel = shocks[k].x;
    cell.vol_abs = shocks[k].y;
    cell.time_years = shocks[k].dt;
    CellSums sum;
    for (std::size_t l = 0; l < contracts.size(); ++l) {
      const CashDividendLegCell &cc = out.legs[l].cells[k];
      ScenarioLegCell lc;
      // The cash route does not split the Taylor terms: the whole value rides on one.
      lc.taylor_delta = cc.taylor;
      lc.taylor_gamma = 0.0;
      lc.taylor_vega = 0.0;
      lc.taylor_theta = 0.0;
      lc.remainder = cc.remainder;
      lc.exact = cc.exact;
      lc.exact_route = cc.exact_route;
      lc.taylor_exclusion = cc.taylor_exclusion;
      lc.exact_exclusion = cc.exact_exclusion;
      const double scale = contracts[l].quantity * contracts[l].multiplier;
      accumulate(lc, scale, std::abs(scale) * out.legs[l].base.price, cell, sum);
    }
    finish(sum, cell);
    cell.taylor_delta = 0.0; // not split on the cash route (see above)
  }
  return Ok(std::move(out));
}

// ── Names ───────────────────────────────────────────────────────────────────────────────

const char *to_string(AnalyticsModel value) noexcept {
  switch (value) {
  case AnalyticsModel::None: return "none";
  case AnalyticsModel::QuotingRecord: return "quoting_record";
  case AnalyticsModel::ColdKernel: return "cold_kernel";
  case AnalyticsModel::EventSettlement: return "event_settlement";
  case AnalyticsModel::DiscreteDividend: return "discrete_dividend";
  case AnalyticsModel::SessionCurve: return "session_curve";
  case AnalyticsModel::CommonModelEuropean: return "common_model_european";
  }
  return "unknown";
}

const char *to_string(HedgeConvention value) noexcept {
  switch (value) {
  case HedgeConvention::ContractSigma: return "contract_sigma";
  case HedgeConvention::StickyStrike: return "sticky_strike";
  case HedgeConvention::StickyDeltaMoneyness: return "sticky_delta_moneyness";
  case HedgeConvention::RefittedSurface: return "refitted_surface";
  }
  return "unknown";
}

const char *to_string(AnalyticsInputClass value) noexcept {
  switch (value) {
  case AnalyticsInputClass::Certified: return "certified";
  case AnalyticsInputClass::Estimate: return "estimate";
  case AnalyticsInputClass::PriceOnly: return "price_only";
  case AnalyticsInputClass::Refused: return "refused";
  }
  return "unknown";
}

const char *to_string(AnalyticsStatus value) noexcept {
  switch (value) {
  case AnalyticsStatus::Ok: return "ok";
  case AnalyticsStatus::Estimate: return "estimate";
  case AnalyticsStatus::PriceOnly: return "price_only";
  case AnalyticsStatus::Refused: return "refused";
  case AnalyticsStatus::VersionMoved: return "version_moved";
  case AnalyticsStatus::InvalidArgument: return "invalid_argument";
  }
  return "unknown";
}

const char *to_string(HedgeGreekReason value) noexcept {
  switch (value) {
  case HedgeGreekReason::NotRequested: return "not_requested";
  case HedgeGreekReason::Served: return "served";
  case HedgeGreekReason::Exercised: return "exercised";
  case HedgeGreekReason::Refused: return "refused";
  case HedgeGreekReason::NoCarryBumps: return "no_carry_bumps";
  case HedgeGreekReason::CarryBumpRefused: return "carry_bump_refused";
  case HedgeGreekReason::NoQAxis: return "no_q_axis";
  case HedgeGreekReason::NearNodeDegenerate: return "near_node_degenerate";
  case HedgeGreekReason::ConventionUnsupported: return "convention_unsupported";
  case HedgeGreekReason::NearExerciseEdge: return "near_exercise_edge";
  case HedgeGreekReason::BundleUnavailable: return "bundle_unavailable";
  case HedgeGreekReason::TierHasNoGreek: return "tier_has_no_greek";
  case HedgeGreekReason::NoMap: return "no_map";
  case HedgeGreekReason::SmileSlopeUnavailable: return "smile_slope_unavailable";
  case HedgeGreekReason::NeedsVannaVolga: return "needs_vanna_volga";
  case HedgeGreekReason::RequiresRefit: return "requires_refit";
  }
  return "unknown";
}

const char *to_string(ScenarioExactRoute value) noexcept {
  switch (value) {
  case ScenarioExactRoute::None: return "none";
  case ScenarioExactRoute::RecordTier: return "record_tier";
  case ScenarioExactRoute::ColdKernel: return "cold_kernel";
  case ScenarioExactRoute::EventSettlement: return "event_settlement";
  case ScenarioExactRoute::DiscreteDividend: return "discrete_dividend";
  }
  return "unknown";
}

const char *to_string(ScenarioExclusion value) noexcept {
  switch (value) {
  case ScenarioExclusion::None: return "none";
  case ScenarioExclusion::Refused: return "refused";
  case ScenarioExclusion::PriceOnly: return "price_only";
  case ScenarioExclusion::Estimate: return "estimate";
  case ScenarioExclusion::MissingGreek: return "missing_greek";
  case ScenarioExclusion::OutsideTaylorBox: return "outside_taylor_box";
  case ScenarioExclusion::RemainderUnbounded: return "remainder_unbounded";
  case ScenarioExclusion::EventOperation: return "event_operation";
  case ScenarioExclusion::EventRequiresRefit: return "event_requires_refit";
  case ScenarioExclusion::ExactRefused: return "exact_refused";
  case ScenarioExclusion::OffNode: return "off_node";
  }
  return "unknown";
}

} // namespace atx::vol
