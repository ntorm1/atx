#include "atx/vol/api/marketdata/implied_forward.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "atx/vol/api/pricing/american_iv.hpp"

namespace atx::vol {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kSqrtTwoPi = 2.5066282746310002;
// Scales a median absolute deviation to a normal standard deviation.
constexpr double kMadToSigma = 1.482602218505602;
// Bounds on the straddle-implied ATM total volatility used as the moneyness
// unit. They only guard the unit against degenerate quotes; they do not
// pretend to measure volatility.
constexpr double kMinTotalVol = 1.0e-4;
constexpr double kMaxTotalVol = 5.0;
// A pair is an outlier when it sits beyond this many robust scales from the
// location AND its own bid/ask channel half-width does not reach the location.
constexpr double kRetainScales = 3.0;
constexpr std::size_t kMaxRobustIterations = 64;
// A single Newton step of the alignment fixed point moves the forward by at
// most this many ATM standard deviations; larger corrections take more steps.
constexpr double kMaxStepSd = 0.25;
// Bounded |rate * years| so exp() of the discount cannot overflow.
constexpr double kMaxLogDiscount = 64.0;
constexpr std::size_t kSkipNone = kImpliedForwardMaxSelected;

bool positive(double value) noexcept { return value > 0.0 && std::isfinite(value); }
bool nonnegative(double value) noexcept { return value >= 0.0 && std::isfinite(value); }

// A point quote needs a positive bid; a missing bid is only an upper bound.
bool two_sided(double bid, double ask) noexcept {
  return positive(bid) && std::isfinite(ask) && ask >= bid;
}

bool usable(const ImpliedForwardPair &pair) noexcept {
  return two_sided(pair.call_bid, pair.call_ask) && two_sided(pair.put_bid, pair.put_ask);
}

double mid(double bid, double ask) noexcept { return 0.5 * bid + 0.5 * ask; }

// Working state of one selected pair. Fixed capacity: the whole strip lives on
// the stack, so the Parity stage never allocates.
struct Candidate {
  std::size_t index = 0;
  double distance = 0.0; // |ln(K / provisional forward)|
  double strike = 0.0;
  double call_mid = 0.0;
  double put_mid = 0.0;
  double half_width = 0.0; // bid/ask half-width of this pair's forward
  double parity = 0.0;     // European parity forward
  double lower = 0.0;      // European parity channel
  double upper = 0.0;
  double value = 0.0; // forward under evaluation (parity or exercise-corrected)
  double weight = 0.0;
  double huber = 0.0;
  double sigma = kNaN;
  Side anchor = Side::Call;
  std::uint8_t iterations = 0;
  ImpliedForwardPairStatus status = ImpliedForwardPairStatus::Retained;
  bool active = false; // eligible for the robust location
};

using Strip = std::array<Candidate, kImpliedForwardMaxSelected>;
using Values = std::array<double, kImpliedForwardMaxSelected>;

struct Location {
  double center = kNaN;
  double scale = kNaN;
};

const char *validate(const ImpliedForwardMarket &market, std::span<const ImpliedForwardPair> pairs,
                     const ImpliedForwardConfig &cfg) noexcept {
  if (!positive(market.spot) || !positive(market.years) || !std::isfinite(market.rate) ||
      std::fabs(market.rate * market.years) > kMaxLogDiscount ||
      (market.reference_yield && !std::isfinite(*market.reference_yield))) {
    return "invalid spot, clock, rate or reference yield";
  }
  if (cfg.min_pairs < 1 || cfg.max_pairs < cfg.min_pairs ||
      cfg.max_pairs > kImpliedForwardMaxSelected || !positive(cfg.band_sd) ||
      !positive(cfg.spread_floor) || !(cfg.max_weight_ratio >= 1.0) ||
      !std::isfinite(cfg.max_weight_ratio) || !positive(cfg.huber_k) ||
      !positive(cfg.max_dispersion_sd) || !positive(cfg.max_leave_one_out_sd) ||
      !(cfg.max_outlier_fraction >= 0.0 && cfg.max_outlier_fraction <= 1.0) ||
      !positive(cfg.max_exercise_shift_sd) || !positive(cfg.max_pair_exercise_shift_sd) ||
      !nonnegative(cfg.min_time_value) || !nonnegative(cfg.min_time_value_fraction) ||
      !positive(cfg.iv_tolerance) || cfg.max_iv_iterations == 0 ||
      !positive(cfg.fixed_point_tolerance) || cfg.max_fixed_point_iterations == 0 ||
      cfg.max_fixed_point_iterations > 255 ||
      (cfg.exercise != ImpliedForwardExercise::European &&
       cfg.exercise != ImpliedForwardExercise::American) ||
      (cfg.method != ImpliedForwardMethod::Parity &&
       cfg.method != ImpliedForwardMethod::AmericanAlignment)) {
    return "invalid implied-forward configuration";
  }
  if (pairs.size() > kImpliedForwardMaxInput) {
    return "more pairs than kImpliedForwardMaxInput";
  }
  double previous = 0.0;
  for (const ImpliedForwardPair &pair : pairs) {
    if (!positive(pair.strike) || !(pair.strike > previous)) {
      return "strikes must be finite, positive and strictly increasing";
    }
    previous = pair.strike;
  }
  return nullptr;
}

double parity_forward(double strike, double call_mid, double put_mid, double df) noexcept {
  return strike + (call_mid - put_mid) / df;
}

// Keep the `limit` usable pairs nearest to `forward` in log-moneyness, ordered
// by increasing distance. Ties keep the lower strike first (input order), so the
// selection is deterministic.
std::size_t select_nearest(std::span<const ImpliedForwardPair> pairs, double forward,
                           std::size_t limit, Strip &strip) noexcept {
  std::size_t count = 0;
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    if (!usable(pairs[i])) {
      continue;
    }
    const double distance = std::fabs(std::log(pairs[i].strike / forward));
    if (count == limit && !(distance < strip[count - 1].distance)) {
      continue;
    }
    std::size_t slot = count < limit ? count++ : limit - 1;
    while (slot > 0 && distance < strip[slot - 1].distance) {
      strip[slot] = strip[slot - 1];
      --slot;
    }
    strip[slot] = Candidate{};
    strip[slot].index = i;
    strip[slot].distance = distance;
  }
  return count;
}

