#pragma once

// MarketState — ONE instant of the market for N underliers: per-underlier scalars
// (spot, rate, carry, cash dividends), per-contract vol MARKS (a vendor's or a
// desk's sigma per listed contract, e.g. SpiderRock srVol) and / or a fitted
// surface per underlier (PricedSurface / PricedSurfaceView) for contracts the marks
// do not cover and for theoretical contracts.
//
// FROZEN API CONTRACT (sprint 2026-10-24; see risk_types.hpp banner).
//
// ## Why marks AND surfaces
//
// An explain between two real snapshots needs, per listed contract, the sigma the
// market marked it at in each state. That is a lookup, not an interpolation, and it
// needs no refit. A theoretical contract (a 7-day 20-delta put that is not listed)
// needs a surface. A state may carry either or both per uid; `resolve` reads marks
// first (exact key) and falls through to the surface (flag MarkMissing). A uid with
// neither refuses ModelUnavailable.
//
// ## Ownership
//
// `MarketState` is a VALUE of non-owning views: every span and SurfaceRef borrows
// storage the caller keeps alive for the state's lifetime (a parquet decode, a
// `MarketStateStorage`, a mapped archive). It owns only its small uid index. Copying
// a state copies the views. `MarketStateStorage` is the owning aggregate for callers
// who build states from scalars (tests, small users, the SpiderRock loader).
//
// ## Thread-safety
//
// Immutable after `create`; every query is const and allocation-free; any number of
// threads may query one state concurrently.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "atx/vol/api/backtest/portfolio_pricer.hpp" // SurfaceRef
#include "atx/vol/api/core/types.hpp"                // Result, Status, Side
#include "atx/vol/api/core/vol_time.hpp"             // TimeSpec
#include "atx/vol/api/pricing/rates_curve.hpp"       // YieldCurve, DividendEvent
#include "atx/vol/api/risk/risk_types.hpp"

namespace atx::vol {

// Where a contract's forward / effective carry come from. The SPOT is always the
// UnderlierState's (`spot`): a surface never overrides it.
enum class CarryFrom : std::uint8_t {
  State = 0,   // term_carry row for the expiry, else flat_q / cash_divs (the default)
  Surface = 1, // the surface's own forward_at(T) / q_eff_at(T) / rate_at(T)
};

// Per-expiry carry inputs (the shape a vendor feed publishes: SpiderRock writes one
// (rate, sdiv, ddiv) tuple per expiry). `pv_divs` is the PRESENT VALUE at the state's
// instant of the cash dividends paid before `expiry_ns`, so the forward is exactly
//   F = (spot - pv_divs) * exp((rate - q) * T).
// Rows sorted ascending by expiry_ns; looked up by EXACT expiry_ns.
struct ExpiryCarry {
  std::int64_t expiry_ns{0};
  double pv_divs{0.0};
  double rate{0.0};
  double q{0.0};
};

// One underlier's scalars at the state's instant.
//
// Carry precedence under CarryFrom::State: a `term_carry` row with the contract's
// exact expiry_ns wins (rate, q, pv_divs from the row); otherwise rate = `yield` at T
// or `flat_rate`, q = `flat_q`, and pv_divs = sum over `cash_divs` with
// 0 < t_ex <= T of amount * exp(-rate * t_ex) (amount = cash at the ex-date).
struct UnderlierState {
  std::uint32_t uid{0};
  double spot{0.0};             // pricing spot (> 0); authoritative for every route
  double flat_rate{0.0};        // continuously-compounded; used when `yield` is null
  const YieldCurve *yield{nullptr};        // nullable term curve (borrowed)
  double flat_q{0.0};           // continuous carry yield when no term_carry row applies
  std::span<const DividendEvent> cash_divs{}; // escrowed into F(T); see precedence above
  std::span<const ExpiryCarry> term_carry{};  // per-expiry overrides; may be empty
  CarryFrom carry{CarryFrom::State};
};

// Listed-contract vol marks for ONE uid. Structure-of-arrays, all spans equal
// length, rows sorted ascending by (expiry_ns, side, strike) so `resolve` can
// binary-search. `vendor_price` / `vendor_delta` are optional (empty) and are used
// only for parity diagnostics, never for pricing.
struct ContractMarks {
  std::uint32_t uid{0};
  std::span<const double> strike{};
  std::span<const std::int64_t> expiry_ns{};
  std::span<const Side> side{};
  std::span<const double> sigma{};
  std::span<const double> vendor_price{};
  std::span<const double> vendor_delta{};
  [[nodiscard]] std::size_t size() const noexcept { return sigma.size(); }
};

// One contract resolved against a state: everything a pricer needs besides K/side.
struct ResolvedContract {
  double T{0.0};        // year-fraction under the state's TimeSpec; <= 0 => Rolled
  double sigma{0.0};    // the vol the contract is priced at (marks first, else surface)
  double forward{0.0};  // F(T)
  double q_eff{0.0};    // effective carry: rate - ln(F/S)/T
  double rate{0.0};     // zero rate at T
  double spot{0.0};
  double vendor_price{0.0}; // NaN unless VendorParityAvailable
  double vendor_delta{0.0}; // NaN unless VendorParityAvailable
  RiskFlags flags{0};
  RiskLaneStatus status{RiskLaneStatus::Ok};
};

class MarketState {
public:
  // Validate and index. `underliers` must have distinct uids; every `marks` and
  // `surfaces` entry must name a uid present in `underliers` (a surface's `uid()` is
  // its key); marks must be sorted as documented and span-length consistent.
  // InvalidArgument otherwise. Empty `marks` / `surfaces` are legal (a uid with
  // neither resolves ModelUnavailable).
  [[nodiscard]] static Result<MarketState> create(std::int64_t ts_ns, const TimeSpec &time,
                                                  std::span<const UnderlierState> underliers,
                                                  std::span<const ContractMarks> marks,
                                                  std::span<const SurfaceRef> surfaces);

