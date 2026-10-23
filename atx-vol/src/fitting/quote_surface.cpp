// Graded admission over one valuation surface (see quote_slice.hpp contract).
//
// Pass A runs the profile's own fit (fit_quote_slice) on every expiry; a served
// pass-A fit with enough own point evidence is the "pass-A tier". Other expiries
// are refitted on the channel objective under a term prior: stage B1 uses the
// pass-A tier as prior nodes (or the flat default shape when there are none),
// stage B2 rebuilds a leave-one-out prior from every fitted expiry (pass-A and
// B1 channel fits) and refits. Final American prices are then accepted greedily
// in confidence order under the unchanged calendar check; a slice that crosses
// an accepted one gets a bounded-move repair, then one stronger-prior retry
// built from the accepted slices only, and is otherwise refused. A pass-A slice
// is graded A only when no repair moved its served prices, B otherwise. Every
// acceptance keeps the AmericanPriceSlice certificate and every accepted pair
// passes check_american_calendar. Work is bounded: at most three channel fits
// per expiry and O(expiries^2) calendar checks. The opt-in channel repair
// (config.repair) adds fallback rungs (repair_rung, repair_in_channel): at most
// one more pass-A fit and two channel calendar repairs per expiry. The opt-in
// dominance gate (config.channel.point_max_outside_fraction) runs last
// (dominance_pass): at most one more channel fit and its calendar repairs per
// gated expiry, adopted only when consistent with every other served expiry.
// The opt-in knot-first ladder (surface.ladder) replaces pass A, the repair
// rung and the calendar pass (knot_pass, ladder_repair_rung,
// ladder_calendar_pass): two pass-A fits per expiry, at most two repair-rung
// fits, one retry with its prior per crossing expiry, O(expiries^2) calendar
// checks; every rung's result must be inside the market (in_market).
// Only the per-expiry stages (pass A or knot_pass, B1, B2) fan out over
// surface.fit_workers (for_each_expiry). Priors, the calendar passes, the
// repair rungs and the dominance gate stay serial, so output is bit-identical
// for any worker count. The opt-in rung memo (surface.rung_memo, rung_memo.hpp)
// gives every expiry slot one memo that all of that expiry's fits share, on
// whichever thread they run; it changes speed only.
#include "atx/vol/api/fitting/quote_slice.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/vol/api/core/types.hpp" // kIvMin (ladder wing tail)
#include "core/parallel_for.hpp"         // parallel_for_dynamic (per-expiry fan-out)
#include "fitting/counters.hpp"
#include "fitting/expiry_schedule.hpp" // longest_first, note_expiry_dispatch
#include "fitting/quote_slice_nodes.hpp" // floor_vol, mark_price_only (lane C0)
#include "fitting/rung_memo.hpp"       // ExpiryRungMemo, ScopedRungMemo
#include "fitting/term_prior.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] bool positive(double value) noexcept { return std::isfinite(value) && value > 0.0; }

[[nodiscard]] Status validate_surface(const QuoteSurfaceConfig &s) {
  if (!std::isfinite(s.term_prior_weight) || s.term_prior_weight < 0.0 ||
      !positive(s.prior_decay_log_time) || !positive(s.prior_quote_halflife) ||
      !positive(s.channel_node_weight) || !std::isfinite(s.calendar_retry_factor) ||
      s.calendar_retry_factor < 1.0 || !std::isfinite(s.calendar_repair_half_spreads) ||
      s.calendar_repair_half_spreads < 0.0 ||
      (s.ladder != QuoteSurfaceLadder::Off && s.ladder != QuoteSurfaceLadder::KnotFirst) ||
      !(s.ladder_max_outside_fraction >= 0.0 && s.ladder_max_outside_fraction <= 1.0) ||
      !(s.ladder_channel_max_outside_fraction >= 0.0 &&
        s.ladder_channel_max_outside_fraction <= 1.0) ||
      s.ladder_min_outside_quotes < 1 ||
      (s.ladder_wing_tail != QuoteSurfaceWingTail::Off &&
       s.ladder_wing_tail != QuoteSurfaceWingTail::FloorPriceOnly &&
       s.ladder_wing_tail != QuoteSurfaceWingTail::FloorPriceOnlyWingFloor) ||
      (s.ladder_wing_tail != QuoteSurfaceWingTail::Off &&
       s.ladder != QuoteSurfaceLadder::KnotFirst)) {
    return Err(ErrorCode::InvalidArgument, "quote surface: invalid graded-admission configuration");
  }
  return Ok();
}

struct OutsideShare {
  double fraction = 0.0;
  std::size_t count = 0;
};

// Share and count of the strictly two-sided quotes whose fitted price lies
// outside [bid, ask]; quotes without a fitted price are not counted. Success
// rows always count; PriceOnly rows (a certified price without a volatility)
// count only with `with_price_only` (the ladder's containment rule; the
// dominance gate keeps its historical Success-only measure).
[[nodiscard]] OutsideShare outside_share(const QuoteSliceFit &fit,
                                         std::span<const QuoteSliceQuote> quotes,
                                         bool with_price_only = false) {
  std::size_t two_sided = 0;
  std::size_t outside = 0;
  for (std::size_t i = 0; i < quotes.size() && i < fit.rows.size(); ++i) {
    const QuoteSliceQuote &quote = quotes[i];
    const QuoteSliceRow &row = fit.rows[i];
    const bool priced = row.evaluation_status == QuoteSliceEvaluationStatus::Success ||
                        (with_price_only &&
                         row.evaluation_status == QuoteSliceEvaluationStatus::PriceOnly);
    if (!positive(quote.bid) || !std::isfinite(quote.ask) || !(quote.ask > quote.bid) ||
        !priced) {
      continue;
    }
    ++two_sided;
    if (row.fitted_price < quote.bid || row.fitted_price > quote.ask) ++outside;
  }
  if (two_sided == 0) return {};
  return {static_cast<double>(outside) / static_cast<double>(two_sided), outside};
}

[[nodiscard]] bool two_sided(const QuoteSliceQuote &quote) noexcept {
  return positive(quote.strike) && positive(quote.bid) && std::isfinite(quote.ask) &&
         quote.ask > quote.bid;
}

// Raw two-sided breadth: distinct strikes carrying a strictly two-sided quote.
[[nodiscard]] std::size_t two_sided_strikes(std::span<const QuoteSliceQuote> quotes) {
  std::vector<double> strikes;
  strikes.reserve(quotes.size());
  for (const QuoteSliceQuote &quote : quotes) {
    if (two_sided(quote)) strikes.push_back(quote.strike);
  }
  std::sort(strikes.begin(), strikes.end());
  return static_cast<std::size_t>(std::unique(strikes.begin(), strikes.end()) - strikes.begin());
}

// Raw two-sided evidence: strictly two-sided quotes (both sides of a strike count).
[[nodiscard]] std::size_t two_sided_quotes(std::span<const QuoteSliceQuote> quotes) {
  return static_cast<std::size_t>(std::count_if(quotes.begin(), quotes.end(), two_sided));
}

// ATM level and shape of a fitted curve on kTermPriorGrid. Points where the curve
// is undefined (e.g. outside a dense curve's band) are filled flat outward from
// the money; multiples are clamped to the channel's served multiple range.
[[nodiscard]] std::optional<TermPriorNode> node_from_fit(const QuoteSliceFit &fit, double weight,
                                                         std::uint64_t id,
                                                         const QuoteSliceChannelConfig &channel) {
  if (!fit.curve || !positive(weight)) {
    return std::nullopt;
  }
  const double years = fit.market.variance_years;
  const double atm = fit.curve->iv(0.0);
  if (!positive(atm) || !positive(years)) {
    return std::nullopt;
  }
  TermPriorNode node;
  node.variance_years = years;
  node.atm_vol = atm;
  node.weight = weight;
  node.id = id;
  std::array<bool, kTermPriorGridSize> valid{};
  const double axis = atm * std::sqrt(years);
  for (std::size_t g = 0; g < kTermPriorGridSize; ++g) {
    const double sigma = fit.curve->iv(kTermPriorGrid[g] * axis);
    if (positive(sigma)) {
      node.multiples[g] = std::clamp(sigma / atm, channel.mult_floor, channel.mult_ceil);
      valid[g] = true;
    }
  }
  const std::size_t centre = kTermPriorGridSize / 2; // z = 0
  node.multiples[centre] = 1.0;
  valid[centre] = true;
  for (std::size_t g = centre + 1; g < kTermPriorGridSize; ++g) {
    if (!valid[g]) node.multiples[g] = node.multiples[g - 1];
  }
  for (std::size_t g = centre; g-- > 0;) {
    if (!valid[g]) node.multiples[g] = node.multiples[g + 1];
  }
  return node;
}