// Weighted median: the smallest value whose cumulative weight reaches half the
// total. `values`/`weights` are scratch copies (reordered in place). n <= 32,
// so an insertion sort is the bounded, allocation-free choice.
double weighted_median(Values values, Values weights, std::size_t n) noexcept {
  for (std::size_t i = 1; i < n; ++i) {
    std::size_t j = i;
    while (j > 0 && values[j] < values[j - 1]) {
      std::swap(values[j], values[j - 1]);
      std::swap(weights[j], weights[j - 1]);
      --j;
    }
  }
  double total = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    total += weights[i];
  }
  double cumulative = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    cumulative += weights[i];
    if (cumulative >= 0.5 * total) {
      return values[i];
    }
  }
  return n > 0 ? values[n - 1] : kNaN;
}

// Deterministic weighted Huber location over the active candidates, skipping
// candidate `skip` (kSkipNone skips nothing). Starts at the weighted median;
// the scale is the weighted MAD floored at half the median bid/ask half-width,
// so perfectly agreeing pairs are not mistaken for an infinitely precise strip.
Location robust_location(Strip &strip, std::size_t n, double huber_k, std::size_t skip,
                         bool store_huber) noexcept {
  Values values{};
  Values weights{};
  Values widths{};
  std::size_t m = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (!strip[i].active || i == skip) {
      continue;
    }
    values[m] = strip[i].value;
    weights[m] = strip[i].weight;
    widths[m] = strip[i].half_width;
    ++m;
  }
  if (m == 0) {
    return {};
  }
  double center = weighted_median(values, weights, m);
  Values deviations{};
  for (std::size_t k = 0; k < m; ++k) {
    deviations[k] = std::fabs(values[k] - center);
  }
  const double mad = kMadToSigma * weighted_median(deviations, weights, m);
  const double floor = 0.5 * weighted_median(widths, weights, m);
  const double scale = std::max({mad, floor, 1.0e-12 * std::fabs(center)});
  for (std::size_t iteration = 0; iteration < kMaxRobustIterations; ++iteration) {
    double numerator = 0.0;
    double denominator = 0.0;
    for (std::size_t k = 0; k < m; ++k) {
      const double residual = std::fabs(values[k] - center) / scale;
      const double h = residual <= huber_k ? 1.0 : huber_k / residual;
      numerator += weights[k] * h * values[k];
      denominator += weights[k] * h;
    }
    const double next = numerator / denominator;
    const bool converged = std::fabs(next - center) <= 1.0e-14 * std::fabs(center);
    center = next;
    if (converged) {
      break;
    }
  }
  if (store_huber) {
    for (std::size_t i = 0; i < n; ++i) {
      if (!strip[i].active || i == skip) {
        continue;
      }
      const double residual = std::fabs(strip[i].value - center) / scale;
      strip[i].huber = residual <= huber_k ? 1.0 : huber_k / residual;
    }
  }
  return {center, scale};
}

