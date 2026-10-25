#include "atx/vol/api/fitting/american_price_slice.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "atx/vol/api/pricing/american.hpp"
#include "fitting/counters.hpp"
#include "fitting/price_slice_walk.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;

[[nodiscard]] double price_tolerance(double spot, double strike) noexcept {
  return 1.0e-8 + 1.0e-9 * std::max(spot, strike);
}

[[nodiscard]] Status node_error(std::size_t index, const char *reason) {
  return Err(ErrorCode::InvalidArgument,
             "AmericanPriceSlice: node " + std::to_string(index) + ": " + reason);
}

[[nodiscard]] bool valid_bounds(const AmericanPriceBounds &b) noexcept {
  return std::isfinite(b.spot) && b.spot > 0.0 && std::isfinite(b.discount_min) &&
         b.discount_min > 0.0 && std::isfinite(b.discount_max) &&
         b.discount_max >= b.discount_min && std::isfinite(b.prepaid_min) && b.prepaid_min >= 0.0 &&
         std::isfinite(b.prepaid_max) && b.prepaid_max >= b.prepaid_min && b.discount_min <= 1.0 &&
         b.discount_max >= 1.0 && b.prepaid_min <= b.spot && b.prepaid_max >= b.spot;
}

[[nodiscard]] Status validate_node(const AmericanPriceNode &node, const AmericanPriceBounds &bounds,
                                   std::size_t index) {
  if (!std::isfinite(node.strike) || !(node.strike > 0.0) || !std::isfinite(node.call) ||
      !std::isfinite(node.put) || node.call < 0.0 || node.put < 0.0) {
    return node_error(index, "strike/prices must be finite, strike positive, prices nonnegative");
  }
  const double tol = price_tolerance(bounds.spot, node.strike);
  const double discounted_max = node.strike * bounds.discount_max;
  const double discounted_min = node.strike * bounds.discount_min;
  if (!std::isfinite(discounted_max) || !std::isfinite(discounted_min)) {
    return node_error(index, "discounted strike is outside the finite numerical range");
  }
  if (node.call < std::max(bounds.spot - node.strike, 0.0) - tol ||
      node.put < std::max(node.strike - bounds.spot, 0.0) - tol) {
    return node_error(index, "American intrinsic bound violated");
  }
  if (node.call - bounds.prepaid_max > tol || node.put - discounted_max > tol) {
    return node_error(index, "American upper price bound violated");
  }
  const double difference = node.call - node.put;
  const double lower = bounds.prepaid_min - discounted_max;
  const double upper = bounds.prepaid_max - discounted_min;
  if (difference < lower - tol || difference > upper + tol) {
    return node_error(index, "affine call-minus-put bound violated");
  }
  return Ok();
}

[[nodiscard]] Status validate_call_lower_slopes(std::span<const AmericanPriceNode> nodes,
                                               double spot, double discount,
                                               const char *reason) {
  std::size_t maximum = 0u;
  for (std::size_t i = 1u; i < nodes.size(); ++i) {
    const auto &left = nodes[maximum];
    const auto &right = nodes[i];
    // Track the running maximum of C(K)+discount*K. Compare differences
    // instead of forming those sums, which can overflow for finite prices.
    const double change = right.call - left.call + discount * (right.strike - left.strike);
    if (change < -price_tolerance(spot, right.strike)) {
      return node_error(i, reason);
    }
    if (change > 0.0) maximum = i;
  }
  return Ok();
}

[[nodiscard]] Status validate_vertical_spreads(std::span<const AmericanPriceNode> nodes,
                                              const AmericanPriceBounds &bounds) {
  double minimum_call = nodes.front().call;
  double maximum_put = nodes.front().put;
  std::size_t minimum_discounted_put = 0u;
  for (std::size_t i = 1u; i < nodes.size(); ++i) {
    const auto &node = nodes[i];
    const auto &left = nodes[minimum_discounted_put];
    const double tol = price_tolerance(bounds.spot, node.strike);
    // P(K)-discount_max*K must be nonincreasing. Running extrema make
    // the tolerance apply to every pair, not once per adjacent segment.
    const double change =
        node.put - left.put - bounds.discount_max * (node.strike - left.strike);
    if (node.call - minimum_call > tol || maximum_put - node.put > tol || change > tol) {
      return node_error(i, "American vertical-spread slope bound violated");
    }
    minimum_call = std::min(minimum_call, node.call);
    maximum_put = std::max(maximum_put, node.put);
    if (change < 0.0) minimum_discounted_put = i;
  }
  return validate_call_lower_slopes(nodes, bounds.spot, bounds.discount_max,
                                     "American vertical-spread slope bound violated");
}

[[nodiscard]] Status validate_convex_envelope(std::span<const AmericanPriceNode> nodes,
                                             const AmericanPriceBounds &bounds, Side side,
                                             std::vector<std::size_t> &hull) {
  const auto value = [side](const AmericanPriceNode &node) noexcept {
    return side == Side::Call ? node.call : node.put;
  };
  hull.clear();
  for (std::size_t i = 0u; i < nodes.size(); ++i) {
    while (hull.size() >= 2u) {
      const auto &left = nodes[hull[hull.size() - 2u]];
      const auto &middle = nodes[hull.back()];
      const auto &right = nodes[i];
      const double fraction = (middle.strike - left.strike) / (right.strike - left.strike);
      // No admission tolerance here: retaining many individually small
      // concave bends can conceal a large butterfly over the full domain.
      if (value(middle) <= std::lerp(value(left), value(right), fraction)) break;
      hull.pop_back();
    }
    hull.push_back(i);
  }
  for (std::size_t segment = 1u; segment < hull.size(); ++segment) {
    const auto &left = nodes[hull[segment - 1u]];
    const auto &right = nodes[hull[segment]];
    for (std::size_t i = hull[segment - 1u] + 1u; i < hull[segment]; ++i) {
      const auto &node = nodes[i];
      const double fraction = (node.strike - left.strike) / (right.strike - left.strike);
      const double envelope = std::lerp(value(left), value(right), fraction);
      if (value(node) - envelope > price_tolerance(bounds.spot, node.strike)) {
        return node_error(i, "call/put convexity violated above the global lower convex envelope");
      }
    }
  }
  return Ok();
}

[[nodiscard]] Status validate_nondividend_segment(const AmericanPriceNode &left,
                                                  const AmericanPriceNode &right, double spot,
                                                  double df, std::size_t index) {
  const double width = right.strike - left.strike;
  const double tol = price_tolerance(spot, right.strike);
  if (right.call - left.call < -df * width - tol) {
    return node_error(index, "nondividend European call slope is below -df");
  }
  const double call_slope = (right.call - left.call) / width;
  const double put_slope = (right.put - left.put) / width;
  // K*f'(K)-f(K) is constant on each affine segment, including its left
  // endpoint under the right-derivative convention. Use one comparison per
  // represented segment rather than a sampled derivative stencil.
  const double gap = left.strike * (put_slope - call_slope) - (left.put - left.call + spot);
  if (!std::isfinite(gap) || gap < -tol) {
    return node_error(index, "nondividend American/European Legendre condition violated");
  }
  return Ok();
}