// Keeps one node per variance time (the most reliable) so TermPrior::create sees
// distinct times even when two calendar expiries share a variance clock value.
[[nodiscard]] Result<TermPrior> make_prior(std::vector<TermPriorNode> nodes,
                                           const QuoteSurfaceConfig &surface) {
  std::sort(nodes.begin(), nodes.end(), [](const TermPriorNode &a, const TermPriorNode &b) {
    return a.variance_years < b.variance_years ||
           (a.variance_years == b.variance_years && a.weight > b.weight);
  });
  nodes.erase(std::unique(nodes.begin(), nodes.end(),
                          [](const TermPriorNode &a, const TermPriorNode &b) {
                            return a.variance_years == b.variance_years;
                          }),
              nodes.end());
  return TermPrior::create(nodes, TermPriorConfig{surface.prior_decay_log_time});
}

// Pass A never runs the channel repair: it is a fallback rung (repair_rung()).
[[nodiscard]] QuoteSliceConfig without_repair(const QuoteSliceConfig &config) {
  QuoteSliceConfig out = config;
  out.repair.mode = QuoteSliceRepairMode::Off;
  return out;
}

// The surface's own copy of the shared configuration: the ladder wing tail
// FloorPriceOnlyWingFloor turns the knot wing floor on for every channel fit.
[[nodiscard]] QuoteSliceConfig surface_config(const QuoteSliceConfig &config,
                                              const QuoteSurfaceConfig &surface) {
  QuoteSliceConfig out = config;
  if (surface.ladder_wing_tail == QuoteSurfaceWingTail::FloorPriceOnlyWingFloor) {
    out.channel.wing_floor = true;
  }
  return out;
}

class SurfaceFitter {
public:
  SurfaceFitter(std::span<const QuoteSurfaceExpiry> expiries, const QuoteSliceConfig &config,
                const QuoteSurfaceConfig &surface)
      : expiries_(expiries), config_(surface_config(config, surface)),
        base_config_(without_repair(config_)),
        surface_(surface), out_(expiries.size()), breadth_(expiries.size()),
        two_sided_(expiries.size()), contained_(expiries.size(), 0),
        held_point_(expiries.size()), cost_(expiries.size()),
        trace_(counters::fit_timing::armed_surface()),
        timing_(trace_ != nullptr ? expiries.size() : 0),
        memos_(surface.rung_memo ? expiries.size() : 0) {
    for (std::size_t i = 0; i < expiries.size(); ++i) {
      cost_[i] = two_sided_quotes(expiries[i].quotes);
    }
  }

  [[nodiscard]] Result<std::vector<QuoteSurfaceSlice>> run() {
    if (ladder()) {
      knot_pass();
    } else {
      point_pass();
    }
    ATX_TRY_VOID(channel_passes());
    if (ladder()) {
      ladder_repair_rung();
      ATX_TRY_VOID(ladder_calendar_pass());
      market_distance_pass();
      measure_containment();
      if (surface_.ladder_wing_tail != QuoteSurfaceWingTail::Off) {
        price_only_floor_vols();
      }
    } else {
      repair_rung();
      ATX_TRY_VOID(calendar_pass());
      ATX_TRY_VOID(dominance_pass());
      market_distance_pass();
    }
    for (QuoteSurfaceSlice &slice : out_) {
      if (slice.grade == QuoteSliceGrade::Refused) {
        strip(slice.fit);
      } else if (slice.grade == QuoteSliceGrade::A && !slice.fit.moved_nodes.empty()) {
        // Pass-A tier, but a repair moved its served prices off its fit. The
        // tier (prior node role, acceptance order) is unchanged; only the label.
        slice.grade = QuoteSliceGrade::B;
      }
    }
    publish_timing();
    return Ok(std::move(out_));
  }

private:
  // Runs body(i) once for every expiry index in `tasks`, fanned out over
  // surface_.fit_workers in longest-first order: the strictly two-sided quote
  // count, the pre-fit proxy for the admitted count, descending, with ties by
  // index. CONTRACT: body(i) writes only expiry i's slots (out_[i], breadth_[i],
  // two_sided_[i], held_point_[i]) and otherwise reads state no task writes
  // (expiries_, the configs, a prior built before the call). The fan-out joins
  // before it returns, so every write happens before the caller's next read.
  // Each slot is a function of its own inputs alone, so the result is identical
  // for any order and any worker count. A throwing body propagates to the caller
  // (parallel_for_dynamic rethrows the first exception after the join), as the
  // serial loop did; fit_quote_surface converts it to its typed error, so no
  // partially written slot is ever returned. Each task runs with its expiry's rung
  // memo installed (none when surface_.rung_memo is off).
  template <class Body> void for_each_expiry(std::vector<std::size_t> tasks, const Body &body) {
    tasks = detail::longest_first(std::move(tasks), cost_);
    parallel_for_dynamic(tasks.size(), surface_.fit_workers, [&](std::size_t task) {
      detail::run_expiry_task_hook(tasks[task]);
      detail::note_expiry_dispatch(tasks[task]);
      const detail::ScopedRungMemo memo(memo_of(tasks[task]));
      body(tasks[task]);
    });
  }

  // Expiry i's opt-in rung memo (rung_memo.hpp), null when surface_.rung_memo is off.
  [[nodiscard]] detail::ExpiryRungMemo *memo_of(std::size_t i) const {
    return memos_.empty() ? nullptr : &memos_[i];
  }

  [[nodiscard]] std::vector<std::size_t> every_expiry() const {
    std::vector<std::size_t> all(expiries_.size());
    for (std::size_t i = 0; i < all.size(); ++i) all[i] = i;
    return all;
  }

  void point_pass() {
    for_each_expiry(every_expiry(), [this](std::size_t i) {
      const counters::fit_timing::ChainScope chain(slot(i), 1);
      const QuoteSurfaceExpiry &e = expiries_[i];
      breadth_[i] = two_sided_strikes(e.quotes);
      auto fit = fit_quote_slice(e.market, e.quotes, base_config_, e.calendar_dividends);
      if (!fit) {
        out_[i].refusal = "invalid_input";
        out_[i].error = fit.error().to_string();
        return;
      }
      out_[i].point_status = fit->fit_status;
      out_[i].point_error = fit->fit_error;
      out_[i].method = pass_a_method();
      out_[i].fit = std::move(*fit);
      if (out_[i].fit.fit_status == QuoteSliceFitStatus::Success && own_evidence(out_[i].fit)) {
        out_[i].grade = QuoteSliceGrade::A;
      }
    });
  }

  [[nodiscard]] bool ladder() const { return surface_.ladder == QuoteSurfaceLadder::KnotFirst; }

  // Expiry i's opt-in timing slot (counters::fit_timing), null when timing is off.
  [[nodiscard]] counters::fit_timing::ChainRecorder *slot(std::size_t i) const {
    return timing_.empty() ? nullptr : &timing_[i];
  }

  void publish_timing() const {
    if (trace_ == nullptr) return;
    trace_->chains.clear();
    for (const counters::fit_timing::ChainRecorder &rec : timing_) {
      trace_->chains.push_back(rec.result());
    }
  }

  // The pass-A method of the profile without the ladder (diagnostic only).
  [[nodiscard]] QuoteSurfaceMethod pass_a_method() const {
    return config_.method == QuoteSliceFitMethod::ChannelSpline ? QuoteSurfaceMethod::Channel
                                                                 : QuoteSurfaceMethod::Point;
  }

  // The A rule's own-evidence half: the profile minimum of admitted point quotes
  // and distinct strikes (a successful point fit always meets it).
  [[nodiscard]] bool own_evidence(const QuoteSliceFit &fit) const {
    return fit.diagnostics.quotes_admitted >= config_.min_quotes &&
           fit.diagnostics.distinct_strikes >= config_.min_quotes;
  }

  // Ladder containment (QuoteSurfaceConfig::ladder_max_outside_fraction for a
  // point fit, ladder_channel_max_outside_fraction for a channel fit: every
  // channel fit records channel diagnostics, a point fit never does).
  [[nodiscard]] bool in_market(const QuoteSliceFit &fit, std::size_t i) const {
    const double limit = fit.diagnostics.channel.used ? surface_.ladder_channel_max_outside_fraction
                                                      : surface_.ladder_max_outside_fraction;
    const OutsideShare share = outside_share(fit, expiries_[i].quotes, true);
    return !(share.fraction > limit && share.count >= surface_.ladder_min_outside_quotes);
  }

  [[nodiscard]] bool served_in_market(const QuoteSliceFit &fit, std::size_t i) const {
    return fit.fit_status == QuoteSliceFitStatus::Success && in_market(fit, i);
  }

  [[nodiscard]] static QuoteSurfaceLadderFallback knot_reason(const QuoteSliceFit &knot) {
    switch (knot.fit_status) {
    case QuoteSliceFitStatus::Success:
      return QuoteSurfaceLadderFallback::Containment;
    case QuoteSliceFitStatus::ArbitrageFailure:
      return QuoteSurfaceLadderFallback::Certificate;
    case QuoteSliceFitStatus::InsufficientQuotes:
    case QuoteSliceFitStatus::FitFailure:
      return QuoteSurfaceLadderFallback::KnotFailure;
    }
    return QuoteSurfaceLadderFallback::KnotFailure;
  }

