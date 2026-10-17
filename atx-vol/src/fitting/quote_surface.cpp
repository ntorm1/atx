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
// (config.repair) and dominance gate (config.channel.point_max_outside_fraction)
// add fallback rungs (repair_rung, point_fallback, repair_in_channel): at most
// one more pass-A fit and two channel calendar repairs per expiry.
#include "atx/vol/api/fitting/quote_slice.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
      s.calendar_repair_half_spreads < 0.0) {
    return Err(ErrorCode::InvalidArgument, "quote surface: invalid graded-admission configuration");
  }
  return Ok();
}

// Share of the strictly two-sided quotes whose served price lies outside
// [bid, ask]; quotes without a served price are not counted.
[[nodiscard]] double outside_fraction(const QuoteSliceFit &fit,
                                      std::span<const QuoteSliceQuote> quotes) {
  std::size_t two_sided = 0;
  std::size_t outside = 0;
  for (std::size_t i = 0; i < quotes.size() && i < fit.rows.size(); ++i) {
    const QuoteSliceQuote &quote = quotes[i];
    const QuoteSliceRow &row = fit.rows[i];
    if (!positive(quote.bid) || !std::isfinite(quote.ask) || !(quote.ask > quote.bid) ||
        row.evaluation_status != QuoteSliceEvaluationStatus::Success) {
      continue;
    }
    ++two_sided;
    if (row.fitted_price < quote.bid || row.fitted_price > quote.ask) ++outside;
  }
  return two_sided == 0 ? 0.0
                        : static_cast<double>(outside) / static_cast<double>(two_sided);
}