[[nodiscard]] Status validate_nondividend_upper(const AmericanPriceSlice &slice, double df) {
  const auto nodes = slice.nodes();
  const double lo = nodes.front().strike;
  const double hi = df * nodes.back().strike;
  if (hi < lo) {
    return Err(ErrorCode::Unavailable,
               "nondividend American: no supported strike interval for scaled upper bound");
  }
  const double spot = slice.bounds().spot;
  std::size_t native = 0u;
  std::size_t scaled = 0u;
  while (native < nodes.size() || scaled < nodes.size()) {
    const bool take_native =
        scaled == nodes.size() ||
        (native < nodes.size() && nodes[native].strike <= df * nodes[scaled].strike);
    const double strike = take_native ? nodes[native++].strike : df * nodes[scaled++].strike;
    if (native < nodes.size() && nodes[native].strike == strike) {
      ++native;
    }
    if (scaled < nodes.size() && df * nodes[scaled].strike == strike) {
      ++scaled;
    }
    if (strike < lo || strike > hi) {
      continue;
    }
    // Membership in [lo, df*Kmax] proves the exact quotient is supported.
    // Clamp only the quotient's last-bit endpoint rounding, never the domain.
    const double call_strike = std::clamp(strike / df, lo, nodes.back().strike);
    const auto call = slice.price(call_strike, Side::Call);
    const auto put = slice.price(strike, Side::Put);
    if (!call || !put) {
      return Err(ErrorCode::Internal, "nondividend American: supported price unavailable");
    }
    if (*put - (*call - spot + strike) > price_tolerance(spot, strike)) {
      return Err(ErrorCode::InvalidArgument,
                 "nondividend American: scaled European upper bound violated at strike " +
                     std::to_string(strike));
    }
  }
  return Ok();
}

// ── Model-priced certification (L2b al-geometry, 2026-10-19) ─────────────

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] Status pricing_error(std::size_t index, const atx::core::Error &error) {
  return Err(error.code(), "AmericanPriceSlice: node " + std::to_string(index) +
                               ": model pricing failed: " + error.message());
}

// Prices model nodes (a call and a put each) with one kernel and one AL scheme and
// counts the andersen_lake prices it computes. Batched keeps ONE geometry, for the
// yield of the current run, and re-prepares it when a run of another yield starts;
// it is allocated on first use, so the Scalar kernel allocates nothing here.
class NodePricer {
public:
  NodePricer(std::span<const AmericanCertificationNode> nodes, double spot,
             const AmericanCertificationModel &model, std::optional<AlOpts> opts,
             CertificationKernel kernel)
      : nodes_(nodes), spot_(spot), T_(model.T), rate_(model.rate), opts_(std::move(opts)),
        kernel_(kernel) {}

  // Prices nodes [begin, end) into prices[begin, end); stops at the first node that
  // andersen_lake refuses and returns its error. Requires end <= nodes.size() ==
  // prices.size().
  [[nodiscard]] Status price(std::size_t begin, std::size_t end,
                             std::span<AmericanPriceNode> prices) {
    if (kernel_ == CertificationKernel::Batched) {
      return price_batched(begin, end, prices);
    }
    for (std::size_t i = begin; i < end; ++i) {
      ATX_TRY_VOID(price_scalar(i, prices[i]));
    }
    return Ok();
  }

  [[nodiscard]] std::size_t priced() const noexcept { return priced_; }

private:
  [[nodiscard]] Status price_scalar(std::size_t i, AmericanPriceNode &out) {
    const AmericanCertificationNode &n = nodes_[i];
    ++priced_;
    const Result<double> call =
        andersen_lake(spot_, n.strike, T_, n.sigma, rate_, n.yield, Side::Call, opts_);
    if (!call) {
      return pricing_error(i, call.error());
    }
    ++priced_;
    const Result<double> put =
        andersen_lake(spot_, n.strike, T_, n.sigma, rate_, n.yield, Side::Put, opts_);
    if (!put) {
      return pricing_error(i, put.error());
    }
    out = {n.strike, *call, *put};
    return Ok();
  }

  [[nodiscard]] Status price_batched(std::size_t begin, std::size_t end,
                                     std::span<AmericanPriceNode> prices) {
    std::size_t i = begin;
    while (i < end) {
      const double yield = nodes_[i].yield;
      std::size_t run_end = i + 1u;
      while (run_end < end && same_bits(nodes_[run_end].yield, yield)) {
        ++run_end;
      }
      if (!geometry_) {
        geometry_.emplace();
      }
      if (!geometry_ready_ || !same_bits(geometry_yield_, yield)) {
        geometry_->prepare(T_, rate_, yield, opts_);
        geometry_yield_ = yield;
        geometry_ready_ = true;
      }
      const std::size_t m = run_end - i;
      contracts_.resize(2u * m);
      sigma_.resize(2u * m);
      values_.resize(2u * m);
      for (std::size_t k = 0; k < m; ++k) {
        const AmericanCertificationNode &n = nodes_[i + k];
        contracts_[2u * k] = AlContract{.S = spot_, .K = n.strike, .side = Side::Call};
        contracts_[2u * k + 1u] = AlContract{.S = spot_, .K = n.strike, .side = Side::Put};
        sigma_[2u * k] = n.sigma;
        sigma_[2u * k + 1u] = n.sigma;
      }
      ATX_TRY_VOID(al_price_pack(*geometry_, contracts_, sigma_, values_, {}));
      priced_ += 2u * m;
      for (std::size_t k = 0; k < m; ++k) {
        const std::size_t node = i + k;
        const double call = values_[2u * k];
        const double put = values_[2u * k + 1u];
        if (std::isnan(call) || std::isnan(put)) {
          // NaN is the pack's channel for an andersen_lake error: the scalar price of
          // this node returns that exact error (or, failing that, the same values).
          ATX_TRY_VOID(price_scalar(node, prices[node]));
          continue;
        }
        prices[node] = {nodes_[node].strike, call, put};
      }
      i = run_end;
    }
    return Ok();
  }

  std::span<const AmericanCertificationNode> nodes_;
  double spot_;
  double T_;
  double rate_;
  std::optional<AlOpts> opts_;
  CertificationKernel kernel_;
  std::size_t priced_ = 0;
  std::optional<AlExpiryGeometry> geometry_;
  double geometry_yield_ = 0.0;
  bool geometry_ready_ = false;
  std::vector<AlContract> contracts_;
  std::vector<double> sigma_;
  std::vector<double> values_;
};

// The Fast-kernel allowance of one price (see kCertificationFastAllowance).
[[nodiscard]] double fast_allowance(double price) noexcept {
  return kCertificationFastAllowance * std::max(std::abs(price), 1.0);
}

[[nodiscard]] std::string screen_reason(std::size_t index, const char *reason) {
  return "AmericanPriceSlice: node " + std::to_string(index) + ": certification screen: " +
         reason + " beyond the Fast-kernel allowance";
}