  // Ladder pass A: both pass-A rungs of every expiry, eagerly. The knot slice
  // serves (A) when it certifies, is inside the market and has own point
  // evidence; else the point fit serves (A) when it certifies and is inside the
  // market; else the expiry takes the channel passes. A knot-served expiry keeps
  // its in-market point fit for the calendar rung (held_point_). ladder_fallback
  // stays None for an in-market knot slice without own evidence: the point fit
  // shares its admission, so it cannot be served either (the `point_ok` branch
  // is reached only when the knot slice failed) and the channel passes decide.
  void knot_pass() {
    for_each_expiry(every_expiry(), [this](std::size_t i) {
      const counters::fit_timing::ChainScope chain(slot(i), 2);
      const QuoteSurfaceExpiry &e = expiries_[i];
      breadth_[i] = two_sided_strikes(e.quotes);
      two_sided_[i] = two_sided_quotes(e.quotes);
      auto knot =
          fit_quote_slice_channel(e.market, e.quotes, base_config_, nullptr, e.calendar_dividends);
      auto point = fit_quote_slice(e.market, e.quotes, base_config_, e.calendar_dividends);
      QuoteSurfaceSlice &slice = out_[i];
      if (!knot || !point) {
        slice.refusal = "invalid_input";
        slice.error = (!knot ? knot.error() : point.error()).to_string();
        return;
      }
      slice.knot_status = knot->fit_status;
      slice.knot_error = knot->fit_error;
      slice.point_status = point->fit_status;
      slice.point_error = point->fit_error;
      const bool knot_ok = served_in_market(*knot, i);
      const bool point_ok = served_in_market(*point, i);
      slice.ladder_fallback = knot_ok ? QuoteSurfaceLadderFallback::None : knot_reason(*knot);
      if (knot_ok && own_evidence(*knot)) {
        slice.fit = std::move(*knot);
        slice.method = QuoteSurfaceMethod::Channel;
        slice.grade = QuoteSliceGrade::A;
        if (point_ok) held_point_[i] = std::move(*point);
      } else if (point_ok) {
        slice.fit = std::move(*point);
        slice.method = QuoteSurfaceMethod::Point;
        slice.grade = QuoteSliceGrade::A;
      } else {
        slice.fit = std::move(*knot); // the channel passes replace it
        slice.method = QuoteSurfaceMethod::Channel;
      }
    });
  }