// Two-pass robust location: mark outliers against the first pass, then locate
// the retained pairs alone. Returns the retained count.
std::size_t locate_retained(Strip &strip, std::size_t n, double huber_k, Location &out) noexcept {
  const Location first = robust_location(strip, n, huber_k, kSkipNone, false);
  if (!std::isfinite(first.center)) {
    out = {};
    return 0;
  }
  std::size_t retained = 0;
  for (std::size_t i = 0; i < n; ++i) {
    Candidate &c = strip[i];
    if (!c.active) {
      continue;
    }
    const double reach = std::max(kRetainScales * first.scale, c.half_width);
    if (std::fabs(c.value - first.center) > reach) {
      c.active = false;
      c.status = ImpliedForwardPairStatus::Outlier;
      c.huber = 0.0;
    } else {
      ++retained;
    }
  }
  out = robust_location(strip, n, huber_k, kSkipNone, true);
  return retained;
}

// ATM total volatility from the straddle time value of the pair nearest the
// forward: (C + P)/DF - |F - K| ~= 2 F sigma sqrt(T) / sqrt(2 pi) near the money.
double straddle_total_vol(const Candidate &c, double forward, double df) noexcept {
  const double time_value = (c.call_mid + c.put_mid) / df - std::fabs(forward - c.strike);
  const double total = time_value * kSqrtTwoPi / (2.0 * forward);
  return std::clamp(std::isfinite(total) ? total : kMinTotalVol, kMinTotalVol, kMaxTotalVol);
}

bool exercise_bound(double leg_mid, double intrinsic, const ImpliedForwardConfig &cfg) noexcept {
  return !(leg_mid > intrinsic + cfg.min_time_value + cfg.min_time_value_fraction * intrinsic);
}

struct NewtonStep {
  double next = kNaN;
  double sigma = kNaN;
  ImpliedForwardPairStatus status = ImpliedForwardPairStatus::Retained;
};

// One Newton step of the American call/put IV alignment at forward x: invert the
// anchor (OTM) leg at the q_eff that reproduces x, reprice the other leg at that
// volatility and move x by the repricing error over DF (see the header).
NewtonStep alignment_step(const Candidate &c, double x, const ImpliedForwardMarket &market,
                          double df, double total_vol, double warm,
                          const ImpliedForwardConfig &cfg) {
  const Side other = c.anchor == Side::Call ? Side::Put : Side::Call;
  const double anchor_mid = c.anchor == Side::Call ? c.call_mid : c.put_mid;
  const double other_mid = c.anchor == Side::Call ? c.put_mid : c.call_mid;
  const double q = market.rate - std::log(x / market.spot) / market.years;
  const Result<double> sigma = american_implied_vol(
      anchor_mid, market.spot, c.strike, market.years, market.rate, q, c.anchor,
      AmericanMethod::AndersenLake, cfg.iv_tolerance, cfg.max_iv_iterations, cfg.al_opts, nullptr,
      warm);
  if (!sigma || !(*sigma > kIvMin) || !(*sigma < kIvMax)) {
    return {kNaN, kNaN, ImpliedForwardPairStatus::Unidentified};
  }
  const Result<double> model = andersen_lake(market.spot, c.strike, market.years, *sigma,
                                             market.rate, q, other, cfg.al_opts);
  if (!model || !std::isfinite(*model)) {
    return {kNaN, *sigma, ImpliedForwardPairStatus::Unidentified};
  }
  // An ITM put that prices rich needs a HIGHER forward; an ITM call, a lower one.
  const double direction = other == Side::Put ? 1.0 : -1.0;
  const double cap = kMaxStepSd * total_vol * x;
  const double step = std::clamp(direction * (*model - other_mid) / df, -cap, cap);
  const double next = x + step;
  if (!positive(next)) {
    return {kNaN, *sigma, ImpliedForwardPairStatus::Contaminated};
  }
  return {next, *sigma, ImpliedForwardPairStatus::Retained};
}