// create()'s price-independent admission (bounds, node count, strikes). The screen runs
// only when it holds, so malformed input still gets create()'s own error.
[[nodiscard]] bool screenable(std::span<const AmericanCertificationNode> nodes,
                              const AmericanPriceBounds &bounds) noexcept {
  if (!valid_bounds(bounds) || nodes.size() < 2u) {
    return false;
  }
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const double strike = nodes[i].strike;
    if (!std::isfinite(strike) || !(strike > 0.0) ||
        !std::isfinite(strike * bounds.discount_max) ||
        !std::isfinite(strike * bounds.discount_min)) {
      return false;
    }
    if (i > 0u && !(strike > nodes[i - 1u].strike)) {
      return false;
    }
  }
  return true;
}

// validate_node's bounds, each widened by the allowance of the prices it reads. Every
// refusal here implies the accurate node fails the same bound: |accurate - Fast| <=
// allowance per price.
[[nodiscard]] std::optional<std::string> screen_node(const AmericanPriceNode &node,
                                                     const AmericanPriceBounds &bounds,
                                                     std::size_t index) {
  const double tol = price_tolerance(bounds.spot, node.strike);
  const double ec = fast_allowance(node.call);
  const double ep = fast_allowance(node.put);
  if (node.call + ec < std::max(bounds.spot - node.strike, 0.0) - tol ||
      node.put + ep < std::max(node.strike - bounds.spot, 0.0) - tol) {
    return screen_reason(index, "American intrinsic bound violated");
  }
  if (node.call - ec - bounds.prepaid_max > tol ||
      node.put - ep - node.strike * bounds.discount_max > tol) {
    return screen_reason(index, "American upper price bound violated");
  }
  const double difference = node.call - node.put;
  const double lower = bounds.prepaid_min - node.strike * bounds.discount_max;
  const double upper = bounds.prepaid_max - node.strike * bounds.discount_min;
  if (difference + ec + ep < lower - tol || difference - ec - ep > upper + tol) {
    return screen_reason(index, "affine call-minus-put bound violated");
  }
  return std::nullopt;
}

// validate_vertical_spreads + validate_call_lower_slopes, widened. Each running extremum
// is taken over the allowance-shifted Fast prices on the side that weakens the check,
// so a refusal names a pair (i, j) whose accurate prices violate the same inequality.
[[nodiscard]] std::optional<std::string>
screen_vertical_spreads(std::span<const AmericanPriceNode> p, const AmericanPriceBounds &bounds) {
  const double dmax = bounds.discount_max;
  double call_high_min = p.front().call + fast_allowance(p.front().call); // min (C + e)
  double put_low_max = p.front().put - fast_allowance(p.front().put);     // max (P - e)
  std::size_t low = 0u;  // argmin over j of (P_j + e_j) - dmax * K_j
  std::size_t high = 0u; // argmax over j of (C_j - e_j) + dmax * K_j
  for (std::size_t i = 1u; i < p.size(); ++i) {
    const AmericanPriceNode &node = p[i];
    const double tol = price_tolerance(bounds.spot, node.strike);
    const double ec = fast_allowance(node.call);
    const double ep = fast_allowance(node.put);
    const AmericanPriceNode &l = p[low];
    const AmericanPriceNode &h = p[high];
    const double l_high = l.put + fast_allowance(l.put);
    const double h_low = h.call - fast_allowance(h.call);
    const double put_change = (node.put - ep) - l_high - dmax * (node.strike - l.strike);
    const double call_change = (node.call + ec) - h_low + dmax * (node.strike - h.strike);
    if ((node.call - ec) - call_high_min > tol || put_low_max - (node.put + ep) > tol ||
        put_change > tol || call_change < -tol) {
      return screen_reason(i, "American vertical-spread slope bound violated");
    }
    call_high_min = std::min(call_high_min, node.call + ec);
    put_low_max = std::max(put_low_max, node.put - ep);
    if ((node.put + ep) - l_high - dmax * (node.strike - l.strike) < 0.0) {
      low = i;
    }
    if ((node.call - ec) - h_low + dmax * (node.strike - h.strike) > 0.0) {
      high = i;
    }
  }
  return std::nullopt;
}

// validate_convex_envelope, widened. The chord between two Fast hull points lies above
// the WHOLE accurate slice's lower envelope once shifted by the larger allowance of its
// ends, so a node above it by more than tol + its own allowance violates convexity in
// the accurate prices too — whatever nodes the prefix has not priced yet.
[[nodiscard]] std::optional<std::string>
screen_convexity(std::span<const AmericanPriceNode> p, const AmericanPriceBounds &bounds,
                 Side side, std::vector<std::size_t> &hull) {
  const auto value = [side](const AmericanPriceNode &node) noexcept {
    return side == Side::Call ? node.call : node.put;
  };
  hull.clear();
  for (std::size_t i = 0u; i < p.size(); ++i) {
    while (hull.size() >= 2u) {
      const AmericanPriceNode &left = p[hull[hull.size() - 2u]];
      const AmericanPriceNode &middle = p[hull.back()];
      const AmericanPriceNode &right = p[i];
      const double fraction = (middle.strike - left.strike) / (right.strike - left.strike);
      if (value(middle) <= std::lerp(value(left), value(right), fraction)) break;
      hull.pop_back();
    }
    hull.push_back(i);
  }
  for (std::size_t segment = 1u; segment < hull.size(); ++segment) {
    const AmericanPriceNode &left = p[hull[segment - 1u]];
    const AmericanPriceNode &right = p[hull[segment]];
    const double chord_allowance =
        std::max(fast_allowance(value(left)), fast_allowance(value(right)));
    for (std::size_t i = hull[segment - 1u] + 1u; i < hull[segment]; ++i) {
      const AmericanPriceNode &node = p[i];
      const double fraction = (node.strike - left.strike) / (right.strike - left.strike);
      const double envelope = std::lerp(value(left), value(right), fraction);
      const double slack = fast_allowance(value(node)) + chord_allowance;
      if (value(node) - envelope > price_tolerance(bounds.spot, node.strike) + slack) {
        return screen_reason(i, "call/put convexity violated above the global lower convex "
                                "envelope");
      }
    }
  }
  return std::nullopt;
}

// Every widened check of create() on the priced prefix; node bounds only for the
// nodes priced since the last call (`fresh_from`).
[[nodiscard]] std::optional<std::string>
screen_prefix(std::span<const AmericanPriceNode> prefix, std::size_t fresh_from,
              const AmericanPriceBounds &bounds, std::vector<std::size_t> &hull) {
  for (std::size_t i = fresh_from; i < prefix.size(); ++i) {
    if (auto why = screen_node(prefix[i], bounds, i)) {
      return why;
    }
  }
  if (auto why = screen_vertical_spreads(prefix, bounds)) {
    return why;
  }
  if (auto why = screen_convexity(prefix, bounds, Side::Call, hull)) {
    return why;
  }
  return screen_convexity(prefix, bounds, Side::Put, hull);
}

