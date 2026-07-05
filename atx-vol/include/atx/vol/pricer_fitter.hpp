#pragma once

// PricerFitter — the atx-vol library lifecycle in one object: fit a surface from
// an OptionChain, OWN it (unique_ptr<FittedSurface>), and price the chain in
// parallel with per-field output flags.
//
//   OptionChain chain = OptionChain::from_frame(frame, r, spot).value();
//   PricerFitter fitter{ PricerConfig{ .preset = FitPreset::Robust } };
//   fitter.fit(chain);                                   // stores the surface
//   auto val = fitter.value_chain(chain,                 // parallel valuation
//       OutputField::ModelPrice | OutputField::BidIV | OutputField::AskIV);
//   // ... a tick arrives ...
//   chain.update_quotes(ids, bids, asks);                // replace bid/ask by id
//   auto bands = fitter.value_chain(chain, OutputField::Bands);   // re-price
//
// The fitter composes the validated primitives — `VolaSession` (fit + cached
// query), `american_implied_vol` (cold IV inversion), `VolSurface` — so every
// number it produces is bit-consistent with those. It adds the two things the
// facade needs on top: ownership of the fitted surface, and a deterministic
// multi-threaded evaluator over the whole chain.
//
// Thread-safety: `fit` mutates (stores the surface) and needs exclusive access.
// `value_chain` is const and internally parallel; concurrent `value_chain` calls
// on one fitter are safe (all state read is immutable after `fit`).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "atx/vol/american.hpp"    // AmericanGreeks
#include "atx/vol/chain.hpp"       // OptionChain, OptionId
#include "atx/vol/curve.hpp"       // DividendEvent
#include "atx/vol/session.hpp"     // VolaSession, FitPreset, SessionDiagnostics
#include "atx/vol/types.hpp"       // Result, Status, Side

namespace atx::vol {

// ── Output-field selector ──────────────────────────────────────────────────
//
// Which per-option outputs a chain valuation should populate. Bitmask; combine
// with `|`. Unrequested scalar columns stay empty; a requested-but-failed cell
// is NaN, so a bad quote never sinks the rest of the valuation.
enum class OutputField : std::uint32_t {
  None = 0,
  ModelPrice = 1u << 0,  // re-Americanized model fair value at (K, T, side)
  ModelIV = 1u << 1,     // surface European-equivalent IV at (K, T)
  BidIV = 1u << 2,       // American implied vol of the bid (on the fit's carry)
  AskIV = 1u << 3,       // American implied vol of the ask
  MidIV = 1u << 4,       // American implied vol of the mid
  Greeks = 1u << 5,      // full AmericanGreeks bundle at the model IV
  Prices = ModelPrice | ModelIV,
  Bands = BidIV | AskIV | MidIV,
  All = ModelPrice | ModelIV | BidIV | AskIV | MidIV | Greeks,
};

[[nodiscard]] constexpr OutputField operator|(OutputField a, OutputField b) noexcept {
  return static_cast<OutputField>(static_cast<std::uint32_t>(a) |
                                  static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr OutputField operator&(OutputField a, OutputField b) noexcept {
  return static_cast<OutputField>(static_cast<std::uint32_t>(a) &
                                  static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool has(OutputField set, OutputField flag) noexcept {
  return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(flag)) != 0u;
}

// ── SoA valuation result ────────────────────────────────────────────────────
//
// One row per queried OptionId (row i <-> ids[i]). Only the requested columns
// are populated (others empty); requested-but-failed cells are NaN.
struct ChainValuation {
  std::vector<OptionId> ids;
  std::vector<double> model_price;
  std::vector<double> model_iv;
  std::vector<double> bid_iv;
  std::vector<double> ask_iv;
  std::vector<double> mid_iv;
  std::vector<AmericanGreeks> greeks;
  OutputField filled{OutputField::None};

  [[nodiscard]] std::size_t size() const noexcept { return ids.size(); }

  // Row index for an id (linear scan), or nullopt.
  [[nodiscard]] std::optional<std::size_t> row_of(OptionId id) const;
};

// ── Fit policy ──────────────────────────────────────────────────────────────
//
// Thin bundle over the session's fit knobs plus the evaluation thread count.
// Every field defaults so `PricerConfig{}` is a valid market-maker config
// (Robust preset: calendar-arb-free near-money at held quality).
struct PricerConfig {
  FitPreset preset{FitPreset::Robust};
  // Worker count for value_chain. 0 => std::thread::hardware_concurrency();
  // 1 => serial. A per-call `value_chain(..., n_threads)` overrides this.
  unsigned n_threads{0};
  // Extra discrete cash dividends the surface build should honour (usually left
  // empty — the chain's installed curve drives the carry).
  std::vector<DividendEvent> cash_divs{};
};

// ── Fitted surface handle (the owned fit output) ────────────────────────────
//
// Wraps a `VolaSession` (the fitted VolSurface + per-slice carry + correction
// caches). Every query is a const read; move-only (heavy fitted state).
class FittedSurface {
 public:
  FittedSurface(FittedSurface&&) noexcept = default;
  FittedSurface& operator=(FittedSurface&&) noexcept = default;
  FittedSurface(const FittedSurface&) = delete;
  FittedSurface& operator=(const FittedSurface&) = delete;

  [[nodiscard]] double iv(double K, double T) const { return sess_.iv(K, T); }
  [[nodiscard]] Result<double> fair_value(double K, double T, Side s) const {
    return sess_.fair_value(K, T, s);
  }
  [[nodiscard]] Result<AmericanGreeks> greeks(double K, double T, Side s) const {
    return sess_.greeks(K, T, s);
  }
  [[nodiscard]] const VolaSession& session() const noexcept { return sess_; }
  [[nodiscard]] const SessionDiagnostics& diagnostics() const noexcept {
    return sess_.diagnostics();
  }

 private:
  friend class PricerFitter;
  explicit FittedSurface(VolaSession&& sess) : sess_(std::move(sess)) {}
  VolaSession sess_;
};

// ── PricerFitter ────────────────────────────────────────────────────────────
class PricerFitter {
 public:
  explicit PricerFitter(PricerConfig cfg = {}) : cfg_(std::move(cfg)) {}

  // Fit the surface from `chain` and STORE it as unique_ptr<FittedSurface>,
  // replacing any prior fit. Maps the config onto SessionInputs and drives
  // `VolaSession::build`. Propagates the build error (the prior surface is left
  // intact on failure).
  [[nodiscard]] Status fit(const OptionChain& chain);

  [[nodiscard]] bool fitted() const noexcept { return surface_ != nullptr; }
  [[nodiscard]] const FittedSurface* surface() const noexcept { return surface_.get(); }

  [[nodiscard]] const PricerConfig& config() const noexcept { return cfg_; }
  void set_threads(unsigned n) noexcept { cfg_.n_threads = n; }

  // Price the chain's options for the requested `fields`, fanned out across
  // `n_threads` workers (0 => cfg.n_threads; final 0 => hardware_concurrency,
  // 1 => serial). DETERMINISTIC: the result is bit-identical for any thread
  // count (disjoint output slots, pure const reads).
  //
  // @return Unavailable if no surface is fitted; otherwise Ok(valuation).
  [[nodiscard]] Result<ChainValuation> value_chain(const OptionChain& chain,
                                                   OutputField fields,
                                                   unsigned n_threads = 0) const;

 private:
  PricerConfig cfg_;
  std::unique_ptr<FittedSurface> surface_;
};

}  // namespace atx::vol
