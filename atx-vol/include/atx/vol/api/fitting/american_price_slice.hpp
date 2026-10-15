#pragma once

#include <optional>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

struct AmericanPriceNode {
  double strike{};
  double call{};
  double put{};
};

// Bounds on discount factors and prepaid stock values over the exercise
// horizon, including immediate exercise. Negative rates are supported:
// discount_max may exceed one. The caller supplies bounds valid for the
// contract's carry/dividend model; this type does not infer them from quotes.
struct AmericanPriceBounds {
  double spot{};
  double discount_min{};
  double discount_max{};
  double prepaid_min{};
  double prepaid_max{};
};

// Immutable, owned call/put prices on a finite strike interval. Construction
// validates intrinsic/upper bounds, vertical spreads, convexity and affine
// call-minus-put bounds. Piecewise linear interpolation preserves these
// inequalities between nodes; no extrapolation is offered.
//
// The only numerical allowance is 1e-8 + 1e-9 * max(spot, strike), in price
// units. Pair/triple checks use their largest strike. Convexity compares the
// middle price with its chord, equivalent to ordering adjacent secants but
// stable for nearly coincident strikes. This is a roundoff allowance, not a
// configurable quote-fit tolerance. It cannot be disabled or relaxed.
//
// Factory/copy operations allocate; price() and nodes() do not. Concurrent
// const access is safe. Queries on an empty moved-from object return nullopt.
class AmericanPriceSlice {
public:
  // Requires at least two nodes, finite positive strictly increasing strikes,
  // finite nonnegative prices and finite, ordered carry bounds. Nodes are
  // copied only after all admission checks pass; malformed or arbitrageable
  // input returns an explicit error and never creates a usable slice.
  [[nodiscard]] static Result<AmericanPriceSlice> create(std::span<const AmericanPriceNode> nodes,
                                                         const AmericanPriceBounds &bounds);

  [[nodiscard]] std::span<const AmericanPriceNode> nodes() const noexcept { return nodes_; }
  [[nodiscard]] const AmericanPriceBounds &bounds() const noexcept { return bounds_; }

  // Includes both endpoints. Nonfinite/out-of-domain strikes and invalid
  // Side values return nullopt without allocating or throwing.
  [[nodiscard]] std::optional<double> price(double strike, Side side) const noexcept;

private:
  AmericanPriceSlice(std::vector<AmericanPriceNode> nodes, AmericanPriceBounds bounds) noexcept;

  std::vector<AmericanPriceNode> nodes_;
  AmericanPriceBounds bounds_;
};

// Check later >= earlier for both American calls and puts at every knot in
// the overlap of their strike domains. Their difference is affine between
// consecutive union knots, so this certifies the whole overlap to the fixed
// price tolerance above (using the larger spot). Disjoint domains are
// vacuously valid. The caller must provide the same contract/deliverable and
// valuation instant, ordered by expiry; this type carries no identity or time.
[[nodiscard]] Status check_american_calendar(const AmericanPriceSlice &earlier,
                                             const AmericanPriceSlice &later);

} // namespace atx::vol