// The Fast screen: prices chunk by chunk in strike order and checks every priced
// prefix. Returns a certain violation's message, or nullopt (the accurate pass decides),
// including when a Fast price errors or is not finite. `prices` is scratch here.
[[nodiscard]] std::optional<std::string>
fast_screen(std::span<const AmericanCertificationNode> nodes,
            const AmericanCertificationModel &model, const AmericanPriceBounds &bounds,
            const std::optional<AlOpts> &opts, CertificationKernel kernel,
            std::span<AmericanPriceNode> prices, AmericanCertificationStats &stats) {
  NodePricer fast(nodes, bounds.spot, model, al_fast_profile(opts), kernel);
  std::vector<std::size_t> hull;
  hull.reserve(nodes.size());
  std::optional<std::string> verdict;
  for (std::size_t done = 0; done < nodes.size();) {
    const std::size_t next = std::min(nodes.size(), done + kCertificationScreenChunk);
    if (!fast.price(done, next, prices)) {
      break;
    }
    bool finite = true;
    for (std::size_t i = done; i < next; ++i) {
      finite = finite && std::isfinite(prices[i].call) && std::isfinite(prices[i].put);
    }
    if (!finite) {
      break;
    }
    verdict = screen_prefix(prices.first(next), done, bounds, hull);
    if (verdict) {
      break;
    }
    done = next;
  }
  stats.fast_prices = fast.priced();
  return verdict;
}

} // namespace

AmericanPriceSlice::AmericanPriceSlice(std::vector<AmericanPriceNode> nodes,
                                       AmericanPriceBounds bounds) noexcept
    : nodes_(std::move(nodes)), bounds_(bounds) {}

Result<AmericanPriceSlice> AmericanPriceSlice::create(std::span<const AmericanPriceNode> nodes,
                                                      const AmericanPriceBounds &bounds) {
  if (!valid_bounds(bounds)) {
    return Err(ErrorCode::InvalidArgument, "AmericanPriceSlice: invalid carry bounds");
  }
  if (nodes.size() < 2u) {
    return Err(ErrorCode::InvalidArgument, "AmericanPriceSlice: at least two nodes required");
  }
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    ATX_TRY_VOID(validate_node(nodes[i], bounds, i));
    if (i > 0u && !(nodes[i].strike > nodes[i - 1u].strike)) {
      ATX_TRY_VOID(node_error(i, "strikes must be strictly increasing"));
    }
  }
  ATX_TRY_VOID(validate_vertical_spreads(nodes, bounds));
  std::vector<std::size_t> hull;
  hull.reserve(nodes.size());
  ATX_TRY_VOID(validate_convex_envelope(nodes, bounds, Side::Call, hull));
  ATX_TRY_VOID(validate_convex_envelope(nodes, bounds, Side::Put, hull));
  return Ok(AmericanPriceSlice(std::vector<AmericanPriceNode>(nodes.begin(), nodes.end()), bounds));
}

Result<AmericanPriceSlice> AmericanPriceSlice::create(
    std::span<const AmericanCertificationNode> nodes, const AmericanCertificationModel &model,
    const AmericanPriceBounds &bounds, CertificationKernel kernel, CertificationScreen screen,
    AmericanCertificationStats *stats) {
  AmericanCertificationStats local;
  AmericanCertificationStats &st = (stats != nullptr) ? *stats : local;
  st = AmericanCertificationStats{};
  const bool kernel_ok =
      kernel == CertificationKernel::Scalar || kernel == CertificationKernel::Batched;
  const bool screen_ok = screen == CertificationScreen::AccurateOnly ||
                         screen == CertificationScreen::FastThenAccurate;
  if (!kernel_ok || !screen_ok) {
    return Err(ErrorCode::InvalidArgument,
               "AmericanPriceSlice: unknown certification kernel or screen");
  }
  const std::optional<AlOpts> opts =
      (model.al_opts != nullptr) ? std::optional<AlOpts>(*model.al_opts) : std::nullopt;
  std::vector<AmericanPriceNode> prices(nodes.size());
  if (screen == CertificationScreen::FastThenAccurate && screenable(nodes, bounds)) {
    if (auto why = fast_screen(nodes, model, bounds, opts, kernel, prices, st)) {
      st.screen_refused = true;
      return Err(ErrorCode::InvalidArgument, std::move(*why));
    }
  }
  NodePricer accurate(nodes, bounds.spot, model, opts, kernel);
  const Status priced = accurate.price(0u, nodes.size(), prices);
  st.accurate_prices = accurate.priced();
  ATX_TRY_VOID(priced);
  return create(prices, bounds);
}

std::optional<double> AmericanPriceSlice::price(double strike, Side side) const noexcept {
  if (!std::isfinite(strike) || nodes_.empty() || strike < nodes_.front().strike ||
      strike > nodes_.back().strike || (side != Side::Call && side != Side::Put)) {
    return std::nullopt;
  }
  const auto right =
      std::lower_bound(nodes_.begin(), nodes_.end(), strike,
                       [](const AmericanPriceNode &node, double k) { return node.strike < k; });
  const auto value = [side](const AmericanPriceNode &node) noexcept {
    return side == Side::Call ? node.call : node.put;
  };
  if (right->strike == strike || right == nodes_.begin()) {
    return value(*right);
  }
  const auto left = right - 1;
  const double fraction = (strike - left->strike) / (right->strike - left->strike);
  return std::lerp(value(*left), value(*right), fraction);
}

// ── Box certificate (lane L3r1 dense-certificate, 2026-10-20) ──────────────