// Raw two-sided breadth: distinct strikes carrying a strictly two-sided quote.
[[nodiscard]] std::size_t two_sided_strikes(std::span<const QuoteSliceQuote> quotes) {
  std::vector<double> strikes;
  strikes.reserve(quotes.size());
  for (const QuoteSliceQuote &quote : quotes) {
    if (positive(quote.strike) && positive(quote.bid) && std::isfinite(quote.ask) &&
        quote.ask > quote.bid) {
      strikes.push_back(quote.strike);
    }
  }
  std::sort(strikes.begin(), strikes.end());
  return static_cast<std::size_t>(std::unique(strikes.begin(), strikes.end()) - strikes.begin());
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

class SurfaceFitter {
public:
  SurfaceFitter(std::span<const QuoteSurfaceExpiry> expiries, const QuoteSliceConfig &config,
                const QuoteSurfaceConfig &surface)
      : expiries_(expiries), config_(config), base_config_(without_repair(config)),
        surface_(surface), out_(expiries.size()), breadth_(expiries.size()),
        gated_(expiries.size()) {}

  [[nodiscard]] Result<std::vector<QuoteSurfaceSlice>> run() {
    point_pass();
    ATX_TRY_VOID(channel_passes());
    repair_rung();
    point_fallback();
    ATX_TRY_VOID(calendar_pass());
    for (QuoteSurfaceSlice &slice : out_) {
      if (slice.grade == QuoteSliceGrade::Refused) {
        strip(slice.fit);
      } else if (slice.grade == QuoteSliceGrade::A && !slice.fit.moved_nodes.empty()) {
        // Pass-A tier, but a repair moved its served prices off its fit. The
        // tier (prior node role, acceptance order) is unchanged; only the label.
        slice.grade = QuoteSliceGrade::B;
      }
    }
    return Ok(std::move(out_));
  }

private:
  void point_pass() {
    for (std::size_t i = 0; i < expiries_.size(); ++i) {
      const QuoteSurfaceExpiry &e = expiries_[i];
      breadth_[i] = two_sided_strikes(e.quotes);
      auto fit = fit_quote_slice(e.market, e.quotes, base_config_, e.calendar_dividends);
      if (!fit) {
        out_[i].refusal = "invalid_input";
        out_[i].error = fit.error().to_string();
        continue;
      }
      out_[i].point_status = fit->fit_status;
      out_[i].point_error = fit->fit_error;
      out_[i].fit = std::move(*fit);
      const auto &diag = out_[i].fit.diagnostics;
      if (out_[i].fit.fit_status == QuoteSliceFitStatus::Success &&
          diag.quotes_admitted >= config_.min_quotes &&
          diag.distinct_strikes >= config_.min_quotes) {
        out_[i].grade = QuoteSliceGrade::A;
        gate(i);
      }
    }
  }

  // Channel dominance (opt-in): a point fit outside the two-sided quotes on more
  // than the configured share is no pass-A evidence. It becomes a channel
  // candidate; the point fit is kept only as the fallback of a refused refit.
  void gate(std::size_t i) {
    const auto &limit = config_.channel.point_max_outside_fraction;
    if (!limit || config_.method != QuoteSliceFitMethod::PointCurve) return;
    QuoteSurfaceSlice &slice = out_[i];
    slice.point_outside_fraction = outside_fraction(slice.fit, expiries_[i].quotes);
    if (slice.point_outside_fraction <= *limit) return;
    slice.point_gated = true;
    slice.grade = QuoteSliceGrade::Refused;
    gated_[i] = std::move(slice.fit);
    slice.fit = QuoteSliceFit{};
    // The candidate keeps the point fit's row provenance until a refit replaces it.
    slice.fit.rows = gated_[i]->rows;
    slice.fit.diagnostics = gated_[i]->diagnostics;
    slice.fit.market = gated_[i]->market;
    slice.fit.fit_status = QuoteSliceFitStatus::ArbitrageFailure;
  }

  // A gated slice whose channel refit was not served falls back to its point fit
  // (grade B: it failed the containment evidence for A).
  void point_fallback() {
    for (std::size_t i = 0; i < out_.size(); ++i) {
      QuoteSurfaceSlice &slice = out_[i];
      if (!gated_[i] || slice.grade != QuoteSliceGrade::Refused) continue;
      slice.fit = std::move(*gated_[i]);
      gated_[i].reset();
      slice.grade = QuoteSliceGrade::B;
      slice.refusal.clear();
      slice.error.clear();
      slice.channel_refit = false;
      slice.point_fallback = true;
    }
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
    }
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
    // B1: prior from point-fitted expiries only.
    ATX_TRY(const TermPrior point_prior, make_prior(nodes(false, nullptr), surface_));
    for (const std::size_t i : candidates) {
      auto fit = channel_fit(i, point_prior, 1.0);
      if (!fit) {
        out_[i].refusal = "fit_failure";
        out_[i].error = fit.error().to_string();
        continue;
      }
      adopt(i, std::move(*fit));
    }
    // B2: leave-one-out prior from every fitted expiry, B1 nodes down-weighted.
    ATX_TRY(const TermPrior full_prior, make_prior(nodes(true, nullptr), surface_));
    for (const std::size_t i : candidates) {
      if (!out_[i].refusal.empty()) continue;
      auto fit = channel_fit(i, full_prior, 1.0);
      if (fit && fit->fit_status == QuoteSliceFitStatus::Success) {
        adopt(i, std::move(*fit));
      } else if (out_[i].fit.fit_status != QuoteSliceFitStatus::Success && fit) {
        adopt(i, std::move(*fit)); // keep the latest diagnostics of a failed slice
      }
      if (out_[i].grade == QuoteSliceGrade::Refused) {
        out_[i].refusal = to_string(out_[i].fit.fit_status);
        out_[i].error = out_[i].fit.fit_error;
      }
    }
    return Ok();
  }

  [[nodiscard]] bool consistent(const QuoteSliceFit &fit,
                                const std::vector<std::size_t> &accepted) const {
    for (const std::size_t j : accepted) {
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
    std::sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
      if (out_[a].grade != out_[b].grade) return out_[a].grade < out_[b].grade;
      if (evidence(a) != evidence(b)) return evidence(a) > evidence(b);
      return expiries_[a].market.calendar_years < expiries_[b].market.calendar_years;
    });
    std::vector<std::size_t> accepted;
    std::vector<std::size_t> rejected;
    accepted.reserve(order.size());
    for (const std::size_t i : order) {
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
      measure_gap(i, accepted);
      refuse_calendar(i);
    }
    return Ok();
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
    commit(fit, trial);
    return true;
  }

  // Opt-in channel calendar repair (config.repair.mode == Channel), the same
  // contract as repair(); it runs only after the bounded-move repair and the
  // stronger-prior retry have both failed.
  [[nodiscard]] bool repair_in_channel(QuoteSliceFit &fit, std::size_t i,
                                       const std::vector<std::size_t> &accepted) {
    if (config_.repair.mode != QuoteSliceRepairMode::Channel || !fit.prices) return false;
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
    commit(fit, trial);
    out_[i].calendar_channel_repaired = true;
    return true;
  }

  // A repair trial: everything a repair reads or writes, with an owned curve.
  [[nodiscard]] static QuoteSliceFit trial_copy(const QuoteSliceFit &fit) {
    QuoteSliceFit trial;
    trial.market = fit.market;
    trial.dividend_model = fit.dividend_model;
    trial.al_opts = fit.al_opts;
    trial.discrete_div_steps = fit.discrete_div_steps;
    trial.variance_clock_dividends = fit.variance_clock_dividends;
    trial.carry_curve = fit.carry_curve;
    trial.diagnostics = fit.diagnostics;
    trial.rows = fit.rows;
    trial.prices = fit.prices;
    trial.moved_nodes = fit.moved_nodes;
    trial.curve = fit.curve ? fit.curve->clone() : nullptr;
    return trial;
  }

  static void commit(QuoteSliceFit &fit, QuoteSliceFit &trial) {
    fit.prices = std::move(trial.prices);
    fit.moved_nodes = std::move(trial.moved_nodes);
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
      if (row.evaluation_status == QuoteSliceEvaluationStatus::Success) {
        row.evaluation_status = QuoteSliceEvaluationStatus::NoFit;
      }
    }
    if (fit.fit_status == QuoteSliceFitStatus::Success) {
      fit.fit_status = QuoteSliceFitStatus::ArbitrageFailure;
    }
  }

  std::span<const QuoteSurfaceExpiry> expiries_;
  const QuoteSliceConfig &config_;
  QuoteSliceConfig base_config_; // config_ without the channel repair (pass A)
  const QuoteSurfaceConfig &surface_;
  std::vector<QuoteSurfaceSlice> out_;
  std::vector<std::size_t> breadth_;
  std::vector<std::optional<QuoteSliceFit>> gated_; // dominance-gated point fits
};

} // namespace

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
  SurfaceFitter fitter(expiries, config, surface);
  return fitter.run();
}

} // namespace atx::vol
