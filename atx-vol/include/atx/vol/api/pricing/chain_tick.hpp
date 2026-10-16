#pragma once

// Tick-path chain refresh (pricing hot-path audit 2026-10-16, items 1-5).
//
// After a spot tick a market maker re-marks a whole listed chain: price plus
// delta / gamma / vega / theta for every contract, inside a few hundred
// microseconds on one core. This header is the API for that path.
//
// ## Execution contract (every entry point in this header)
//
//   * INLINE on the calling thread. Nothing here dispatches to the pricing pool:
//     a PricingExecutor wake costs ~124 us (pricing_executor.cpp), 41% of the
//     300 us chain budget, so the tick path must never touch it. Pin the calling
//     thread to an isolated P-core instead (the bench does, audit item 5a).
//   * Caller-owned structure-of-arrays outputs, sized once and reused per tick.
//   * One `TickStatus` byte per contract instead of a per-contract
//     Result<..., std::string>; `Status` reports only call-shape errors, at the
//     boundary.
//
// Tier-B: public and includable, deliberately outside the frozen umbrella.
//
// Thread-safety: the free functions are pure in their inputs and write only the
// caller's output spans, so concurrent calls with disjoint outputs are safe.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"            // Side, Status
#include "atx/vol/api/fitting/correction.hpp"    // AmericanCorrectionCaches
#include "atx/vol/api/fitting/expiry_record.hpp" // ExpiryRecord, ExpiryRecordBank
#include "atx/vol/api/pricing/american.hpp"      // AlOpts

namespace atx::vol {

// Per-contract outcome of one tick refresh. A single byte so a 2,000-contract
// status column is 2 KB and the hot loop never builds an error string.
enum class TickStatus : std::uint8_t {
  Ok = 0,          // served by the route's continuation-region model
  Exercised = 1,   // spot is in the early-exercise region: intrinsic value,
                   // delta = +/-1 and gamma = vega = theta = 0 (the cold
                   // reference american_greeks_al reports theta = 0 there too)
  OutOfWindow = 2, // reserved: the spot ladder's kernel flags a lane whose spot
                   // left its window, and the tick resolves every such lane
                   // (rebuild, or Fallback) before it returns, so no public
                   // route emits this value today
  Fallback = 3,    // served by the route's documented fallback (e.g. the cached
                   // route with no cache for this side serves the Black-76 leg;
                   // the record routes serve the box-clamped record value when
                   // (k, sigma) has left the record's box, which is NOT covered
                   // by any certificate)
  Unpriceable = 4, // the model refused this contract; outputs are NaN
};

// A listed chain as caller-owned columns, one entry per contract. `expiry` is
// the year fraction T, `rate`/`carry` the continuous r and q (carry = the
// expiry's effective q, i.e. borrow plus dividend yield). Contracts may be in
// any order; routes that group by expiry do so at build time.
struct ChainContractsView {
  std::span<const double> strike;
  std::span<const double> expiry;
  std::span<const double> sigma;
  std::span<const double> rate;
  std::span<const double> carry;
  std::span<const Side> side;

  [[nodiscard]] std::size_t size() const noexcept { return strike.size(); }
  [[nodiscard]] bool consistent() const noexcept {
    const std::size_t n = strike.size();
    return expiry.size() == n && sigma.size() == n && rate.size() == n &&
           carry.size() == n && side.size() == n;
  }
};

// Caller-owned output columns, one entry per contract, in the input order.
// theta is the calendar derivative dV/dt per year (the american_greeks_al
// convention), vega is per unit of sigma.
struct ChainTickOutputs {
  std::span<double> price;
  std::span<double> delta;
  std::span<double> gamma;
  std::span<double> vega;
  std::span<double> theta;
  std::span<TickStatus> status;