bool project_convex_price_box(std::span<const double> strikes, std::span<double> prices,
                              std::span<const double> lo, std::span<const double> hi,
                              std::span<const double> weights, double slope_min,
                              double slope_max) {
  constexpr int kMaxSweeps = 20000;
  const std::size_t n = prices.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (!(lo[i] <= hi[i])) return false;
  }
  if (n < 2u) return true;
  // Half-space j: sum_k a_jk p_k >= b_j over three (convexity) or two (slope)
  // consecutive nodes starting at first[j].
  struct HalfSpace {
    std::size_t first;
    std::size_t count;
    std::array<double, 3> a;
    double b;
    double norm; // a W^-1 a
    double mu;   // Dykstra multiplier, >= 0
  };
  std::vector<HalfSpace> spaces;
  spaces.reserve(n);
  const auto inverse_norm = [&](HalfSpace &h) {
    h.norm = 0.0;
    for (std::size_t k = 0; k < h.count; ++k) h.norm += h.a[k] * h.a[k] / weights[h.first + k];
  };
  for (std::size_t i = 1; i + 1 < n; ++i) {
    const double left = 1.0 / (strikes[i] - strikes[i - 1]);
    const double right = 1.0 / (strikes[i + 1] - strikes[i]);
    HalfSpace h{i - 1, 3, {left, -(left + right), right}, 0.0, 0.0, 0.0};
    inverse_norm(h);
    spaces.push_back(h);
  }
  const double first_width = 1.0 / (strikes[1] - strikes[0]);
  const double last_width = 1.0 / (strikes[n - 1] - strikes[n - 2]);
  HalfSpace first{0, 2, {-first_width, first_width, 0.0}, slope_min, 0.0, 0.0};
  HalfSpace last{n - 2, 2, {last_width, -last_width, 0.0}, -slope_max, 0.0, 0.0};
  inverse_norm(first);
  inverse_norm(last);
  spaces.push_back(first);
  spaces.push_back(last);
  std::vector<double> box(n, 0.0);
  double scale = 1.0;
  for (const double value : prices) scale = std::max(scale, std::abs(value));
  const double tolerance = 1.0e-13 * scale;
  for (int sweep = 0; sweep < kMaxSweeps; ++sweep) {
    double change = 0.0;
    for (HalfSpace &h : spaces) {
      double dot = 0.0;
      for (std::size_t k = 0; k < h.count; ++k) dot += h.a[k] * prices[h.first + k];
      const double mu = std::max(0.0, h.mu + (h.b - dot) / h.norm);
      const double step = mu - h.mu;
      if (step == 0.0) continue;
      h.mu = mu;
      for (std::size_t k = 0; k < h.count; ++k) {
        const double delta = step * h.a[k] / weights[h.first + k];
        prices[h.first + k] += delta;
        change = std::max(change, std::abs(delta));
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      const double z = prices[i] + box[i];
      const double clamped = std::clamp(z, lo[i], hi[i]);
      change = std::max(change, std::abs(clamped - prices[i]));
      box[i] = z - clamped;
      prices[i] = clamped;
    }
    if (change <= tolerance) return true;
  }
  return false;
}

namespace {

// Greatest convex minorant of `upper` on `strikes` (the pointwise largest convex
// function below the upper edges), written to `minorant`. A convex curve inside
// [lower, upper] exists iff this minorant dominates `lower`: every convex p <=
// upper satisfies p <= minorant, and the minorant itself is admissible then.
void convex_minorant(std::span<const double> strikes, std::span<const double> upper,
                     std::vector<std::size_t> &hull, std::span<double> minorant) {
  hull.clear();
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    while (hull.size() >= 2u) {
      const std::size_t l = hull[hull.size() - 2u];
      const std::size_t m = hull.back();
      const double fraction = (strikes[m] - strikes[l]) / (strikes[i] - strikes[l]);
      if (upper[m] <= std::lerp(upper[l], upper[i], fraction)) break;
      hull.pop_back();
    }
    hull.push_back(i);
  }
  for (std::size_t segment = 0; segment < hull.size(); ++segment) {
    minorant[hull[segment]] = upper[hull[segment]];
    if (segment == 0u) continue;
    const std::size_t l = hull[segment - 1u];
    const std::size_t r = hull[segment];
    for (std::size_t i = l + 1u; i < r; ++i) {
      const double fraction = (strikes[i] - strikes[l]) / (strikes[r] - strikes[l]);
      minorant[i] = std::lerp(upper[l], upper[r], fraction);
    }
  }
}

[[nodiscard]] Status box_infeasible(std::size_t index, const char *reason) {
  return Err(ErrorCode::Unavailable, "AmericanPriceSlice box: node " + std::to_string(index) +
                                         ": no arbitrage-free curve inside the boxes (" +
                                         reason + ")");
}

} // namespace

Result<AmericanBoxCertificate> certify_american_box(std::span<const AmericanPriceNode> fitted,
                                                    std::span<const AmericanPriceBox> boxes,
                                                    const AmericanPriceBounds &bounds) {
  if (!valid_bounds(bounds)) {
    return Err(ErrorCode::InvalidArgument, "AmericanPriceSlice box: invalid carry bounds");
  }
  const std::size_t n = fitted.size();
  if (n < 2u || boxes.size() != n) {
    return Err(ErrorCode::InvalidArgument,
               "AmericanPriceSlice box: at least two nodes and one box per node required");
  }
  const auto finite_box = [](double lo, double hi) noexcept {
    return std::isfinite(lo) && std::isfinite(hi) && lo <= hi;
  };
  std::vector<double> strikes(n);
  for (std::size_t i = 0; i < n; ++i) {
    const AmericanPriceNode &node = fitted[i];
    const AmericanPriceBox &box = boxes[i];
    if (!std::isfinite(node.strike) || !(node.strike > 0.0) || !std::isfinite(node.call) ||
        !std::isfinite(node.put) || (i > 0u && !(node.strike > fitted[i - 1u].strike)) ||
        !finite_box(box.call_lo, box.call_hi) || !finite_box(box.put_lo, box.put_hi)) {
      return Err(ErrorCode::InvalidArgument,
                 "AmericanPriceSlice box: node " + std::to_string(i) +
                     ": finite increasing strikes, finite prices and boxes with lo <= hi "
                     "required");
    }
    strikes[i] = node.strike;
  }
  std::vector<AmericanPriceNode> nodes(fitted.begin(), fitted.end());
  std::vector<double> lo(n);
  std::vector<double> hi(n);
  std::vector<double> minorant(n);
  std::vector<double> prices(n);
  const std::vector<double> weights(n, 1.0);
  std::vector<std::size_t> hull;
  hull.reserve(n);
  for (const Side side : {Side::Call, Side::Put}) {
    const bool call = side == Side::Call;
    for (std::size_t i = 0; i < n; ++i) {
      const double k = strikes[i];
      // The certificate's own per-node bounds are hard: the box is intersected
      // with them (a quote below intrinsic or above the upper bound is no box).
      const double intrinsic = std::max(call ? bounds.spot - k : k - bounds.spot, 0.0);
      const double upper = call ? bounds.prepaid_max : k * bounds.discount_max;
      lo[i] = std::max(call ? boxes[i].call_lo : boxes[i].put_lo, intrinsic);
      hi[i] = std::min(call ? boxes[i].call_hi : boxes[i].put_hi, upper);
      if (lo[i] > hi[i]) {
        if (lo[i] - hi[i] > price_tolerance(bounds.spot, k)) {
          ATX_TRY_VOID(box_infeasible(i, "box outside the exercise bounds"));
        }
        lo[i] = hi[i];
      }
      prices[i] = call ? nodes[i].call : nodes[i].put;
    }
    convex_minorant(strikes, hi, hull, minorant);
    for (std::size_t i = 0; i < n; ++i) {
      if (lo[i] - minorant[i] > price_tolerance(bounds.spot, strikes[i])) {
        ATX_TRY_VOID(box_infeasible(i, "no convex curve inside the boxes"));
      }
    }
    // Calls fall with slope >= -discount_max; puts rise with slope <= discount_max.
    const double slope_min = call ? -bounds.discount_max : 0.0;
    const double slope_max = call ? 0.0 : bounds.discount_max;
    if (!project_convex_price_box(strikes, prices, lo, hi, weights, slope_min, slope_max)) {
      ATX_TRY_VOID(box_infeasible(0u, "projection did not settle"));
    }
    for (std::size_t i = 0; i < n; ++i) (call ? nodes[i].call : nodes[i].put) = prices[i];
  }
  auto certified = AmericanPriceSlice::create(nodes, bounds);
  if (!certified) {
    return Err(certified.error().code(),
               "AmericanPriceSlice box: in-box curve refused: " + certified.error().message());
  }
  AmericanBoxCertificate out{std::move(*certified), std::vector<std::uint8_t>(n, 0u), 0u, 0.0};
  for (std::size_t i = 0; i < n; ++i) {
    const double call = std::abs(nodes[i].call - fitted[i].call);
    const double put = std::abs(nodes[i].put - fitted[i].put);
    if (call > 0.0) out.moved[i] |= 1u;
    if (put > 0.0) out.moved[i] |= 2u;
    if (out.moved[i] != 0u) ++out.moved_nodes;
    out.max_price_move = std::max({out.max_price_move, call, put});
  }
  return Ok(std::move(out));
}

