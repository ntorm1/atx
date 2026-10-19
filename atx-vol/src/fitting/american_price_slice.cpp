#include "atx/vol/api/fitting/american_price_slice.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

#include "atx/vol/api/pricing/american.hpp"

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

Status check_american_calendar(const AmericanPriceSlice &earlier, const AmericanPriceSlice &later) {
  const auto first = earlier.nodes();
  const auto second = later.nodes();
  if (first.empty() || second.empty()) {
    return Err(ErrorCode::InvalidArgument, "American calendar: empty or moved-from slice");
  }
  const double lo = std::max(first.front().strike, second.front().strike);
  const double hi = std::min(first.back().strike, second.back().strike);
  if (hi < lo) {
    return Ok();
  }
  const double spot = std::max(earlier.bounds().spot, later.bounds().spot);
  // Both node sets are already sorted and validated. A merge visits every
  // possible extremum of the piecewise affine calendar spread, once each.
  std::size_t i = 0u;
  std::size_t j = 0u;
  while (i < first.size() || j < second.size()) {
    const double strike =
        j == second.size() || (i < first.size() && first[i].strike <= second[j].strike)
            ? first[i++].strike
            : second[j++].strike;
    if (i < first.size() && first[i].strike == strike) {
      ++i;
    }
    if (j < second.size() && second[j].strike == strike) {
      ++j;
    }
    if (strike < lo || strike > hi) {
      continue;
    }
    const double tol = price_tolerance(spot, strike);
    for (const Side side : {Side::Call, Side::Put}) {
      const auto near_price = earlier.price(strike, side);
      const auto far_price = later.price(strike, side);
      if (!near_price || !far_price) {
        return Err(ErrorCode::Internal, "American calendar: overlap price unavailable");
      }
      if (*near_price - *far_price > tol) {
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