// Run the bounded alignment fixed point for one pair, from its parity forward.
void align_pair(Candidate &c, const ImpliedForwardMarket &market, double df, double total_vol,
                const ImpliedForwardConfig &cfg) {
  double x = c.parity;
  double warm = 0.0;
  for (std::uint16_t iteration = 1; iteration <= cfg.max_fixed_point_iterations; ++iteration) {
    const NewtonStep step = alignment_step(c, x, market, df, total_vol, warm, cfg);
    c.iterations = static_cast<std::uint8_t>(iteration);
    c.sigma = step.sigma;
    if (step.status != ImpliedForwardPairStatus::Retained) {
      c.status = step.status;
      c.active = false;
      return;
    }
    warm = step.sigma;
    const bool converged = std::fabs(step.next - x) <= cfg.fixed_point_tolerance * step.next;
    x = step.next;
    if (converged) {
      c.value = x;
      return;
    }
  }
  c.value = x;
  c.status = ImpliedForwardPairStatus::Contaminated;
  c.active = false;
}

// Dispersion and leave-one-out shift of the retained strip around `center`.
void spread_statistics(Strip &strip, std::size_t n, double huber_k, double center,
                       double &dispersion, double &max_loo) noexcept {
  double sum_w = 0.0;
  double sum_var = 0.0;
  std::size_t retained = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const Candidate &c = strip[i];
    if (!c.active) {
      continue;
    }
    const double w = c.weight * c.huber;
    sum_w += w;
    sum_var += w * (c.value - center) * (c.value - center);
    ++retained;
  }
  dispersion = sum_w > 0.0 ? std::sqrt(sum_var / sum_w) : 0.0;
  max_loo = 0.0;
  if (retained < 2) {
    return;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (!strip[i].active) {
      continue;
    }
    const Location loo = robust_location(strip, n, huber_k, i, false);
    if (std::isfinite(loo.center)) {
      max_loo = std::max(max_loo, std::fabs(loo.center - center));
    }
  }
}

void winsorize_weights(Strip &strip, std::size_t n, double ratio) noexcept {
  Values weights{};
  Values ones{};
  std::size_t m = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (strip[i].active) {
      weights[m] = strip[i].weight;
      ones[m] = 1.0;
      ++m;
    }
  }
  if (m == 0) {
    return;
  }
  const double cap = ratio * weighted_median(weights, ones, m);
  for (std::size_t i = 0; i < n; ++i) {
    strip[i].weight = std::min(strip[i].weight, cap);
  }
}

void set_status(ImpliedForwardResult &out, ImpliedForwardStatus status,
                const char *reason) noexcept {
  out.status = status;
  out.reason = reason;
}