// ── Distance to market (lane L3q market-distance, 2026-10-21) ─────────────

double market_distance_spreads(double price, double bid, double ask, double tick) noexcept {
  if (!std::isfinite(price) || !std::isfinite(bid) || !std::isfinite(ask) ||
      !std::isfinite(tick) || !(tick > 0.0) || bid < 0.0 || !(ask > bid)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double spread = std::max(ask - bid, tick);
  if (price > ask) return (price - ask) / spread;
  if (price < bid) return (bid - price) / spread;
  return 0.0;
}

namespace {

// One half-space sum_k a[k] x[index[k]] >= b of the joint projection (at most
// four variables: two interpolated prices of two slices).
struct SparseHalfSpace {
  std::array<std::size_t, 4> index{};
  std::array<double, 4> a{};
  std::size_t count = 0;
  double b = 0.0;
  double norm = 0.0; // sum a^2 (uniform weights)
  double mu = 0.0;   // Dykstra multiplier, >= 0
};

void add_term(SparseHalfSpace &h, std::size_t index, double a) {
  for (std::size_t k = 0; k < h.count; ++k) {
    if (h.index[k] == index) {
      h.a[k] += a;
      return;
    }
  }
  h.index[h.count] = index;
  h.a[h.count] = a;
  ++h.count;
}

// Adds sign * P(K) of one slice (first variable `offset`) to `h`: the node price at
// a knot, else the linear interpolation of the two knots around K. Requires K in
// the slice's domain.
void add_price(SparseHalfSpace &h, std::span<const double> strikes, std::size_t offset,
               double strike, double sign) {
  const auto right = static_cast<std::size_t>(
      std::lower_bound(strikes.begin(), strikes.end(), strike) - strikes.begin());
  if (right < strikes.size() && strikes[right] == strike) {
    add_term(h, offset + right, sign);
    return;
  }
  const std::size_t left = right - 1u;
  const double fraction = (strike - strikes[left]) / (strikes[right] - strikes[left]);
  if (fraction < 1.0) add_term(h, offset + left, sign * (1.0 - fraction));
  if (fraction > 0.0) add_term(h, offset + right, sign * fraction);
}

void finish(SparseHalfSpace &h, std::vector<SparseHalfSpace> &spaces) {
  h.norm = 0.0;
  for (std::size_t k = 0; k < h.count; ++k) h.norm += h.a[k] * h.a[k];
  if (h.norm > 0.0) spaces.push_back(h);
}

// Calendar half-spaces P_later(K) - P_earlier(K) >= 0 at union knots inside both
// domains. A knot inside every intermediate slice's domain is implied by the
// adjacent pairs (each pair's spread is affine between its union knots, and the
// constraints are exact, so the chain adds no slack), so only the others are added.
void add_calendar(std::span<const JointPriceSide> slices, std::size_t a, std::size_t b,
                  std::span<const std::size_t> offset, std::vector<SparseHalfSpace> &spaces) {
  const std::span<const double> earlier = slices[a].strikes;
  const std::span<const double> later = slices[b].strikes;
  const double lo = std::max(earlier.front(), later.front());
  const double hi = std::min(earlier.back(), later.back());
  if (hi < lo) return;
  const auto implied = [&](double k) {
    if (b == a + 1u) return false;
    for (std::size_t c = a + 1u; c < b; ++c) {
      if (k < slices[c].strikes.front() || k > slices[c].strikes.back()) return false;
    }
    return true;
  };
  std::vector<double> knots;
  knots.reserve(earlier.size() + later.size());
  for (const double k : earlier) {
    if (k >= lo && k <= hi && !implied(k)) knots.push_back(k);
  }
  for (const double k : later) {
    if (k >= lo && k <= hi && !implied(k)) knots.push_back(k);
  }
  std::sort(knots.begin(), knots.end());
  knots.erase(std::unique(knots.begin(), knots.end()), knots.end());
  for (const double k : knots) {
    SparseHalfSpace h;
    add_price(h, later, offset[b], k, 1.0);
    add_price(h, earlier, offset[a], k, -1.0);
    finish(h, spaces);
  }
}

// Dykstra / Hildreth over sparse half-spaces plus the box, uniform weights: the
// loop of project_convex_price_box with general (index, coefficient) rows, at most
// `max_sweeps` sweeps; `sweeps` receives the sweeps run.
[[nodiscard]] bool dykstra_sparse(std::span<double> x, std::span<const double> lo,
                                  std::span<const double> hi,
                                  std::vector<SparseHalfSpace> &spaces, std::size_t max_sweeps,
                                  double relative_tolerance, std::size_t &sweeps) {
  const std::size_t n = x.size();
  std::vector<double> box(n, 0.0);
  double scale = 1.0;
  for (const double value : x) scale = std::max(scale, std::abs(value));
  // Coupled slices converge more slowly than one; the unchanged certificate and
  // calendar checks downstream judge the result, so the stop is looser than
  // project_convex_price_box's (still ~1e-4 of the certificate allowance).
  const double tolerance = relative_tolerance * scale;
  for (std::size_t sweep = 0; sweep < max_sweeps; ++sweep) {
    sweeps = sweep + 1u;
    double change = 0.0;
    for (SparseHalfSpace &h : spaces) {
      double dot = 0.0;
      for (std::size_t k = 0; k < h.count; ++k) dot += h.a[k] * x[h.index[k]];
      const double mu = std::max(0.0, h.mu + (h.b - dot) / h.norm);
      const double step = mu - h.mu;
      if (step == 0.0) continue;
      h.mu = mu;
      for (std::size_t k = 0; k < h.count; ++k) {
        const double delta = step * h.a[k];
        x[h.index[k]] += delta;
        change = std::max(change, std::abs(delta));
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      const double z = x[i] + box[i];
      const double clamped = std::clamp(z, lo[i], hi[i]);
      change = std::max(change, std::abs(clamped - x[i]));
      box[i] = z - clamped;
      x[i] = clamped;
    }
    if (change <= tolerance) return true;
  }
  return false;
}

[[nodiscard]] bool valid_joint_side(const JointPriceSide &s) noexcept {
  const std::size_t n = s.strikes.size();
  if (n < 2u || s.prices.size() != n || s.lo.size() != n || s.hi.size() != n ||
      !std::isfinite(s.spot) || s.spot < 0.0) {
    return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (!std::isfinite(s.strikes[i]) || !std::isfinite(s.prices[i]) || !std::isfinite(s.lo[i]) ||
        !std::isfinite(s.hi[i]) || !(s.lo[i] <= s.hi[i]) ||
        (i > 0u && !(s.strikes[i] > s.strikes[i - 1u]))) {
      return false;
    }
  }
  return true;
}

} // namespace

namespace {

// The joint problem of project_calendar_joint: the stacked prices `x` and boxes, each
// slice's first variable `offset`, and every half-space (convexity and end slopes per
// slice, calendar knots per pair). Requires well-formed slices.
void build_joint(std::span<const JointPriceSide> slices, std::vector<double> &x,
                 std::vector<double> &lo, std::vector<double> &hi,
                 std::vector<std::size_t> &offset, std::vector<SparseHalfSpace> &spaces) {
  std::size_t total = 0;
  for (const JointPriceSide &s : slices) total += s.strikes.size();
  x.reserve(total);
  lo.reserve(total);
  hi.reserve(total);
  offset.reserve(slices.size());
  spaces.reserve(2u * total);
  for (const JointPriceSide &s : slices) {
    const std::size_t base = x.size();
    offset.push_back(base);
    x.insert(x.end(), s.prices.begin(), s.prices.end());
    lo.insert(lo.end(), s.lo.begin(), s.lo.end());
    hi.insert(hi.end(), s.hi.begin(), s.hi.end());
    const std::size_t n = s.strikes.size();
    // Convexity and the end slopes are exact (no slack: the certificate's global
    // envelope test would accumulate a local one).
    for (std::size_t i = 1; i + 1 < n; ++i) {
      const double left = 1.0 / (s.strikes[i] - s.strikes[i - 1]);
      const double right = 1.0 / (s.strikes[i + 1] - s.strikes[i]);
      SparseHalfSpace h;
      add_term(h, base + i - 1, left);
      add_term(h, base + i, -(left + right));
      add_term(h, base + i + 1, right);
      finish(h, spaces);
    }
    const double first = 1.0 / (s.strikes[1] - s.strikes[0]);
    const double last = 1.0 / (s.strikes[n - 1] - s.strikes[n - 2]);
    SparseHalfSpace front;
    add_term(front, base, -first);
    add_term(front, base + 1, first);
    front.b = s.slope_min;
    finish(front, spaces);
    SparseHalfSpace back;
    add_term(back, base + n - 2, last);
    add_term(back, base + n - 1, -last);
    back.b = -s.slope_max;
    finish(back, spaces);
  }
  for (std::size_t a = 0; a < slices.size(); ++a) {
    for (std::size_t b = a + 1; b < slices.size(); ++b) {
      add_calendar(slices, a, b, offset, spaces);
    }
  }
}

void scatter_joint(std::span<const JointPriceSide> slices, std::span<const double> x,
                   std::span<const std::size_t> offset) {
  for (std::size_t m = 0; m < slices.size(); ++m) {
    std::copy_n(x.begin() + static_cast<std::ptrdiff_t>(offset[m]), slices[m].prices.size(),
                slices[m].prices.begin());
  }
}

// The value at `strike` of the piecewise-linear interpolation of `values` on `strikes`
// (AmericanPriceSlice::price's rule). Requires strike inside the strikes' range.
[[nodiscard]] double interpolate(std::span<const double> strikes, std::span<const double> values,
                                 double strike) noexcept {
  const auto right = static_cast<std::size_t>(
      std::lower_bound(strikes.begin(), strikes.end(), strike) - strikes.begin());
  if (strikes[right] == strike || right == 0u) return values[right];
  const double fraction = (strike - strikes[right - 1u]) / (strikes[right] - strikes[right - 1u]);
  return std::lerp(values[right - 1u], values[right], fraction);
}

[[nodiscard]] JointProjectionConflict conflict_at(JointConflictKind kind, std::size_t slice,
                                                  std::size_t node, std::size_t other,
                                                  double strike, double excess) noexcept {
  return JointProjectionConflict{kind, slice, node, other, strike, excess};
}

// Exact infeasibility certificates of one slice: an empty box, or boxes that admit no
// convex curve passing the certificate's convexity test (a lower edge above the greatest
// convex minorant of the upper edges beyond the allowance).
[[nodiscard]] std::optional<JointProjectionConflict> slice_conflict(const JointPriceSide &s,
                                                                    std::size_t index) {
  const std::size_t n = s.strikes.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (s.lo[i] > s.hi[i]) {
      return conflict_at(JointConflictKind::EmptyBox, index, i, index, s.strikes[i],
                         s.lo[i] - s.hi[i]);
    }
  }
  std::vector<std::size_t> hull;
  hull.reserve(n);
  std::vector<double> minorant(n);
  convex_minorant(s.strikes, s.hi, hull, minorant);
  for (std::size_t i = 0; i < n; ++i) {
    const double excess = s.lo[i] - minorant[i] - price_tolerance(s.spot, s.strikes[i]);
    if (excess > 0.0) {
      return conflict_at(JointConflictKind::NoConvexCurve, index, i, index, s.strikes[i], excess);
    }
  }
  return std::nullopt;
}

// Exact calendar certificate of a pair (earlier a, later b): at a knot of either inside
// both domains P_a lies on or above a's lower edges and P_b on or below b's upper edges
// (both interpolated where the knot is not their own), so a's lower edge above b's upper
// edge beyond the allowance leaves no prices inside the boxes that check_american_calendar
// accepts.
[[nodiscard]] std::optional<JointProjectionConflict>
calendar_conflict(std::span<const JointPriceSide> slices, std::size_t a, std::size_t b) {
  const JointPriceSide &early = slices[a];
  const JointPriceSide &late = slices[b];
  const double lo = std::max(early.strikes.front(), late.strikes.front());
  const double hi = std::min(early.strikes.back(), late.strikes.back());
  if (hi < lo) return std::nullopt;
  const double spot = std::max(early.spot, late.spot);
  const auto test = [&](double k, std::size_t own, std::size_t node,
                        std::size_t other) -> std::optional<JointProjectionConflict> {
    const double floor = interpolate(early.strikes, early.lo, k);
    const double cap = interpolate(late.strikes, late.hi, k);
    const double excess = floor - cap - price_tolerance(spot, k);
    if (!(excess > 0.0)) return std::nullopt;
    return conflict_at(JointConflictKind::CalendarBox, own, node, other, k, excess);
  };
  for (std::size_t i = 0; i < early.strikes.size(); ++i) {
    const double k = early.strikes[i];
    if (k < lo || k > hi) continue;
    if (auto c = test(k, a, i, b)) return c;
  }
  for (std::size_t j = 0; j < late.strikes.size(); ++j) {
    const double k = late.strikes[j];
    if (k < lo || k > hi) continue;
    if (auto c = test(k, b, j, a)) return c;
  }
  return std::nullopt;
}

// valid_joint_side without the box test: an empty box is infeasible data, not a
// malformed input.
[[nodiscard]] bool well_formed_joint_side(const JointPriceSide &s) noexcept {
  const std::size_t n = s.strikes.size();
  if (n < 2u || s.prices.size() != n || s.lo.size() != n || s.hi.size() != n ||
      !std::isfinite(s.spot) || s.spot < 0.0) {
    return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (!std::isfinite(s.strikes[i]) || !std::isfinite(s.prices[i]) || !std::isfinite(s.lo[i]) ||
        !std::isfinite(s.hi[i]) || (i > 0u && !(s.strikes[i] > s.strikes[i - 1u]))) {
      return false;
    }
  }
  return true;
}

} // namespace

bool project_calendar_joint(std::span<const JointPriceSide> slices) {
  for (const JointPriceSide &s : slices) {
    if (!valid_joint_side(s)) return false;
  }
  std::vector<double> x;
  std::vector<double> lo;
  std::vector<double> hi;
  std::vector<std::size_t> offset;
  std::vector<SparseHalfSpace> spaces;
  build_joint(slices, x, lo, hi, offset, spaces);
  std::size_t sweeps = 0;
  if (!dykstra_sparse(x, lo, hi, spaces, 20000u, 1.0e-10, sweeps)) return false;
  scatter_joint(slices, x, offset);
  return true;
}

const char *to_string(JointProjectionOutcome value) noexcept {
  switch (value) {
  case JointProjectionOutcome::Feasible:
    return "feasible";
  case JointProjectionOutcome::InfeasibleData:
    return "infeasible_data";
  case JointProjectionOutcome::NonConverged:
    return "nonconverged";
  case JointProjectionOutcome::InvalidInput:
    return "invalid_input";
  }
  return "unknown";
}

const char *to_string(JointConflictKind value) noexcept {
  switch (value) {
  case JointConflictKind::None:
    return "none";
  case JointConflictKind::EmptyBox:
    return "empty_box";
  case JointConflictKind::NoConvexCurve:
    return "no_convex_curve";
  case JointConflictKind::CalendarBox:
    return "calendar_box";
  case JointConflictKind::LinearConstraint:
    return "linear_constraint";
  }
  return "unknown";
}

JointProjectionResult project_calendar_joint_bounded(std::span<const JointPriceSide> slices,
                                                     const JointProjectionBudget &budget) {
  JointProjectionResult result;
  if (budget.max_sweeps < 1u || !std::isfinite(budget.relative_tolerance) ||
      !(budget.relative_tolerance > 0.0)) {
    return result; // InvalidInput
  }
  for (const JointPriceSide &s : slices) {
    if (!well_formed_joint_side(s)) return result;
  }
  // Certificates first: each proves that no prices inside the boxes pass the caller's
  // unchanged certificate and calendar checks, so the sweeps would be wasted work.
  for (std::size_t m = 0; m < slices.size(); ++m) {
    if (auto conflict = slice_conflict(slices[m], m)) {
      result.outcome = JointProjectionOutcome::InfeasibleData;
      result.conflict = *conflict;
      return result;
    }
  }
  for (std::size_t a = 0; a < slices.size(); ++a) {
    for (std::size_t b = a + 1; b < slices.size(); ++b) {
      if (auto conflict = calendar_conflict(slices, a, b)) {
        result.outcome = JointProjectionOutcome::InfeasibleData;
        result.conflict = *conflict;
        return result;
      }
    }
  }
  std::vector<double> x;
  std::vector<double> lo;
  std::vector<double> hi;
  std::vector<std::size_t> offset;
  std::vector<SparseHalfSpace> spaces;
  build_joint(slices, x, lo, hi, offset, spaces);
  if (!dykstra_sparse(x, lo, hi, spaces, budget.max_sweeps, budget.relative_tolerance,
                      result.sweeps)) {
    result.outcome = JointProjectionOutcome::NonConverged;
    return result;
  }
  scatter_joint(slices, x, offset);
  result.outcome = JointProjectionOutcome::Feasible;
  return result;
}

Status check_american_calendar(const AmericanPriceSlice &earlier, const AmericanPriceSlice &later) {
  auto *counts = counters::fit_timing::current_counts();
  auto *work = counts ? &counts->calendar_certificate : nullptr;
  detail::CalendarWalkTimer timer(work);
  const auto first = earlier.nodes();
  const auto second = later.nodes();
  detail::PriceSlicePairWalk walk({first, {}}, {second, {}}, false, work);
  if (first.empty() || second.empty()) {
    return Err(ErrorCode::InvalidArgument, "American calendar: empty or moved-from slice");
  }
  const double spot = std::max(earlier.bounds().spot, later.bounds().spot);
  while (const auto point = walk.next()) {
    const double strike = point->strike;
    const double tol = price_tolerance(spot, strike);
    for (const Side side : {Side::Call, Side::Put}) {
      if (!point->early.available || !point->late.available) {
        return Err(ErrorCode::Internal, "American calendar: overlap price unavailable");
      }
      if (work) ++work->side_comparisons;
      const double near_price = side == Side::Call ? point->early.call : point->early.put;
      const double far_price = side == Side::Call ? point->late.call : point->late.put;
      if (near_price - far_price > tol) {
        return Err(ErrorCode::InvalidArgument,
                   "American calendar: later " + std::string(side == Side::Call ? "call" : "put") +
                       " price decreases at strike " + std::to_string(strike));
      }
    }
  }
  return Ok();
}

Status check_nondividend_american(const AmericanPriceSlice &slice, double df) {
  if (!std::isfinite(df) || !(df > 0.0) || df > 1.0) {
    return Err(ErrorCode::InvalidArgument,
               "nondividend American: constant nonnegative rate requires 0 < df <= 1");
  }
  const auto nodes = slice.nodes();
  if (nodes.size() < 2u) {
    return Err(ErrorCode::InvalidArgument, "nondividend American: empty or moved-from slice");
  }
  const double spot = slice.bounds().spot;
  for (std::size_t i = 0u; i < nodes.size(); ++i) {
    const auto &node = nodes[i];
    const double tol = price_tolerance(spot, node.strike);
    const double european = node.call - spot + df * node.strike;
    if (node.call < std::max(spot - df * node.strike, 0.0) - tol || node.call - spot > tol) {
      return node_error(i, "nondividend European call price bound violated");
    }
    if (european - node.put > tol) {
      return node_error(i, "nondividend American put is below its European put");
    }
    if (i > 0u) {
      ATX_TRY_VOID(validate_nondividend_segment(nodes[i - 1u], node, spot, df, i - 1u));
    }
  }
  ATX_TRY_VOID(validate_call_lower_slopes(nodes, spot, df,
                                          "nondividend European call slope is below -df"));
  return validate_nondividend_upper(slice, df);
}

} // namespace atx::vol