  [[nodiscard]] std::int64_t ts_ns() const noexcept { return ts_ns_; }
  [[nodiscard]] const TimeSpec &time() const noexcept { return time_; }
  [[nodiscard]] std::size_t n_underliers() const noexcept { return underliers_.size(); }
  [[nodiscard]] std::span<const UnderlierState> underliers() const noexcept {
    return underliers_;
  }

  // nullptr / null ref when the uid is absent from the state.
  [[nodiscard]] const UnderlierState *underlier(std::uint32_t uid) const noexcept;
  [[nodiscard]] const ContractMarks *marks(std::uint32_t uid) const noexcept;
  [[nodiscard]] SurfaceRef surface(std::uint32_t uid) const noexcept;

  // Year-fraction from this state's instant to `expiry_ns` under `time()`.
  // Calendar365 is infallible; VolTime may return OutOfRange (vol_time.hpp).
  [[nodiscard]] Result<double> time_to_expiry(std::int64_t expiry_ns) const noexcept;

  // Resolve one contract. Never throws; a non-Ok `status` comes with the reason and
  // NaN numeric fields. Marks are keyed on (expiry_ns, side, strike) by EXACT bits.
  [[nodiscard]] ResolvedContract resolve(std::uint32_t uid, double K, std::int64_t expiry_ns,
                                         Side side) const noexcept;

  // The contract's vol in THIS state at an EXPLICIT year-fraction `T` (the explain's
  // CommonBaseMaturity convention reads the shifted state at the base maturity). A
  // mark is independent of T (returned as is, flag-free); a surface answers iv(K, T)
  // with the tenor flag. NaN with status != Ok when neither can answer.
  struct SigmaAt {
    double sigma{0.0};
    RiskFlags flags{0};
    RiskLaneStatus status{RiskLaneStatus::Ok};
  };
  [[nodiscard]] SigmaAt sigma_at(std::uint32_t uid, double K, std::int64_t expiry_ns, Side side,
                                 double T) const noexcept;

  // Forward and carry at T for a uid under its CarryFrom (what `resolve` uses).
  // Returns {0, 0, rate} with status != Ok when the uid is absent.
  struct Carry {
    double forward{0.0};
    double q_eff{0.0};
    double rate{0.0};
    RiskLaneStatus status{RiskLaneStatus::Ok};
    RiskFlags flags{0};
  };
  [[nodiscard]] Carry carry_at(std::uint32_t uid, double T) const noexcept;

private:
  std::int64_t ts_ns_{0};
  TimeSpec time_{};
  std::vector<UnderlierState> underliers_{}; // sorted by uid (copied: small)
  std::vector<ContractMarks> marks_{};       // sorted by uid (views)
  std::vector<SurfaceRef> surfaces_{};       // aligned with underliers_; null when absent
  std::vector<std::uint32_t> marks_slot_{};  // per underlier: index into marks_ or UINT32_MAX
};

// Owning aggregate for a state built from scalars. Fill the vectors, then
// `make_market_state(storage)` validates and returns the view. The storage must
// outlive every MarketState made from it and must not be mutated while one exists.
struct MarketStateStorage {
  std::int64_t ts_ns{0};
  TimeSpec time{};
  std::vector<UnderlierState> underliers{};     // spans / pointers may point into the
  std::vector<YieldCurve> yield_curves{};       //   vectors below
  std::vector<std::vector<DividendEvent>> dividend_schedules{};
  std::vector<std::vector<ExpiryCarry>> term_carries{}; // sorted by expiry_ns per uid
  // Per-uid marks storage (one entry per ContractMarks row, same order as `marks`).
  struct MarksColumns {
    std::uint32_t uid{0};
    std::vector<double> strike{};
    std::vector<std::int64_t> expiry_ns{};
    std::vector<Side> side{};
    std::vector<double> sigma{};
    std::vector<double> vendor_price{};
    std::vector<double> vendor_delta{};
  };
  std::vector<MarksColumns> marks{};
  std::vector<SurfaceRef> surfaces{};

  // Sort every MarksColumns by (expiry_ns, side, strike) in place and drop exact
  // duplicate keys (keeping the first). Idempotent.
  void normalize_marks();
};

// Validate `storage` and build the state over its storage (no copies of columns).
[[nodiscard]] Result<MarketState> make_market_state(const MarketStateStorage &storage);

// Convenience: a state whose underliers and carry come from fitted surfaces only
// (spot / rate from `pricing()`, carry from the surface: CarryFrom::Surface), no
// marks. `ts_ns` must equal every surface's `pricing().now_ts_ns` unless
// `allow_timestamp_mismatch` (then `ts_ns` wins and T is measured from it).
[[nodiscard]] Result<MarketStateStorage>
market_state_from_surfaces(std::int64_t ts_ns, const TimeSpec &time,
                           std::span<const SurfaceRef> surfaces,
                           bool allow_timestamp_mismatch = false);

} // namespace atx::vol
