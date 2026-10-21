#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

struct AlOpts; // api/pricing/american.hpp

struct AmericanPriceNode {
  double strike{};
  double call{};
  double put{};
};

// ── Model-priced certification (L2b al-geometry, 2026-10-19; opt-in) ──────
//
// The second AmericanPriceSlice::create overload prices every node itself with
// Andersen-Lake — the call and the put at the node's volatility and yield, the model's
// expiry, rate and AlOpts, at bounds.spot — and certifies those prices with the SAME
// checks as the price-input create(). Its defaults (Scalar, AccurateOnly) are exactly
// the composition a caller writes today: andersen_lake per price, then create().
//
// CertificationKernel::Batched prices through al_price_pack on one AlExpiryGeometry per
// run of consecutive equal-yield nodes: bit-identical prices, hence the same verdict.
//
// CertificationScreen::FastThenAccurate runs a Fast-kernel screen first. Nodes are
// priced in strike order on al_fast_profile(opts) (the same scheme on the Fast kernel)
// in chunks of kCertificationScreenChunk, and after each chunk the priced prefix is
// checked with every inequality of create() WIDENED by the Fast allowance
// kCertificationFastAllowance * max(|P|, 1) of each price it reads (a chord by the
// larger allowance of its two ends). A violation that survives the widening is certain
// for the accurate prices too — a prefix's convexity chord lies above the whole slice's
// lower envelope, and its running extrema are a subset of the full ones — so the slice
// is refused (InvalidArgument, "certification screen" in the message) with no accurate
// price. Otherwise, or when the screen cannot run (malformed bounds or strikes, a Fast
// price that errors or is not finite), every node is priced accurately and certified
// exactly as AccurateOnly: an accepted slice, and every refusal the screen could not
// prove, carry the accurate kernel's own verdict and message. The allowance is 100x the
// AlProfile::Fast price bound pinned by AlFastProfile.* (american.hpp, 1e-12 * max(P, 1)).
enum class CertificationKernel : std::uint8_t { Scalar = 0, Batched = 1 };
enum class CertificationScreen : std::uint8_t { AccurateOnly = 0, FastThenAccurate = 1 };

inline constexpr double kCertificationFastAllowance = 1.0e-10;
inline constexpr std::size_t kCertificationScreenChunk = 8;

// One model-priced node: its strike, volatility and continuous yield (strike-varying
// carry is allowed; Batched shares a geometry across consecutive equal yields).
struct AmericanCertificationNode {
  double strike{};
  double sigma{};
  double yield{};
};

// The model every node shares: expiry (year fraction) and continuous rate. `al_opts`
// observes the caller's options for the call's duration; nullptr selects andersen_lake's
// accurate preset (its std::nullopt).
struct AmericanCertificationModel {
  double T{};
  double rate{};
  const AlOpts *al_opts = nullptr;
};

// Price counts of one model-priced create(): accurate-kernel and Fast-kernel
// andersen_lake prices, and whether the Fast screen alone refused the slice.
struct AmericanCertificationStats {
  std::size_t accurate_prices = 0;
  std::size_t fast_prices = 0;
  bool screen_refused = false;
};

// Bounds on discount factors and prepaid stock values over the exercise
// horizon, including immediate exercise. Negative rates are supported:
// discount_max may exceed one. The caller supplies bounds valid for the
// contract's carry/dividend model; this type does not infer them from quotes.
// Since immediate exercise is available, [discount_min, discount_max] must
// contain one and [prepaid_min, prepaid_max] must contain spot.
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
// units. Vertical-spread checks apply to ALL node pairs, with the tolerance
// at their larger strike. Convexity bounds every node's distance above its
// side's global lower convex envelope by the tolerance at that node. These
// O(n) checks prevent many small local violations from accumulating into a
// material violation across the domain. Prices are never projected or changed.
// This is a fixed numerical allowance, not a configurable quote-fit tolerance.
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

  // Model-priced certification (see CertificationKernel / CertificationScreen above).
  // A node andersen_lake refuses returns its error, prefixed with the node index; an
  // out-of-range kernel or screen value returns InvalidArgument. `stats` (nullable) is
  // reset, then receives the price counts. Allocates; thread-compatible.
  [[nodiscard]] static Result<AmericanPriceSlice>
  create(std::span<const AmericanCertificationNode> nodes, const AmericanCertificationModel &model,
         const AmericanPriceBounds &bounds, CertificationKernel kernel = CertificationKernel::Scalar,
         CertificationScreen screen = CertificationScreen::AccurateOnly,
         AmericanCertificationStats *stats = nullptr);

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