// Copy the strip into the published diagnostics and the channel intersection.
void publish_strip(const Strip &strip, std::size_t n, ImpliedForwardResult &out) noexcept {
  double lower = -std::numeric_limits<double>::infinity();
  double upper = std::numeric_limits<double>::infinity();
  bool any = false;
  for (std::size_t i = 0; i < n; ++i) {
    const Candidate &c = strip[i];
    ImpliedForwardPairDiagnostic &d = out.pairs[i];
    const double shift = std::isfinite(c.value) ? c.value - c.parity : 0.0;
    d.strike = c.strike;
    d.parity_forward = c.parity;
    d.forward = c.value;
    d.exercise_shift = shift;
    d.lower = c.lower + shift;
    d.upper = c.upper + shift;
    d.weight = c.weight;
    d.robust_weight = c.active ? c.huber : 0.0;
    d.sigma = c.sigma;
    d.anchor_side = c.anchor;
    d.iterations = c.iterations;
    d.status = c.status;
    if (c.active) {
      lower = std::max(lower, d.lower);
      upper = std::min(upper, d.upper);
      any = true;
    }
    switch (c.status) {
    case ImpliedForwardPairStatus::Retained:
      ++out.pairs_retained;
      break;
    case ImpliedForwardPairStatus::ExerciseBound:
      ++out.pairs_exercise_bound;
      break;
    case ImpliedForwardPairStatus::Contaminated:
      ++out.pairs_contaminated;
      break;
    case ImpliedForwardPairStatus::Unidentified:
      ++out.pairs_unidentified;
      break;
    case ImpliedForwardPairStatus::Outlier:
      ++out.pairs_outlier;
      break;
    }
  }
  if (any) {
    out.forward_lower = lower;
    out.forward_upper = upper;
    out.channel_consistent = lower <= upper;
  }
}

// Build the near-money strip around the provisional forward. Returns its size.
std::size_t build_strip(std::span<const ImpliedForwardPair> pairs,
                        const ImpliedForwardMarket &market, const ImpliedForwardConfig &cfg,
                        double df, double provisional, double &total_vol, Strip &strip) noexcept {
  const std::size_t found = select_nearest(pairs, provisional, cfg.max_pairs, strip);
  if (found == 0) {
    return 0;
  }
  const ImpliedForwardPair &nearest = pairs[strip[0].index];
  Candidate atm{};
  atm.strike = nearest.strike;
  atm.call_mid = mid(nearest.call_bid, nearest.call_ask);
  atm.put_mid = mid(nearest.put_bid, nearest.put_ask);
  total_vol = straddle_total_vol(atm, provisional, df);
  const double band = cfg.band_sd * total_vol;
  std::size_t keep = 0;
  while (keep < found && strip[keep].distance <= band) {
    ++keep;
  }
  keep = std::min(std::max(keep, cfg.min_pairs), found);
  const bool american = cfg.exercise == ImpliedForwardExercise::American;
  for (std::size_t i = 0; i < keep; ++i) {
    Candidate &c = strip[i];
    const ImpliedForwardPair &p = pairs[c.index];
    c.strike = p.strike;
    c.call_mid = mid(p.call_bid, p.call_ask);
    c.put_mid = mid(p.put_bid, p.put_ask);
    const double call_spread = std::max(p.call_ask - p.call_bid, cfg.spread_floor);
    const double put_spread = std::max(p.put_ask - p.put_bid, cfg.spread_floor);
    c.half_width = 0.5 * (call_spread + put_spread) / df;
    c.parity = parity_forward(p.strike, c.call_mid, c.put_mid, df);
    c.lower = p.strike + (p.call_bid - p.put_ask) / df;
    c.upper = p.strike + (p.call_ask - p.put_bid) / df;
    c.value = c.parity;
    const double proximity = c.distance / band;
    c.weight = 1.0 / (c.half_width * c.half_width) / (1.0 + proximity * proximity);
    c.anchor = p.strike >= provisional ? Side::Call : Side::Put;
    c.active = std::isfinite(c.parity) && std::isfinite(c.weight) && c.weight > 0.0;
    c.status = c.active ? ImpliedForwardPairStatus::Retained : ImpliedForwardPairStatus::Outlier;
    if (american && c.active &&
        (exercise_bound(c.call_mid, std::max(market.spot - p.strike, 0.0), cfg) ||
         exercise_bound(c.put_mid, std::max(p.strike - market.spot, 0.0), cfg))) {
      c.active = false;
      c.status = ImpliedForwardPairStatus::ExerciseBound;
    }
  }
  winsorize_weights(strip, keep, cfg.max_weight_ratio);
  return keep;
}