  [[nodiscard]] std::size_t size() const noexcept { return price.size(); }
  [[nodiscard]] bool consistent() const noexcept {
    const std::size_t n = price.size();
    return delta.size() == n && gamma.size() == n && vega.size() == n &&
           theta.size() == n && status.size() == n;
  }
};

// Baseline tick route (audit §4 `cached_scalar`): every contract is re-marked
// with the cached-correction Greek bundle, american_greeks(S, K, T, sigma, r, q,
// side, cache), inline. The same numbers a per-contract loop over that function
// produces (bit-identical), with TickStatus::Fallback where `caches` has no
// cache for the contract's side (the Black-76 leg is then served, exactly as
// american_greeks documents) and TickStatus::Unpriceable + NaN where it refuses.
// This route still pays a full 3-D second-order correction sweep per contract;
// it exists as the measured baseline the record routes are compared against.
// Allocation-free on the served path; a refused contract goes through
// american_greeks' error Result, which builds its message string (an
// allocation) before the contract is written Unpriceable.
//
// @return InvalidArgument when the columns disagree in length or `spot` is not
//         a finite positive number; nothing is written then.
[[nodiscard]] Status refresh_chain_cached(double spot, const ChainContractsView &chain,
                                          const AmericanCorrectionCaches &caches,
                                          const ChainTickOutputs &out);

// ── Per-expiry calibration records for a chain (audit item 2) ────────────────

// How a chain's records are sized at refit. One record per distinct (T, r, q,
// side) group of contracts.
struct ChainRecordPolicy {
  // ln-spot excursion, both ways, the moneyness box must cover beyond the listed
  // strikes at the build spot. A spot move past it takes contracts out of their
  // record's box (served clamped, TickStatus::Fallback).
  double spot_move = 0.05;
  // sigma box = [sigma_lo_mult * min sigma, sigma_hi_mult * max sigma] of the group.
  double sigma_lo_mult = 0.6;
  double sigma_hi_mult = 1.6;
  std::optional<AlOpts> sample_opts = std::nullopt; // nullopt = accurate preset
  bool certify = true;
};

// The records of one chain plus the contract -> record map. Built once per refit
// (allocates); immutable afterwards and safe for concurrent reads.
class ChainRecords {
public:
  // @return InvalidArgument for mismatched columns / non-positive spot, or the
  //         first ExpiryRecord::build error.
  [[nodiscard]] static Result<ChainRecords> build(double spot, const ChainContractsView &chain,
                                                  const ChainRecordPolicy &policy = {});