// ── Box certificate (lane L3r1 dense-certificate, 2026-10-20; opt-in) ──────
//
// The exact certificate asks "is this point curve arbitrage-free to the numerical
// allowance?". A quote can only answer "does an arbitrage-free curve exist inside
// the bid/ask boxes?". certify_american_box answers the second question and, when
// the answer is yes, returns that curve: the fitted node prices are projected
// (least squares, Dykstra, uniform weights) side by side onto the set the exact
// certificate's strike checks describe — convex in strike, call slopes in
// [-discount_max, 0], put slopes in [0, discount_max], the intrinsic and upper
// price bounds — intersected with every node side's box, and the UNCHANGED
// create() then certifies the projected curve P*. Nothing about certification is
// relaxed: P* passes exactly the tests the fitted prices failed. The caller
// chooses the boxes (a quote's [bid - h, ask + h]; a side without a quote is
// held at its fitted price with lo == hi). Nodes whose constraints never bind
// keep their fitted price bit for bit.

// Per node, the interval each side's price may take; lo <= hi, finite. A side
// held fixed has lo == hi == its fitted price.
struct AmericanPriceBox {
  double call_lo{};
  double call_hi{};
  double put_lo{};
  double put_hi{};
};

// The certified in-box curve and its distance from the fitted prices.
struct AmericanBoxCertificate {
  AmericanPriceSlice slice;        // P*, certified by create()
  std::vector<std::uint8_t> moved; // per node: 1 = call moved, 2 = put moved (OR'd)
  std::size_t moved_nodes = 0;     // strikes whose call or put price moved
  double max_price_move = 0.0;     // largest |P* - fitted| over both sides
};

// Weighted least-squares projection of one side's node prices `prices` (in
// place) onto { convex in strike; first slope >= slope_min; last slope <=
// slope_max; lo <= p <= hi }: argmin sum w_i (p_i - p0_i)^2 by Dykstra's
// algorithm (Hildreth's form for the half-spaces, a full increment for the
// box). Convex + end slopes cover every vertical-spread bound of the
// certificate. Nodes whose constraints never bind are untouched bit for bit.
// Bounded: at most 20,000 sweeps of O(n). Returns false when a box is empty
// (lo > hi) or the sweeps do not settle; `prices` is then unspecified.
// Requires equal lengths, strictly increasing finite strikes, positive weights.
[[nodiscard]] bool project_convex_price_box(std::span<const double> strikes,
                                            std::span<double> prices, std::span<const double> lo,
                                            std::span<const double> hi,
                                            std::span<const double> weights, double slope_min,
                                            double slope_max);

// Box certificate of `fitted` (one box per node, same order). Each side's box
// is first intersected with the certificate's own per-node bounds (intrinsic,
// upper); an empty intersection, a box set with no convex curve inside it (the
// greatest convex minorant of the upper edges falls below a lower edge — an
// exact test), or a projection that does not settle returns Unavailable ("no
// arbitrage-free curve inside the boxes"). The projected curve is then handed
// to create(); its refusal (for example the affine call-minus-put bound, which
// couples the two sides) is returned with create()'s code and message. Malformed
// input (sizes, non-finite values, a box with lo > hi, non-increasing strikes,
// invalid bounds) is InvalidArgument. Allocates O(n); thread-compatible.
[[nodiscard]] Result<AmericanBoxCertificate>
certify_american_box(std::span<const AmericanPriceNode> fitted,
                     std::span<const AmericanPriceBox> boxes, const AmericanPriceBounds &bounds);

// Check later >= earlier for both American calls and puts at every knot in
// the overlap of their strike domains. Their difference is affine between
// consecutive union knots, so this certifies the whole overlap to the fixed
// price tolerance above (using the larger spot). Disjoint domains are
// vacuously valid. The caller must provide the same contract/deliverable and
// valuation instant, ordered by expiry; this type carries no identity or time.
[[nodiscard]] Status check_american_calendar(const AmericanPriceSlice &earlier,
                                             const AmericanPriceSlice &later);

// Additional necessary conditions for a NON-DIVIDEND stock at a constant
// nonnegative rate, df = exp(-r*T) in (0,1]. The caller must establish this
// regime (no cash dividends or effective stock yield); the bounds alone do
// not identify a dividend model. Negative-rate inputs are rejected.
//
// American calls are European in this regime, so E(K)=C(K)-spot+df*K.
// Check European call bounds/slopes, A>=E, and the Cox-Hoeggerl condition
// K*A'_+(K)-A(K) >= K*C'_+(K)-C(K)+spot on every represented interval.
// Check A(K)<=C(K/df)-spot+K on [K_min, df*K_max] at the union of native
// and df-scaled knots; an empty comparison interval returns Unavailable.
// No extrapolation or right derivative at the final endpoint is inferred.
//
// Source: Cox and Hoeggerl, Theorem 2.3, https://arxiv.org/pdf/1301.5467.
// Success is a finite-domain necessary-condition certificate, not the
// global model-existence conclusion of their Theorem 3.9.
[[nodiscard]] Status check_nondividend_american(const AmericanPriceSlice &slice, double df);

} // namespace atx::vol