// Provisional forward: the parity forward where |C - P| is smallest, refined by
// the weighted median of the min_pairs pairs nearest to it.
double provisional_forward(std::span<const ImpliedForwardPair> pairs,
                           const ImpliedForwardConfig &cfg, double df) noexcept {
  std::size_t seed = pairs.size();
  double best = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    if (!usable(pairs[i])) {
      continue;
    }
    const double gap = std::fabs(mid(pairs[i].call_bid, pairs[i].call_ask) -
                                 mid(pairs[i].put_bid, pairs[i].put_ask));
    if (gap < best) {
      best = gap;
      seed = i;
    }
  }
  if (seed == pairs.size()) {
    return kNaN;
  }
  const ImpliedForwardPair &s = pairs[seed];
  const double first = parity_forward(s.strike, mid(s.call_bid, s.call_ask),
                                      mid(s.put_bid, s.put_ask), df);
  if (!positive(first)) {
    return kNaN;
  }
  Strip strip{};
  const std::size_t n = select_nearest(pairs, first, cfg.min_pairs, strip);
  Values values{};
  Values weights{};
  for (std::size_t i = 0; i < n; ++i) {
    const ImpliedForwardPair &p = pairs[strip[i].index];
    values[i] =
        parity_forward(p.strike, mid(p.call_bid, p.call_ask), mid(p.put_bid, p.put_ask), df);
    const double width = std::max(p.call_ask - p.call_bid, cfg.spread_floor) +
                         std::max(p.put_ask - p.put_bid, cfg.spread_floor);
    weights[i] = 1.0 / (width * width);
  }
  const double refined = weighted_median(values, weights, n);
  return positive(refined) ? refined : first;
}

double sd_units(double value, double forward, double total_vol) noexcept {
  return value / forward / total_vol;
}

// Stage 2, AmericanAlignment: re-admit stage-1 outliers (the exercise premium
// varies by strike, so a pair can disagree with the European fit only because
// of early exercise), align every active pair, rescale the ATM unit from the
// aligned IVs, screen per-pair corrections and re-locate. Returns the retained
// count; `location` becomes the exercise-corrected location.
std::size_t align_strip(Strip &strip, std::size_t n, const ImpliedForwardMarket &market,
                        double df, const ImpliedForwardConfig &cfg, double &total_vol,
                        Location &location) {
  for (std::size_t i = 0; i < n; ++i) {
    Candidate &c = strip[i];
    if (c.status == ImpliedForwardPairStatus::Outlier && std::isfinite(c.parity) &&
        positive(c.weight)) {
      c.active = true;
      c.status = ImpliedForwardPairStatus::Retained;
    }
    if (!c.active) {
      continue;
    }
    c.anchor = c.strike >= location.center ? Side::Call : Side::Put;
    align_pair(c, market, df, total_vol, cfg);
  }
  // The aligned American IVs measure the ATM level better than a straddle.
  double sum_w = 0.0;
  double sum_ws = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    if (strip[i].active && std::isfinite(strip[i].sigma)) {
      sum_w += strip[i].weight;
      sum_ws += strip[i].weight * strip[i].sigma;
    }
  }
  if (sum_w > 0.0) {
    total_vol = std::clamp(sum_ws / sum_w * std::sqrt(market.years), kMinTotalVol, kMaxTotalVol);
  }
  for (std::size_t i = 0; i < n; ++i) {
    Candidate &c = strip[i];
    if (c.active && std::fabs(sd_units(c.value - c.parity, c.value, total_vol)) >
                        cfg.max_pair_exercise_shift_sd) {
      c.active = false;
      c.status = ImpliedForwardPairStatus::Contaminated;
    }
  }
  return locate_retained(strip, n, cfg.huber_k, location);
}