  // Channel dominance (opt-in, QuoteSliceChannelConfig::point_max_outside_fraction),
  // after every other rung: an expiry whose final fit is a point fit (served,
  // or refused only for a calendar crossing) and lies outside too many of its
  // two-sided quotes is offered a channel refit under the prior of the other
  // served expiries. The refit is adopted only if it is calendar-consistent
  // with every other served expiry (after the bounded-move and channel calendar
  // repairs), so the gate never refuses an expiry and never moves another one;
  // otherwise the point fit is kept (a served A becomes B).
  //
  // Three phases, all in calendar confidence order:
  //  1. every served gated expiry tentatively adopts its refit when the refit is
  //     consistent with the served expiries that are not still-gated point fits
  //     (two neighbouring point fits with bad wings would otherwise block each
  //     other's refit);
  //  2. while an adopted refit crosses any served expiry, the lowest-confidence
  //     such adoption is undone (its point fit restored), so the served set is
  //     calendar-consistent again; with every adoption undone it is the
  //     calendar pass's own set;
  //  3. if a refit survived, calendar refusals are offered re-admission against
  //     the final served set: a gated point fit's refit first, then the expiry's
  //     own fit (clean, bounded-move repair, channel calendar repair), grade B.
  [[nodiscard]] Status dominance_pass() {
    const auto &limit = config_.channel.point_max_outside_fraction;
    if (!limit || config_.method != QuoteSliceFitMethod::PointCurve) return Ok();
    const std::size_t n = out_.size();
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [this](std::size_t a, std::size_t b) { return ranks_before(a, b); });
    std::vector<bool> gated(n, false);
    for (const std::size_t i : order) {
      QuoteSurfaceSlice &slice = out_[i];
      const bool candidate = !slice.channel_refit && slice.fit.prices &&
                             (served(i) || slice.refusal == "calendar_arbitrage");
      if (!candidate) continue;
      const OutsideShare share = outside_share(slice.fit, expiries_[i].quotes);
      slice.point_outside_fraction = share.fraction;
      slice.point_outside_quotes = share.count;
      gated[i] = share.fraction > *limit && share.count >= config_.channel.point_min_outside_quotes;
      slice.point_gated = gated[i];
    }
    std::vector<std::optional<HeldPoint>> held(n);
    for (const std::size_t i : order) { // phase 1
      if (!gated[i] || !served(i)) continue;
      const counters::fit_timing::ChainScope chain(slot(i));
      std::vector<std::size_t> settled;
      for (std::size_t j = 0; j < n; ++j) {
        if (j != i && served(j) && !(gated[j] && !held[j])) settled.push_back(j);
      }
      HeldPoint point{std::move(out_[i].fit), out_[i].grade, out_[i].calendar_repaired,
                      out_[i].calendar_channel_repaired};
      ATX_TRY(const bool adopted, adopt_refit(i, settled, settled));
      if (adopted) {
        held[i] = std::move(point);
      } else {
        out_[i].fit = std::move(point.fit);
      }
    }
    for (bool undone = true; undone;) { // phase 2
      undone = false;
      for (auto it = order.rbegin(); it != order.rend() && !undone; ++it) {
        const std::size_t i = *it;
        const counters::fit_timing::ChainScope chain(slot(i));
        if (!held[i] || consistent(out_[i].fit, served_except(i))) continue;
        restore_point(i, *held[i]);
        held[i].reset();
        undone = true;
      }
    }
    const bool any = std::any_of(held.begin(), held.end(),
                                 [](const std::optional<HeldPoint> &h) { return h.has_value(); });
    for (const std::size_t i : order) { // phase 3
      if (!any || served(i) || out_[i].refusal != "calendar_arbitrage" || !out_[i].fit.prices) {
        continue;
      }
      const counters::fit_timing::ChainScope chain(slot(i));
      const std::vector<std::size_t> others = served_except(i);
      if (gated[i]) {
        std::vector<std::size_t> evidence;
        for (const std::size_t j : others) {
          if (!(gated[j] && !out_[j].channel_refit)) evidence.push_back(j);
        }
        ATX_TRY(const bool adopted, adopt_refit(i, others, evidence));
        if (adopted) {
          out_[i].calendar_readmitted = true;
          continue;
        }
      }
      readmit_own_fit(i, others);
    }
    for (std::size_t i = 0; i < n; ++i) {
      QuoteSurfaceSlice &slice = out_[i];
      if (!gated[i] || !served(i) || slice.channel_refit) continue;
      slice.point_fallback = true; // gated, its refit not adopted: the point fit stays
      if (slice.grade == QuoteSliceGrade::A) slice.grade = QuoteSliceGrade::B;
    }
    return Ok();
  }

  // A gated point fit kept aside while its refit is tentatively served.
  struct HeldPoint {
    QuoteSliceFit fit;
    QuoteSliceGrade grade;
    bool calendar_repaired;
    bool calendar_channel_repaired;
  };

  [[nodiscard]] bool served(std::size_t i) const {
    return out_[i].grade != QuoteSliceGrade::Refused;
  }

  [[nodiscard]] std::vector<std::size_t> served_except(std::size_t i) const {
    std::vector<std::size_t> out;
    for (std::size_t j = 0; j < out_.size(); ++j) {
      if (j != i && served(j)) out.push_back(j);
    }
    return out;
  }

  // Channel refit of expiry i under the prior of the `evidence` expiries (the
  // callers leave gated point fits out: they are no evidence); adopted (graded
  // as a channel refit) when consistent with `check` after the bounded-move and
  // channel calendar repairs. Reads no part of out_[i].fit (it may be held
  // aside); leaves the slice untouched when not adopted.
  [[nodiscard]] Result<bool> adopt_refit(std::size_t i, const std::vector<std::size_t> &check,
                                         const std::vector<std::size_t> &evidence) {
    ATX_TRY(const TermPrior prior, make_prior(nodes(true, &evidence), surface_));
    auto refit = channel_fit(i, prior, 1.0);
    if (!refit || refit->fit_status != QuoteSliceFitStatus::Success) return Ok(false);
    const bool clean = consistent(*refit, check);
    if (!clean && !repair(*refit, i, check) && !repair_in_channel(*refit, i, check)) {
      return Ok(false);
    }
    adopt(i, std::move(*refit));
    out_[i].refusal.clear();
    out_[i].error.clear();
    if (!clean) mark_repaired(i);
    return Ok(true);
  }

  void restore_point(std::size_t i, HeldPoint &point) {
    QuoteSurfaceSlice &slice = out_[i];
    slice.fit = std::move(point.fit);
    slice.grade = point.grade;
    slice.channel_refit = false;
    slice.method = QuoteSurfaceMethod::Point;
    slice.calendar_repaired = point.calendar_repaired;
    slice.calendar_channel_repaired = point.calendar_channel_repaired;
  }

  // Re-admission of a calendar refusal's own fit against the final served set.
  void readmit_own_fit(std::size_t i, const std::vector<std::size_t> &others) {
    QuoteSurfaceSlice &slice = out_[i];
    const bool clean = consistent(slice.fit, others);
    if (!clean && !repair(slice.fit, i, others) && !repair_in_channel(slice.fit, i, others)) {
      return;
    }
    slice.fit.fit_status = QuoteSliceFitStatus::Success;
    slice.fit.fit_error.clear();
    slice.grade = slice.channel_refit &&
                          slice.fit.diagnostics.channel.mid_strikes < surface_.grade_b_min_mid_strikes
                      ? QuoteSliceGrade::C
                      : QuoteSliceGrade::B;
    slice.refusal.clear();
    slice.error.clear();
    slice.calendar_readmitted = true;
    if (!clean) slice.calendar_repaired = true;
  }

  // Confidence order of the calendar pass: grade, then own evidence, then the
  // nearer maturity (total, since maturities are distinct).
  [[nodiscard]] bool ranks_before(std::size_t a, std::size_t b) const {
    if (out_[a].grade != out_[b].grade) return out_[a].grade < out_[b].grade;
    if (evidence(a) != evidence(b)) return evidence(a) > evidence(b);
    return expiries_[a].market.calendar_years < expiries_[b].market.calendar_years;
  }

  // Channel repair rung (config.repair.mode == Channel): an expiry still refused
  // for a failed certificate after both channel passes (which already ran with
  // the repair) whose pass-A fit also failed its certificate gets that pass-A
  // fit again WITH the repair; served, it is grade B.
  void repair_rung() {
    if (config_.repair.mode != QuoteSliceRepairMode::Channel) return;
    for (std::size_t i = 0; i < out_.size(); ++i) {
      QuoteSurfaceSlice &slice = out_[i];
      if (slice.grade != QuoteSliceGrade::Refused || slice.refusal != "arbitrage_failure" ||
          slice.point_status != QuoteSliceFitStatus::ArbitrageFailure) {
        continue;
      }
      const counters::fit_timing::ChainScope chain(slot(i), 1);
      const counters::fit_timing::RepairRungScope repair_timing;
      const detail::ScopedRungMemo memo(memo_of(i));
      const QuoteSurfaceExpiry &e = expiries_[i];
      auto fit = fit_quote_slice(e.market, e.quotes, config_, e.calendar_dividends);
      if (!fit) continue;
      slice.rung_repair = fit->diagnostics.repair;
      if (fit->fit_status != QuoteSliceFitStatus::Success) continue;
      slice.fit = std::move(*fit);
      slice.grade = QuoteSliceGrade::B;
      slice.refusal.clear();
      slice.error.clear();
      slice.channel_refit = false;
      slice.repair_rung = true;
      slice.method = pass_a_method();
    }
  }

  // Ladder repair rung (config.repair.mode == Channel): an expiry still refused
  // for a failed certificate after both channel passes gets the pass-A rung(s)
  // that failed their certificate again WITH the repair, the knot fit first,
  // then the point fit; the first certified fit inside the market serves (the
  // point fit as B, the knot fit graded like a channel fit by its mid strikes).
  void ladder_repair_rung() {
    if (config_.repair.mode != QuoteSliceRepairMode::Channel) return;
    for (std::size_t i = 0; i < out_.size(); ++i) {
      QuoteSurfaceSlice &slice = out_[i];
      if (slice.grade != QuoteSliceGrade::Refused || slice.refusal != "arbitrage_failure") {
        continue;
      }
      counters::fit_timing::ChainScope chain(slot(i));
      const counters::fit_timing::RepairRungScope repair_timing;
      const detail::ScopedRungMemo memo(memo_of(i));
      const QuoteSurfaceExpiry &e = expiries_[i];
      if (slice.knot_status == QuoteSliceFitStatus::ArbitrageFailure) {
        chain.rung();
        auto knot =
            fit_quote_slice_channel(e.market, e.quotes, config_, nullptr, e.calendar_dividends);
        if (knot) slice.rung_repair = knot->diagnostics.repair;
        if (knot && served_in_market(*knot, i)) {
          serve_rung(i, std::move(*knot), QuoteSurfaceMethod::Channel);
          continue;
        }
      }
      if (slice.point_status == QuoteSliceFitStatus::ArbitrageFailure) {
        chain.rung();
        auto point = fit_quote_slice(e.market, e.quotes, config_, e.calendar_dividends);
        if (point) slice.rung_repair = point->diagnostics.repair;
        if (point && served_in_market(*point, i)) {
          serve_rung(i, std::move(*point), QuoteSurfaceMethod::Point);
        }
      }
    }
  }

  void serve_rung(std::size_t i, QuoteSliceFit fit, QuoteSurfaceMethod method) {
    QuoteSurfaceSlice &slice = out_[i];
    slice.fit = std::move(fit);
    const bool thin = method == QuoteSurfaceMethod::Channel &&
                      slice.fit.diagnostics.channel.mid_strikes < surface_.grade_b_min_mid_strikes;
    slice.grade = thin ? QuoteSliceGrade::C : QuoteSliceGrade::B;
    slice.refusal.clear();
    slice.error.clear();
    slice.channel_refit = false;
    slice.repair_rung = true;
    slice.method = method;
  }

  [[nodiscard]] bool candidate(std::size_t i) const {
    return out_[i].refusal.empty() && out_[i].grade != QuoteSliceGrade::A;
  }

  [[nodiscard]] double node_weight(std::size_t i) const {
    const QuoteSurfaceSlice &slice = out_[i];
    if (slice.grade == QuoteSliceGrade::A) {
      return static_cast<double>(std::max<std::size_t>(slice.fit.diagnostics.distinct_strikes, 1));
    }
    return surface_.channel_node_weight *
           static_cast<double>(std::max<std::size_t>(slice.fit.diagnostics.channel.mid_strikes, 1));
  }

  // Channel fit of expiry i under `prior` (leave-one-out on i), strength scaled
  // by `factor`. Without a prior node the flat default shape applies.
  [[nodiscard]] Result<QuoteSliceFit> channel_fit(std::size_t i, const TermPrior &prior,
                                                  double factor) const {
    const counters::fit_timing::ChainScope chain(slot(i), 1);
    const detail::ScopedRungMemo memo(memo_of(i));
    const QuoteSurfaceExpiry &e = expiries_[i];
    const auto point = prior.at(e.market.variance_years, static_cast<std::uint64_t>(i));
    if (!point || !(surface_.term_prior_weight > 0.0)) {
      return fit_quote_slice_channel(e.market, e.quotes, config_, nullptr, e.calendar_dividends);
    }
    const double n0 = surface_.prior_quote_halflife;
    const double strength = surface_.term_prior_weight * point->confidence * factor * n0 /
                            (n0 + static_cast<double>(breadth_[i]));
    const QuoteSlicePrior applied{point->atm_vol, kTermPriorGrid, point->multiples, strength};
    return fit_quote_slice_channel(e.market, e.quotes, config_, &applied, e.calendar_dividends);
  }

  [[nodiscard]] std::vector<TermPriorNode> nodes(bool include_channel,
                                                 const std::vector<std::size_t> *only) const {
    std::vector<TermPriorNode> result;
    result.reserve(out_.size());
    const auto add = [&](std::size_t i) {
      const QuoteSurfaceSlice &slice = out_[i];
      const bool channel = slice.grade != QuoteSliceGrade::A;
      if (slice.fit.fit_status != QuoteSliceFitStatus::Success || (channel && !include_channel)) {
        return;
      }
      if (auto node = node_from_fit(slice.fit, node_weight(i), i, config_.channel)) {
        result.push_back(*node);
      }
    };
    if (only != nullptr) {
      for (const std::size_t i : *only) add(i);
    } else {
      for (std::size_t i = 0; i < out_.size(); ++i) {
        if (out_[i].refusal.empty() &&
            (out_[i].grade == QuoteSliceGrade::A || out_[i].channel_refit)) {
          add(i);
        }
      }
    }
    return result;
  }

  void adopt(std::size_t i, QuoteSliceFit fit) {
    QuoteSurfaceSlice &slice = out_[i];
    slice.fit = std::move(fit);
    slice.channel_refit = true;
    slice.method = QuoteSurfaceMethod::ChannelRefit;
    if (slice.fit.fit_status == QuoteSliceFitStatus::Success) {
      slice.grade = slice.fit.diagnostics.channel.mid_strikes >= surface_.grade_b_min_mid_strikes
                        ? QuoteSliceGrade::B
                        : QuoteSliceGrade::C;
    } else {
      slice.grade = QuoteSliceGrade::Refused;
    }
  }

  [[nodiscard]] Status channel_passes() {
    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < out_.size(); ++i) {
      if (candidate(i)) candidates.push_back(i);
    }
    if (candidates.empty()) {
      return Ok();
    }
    // B1: prior from point-fitted expiries only. Each stage's prior is built
    // before its fan-out and only read inside it, so a refit depends on no
    // other refit of the same stage (for_each_expiry's contract).
    ATX_TRY(const TermPrior point_prior, make_prior(nodes(false, nullptr), surface_));
    for_each_expiry(candidates, [&](std::size_t i) {
      auto fit = channel_fit(i, point_prior, 1.0);
      if (!fit) {
        out_[i].refusal = "fit_failure";
        out_[i].error = fit.error().to_string();
        return;
      }
      adopt(i, std::move(*fit));
    });
    // B2: leave-one-out prior from every fitted expiry, B1 nodes down-weighted.
    ATX_TRY(const TermPrior full_prior, make_prior(nodes(true, nullptr), surface_));
    for_each_expiry(candidates, [&](std::size_t i) {
      if (!out_[i].refusal.empty()) return;
      auto fit = channel_fit(i, full_prior, 1.0);
      if (fit && fit->fit_status == QuoteSliceFitStatus::Success) {
        adopt(i, std::move(*fit));
      } else if (out_[i].fit.fit_status != QuoteSliceFitStatus::Success && fit) {
        adopt(i, std::move(*fit)); // keep the latest diagnostics of a failed slice
      }
      if (out_[i].grade == QuoteSliceGrade::Refused) {
        out_[i].refusal = to_string(out_[i].fit.fit_status);
        out_[i].error = out_[i].fit.fit_error;
      } else if (ladder() && !in_market(out_[i].fit, i)) {
        refuse_outside(i);
      }
    });
    return Ok();
  }

  // Ladder containment of a served channel refit: refused, never served outside.
  void refuse_outside(std::size_t i) {
    QuoteSurfaceSlice &slice = out_[i];
    const OutsideShare share = outside_share(slice.fit, expiries_[i].quotes, true);
    slice.grade = QuoteSliceGrade::Refused;
    slice.refusal = "outside_market";
    slice.error = "served prices outside bid/ask on " + std::to_string(share.count) +
                  " strictly two-sided quotes (share " + std::to_string(share.fraction) + ")";
  }

  [[nodiscard]] bool consistent(const QuoteSliceFit &fit,
                                const std::vector<std::size_t> &accepted) const {
    const counters::fit_timing::BucketScope timing(counters::fit_timing::Bucket::Calendar);
    for (const std::size_t j : accepted) {
      counters::fit_timing::count(&counters::fit_timing::FitTimingCounters::calendar_pairs);
      const QuoteSliceFit &other = out_[j].fit;
      const bool earlier = fit.market.calendar_years < other.market.calendar_years;
      const auto status = earlier ? check_american_calendar(*fit.prices, *other.prices)
                                  : check_american_calendar(*other.prices, *fit.prices);
      if (!status) return false;
    }
    return true;
  }

  [[nodiscard]] std::size_t evidence(std::size_t i) const {
    const auto &diag = out_[i].fit.diagnostics;
    return out_[i].grade == QuoteSliceGrade::A ? diag.distinct_strikes : diag.channel.mid_strikes;
  }

  // Largest calendar crossing (earlier minus later) over the union of both
  // slices' knots inside their overlap, for diagnostics only.
  static void calendar_gap(const AmericanPriceSlice &earlier, const AmericanPriceSlice &later,
                           double &gap, double &strike) {
    const auto first = earlier.nodes();
    const auto second = later.nodes();
    const double lo = std::max(first.front().strike, second.front().strike);
    const double hi = std::min(first.back().strike, second.back().strike);
    const auto visit = [&](double k) {
      if (k < lo || k > hi) return;
      for (const Side side : {Side::Call, Side::Put}) {
        const auto near = earlier.price(k, side);
        const auto far = later.price(k, side);
        if (near && far && *near - *far > gap) {
          gap = *near - *far;
          strike = k;
        }
      }
    };
    for (const auto &node : first) visit(node.strike);
    for (const auto &node : second) visit(node.strike);
  }

  void measure_gap(std::size_t i, const std::vector<std::size_t> &accepted) {
    QuoteSurfaceSlice &slice = out_[i];
    if (!slice.fit.prices) return;
    for (const std::size_t j : accepted) {
      const QuoteSliceFit &other = out_[j].fit;
      if (!other.prices) continue;
      const bool earlier = slice.fit.market.calendar_years < other.market.calendar_years;
      if (earlier) {
        calendar_gap(*slice.fit.prices, *other.prices, slice.calendar_gap,
                     slice.calendar_gap_strike);
      } else {
        calendar_gap(*other.prices, *slice.fit.prices, slice.calendar_gap,
                     slice.calendar_gap_strike);
      }
    }
  }

  void refuse_calendar(std::size_t i) {
    QuoteSurfaceSlice &slice = out_[i];
    slice.grade = QuoteSliceGrade::Refused;
    slice.refusal = "calendar_arbitrage";
    slice.error = "American calendar price crossing";
    slice.fit.fit_status = QuoteSliceFitStatus::ArbitrageFailure;
    slice.fit.fit_error = slice.error;
  }

  [[nodiscard]] Status calendar_pass() {
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < out_.size(); ++i) {
      if (out_[i].grade != QuoteSliceGrade::Refused && out_[i].fit.prices) order.push_back(i);
    }
    std::sort(order.begin(), order.end(),
              [this](std::size_t a, std::size_t b) { return ranks_before(a, b); });
    std::vector<std::size_t> accepted;
    std::vector<std::size_t> rejected;
    accepted.reserve(order.size());
    for (const std::size_t i : order) {
      const counters::fit_timing::ChainScope chain(slot(i));
      if (consistent(out_[i].fit, accepted)) {
        accepted.push_back(i);
      } else {
        rejected.push_back(i);
      }
    }
    if (rejected.empty()) {
      return Ok();
    }
    // Per rejected slice, in confidence order: a bounded-move repair of its own
    // prices; else refit once with a stronger prior built from accepted slices
    // (and repair that refit); else (opt-in fallback rung) a channel calendar
    // repair of its own prices, then of the refit; else refuse. Every
    // acceptance is re-checked.
    ATX_TRY(const TermPrior accepted_prior, make_prior(nodes(true, &accepted), surface_));
    for (const std::size_t i : rejected) {
      const counters::fit_timing::ChainScope chain(slot(i));
      if (repair(out_[i].fit, i, accepted)) {
        mark_repaired(i);
        accepted.push_back(i);
        continue;
      }
      out_[i].calendar_retry = true;
      auto fit = channel_fit(i, accepted_prior, surface_.calendar_retry_factor);
      const bool refit = fit && fit->fit_status == QuoteSliceFitStatus::Success;
      if (refit) {
        const bool clean = consistent(*fit, accepted);
        if (clean || repair(*fit, i, accepted)) {
          adopt(i, std::move(*fit));
          if (!clean) mark_repaired(i);
          accepted.push_back(i);
          continue;
        }
      }
      if (repair_in_channel(out_[i].fit, i, accepted)) {
        mark_repaired(i);
        accepted.push_back(i);
        continue;
      }
      if (refit && repair_in_channel(*fit, i, accepted)) {
        adopt(i, std::move(*fit));
        mark_repaired(i);
        accepted.push_back(i);
        continue;
      }
      if (repair_joint(i, accepted)) { // opt-in bidirectional rung (lane L3q)
        mark_repaired(i);
        accepted.push_back(i);
        continue;
      }
      measure_gap(i, accepted);
      refuse_calendar(i);
    }
    return Ok();
  }

  // Whether a slice's served prices left its own fit substantively: certified
  // only by the channel repair, or served by a repair rung (a quoted side can
  // move across its whole bid/ask channel). The bounded-move convexity repair
  // (each node within channel.repair_half_spreads of its own half-spread) does
  // not count: it removes sub-spread kinks and leaves the fit's prices in place
  // to quote precision.
  [[nodiscard]] bool channel_repaired(std::size_t i) const {
    const QuoteSurfaceSlice &slice = out_[i];
    return slice.repair_rung ||
           slice.fit.diagnostics.repair.outcome == QuoteSliceRepairOutcome::Applied;
  }

  // Market containment under the strictest limit the policy states (the smaller
  // of the point and channel limits). Every slice reaching the calendar pass
  // met its own rung's limit; only a slice served under an explicitly relaxed
  // limit (a limit above the other one) can fail this.
  [[nodiscard]] bool contained(std::size_t i) const {
    const double limit = std::min(surface_.ladder_max_outside_fraction,
                                  surface_.ladder_channel_max_outside_fraction);
    const OutsideShare share = outside_share(out_[i].fit, expiries_[i].quotes, true);
    return !(share.fraction > limit && share.count >= surface_.ladder_min_outside_quotes);
  }

  // Confidence order of the ladder's calendar pass, a total order on intrinsic
  // slice properties (never on processing order): contained slices first (a
  // slice whose prices leave the market, served only under a relaxed limit,
  // never out-ranks one inside it), then grade tier (own evidence first; a
  // pass-A slice keeps its A tier until the final relabel), then prices still
  // the fit's own before channel-repaired ones (channel_repaired), then more
  // strictly two-sided quotes (more market information), then the nearer
  // maturity (distinct maturities make it total).
  [[nodiscard]] bool ladder_ranks_before(std::size_t a, std::size_t b) const {
    if (contained_[a] != contained_[b]) return contained_[a] != 0;
    if (out_[a].grade != out_[b].grade) return out_[a].grade < out_[b].grade;
    const bool repaired_a = channel_repaired(a);
    const bool repaired_b = channel_repaired(b);
    if (repaired_a != repaired_b) return repaired_b;
    if (two_sided_[a] != two_sided_[b]) return two_sided_[a] > two_sided_[b];
    return expiries_[a].market.calendar_years < expiries_[b].market.calendar_years;
  }

  // Ladder calendar pass: ONE visit per slice in confidence order, each resolved
  // against the slices accepted before it (ladder_resolve) before the next is
  // visited. A lower-confidence slice therefore never constrains a
  // higher-confidence one's repair or fallback (the two-pass calendar_pass
  // accepts every clean slice first, so a newly served low-confidence slice can
  // block a higher-confidence slice's repair: the displacement this removes).
  [[nodiscard]] Status ladder_calendar_pass() {
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < out_.size(); ++i) {
      if (served(i) && out_[i].fit.prices) {
        order.push_back(i);
        contained_[i] = contained(i) ? 1 : 0;
      }
    }
    std::sort(order.begin(), order.end(),
              [this](std::size_t a, std::size_t b) { return ladder_ranks_before(a, b); });
    std::vector<std::size_t> accepted;
    accepted.reserve(order.size());
    for (const std::size_t i : order) {
      const counters::fit_timing::ChainScope chain(slot(i));
      if (consistent(out_[i].fit, accepted)) {
        accepted.push_back(i);
        continue;
      }
      ATX_TRY(const bool kept, ladder_resolve(i, accepted));
      if (kept) {
        accepted.push_back(i);
        continue;
      }
      measure_gap(i, accepted);
      refuse_calendar(i);
    }
    return Ok();
  }

  // A crossing slice's ladder rungs against the accepted slices, in order: its
  // own bounded-move repair; the held point fit (knot-served slices), clean then
  // repaired; the stronger-prior retry, clean then repaired; the channel
  // calendar repair of its own prices, then of the retry's. Every result is
  // inside the market (repair, repair_in_channel and the clean retry check it).
  [[nodiscard]] Result<bool> ladder_resolve(std::size_t i, const std::vector<std::size_t> &accepted) {
    if (repair(out_[i].fit, i, accepted)) {
      mark_repaired(i);
      return Ok(true);
    }
    if (held_point_[i]) {
      const bool clean = consistent(*held_point_[i], accepted);
      if (clean || repair(*held_point_[i], i, accepted)) {
        serve_held_point(i);
        if (!clean) mark_repaired(i);
        return Ok(true);
      }
    }
    out_[i].calendar_retry = true;
    ATX_TRY(const TermPrior prior, make_prior(nodes(true, &accepted), surface_));
    auto fit = channel_fit(i, prior, surface_.calendar_retry_factor);
    const bool refit = fit && fit->fit_status == QuoteSliceFitStatus::Success;
    if (refit) {
      const bool clean = consistent(*fit, accepted) && in_market(*fit, i);
      if (clean || repair(*fit, i, accepted)) {
        adopt_retry(i, std::move(*fit), !clean);
        return Ok(true);
      }
    }
    if (repair_in_channel(out_[i].fit, i, accepted)) {
      mark_repaired(i);
      return Ok(true);
    }
    if (refit && repair_in_channel(*fit, i, accepted)) {
      adopt_retry(i, std::move(*fit), true);
      return Ok(true);
    }
    if (repair_joint(i, accepted)) { // opt-in bidirectional rung (lane L3q)
      mark_repaired(i);
      return Ok(true);
    }
    return Ok(false);
  }

  void serve_held_point(std::size_t i) {
    QuoteSurfaceSlice &slice = out_[i];
    slice.fit = std::move(*held_point_[i]);
    held_point_[i].reset();
    slice.method = QuoteSurfaceMethod::Point;
    slice.channel_refit = false;
    if (slice.ladder_fallback == QuoteSurfaceLadderFallback::None) {
      slice.ladder_fallback = QuoteSurfaceLadderFallback::Calendar;
    }
  }

  void adopt_retry(std::size_t i, QuoteSliceFit fit, bool repaired) {
    adopt(i, std::move(fit));
    if (repaired) mark_repaired(i);
    if (out_[i].ladder_fallback == QuoteSurfaceLadderFallback::None) {
      out_[i].ladder_fallback = QuoteSurfaceLadderFallback::Calendar;
    }
  }

  // Ladder wing tail FloorPriceOnly (QuoteSurfaceConfig::ladder_wing_tail): a served
  // row re-implied from a repaired price at the inverter's floor kIvMin has no
  // identified volatility. It becomes PriceOnly with its certified price, as a failed
  // re-implication does (certify_and_serve, commit_calendar_repair); prices, and so
  // containment and calendar consistency, are untouched. Runs after the calendar pass.
  void price_only_floor_vols() {
    for (std::size_t i = 0; i < out_.size(); ++i) {
      QuoteSurfaceSlice &slice = out_[i];
      QuoteSliceFit &fit = slice.fit;
      if (slice.grade == QuoteSliceGrade::Refused || !fit.prices || fit.moved_nodes.empty()) {
        continue;
      }
      const auto nodes = fit.prices->nodes();
      const std::span<const QuoteSliceQuote> quotes = expiries_[i].quotes;
      const std::size_t n = std::min(fit.rows.size(), quotes.size());
      for (std::size_t r = 0; r < n; ++r) {
        QuoteSliceRow &row = fit.rows[r];
        if (row.evaluation_status != QuoteSliceEvaluationStatus::Success ||
            !(std::isfinite(row.fitted_iv) && row.fitted_iv <= kIvMin)) {
          continue;
        }
        const double strike = quotes[r].strike;
        const auto at = std::lower_bound(nodes.begin(), nodes.end(), strike,
            [](const AmericanPriceNode &node, double k) { return node.strike < k; });
        const auto node = static_cast<std::size_t>(at - nodes.begin());
        const auto bit = static_cast<std::uint8_t>(quotes[r].side == Side::Call ? 1u : 2u);
        if (at == nodes.end() || at->strike != strike || node >= fit.moved_nodes.size() ||
            (fit.moved_nodes[node] & bit) == 0u) {
          continue; // the curve's own volatility, not a re-implied one
        }
        // Lane C0: the typed floor answer of the served price (reason and interval).
        const detail::VolIdentification id =
            detail::floor_vol(fit, strike, quotes[r].side, row.fitted_price);
        row.fitted_iv = kNaN;
        row.evaluation_status = QuoteSliceEvaluationStatus::PriceOnly;
        row.price_only_reason = id.reason;
        row.iv_lower = id.iv_lower;
        row.iv_upper = id.iv_upper;
        detail::mark_price_only(fit, node, bit, nodes.size());
        ++slice.wing_tail_price_only;
      }
    }
  }

  // Ladder provenance: the final prices' containment (before a refusal strips them).
  void measure_containment() {
    for (std::size_t i = 0; i < out_.size(); ++i) {
      if (!out_[i].fit.prices) continue;
      const OutsideShare share = outside_share(out_[i].fit, expiries_[i].quotes, true);
      out_[i].outside_fraction = share.fraction;
      out_[i].outside_quotes = share.count;
    }
  }

  // The accepted slices' certified prices, split by maturity around `fit`.
  void neighbours(const QuoteSliceFit &fit, const std::vector<std::size_t> &accepted,
                  std::vector<const AmericanPriceSlice *> &earlier,
                  std::vector<const AmericanPriceSlice *> &later) const {
    for (const std::size_t j : accepted) {
      const QuoteSliceFit &other = out_[j].fit;
      (other.market.calendar_years < fit.market.calendar_years ? earlier : later)
          .push_back(&*other.prices);
    }
  }

  // Bounded-move calendar repair of `fit` (expiry i) against the accepted
  // slices; true only when the repaired prices are certified and consistent
  // with all of them.
  [[nodiscard]] bool repair(QuoteSliceFit &fit, std::size_t i,
                            const std::vector<std::size_t> &accepted) {
    if (!(surface_.calendar_repair_half_spreads > 0.0) || !fit.prices) return false;
    std::vector<const AmericanPriceSlice *> earlier;
    std::vector<const AmericanPriceSlice *> later;
    neighbours(fit, accepted, earlier, later);
    QuoteSliceFit trial = trial_copy(fit);
    double required = 0.0;
    const bool repaired = repair_quote_slice_calendar(trial, expiries_[i].quotes, earlier, later,
                                                      surface_.calendar_repair_half_spreads,
                                                      &required);
    out_[i].calendar_repair_required = std::max(out_[i].calendar_repair_required, required);
    if (!repaired || !consistent(trial, accepted)) return false;
    if (ladder() && !in_market(trial, i)) return false; // a bounded move can leave the quotes
    commit(fit, trial);
    return true;
  }

  // Opt-in channel calendar repair (config.repair.mode == Channel, or — lane
  // L3r1 — its box-constrained form under QuoteSliceCertificate::Box: the quote
  // boxes instead of the fitted-widened channels, no budget), the same contract
  // as repair(); it runs only after the bounded-move repair and the
  // stronger-prior retry have both failed.
  [[nodiscard]] bool repair_in_channel(QuoteSliceFit &fit, std::size_t i,
                                       const std::vector<std::size_t> &accepted) {
    if ((config_.repair.mode != QuoteSliceRepairMode::Channel &&
         config_.repair.certificate != QuoteSliceCertificate::Box) ||
        !fit.prices) {
      return false;
    }
    std::vector<const AmericanPriceSlice *> earlier;
    std::vector<const AmericanPriceSlice *> later;
    neighbours(fit, accepted, earlier, later);
    QuoteSliceFit trial = trial_copy(fit);
    QuoteSliceRepairRecord &record = out_[i].calendar_channel_repair;
    const bool repaired = repair_quote_slice_calendar_in_channel(
        trial, expiries_[i].quotes, earlier, later, config_.repair, &record);
    if (!repaired) return false;
    if (!consistent(trial, accepted)) {
      record.outcome = QuoteSliceRepairOutcome::NotCertified;
      return false;
    }
    if (ladder() && !in_market(trial, i)) {
      record.outcome = QuoteSliceRepairOutcome::LeftChannel;
      return false;
    }
    commit(fit, trial);
    out_[i].calendar_channel_repaired = true;
    return true;
  }

  // ── Distance to market (QuoteSliceMarketDistance, lane L3q 2026-10-21) ──────

  [[nodiscard]] bool market_repair_on() const {
    return config_.repair.market_distance == QuoteSliceMarketDistance::Repair;
  }

  // Bidirectional calendar rung (Repair only), after every one-sided rung failed:
  // expiry i is projected jointly (repair_quote_slice_calendar_joint) first with the
  // accepted slices it crosses, the others fixed, then with EVERY accepted slice,
  // because a crossing can need a chain of maturities to move (deep in-the-money
  // puts pinned at intrinsic under per-expiry spots a cent apart). The trials are
  // committed only when every member certifies and is calendar-consistent with every
  // other and, under the ladder, every moved member is still inside the market; a
  // moved accepted slice is marked repaired (grade B).
  [[nodiscard]] bool repair_joint(std::size_t i, const std::vector<std::size_t> &accepted) {
    if (!market_repair_on() || !out_[i].fit.prices || accepted.empty()) return false;
    // First the accepted slices i crosses (the others fixed: a small projection),
    // then every accepted slice (a chain of maturities can need to move).
    std::vector<std::size_t> partners;
    for (const std::size_t j : accepted) {
      if (!consistent(out_[i].fit, std::vector<std::size_t>{j})) partners.push_back(j);
    }
    if (joint_attempt(i, partners, accepted)) return true;
    return partners.size() < accepted.size() && joint_attempt(i, accepted, accepted);
  }

  // One bidirectional attempt: expiry i and `partners` move, the rest of `accepted`
  // stays fixed.
  [[nodiscard]] bool joint_attempt(std::size_t i, const std::vector<std::size_t> &partners,
                                   const std::vector<std::size_t> &accepted) {
    if (partners.empty()) return false;
    std::vector<std::size_t> slots{i};
    slots.insert(slots.end(), partners.begin(), partners.end());
    std::vector<QuoteSliceFit> trials;
    trials.reserve(slots.size());
    std::vector<QuoteSliceJointMember> members;
    members.reserve(slots.size());
    for (const std::size_t s : slots) trials.push_back(trial_copy(out_[s].fit));
    for (std::size_t m = 0; m < slots.size(); ++m) {
      members.push_back({&trials[m], expiries_[slots[m]].quotes});
    }
    std::vector<QuoteSliceCalendarNeighbour> fixed;
    for (const std::size_t j : accepted) {
      if (std::find(partners.begin(), partners.end(), j) != partners.end()) continue;
      fixed.push_back({&*out_[j].fit.prices, out_[j].fit.market.calendar_years});
    }
    std::vector<std::size_t> moved;
    if (!repair_quote_slice_calendar_joint(members, fixed, config_.repair, &moved,
                                           &out_[i].calendar_channel_repair)) {
      return false;
    }
    for (std::size_t m = 0; ladder() && m < slots.size(); ++m) {
      if (moved[m] > 0 && !in_market(trials[m], slots[m])) return false;
    }
    for (std::size_t m = 0; m < slots.size(); ++m) {
      if (moved[m] == 0) continue;
      commit(out_[slots[m]].fit, trials[m]);
      if (m > 0) {
        mark_repaired(slots[m]);
        out_[slots[m]].calendar_joint_partner_moves += moved[m];
      }
    }
    out_[i].calendar_joint_repaired = true;
    return true;
  }

  // Largest distance to market (spreads) over expiry i's served rows (Success or
  // PriceOnly with a valid quote); `checked` (nullable) receives how many it measured.
  [[nodiscard]] double market_distance(std::size_t i, std::size_t *checked = nullptr) const {
    const std::span<const QuoteSliceQuote> quotes = expiries_[i].quotes;
    const std::vector<QuoteSliceRow> &rows = out_[i].fit.rows;
    double worst = 0.0;
    std::size_t count = 0;
    for (std::size_t k = 0; k < quotes.size() && k < rows.size(); ++k) {
      const QuoteSliceRow &row = rows[k];
      if (row.evaluation_status != QuoteSliceEvaluationStatus::Success &&
          row.evaluation_status != QuoteSliceEvaluationStatus::PriceOnly) {
        continue;
      }
      const double d = market_distance_spreads(row.fitted_price, quotes[k].bid, quotes[k].ask,
                                               config_.repair.box_tick);
      if (std::isnan(d)) continue;
      ++count;
      worst = std::max(worst, d);
    }
    if (checked != nullptr) *checked = count;
    return worst;
  }

  // The distance-to-market certificate (Gate / Repair), last, on the final served
  // set in maturity order: under Repair a slice with a row beyond the bound first
  // gets the in-box market repair against every other served slice
  // (repair_quote_slice_market); then every served row still beyond the bound is
  // refused row by row (MarketDistanceFailure: no volatility, not a served price).
  void market_distance_pass() {
    if (config_.repair.market_distance == QuoteSliceMarketDistance::Off) return;
    const double bound = config_.repair.market_distance_spreads;
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < out_.size(); ++i) {
      if (served(i) && out_[i].fit.prices) order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
      return expiries_[a].market.calendar_years < expiries_[b].market.calendar_years;
    });
    for (const std::size_t i : order) {
      QuoteSurfaceSlice &slice = out_[i];
      slice.market_max_spreads = market_distance(i, &slice.market_checked_rows);
      if (market_repair_on() && slice.market_max_spreads > bound) {
        std::vector<std::size_t> others;
        for (const std::size_t j : served_except(i)) {
          if (out_[j].fit.prices) others.push_back(j);
        }
        std::vector<const AmericanPriceSlice *> earlier;
        std::vector<const AmericanPriceSlice *> later;
        neighbours(slice.fit, others, earlier, later);
        QuoteSliceFit trial = trial_copy(slice.fit);
        if (repair_quote_slice_market(trial, expiries_[i].quotes, earlier, later, config_.repair,
                                      &slice.market_repair)) {
          commit(slice.fit, trial);
        }
      }
      slice.market_max_spreads_after = market_distance(i);
      gate_rows(i, bound);
    }
  }

  void gate_rows(std::size_t i, double bound) {
    QuoteSurfaceSlice &slice = out_[i];
    const std::span<const QuoteSliceQuote> quotes = expiries_[i].quotes;
    for (std::size_t k = 0; k < quotes.size() && k < slice.fit.rows.size(); ++k) {
      QuoteSliceRow &row = slice.fit.rows[k];
      if (row.evaluation_status != QuoteSliceEvaluationStatus::Success &&
          row.evaluation_status != QuoteSliceEvaluationStatus::PriceOnly) {
        continue;
      }
      const double d = market_distance_spreads(row.fitted_price, quotes[k].bid, quotes[k].ask,
                                               config_.repair.box_tick);
      if (!(d > bound)) continue;
      row.evaluation_status = QuoteSliceEvaluationStatus::MarketDistanceFailure;
      row.fitted_iv = kNaN;
      auto &produced = slice.fit.diagnostics.prices_produced;
      if (produced > 0) --produced;
      ++slice.market_gated_rows;
    }
  }

  // A repair trial: everything a repair reads or writes, with an owned curve.
  [[nodiscard]] static QuoteSliceFit trial_copy(const QuoteSliceFit &fit) {
    QuoteSliceFit trial;
    trial.market = fit.market;
    trial.dividend_model = fit.dividend_model;
    trial.al_opts = fit.al_opts;
    trial.discrete_div_steps = fit.discrete_div_steps;
    trial.discrete_div_scheme = fit.discrete_div_scheme;
    trial.variance_clock_dividends = fit.variance_clock_dividends;
    trial.carry_curve = fit.carry_curve;
    trial.diagnostics = fit.diagnostics;
    trial.rows = fit.rows;
    trial.prices = fit.prices;
    trial.moved_nodes = fit.moved_nodes;
    trial.price_only_nodes = fit.price_only_nodes;
    // The calendar repair re-implies moved rows under the fit's identifiability policy
    // (lane C0): a trial without it would re-imply under Legacy.
    trial.floor_policy = fit.floor_policy;
    trial.snapshot = fit.snapshot;
    trial.curve = fit.curve ? fit.curve->clone() : nullptr;
    return trial;
  }

  static void commit(QuoteSliceFit &fit, QuoteSliceFit &trial) {
    fit.prices = std::move(trial.prices);
    fit.moved_nodes = std::move(trial.moved_nodes);
    fit.price_only_nodes = std::move(trial.price_only_nodes);
    fit.rows = std::move(trial.rows);
  }

  void mark_repaired(std::size_t i) {
    out_[i].calendar_repaired = true;
    if (out_[i].grade == QuoteSliceGrade::A) out_[i].grade = QuoteSliceGrade::B;
  }

  static void strip(QuoteSliceFit &fit) {
    fit.prices.reset();
    for (QuoteSliceRow &row : fit.rows) {
      row.fitted_iv = kNaN;
      row.fitted_price = kNaN;
      if (row.evaluation_status == QuoteSliceEvaluationStatus::Success ||
          row.evaluation_status == QuoteSliceEvaluationStatus::PriceOnly) {
        row.evaluation_status = QuoteSliceEvaluationStatus::NoFit;
      }
    }
    if (fit.fit_status == QuoteSliceFitStatus::Success) {
      fit.fit_status = QuoteSliceFitStatus::ArbitrageFailure;
    }
  }

  std::span<const QuoteSurfaceExpiry> expiries_;
  QuoteSliceConfig config_;      // the caller's, with the surface's opt-in wing floor
  QuoteSliceConfig base_config_; // config_ without the channel repair (pass A)
  const QuoteSurfaceConfig &surface_;
  std::vector<QuoteSurfaceSlice> out_;
  std::vector<std::size_t> breadth_;
  std::vector<std::size_t> two_sided_; // ladder only: strictly two-sided quotes
  std::vector<std::uint8_t> contained_; // ladder only: contained() at the calendar pass
  // Ladder only: the in-market point fit of a knot-served expiry (calendar rung).
  std::vector<std::optional<QuoteSliceFit>> held_point_;
  std::vector<std::size_t> cost_; // fan-out order key: strictly two-sided quotes
  // Opt-in per-expiry fit timing: armed only when the caller armed a SurfaceTrace
  // on its thread (else empty); slot i is written only while expiry i is fitted.
  counters::fit_timing::SurfaceTrace *trace_;
  mutable std::vector<counters::fit_timing::ChainRecorder> timing_;
  // Opt-in rung memo per expiry (surface_.rung_memo, else empty); slot i is touched
  // only while expiry i is fitted, by the one thread fitting it (rung_memo.hpp).
  mutable std::vector<detail::ExpiryRungMemo> memos_;
};