  [[nodiscard]] const ExpiryRecordBank &bank() const noexcept { return bank_; }
  // record_of_contract()[i] indexes bank() for contract i.
  [[nodiscard]] std::span<const std::uint32_t> record_of_contract() const noexcept {
    return record_of_;
  }
  // Largest certified_max_abs_error_fwd() over the records (NaN if uncertified).
  [[nodiscard]] double worst_certified_error_fwd() const noexcept { return worst_cert_; }

private:
  ExpiryRecordBank bank_{0};
  std::vector<std::uint32_t> record_of_;
  double worst_cert_ = 0.0;
};

// Record tick route (audit item 2): each contract is re-marked from its expiry
// record — price, delta, gamma, vega and the continuation-PDE theta — inline.
// TickStatus::Fallback when the contract's (k, sigma) has left its record's box
// (the box-clamped record value is served, flagged, uncertified; this takes
// precedence over the exercise test, whose edge is clamped in sigma there too);
// otherwise TickStatus::Exercised in the exercise region and Ok elsewhere;
// Unpriceable + NaN for a non-positive strike or sigma. Only `strike` and
// `sigma` are read per contract: T, r and q are the record's own (the chain was
// grouped by them when `records` was built).
//
// @return InvalidArgument for mismatched columns, a non-positive / non-finite
//         spot, or `records` built for a different contract count.
[[nodiscard]] Status refresh_chain_records(double spot, const ChainContractsView &chain,
                                           const ChainRecords &records,
                                           const ChainTickOutputs &out);

// Record IV route (audit item 3): sigma_out[i] = the American implied vol of
// target_price[i] against contract i's expiry record (ExpiryRecord::implied_vol),
// warm-started at chain.sigma[i] — the surface vol at that strike. Inline and
// allocation-free; status_out[i] carries the per-contract RecordIvStatus.
//
// @return InvalidArgument when any span length disagrees with the chain or the
//         records, or spot is not finite > 0 (nothing is written).
[[nodiscard]] Status implied_vols_from_records(double spot, const ChainContractsView &chain,
                                               const ChainRecords &records,
                                               std::span<const double> target_price,
                                               std::span<double> sigma_out,
                                               std::span<RecordIvStatus> status_out);

// ── Per-contract Chebyshev spot ladders (audit items 1 + 4) ──────────────────
//
// sigma is fixed per contract between refits, so after a spot tick each
// contract's value is a smooth function of ONE variable. At refit every
// contract gets two Chebyshev series in spot over a window around the build
// spot — price and vega — sampled from its expiry record (ChainRecords).
// A tick then costs two short Clenshaw recurrences per contract and no
// transcendental: delta and gamma are the first and second derivatives of the
// SAME price series (so they are exactly consistent with the served price), and
// theta is the continuation-region PDE identity
// theta = r V - (r - q) S delta - 0.5 sigma^2 S^2 gamma on those values (the
// american_greeks_al definition; no third series is stored).
//
// The exercise boundary splits the window: on the exercise side the contract is
// served intrinsic (TickStatus::Exercised) and the series covers only the
// continuation side, where the price is smooth.
//
// Layout: contracts are packed in blocks of kSpotLadderLanes consecutive
// contracts (input order), each block holding its coefficients lane-interleaved
// ([node][lane]) with the per-contract window/affine map/carry beside them, so
// the per-tick loop streams the blocks in order and the lane loops vectorize
// under /arch:AVX2. 2,000 contracts at 16 nodes are ~0.7 MB, L2-resident.
//
// Record box: the record's certificate covers its (k, sigma) box only, so every
// window is clipped to the spot range that keeps k = ln(K / F) inside
// [k_min, k_max], and a contract whose sigma is outside the record's sigma box
// has no window at all. A lane reported Ok or Exercised is therefore always
// inside its record's box. A spot outside the box is served from the record —
// the box-clamped value, flagged TickStatus::Fallback and covered by no
// certificate — until the spot comes back, when the next tick rebuilds it. A
// lane that STAYS outside its box (or whose sigma is outside the sigma box)
// needs a refit: rebuild the ChainRecords at the new spot / smile and then the
// ladder. counters() tells that case (out_of_box / sigma_out_of_box / refused)
// apart from transient rebuild-budget pressure (budget).
//
// Certification (per contract, at build and at every rebuild): the price series
// is compared with the record at every midpoint between adjacent nodes and at
// the domain ends; certified_error_usd()[i] = that maximum + F_max x the record's
// own certified error against the cold solver (F_max at the window's top) + an
// ulp-scale floor (8 ulps of max(F_max, K): the forward-unit grid certificate
// cannot see rounding in a price of a few hundred dollars). The certificate is
// accepted when it is at most max(SpotLadderPolicy::max_cert_usd,
// max_cert_rel x F_max). A contract whose certificate exceeds that even at the
// smallest window, or whose record is uncertified (ChainRecordPolicy::certify =
// false, or a certification row the cold solver could not price), is REFUSED:
// it is served from its record every tick (TickStatus::Fallback).
// certified_error_usd()[i] describes contract i's current window and is NaN
// when it has none (uncertified record, a build spot outside its record box, or
// a sigma outside the record's sigma box); a tick whose spot is outside the
// window serves the record (Fallback) and makes no use of it. Greeks are not
// separately certified; the bench measures them against american_greeks_al.
// Where the series falls to intrinsic the floor is served with the exercise
// Greeks (delta +/-1, gamma = vega = theta = 0) and TickStatus::Exercised, so the
// Greeks always belong to the price that is served.
//
// Window exits: when the spot leaves a contract's window (inside its record
// box) the tick rebuilds that contract's ladder centred on the new spot (from
// the record, ~16 record evaluations), up to max_rebuilds_per_tick per tick;
// past the budget the contract is served from its record that tick
// (TickStatus::Fallback) and rebuilt on a later tick. A spot outside the box
// does not spend the budget. Rebuilds are counted (rebuilds()).

inline constexpr std::size_t kSpotLadderLanes = 4;
inline constexpr std::uint16_t kSpotLadderNodes = 16;

struct SpotLadderPolicy {
  // Window half-width in ln-spot = clamp(window_sigmas * sigma * sqrt(T),
  // window_min, window_max), halved (down to window_min) while a contract's
  // certificate exceeds its acceptance threshold.
  double window_sigmas = 2.0;
  double window_min = 0.0025;
  double window_max = 0.04;
  // Acceptance threshold = max(max_cert_usd, max_cert_rel x F_max): an absolute
  // dollar floor plus a forward-scaled allowance. The record certificate is a
  // forward-unit bound, so its dollar size grows with the underlying; a fixed
  // $5e-4 refused every contract of a ~$5,000 underlying (record certificates
  // ~3.3e-7 fwd = $1.7e-3 there). The defaults keep the floor binding below
  // F ~ $1,000, so a $600 chain is unchanged.
  double max_cert_usd = 5.0e-4;
  double max_cert_rel = 5.0e-7;
  std::uint32_t max_rebuilds_per_tick = 32;
  bool rebuild_on_exit = true;
};

// Where a ChainSpotLadder's record fallbacks came from, since build(). Every
// Fallback lane-tick lands in exactly one bucket; fallbacks() is their sum.
struct SpotLadderCounters {
  std::uint64_t rebuilds = 0; // in-tick window rebuilds
  // Transient: an in-box lane left its window after this tick's rebuild budget
  // was spent; it is rebuilt on a later tick.
  std::uint64_t budget_fallbacks = 0;
  // The spot is outside the lane's record (k) box. Clears if the spot comes
  // back; a lane that stays out needs a refit (ChainRecords at the new spot).
  std::uint64_t out_of_box_fallbacks = 0;
  // The contract's sigma is outside its record's sigma box: no window ever. A
  // refit (ChainRecords from the current smile) is required.
  std::uint64_t sigma_out_of_box_fallbacks = 0;
  // The contract is refused (uncertified record, or no certifiable window).
  // A refit is required.
  std::uint64_t refused_fallbacks = 0;
};

class ChainSpotLadder {
public:
  ChainSpotLadder();
  ~ChainSpotLadder();
  ChainSpotLadder(ChainSpotLadder &&) noexcept;
  ChainSpotLadder &operator=(ChainSpotLadder &&) noexcept;
  ChainSpotLadder(const ChainSpotLadder &) = delete;
  ChainSpotLadder &operator=(const ChainSpotLadder &) = delete;