// Stage 2, Parity on an American chain: measure (never apply) the EEP bias
// with one alignment step per active pair, anchored at the stage-1 location.
// A pair whose step fails has no measured bias: it keeps the step's typed
// status (Unidentified when the IV inversion or repricing fails, Contaminated
// when the step leaves the positive forwards) and is deactivated, and the
// measured pairs are re-located (updating `retained`), so an unscreened pair
// never contributes to a published forward. Returns the weighted mean shift
// of the pairs that remain active, or NaN when none does.
double measure_parity_bias(Strip &strip, std::size_t n, const ImpliedForwardMarket &market,
                           double df, double total_vol, const ImpliedForwardConfig &cfg,
                           Location &location, std::size_t &retained) {
  Values shifts{};
  bool excluded = false;
  for (std::size_t i = 0; i < n; ++i) {
    Candidate &c = strip[i];
    if (!c.active) {
      continue;
    }
    c.anchor = c.strike >= location.center ? Side::Call : Side::Put;
    const NewtonStep step = alignment_step(c, c.parity, market, df, total_vol, 0.0, cfg);
    c.sigma = step.sigma;
    if (step.status != ImpliedForwardPairStatus::Retained) {
      c.status = step.status;
      c.active = false;
      c.huber = 0.0;
      excluded = true;
      continue;
    }
    shifts[i] = step.next - c.parity;
  }
  if (excluded) {
    retained = locate_retained(strip, n, cfg.huber_k, location);
  }
  double sum_w = 0.0;
  double sum_shift = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    if (!strip[i].active) {
      continue;
    }
    const double w = strip[i].weight * strip[i].huber;
    sum_w += w;
    sum_shift += w * shifts[i];
  }
  return sum_w > 0.0 ? sum_shift / sum_w : kNaN;
}

// Stage 3, after a finite positive forward: restate it as carry and measure
// the retained strip's dispersion and leave-one-out sensitivity.
void publish_carry(ImpliedForwardResult &out, Strip &strip, std::size_t n,
                   const ImpliedForwardMarket &market, double df, double total_vol,
                   double exercise_shift, bool american, double huber_k) noexcept {
  const double forward = out.forward;
  out.implied_yield = market.rate - std::log(forward / market.spot) / market.years;
  out.prepaid_deficit = market.spot - forward * df;
  if (market.reference_yield) {
    out.forward_offset =
        forward - market.spot * std::exp((market.rate - *market.reference_yield) * market.years);
  }
  double dispersion = 0.0;
  double max_loo = 0.0;
  spread_statistics(strip, n, huber_k, forward, dispersion, max_loo);
  out.dispersion = dispersion;
  out.dispersion_sd = sd_units(dispersion, forward, total_vol);
  out.max_leave_one_out_shift_sd = sd_units(max_loo, forward, total_vol);
  out.exercise_shift_sd = american ? sd_units(exercise_shift, forward, total_vol) : 0.0;
}

// Stage 3 refusal ladder, in order; Ok only when every gate passes.
void classify(ImpliedForwardResult &out, std::size_t retained, bool parity_on_american,
              const ImpliedForwardConfig &cfg) noexcept {
  const std::size_t lost = out.pairs_contaminated + out.pairs_unidentified +
                           out.pairs_exercise_bound;
  if (retained < cfg.min_pairs) {
    const bool lost_to_exercise = lost > 0 && retained + lost >= cfg.min_pairs;
    set_status(out, lost_to_exercise ? ImpliedForwardStatus::Contaminated
                                     : ImpliedForwardStatus::InsufficientPairs,
               lost_to_exercise ? "early exercise removed too many near-money pairs"
                                : "fewer retained near-money pairs than min_pairs");
    return;
  }
  if (static_cast<double>(out.pairs_outlier) >
      cfg.max_outlier_fraction * static_cast<double>(out.pairs_outlier + retained)) {
    set_status(out, ImpliedForwardStatus::Dispersion, "too many pairs rejected as outliers");
    return;
  }
  if (out.dispersion_sd > cfg.max_dispersion_sd) {
    set_status(out, ImpliedForwardStatus::Dispersion,
               "pair forwards disagree beyond max_dispersion_sd");
    return;
  }
  if (out.max_leave_one_out_shift_sd > cfg.max_leave_one_out_sd) {
    set_status(out, ImpliedForwardStatus::Dispersion,
               "one pair moves the forward beyond max_leave_one_out_sd");
    return;
  }
  if (parity_on_american && !(std::fabs(out.exercise_shift_sd) <= cfg.max_exercise_shift_sd)) {
    set_status(out, ImpliedForwardStatus::Contaminated,
               "parity forward carries early-exercise bias beyond max_exercise_shift_sd");
    return;
  }
  set_status(out, ImpliedForwardStatus::Ok, "ok");
}

} // namespace