// fit_quote_surface's typed error for an exception that escaped the fit: a worker's
// exception rethrown on the calling thread after the fan-out's join (for example
// std::bad_alloc), a std::system_error from creating a worker thread, or any throw of a
// serial stage. The fit's slots are discarded, never returned. Call only inside a catch
// handler.
[[nodiscard]] Status caught_exception() {
  try {
    throw;
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "quote surface: out of memory");
  } catch (const std::system_error &e) {
    return Err(ErrorCode::Unavailable, std::string("quote surface: system error: ") + e.what());
  } catch (const std::exception &e) {
    return Err(ErrorCode::Internal, std::string("quote surface: ") + e.what());
  } catch (...) {
    return Err(ErrorCode::Internal, "quote surface: unknown exception");
  }
}

} // namespace

const char *to_string(QuoteSurfaceMethod value) noexcept {
  switch (value) {
  case QuoteSurfaceMethod::Point:
    return "point";
  case QuoteSurfaceMethod::Channel:
    return "channel";
  case QuoteSurfaceMethod::ChannelRefit:
    return "channel_refit";
  }
  return "unknown";
}

const char *to_string(QuoteSurfaceLadderFallback value) noexcept {
  switch (value) {
  case QuoteSurfaceLadderFallback::None:
    return "none";
  case QuoteSurfaceLadderFallback::Certificate:
    return "certificate";
  case QuoteSurfaceLadderFallback::KnotFailure:
    return "knot_failure";
  case QuoteSurfaceLadderFallback::Containment:
    return "containment";
  case QuoteSurfaceLadderFallback::Calendar:
    return "calendar";
  }
  return "unknown";
}