  // Build every contract's ladder around `spot` from `records` (copied: the
  // ladder owns everything a tick reads). `records` must have been built for
  // `chain` (same contract count and order).
  // @return InvalidArgument for mismatched columns / records / non-positive spot.
  [[nodiscard]] static Result<ChainSpotLadder> build(double spot, const ChainContractsView &chain,
                                                     const ChainRecords &records,
                                                     const SpotLadderPolicy &policy = {});

  // The tick: price, delta, gamma, vega, theta for every contract at `spot`,
  // inline, allocation-free. Not const: a window exit rebuilds that contract.
  // @return InvalidArgument when `out` has the wrong length or `spot` is not a
  //         finite positive number (nothing is written).
  [[nodiscard]] Status refresh(double spot, const ChainTickOutputs &out);

  [[nodiscard]] std::size_t size() const noexcept;
  // In-tick rebuilds and record fallbacks since build(); fallbacks() is the sum
  // of the four fallback buckets of counters().
  [[nodiscard]] std::uint64_t rebuilds() const noexcept;
  [[nodiscard]] std::uint64_t fallbacks() const noexcept;
  [[nodiscard]] SpotLadderCounters counters() const noexcept;
  // Per-contract certified price error in dollars of the current windows (NaN
  // for a contract with no window), the contracts refused (served from their
  // record every tick), and the contracts whose sigma is outside their record's
  // sigma box (no window ever; also served from the record every tick). Both of
  // the latter need a refit.
  [[nodiscard]] std::span<const double> certified_error_usd() const noexcept;
  [[nodiscard]] std::size_t refused_contracts() const noexcept;
  [[nodiscard]] std::size_t sigma_out_of_box_contracts() const noexcept;
  // Bytes of tick-path data (blocks of coefficients and per-contract scalars).
  [[nodiscard]] std::size_t tick_bytes() const noexcept;

private:
  struct Impl;
  explicit ChainSpotLadder(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::vol