const char *to_string(ImpliedForwardStatus value) noexcept {
  switch (value) {
  case ImpliedForwardStatus::Ok:
    return "ok";
  case ImpliedForwardStatus::InsufficientPairs:
    return "insufficient_pairs";
  case ImpliedForwardStatus::Dispersion:
    return "dispersion";
  case ImpliedForwardStatus::Contaminated:
    return "contaminated";
  case ImpliedForwardStatus::Invalid:
    return "invalid";
  }
  return "unknown";
}

const char *to_string(ImpliedForwardPairStatus value) noexcept {
  switch (value) {
  case ImpliedForwardPairStatus::Retained:
    return "retained";
  case ImpliedForwardPairStatus::Outlier:
    return "outlier";
  case ImpliedForwardPairStatus::ExerciseBound:
    return "exercise_bound";
  case ImpliedForwardPairStatus::Contaminated:
    return "contaminated";
  case ImpliedForwardPairStatus::Unidentified:
    return "unidentified";
  }
  return "unknown";
}

const char *to_string(ImpliedForwardMethod value) noexcept {
  switch (value) {
  case ImpliedForwardMethod::Parity:
    return "parity";
  case ImpliedForwardMethod::AmericanAlignment:
    return "american_alignment";
  }
  return "unknown";
}

ImpliedForwardResult estimate_implied_forward(const ImpliedForwardMarket &market,
                                              std::span<const ImpliedForwardPair> pairs,
                                              const ImpliedForwardConfig &cfg) {
  ImpliedForwardResult out;
  out.method = cfg.method;
  out.pairs_input = pairs.size();
  if (const char *invalid = validate(market, pairs, cfg)) {
    set_status(out, ImpliedForwardStatus::Invalid, invalid);
    return out;
  }
  const double df = std::exp(-market.rate * market.years);
  out.discount = df;
  for (const ImpliedForwardPair &pair : pairs) {
    out.pairs_valid += usable(pair) ? 1u : 0u;
  }
  if (out.pairs_valid < cfg.min_pairs) {
    set_status(out, ImpliedForwardStatus::InsufficientPairs,
               "fewer two-sided pairs than min_pairs");
    return out;
  }
  const double provisional = provisional_forward(pairs, cfg, df);
  if (!positive(provisional)) {
    set_status(out, ImpliedForwardStatus::InsufficientPairs, "parity forward is not positive");
    return out;
  }

  Strip strip{};
  double total_vol = kMinTotalVol;
  const std::size_t n = build_strip(pairs, market, cfg, df, provisional, total_vol, strip);
  out.pairs_selected = n;
  Location location{};
  std::size_t retained = locate_retained(strip, n, cfg.huber_k, location);
  out.parity_forward = location.center;
  const bool american = cfg.exercise == ImpliedForwardExercise::American;
  const bool align = american && cfg.method == ImpliedForwardMethod::AmericanAlignment;
  double exercise_shift = 0.0;
  if (align && std::isfinite(location.center)) {
    retained = align_strip(strip, n, market, df, cfg, total_vol, location);
    exercise_shift = location.center - out.parity_forward;
  } else if (american && std::isfinite(location.center)) {
    exercise_shift =
        measure_parity_bias(strip, n, market, df, total_vol, cfg, location, retained);
  }

  out.forward = location.center;
  out.atm_total_vol = total_vol;
  publish_strip(strip, n, out);
  if (!std::isfinite(location.center) || !positive(location.center)) {
    const bool lost_to_exercise =
        out.pairs_contaminated + out.pairs_unidentified + out.pairs_exercise_bound > 0;
    set_status(out, lost_to_exercise ? ImpliedForwardStatus::Contaminated
                                     : ImpliedForwardStatus::InsufficientPairs,
               "no pair supports a forward");
    return out;
  }
  publish_carry(out, strip, n, market, df, total_vol, exercise_shift, american, cfg.huber_k);
  classify(out, retained, american && !align, cfg);
  return out;
}

} // namespace atx::vol