const char *to_string(QuoteSurfaceWingTail value) noexcept {
  switch (value) {
  case QuoteSurfaceWingTail::Off:
    return "off";
  case QuoteSurfaceWingTail::FloorPriceOnly:
    return "floor-price-only";
  case QuoteSurfaceWingTail::FloorPriceOnlyWingFloor:
    return "floor-price-only+wing-floor";
  }
  return "unknown";
}

Result<std::vector<QuoteSurfaceSlice>>
fit_quote_surface(std::span<const QuoteSurfaceExpiry> expiries, const QuoteSliceConfig &config,
                  const QuoteSurfaceConfig &surface) {
  ATX_TRY_VOID(validate_surface(surface));
  // Shared configuration: refuse it once rather than as a per-expiry failure.
  ATX_TRY_VOID(validate_quote_slice_channel(config.channel));
  ATX_TRY_VOID(validate_quote_slice_repair(config.repair));
  if (config.carry.mode != QuoteSliceCarryMode::Supplied) {
    return Err(ErrorCode::InvalidArgument,
               "quote surface: graded admission requires supplied carry");
  }
  // The ladder's own knot rung and containment rule replace a knot-spline pass A
  // and the dominance gate; combining them is refused rather than guessed.
  if (surface.ladder == QuoteSurfaceLadder::KnotFirst &&
      (config.method != QuoteSliceFitMethod::PointCurve ||
       config.channel.point_max_outside_fraction.has_value())) {
    return Err(ErrorCode::InvalidArgument,
               "quote surface: the knot-first ladder requires a point profile without the "
               "dominance gate");
  }
  if (expiries.size() > kQuoteSurfaceMaxExpiries) {
    return Err(ErrorCode::InvalidArgument, "quote surface: too many expiries");
  }
  std::vector<double> maturities;
  maturities.reserve(expiries.size());
  for (const QuoteSurfaceExpiry &e : expiries) {
    maturities.push_back(e.market.calendar_years);
  }
  std::sort(maturities.begin(), maturities.end());
  if (std::adjacent_find(maturities.begin(), maturities.end()) != maturities.end()) {
    return Err(ErrorCode::InvalidArgument, "quote surface: duplicate calendar maturity");
  }
  // Specified to return errors: no exception (a worker's, rethrown after the
  // fan-out's join, or a serial stage's) escapes.
  try {
    SurfaceFitter fitter(expiries, config, surface);
    auto slices = fitter.run();
    if (slices) {
      // Lane C0: the caller's snapshot provenance rides on each expiry's fit.
      for (std::size_t i = 0; i < slices->size(); ++i) {
        (*slices)[i].fit.snapshot = expiries[i].snapshot;
      }
    }
    return slices;
  } catch (...) {
    return Err(caught_exception().error());
  }
}

} // namespace atx::vol
