#pragma once

// De-Americanised IV cache of one expiry chain (lane L2a, 2026-10-18; opt-in
// QuoteSliceFitSpace::VolChannel).
//
// Every quote of a chain is inverted through the slice fit's OWN American model
// (the forward map, contract carry and inverter controls of quote_slice.cpp)
// once: the bid, ask and mid volatilities, the model prices at kIvMin / kIvMax
// that bound the identifiable price range, and the contract carry. A fit reads
// its point-admission mids and its channel bands from here instead of inverting
// again. The recovered American sigma IS the European-equivalent volatility of
// the quote (deamer.hpp, header rationale): the same lognormal sigma reprices a
// European option on the pricer's forward, so this is the de-Americanisation
// de_americanize_chain performs, kept per quote and at the slice's own carry
// rather than re-deriving a borrow and keeping only the out-of-the-money mids.
//
// Entries are filled lazily (a point fit needs only the mids of the quotes it
// admits; a channel fit needs every band) and each value is computed at most
// once per cache. Under DeAmKernel::Accurate every cached value is bit-identical
// to the PriceChannel path's own call, so a VolChannel fit on the accurate
// kernel serves exactly what the PriceChannel fit serves.
//
// The memo (QuoteSliceChannelConfig::deam_cache) keeps caches per thread, keyed
// by the EXACT inputs - every bit of the market, the derived carry and clocks,
// the quotes (with their contract carry), the dated dividends, the American
// model (AlOpts and its profile), the inverter controls and the cache kernel -
// compared word for word; the 64-bit quote_set_hash only filters. A hit
// therefore returns exactly what a rebuild would compute, so output never
// depends on hit or miss, on eviction order, or on which thread fitted a chain,
// and a Fast-kernel cache can never answer an Accurate request (the kernel is a
// key word). Thread-local: no lock, no sharing; bounded at kDeAmIvMemoCapacity
// caches per thread (oldest evicted; a fit still holding an evicted cache keeps
// it alive through its shared_ptr).
//
// Reuse across graded rungs and ladder rungs (lane L2c): a rung that fits the
// same chain with the same carry and American model computes the same key, so
// it shares every inversion already made, whichever rung made it.
// Internal header (src/fitting); not part of the public API.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american.hpp" // AlExpiryGeometry, AlSigmaFamily
#include "pricing/american_iv_premium.hpp"

namespace atx::vol {

// What one quote's American model says about its bid, ask and mid.
struct DeAmQuoteIv {
  static constexpr std::uint8_t kCarry = 1u; // q_effective, local_forward
  static constexpr std::uint8_t kMid = 2u;   // mid_iv
  static constexpr std::uint8_t kBand = 4u;  // floor/cap prices, bid_iv, ask_iv, well_posed
  // Contract carry, exactly as the slice fit derives it: the effective yield on
  // the variance clock and the contract's local forward.
  double q_effective = std::numeric_limits<double>::quiet_NaN();
  double local_forward = std::numeric_limits<double>::quiet_NaN();
  // Model prices at kIvMin and kIvMax: the identifiable price range.
  double floor_price = std::numeric_limits<double>::quiet_NaN();
  double cap_price = std::numeric_limits<double>::quiet_NaN();
  double bid_iv = 0.0; // 0: the bid carries no lower information
  double ask_iv = std::numeric_limits<double>::infinity(); // +inf: no upper information
  double mid_iv = std::numeric_limits<double>::quiet_NaN(); // NaN: no identifiable mid IV
  // The quote's bid/ask band is usable: a valid contract with 0 <= bid < ask,
  // a finite carry, a band not entirely outside [floor_price, cap_price] and
  // carrying at least one finite edge.
  bool well_posed = false;
  std::uint8_t computed = 0; // kCarry | kMid | kBand
  // QuoteSliceDeamInversion::Premium: the cold points of the mid's inversion (made from
  // scratch); the bid and the ask are each seeded from a copy, so every cached value
  // is a function of the quote alone, whatever order the fits ask for them in.
  IvPremiumMemory mid_memory{};
};

// The sigma families of one expiry (QuoteSliceDeamFamily; SPY1s Phase 2): one prepared
// geometry at the expiry's representative yield `q` and the side families built on it.
// The families observe the geometry, so the holder never moves (heap-held, not copyable or
// movable); immutable once built, shared read-only by every fit of the cache.
struct DeAmSigmaFamilies {
  DeAmSigmaFamilies() = default;
  DeAmSigmaFamilies(const DeAmSigmaFamilies &) = delete;
  DeAmSigmaFamilies &operator=(const DeAmSigmaFamilies &) = delete;
  DeAmSigmaFamilies(DeAmSigmaFamilies &&) = delete;
  DeAmSigmaFamilies &operator=(DeAmSigmaFamilies &&) = delete;
  ~DeAmSigmaFamilies() = default;

  AlExpiryGeometry geometry;
  std::optional<AlSigmaFamily> put;
  std::optional<AlSigmaFamily> call;
  double q = std::numeric_limits<double>::quiet_NaN();
  double build_us = 0.0; // wall time of the node solves (diagnostics)

  // The prepared family of `side`, nullptr when none was built.
  [[nodiscard]] const AlSigmaFamily *of(Side side) const noexcept {
    const std::optional<AlSigmaFamily> &f = side == Side::Put ? put : call;
    return f && f->prepared() ? &*f : nullptr;
  }
};

// The sigma-family nodes and axis of the de-Am cache (L2h section 7: 12 LogSigma nodes).
inline constexpr std::uint16_t kDeAmFamilyNodes = 12;
inline constexpr AlSigmaFamilyAxis kDeAmFamilyAxis = AlSigmaFamilyAxis::LogSigma;
// Expected family-priced calls per two-sided quote of a side (the adaptive rule): the
// inversions of the mid, bid and ask at ~1.8 iterates each.
inline constexpr double kDeAmFamilyCallsPerQuote = 5.4;

struct DeAmIvCache {
  std::uint64_t quote_set_hash = 0;
  std::vector<DeAmQuoteIv> ivs; // one per input quote, input order
  // Work spent filling this cache, cumulative over every fit that used it.
  std::size_t inversions = 0;
  std::size_t model_prices = 0;
  std::size_t accurate_fallbacks = 0;
  // Sigma families (QuoteSliceDeamFamily): decided once per cache, on the first Premium
  // inversion that could use one; nullptr when none was built.
  bool families_decided = false;
  std::shared_ptr<const DeAmSigmaFamilies> families;
};

// The kernels the cache can price and invert with (QuoteSliceDeamProfile). Screen (SPY1s
// Phase 1) is a SEED kernel: it prices band edges (bound-decided) and runs the Premium
// iterates; it never answers an inversion on its own.
enum class DeAmKernel : std::uint8_t { Accurate, Fast, Screen };

// Per-inversion controls a Premium view hands the model (SPY1s Phase 1): the seed kernel
// (Screen: IvOpts::seed_profile Screen; Accurate: no seed tier), the confirm policy with
// its k and the quote's half-spread in price (IvPremiumGate), and whether the confirm
// runs on AlProfile::Fast instead of the accurate kernel. seed_gate_kappa (SPY1s Phase 2,
// opt-in): the seed tier's routing gate kappa (IvPremiumGate::seed_gate_kappa; 0 = the
// tier's own non-monotone gate).
struct DeAmPremiumControls {
  DeAmKernel seed_kernel = DeAmKernel::Accurate;
  IvConfirmPolicy confirm = IvConfirmPolicy::Always;
  double k = 10.0;
  double half_spread_usd = 0.0;
  bool confirm_fast = false;
  double seed_gate_kappa = 0.0;
  // The quote's side family and the yield it was built at (IvPremiumGate::family; nullptr:
  // cold seed-tier iterates).
  const AlSigmaFamily *family = nullptr;
  double family_q = std::numeric_limits<double>::quiet_NaN();
};

// The slice fit's American model at one contract's own carry, implemented by
// quote_slice.cpp so that the cache and the PriceChannel path share one forward
// map. Every member is a pure function of the fit it was made for.
class DeAmModel {
public:
  // Calendar yield -> effective yield on the variance clock; the local forward.
  [[nodiscard]] virtual double effective_yield_of(double calendar_yield) const = 0;
  [[nodiscard]] virtual double local_forward_of(double calendar_yield,
                                                double q_effective) const = 0;
  [[nodiscard]] virtual double reference_yield() const = 0; // the market's calendar yield
  // American price / implied volatility of the quote's contract.
  [[nodiscard]] virtual Result<double> price(const QuoteSliceQuote &quote, double sigma,
                                             double q_effective, DeAmKernel kernel) const = 0;
  [[nodiscard]] virtual Result<double> implied_vol(const QuoteSliceQuote &quote, double target,
                                                   double q_effective, DeAmKernel kernel) const = 0;
  // The accurate-kernel IV under QuoteSliceDeamInversion::Premium (american_iv_premium.hpp),
  // with the quote's memory (nullptr: none) and the attempt's stats. A model without a
  // Premium route (the default here; the dated-cash lattice) runs implied_vol on the
  // Accurate kernel and reports NotApplicable.
  [[nodiscard]] virtual Result<double> implied_vol_premium(const QuoteSliceQuote &quote,
                                                           double target, double q_effective,
                                                           IvPremiumMemory *memory,
                                                           IvPremiumStats &stats,
                                                           const DeAmPremiumControls &controls) const {
    static_cast<void>(memory);
    static_cast<void>(controls);
    stats = IvPremiumStats{};
    return implied_vol(quote, target, q_effective, DeAmKernel::Accurate);
  }
  // False when the model has no Fast kernel (the dated-cash lattice).
  [[nodiscard]] virtual bool has_fast_kernel() const = 0;
  // False when the model has no Screen kernel (the default; the dated-cash lattice).
  [[nodiscard]] virtual bool has_screen_kernel() const { return false; }
  // The tested price bound of `kernel` versus the accurate kernel at a price near `price`
  // (american_iv_premium.hpp al_profile_price_bound): the margin a band-edge comparison
  // on that kernel must clear to be decided. Accurate: 0. Default for the other kernels:
  // +inf (every comparison re-priced accurately), a model with a bound overrides.
  [[nodiscard]] virtual double seed_price_bound(double price, DeAmKernel kernel) const {
    static_cast<void>(price);
    return kernel == DeAmKernel::Accurate ? 0.0 : std::numeric_limits<double>::infinity();
  }
  // Black-76 vega of the contract at `sigma` on `forward` (the Fast guard).
  [[nodiscard]] virtual double black_vega(const QuoteSliceQuote &quote, double sigma,
                                          double forward) const = 0;
  // Sigma families (QuoteSliceDeamFamily; SPY1s Phase 2). black_iv: the Black-76 IV of
  // `target` for the quote's contract on `forward` (the family box), NaN when none.
  // sigma_families: the expiry's families over the per-side boxes [lo, hi] (an empty box,
  // lo >= hi, builds no family of that side), at effective yield `q` on the model's
  // accurate kernel; nullptr when the model has none (the default; the dated-cash lattice,
  // expiries below kAlSigmaFamilyMinTau) or no side could be built.
  [[nodiscard]] virtual double black_iv(const QuoteSliceQuote &quote, double target,
                                        double forward) const {
    static_cast<void>(quote);
    static_cast<void>(target);
    static_cast<void>(forward);
    return std::numeric_limits<double>::quiet_NaN();
  }
  [[nodiscard]] virtual std::shared_ptr<const DeAmSigmaFamilies>
  sigma_families(double q, double put_lo, double put_hi, double call_lo, double call_hi) const {
    static_cast<void>(q);
    static_cast<void>(put_lo);
    static_cast<void>(put_hi);
    static_cast<void>(call_lo);
    static_cast<void>(call_hi);
    return nullptr;
  }

protected:
  DeAmModel() = default;
  DeAmModel(const DeAmModel &) = default;
  DeAmModel &operator=(const DeAmModel &) = default;
  DeAmModel(DeAmModel &&) = default;
  DeAmModel &operator=(DeAmModel &&) = default;
  ~DeAmModel() = default; // never owned or deleted through the base
};

// Lower bound on the Black-76 vega, relative to max(target, 1), above which a
// Fast-kernel IV is accepted. american.hpp TESTS |P_fast - P_ref| <= 1e-12 *
// max(P, 1) and documents the IV bound 0.0054 bp (tested 0.01 bp) on
// vega-bearing contracts. At the Fast root the Reference map misses the target
// by at most that price bound, i.e. by 1e-12 * max(P, 1) / vega_American in
// volatility. The Black-76 vega overstates the American slope by 2-3x deep in
// the money (LEDGER 2026-08-23); with a 10x margin, vega_Black >= 1e-5 *
// max(P, 1) keeps the Fast-induced shift <= 1e-6 vol = 0.01 bp, the tested
// bound. Below it (or on a Fast failure, or a root at a volatility bound) the
// inversion is redone on the accurate kernel.
inline constexpr double kDeAmFastMinRelativeVega = 1.0e-5;

// One fit's view of its chain's cache: fills entries on demand through `model`
// with `profile`'s kernel and adds the work it spends to `work` (the fit's own
// diagnostics) as well as to the cache's totals. The cache, model, quotes and
// `work` must outlive the view; `quotes` must be the quotes the cache was built
// for (same order, ivs sized to match).
// Confirm controls of a Screen-seeded Premium view (QuoteSliceChannelConfig deam_confirm,
// deam_confirm_k, deam_confirm_kernel, deam_seed_gate_kappa); the default confirms every
// seed-tier root on the accurate kernel and routes inside the seed tier's own gate.
struct DeAmConfirmControls {
  QuoteSliceDeamConfirm policy = QuoteSliceDeamConfirm::Always;
  double k = 10.0;
  QuoteSliceDeamConfirmKernel kernel = QuoteSliceDeamConfirmKernel::Accurate;
  double seed_gate_kappa = 0.0;
  QuoteSliceDeamFamily family = QuoteSliceDeamFamily::Off; // deam_family
  double family_c = 3.0;                                   // deam_family_c
};

class DeAmIvView {
public:
  // `inversion` Premium (accurate or Screen kernel; ignored under the Fast profile, which
  // validate_channel refuses in combination): every inversion runs the Premium strategy,
  // a band's mid first. Under the Screen profile the iterates run on the Screen tier with
  // `confirm` governing the accurate confirm, and the band edges are bound-decided.
  DeAmIvView(DeAmIvCache &cache, const DeAmModel &model, std::span<const QuoteSliceQuote> quotes,
             QuoteSliceDeamProfile profile, QuoteSliceDeamDiagnostics &work,
             QuoteSliceDeamInversion inversion = QuoteSliceDeamInversion::Search,
             DeAmConfirmControls confirm = {}) noexcept;

  // The quote's mid IV exactly as point admission inverts it: Err when the
  // inversion fails or returns a non-positive volatility. Precondition: quote
  // `i` is a valid contract with 0 < bid < ask and a finite carry.
  [[nodiscard]] Result<double> mid_iv(std::size_t i);
  // The quote's carry and band, computed on first use.
  [[nodiscard]] const DeAmQuoteIv &band(std::size_t i);
  [[nodiscard]] const DeAmIvCache &cache() const noexcept { return cache_; }

private:
  [[nodiscard]] DeAmQuoteIv &carry(std::size_t i);
  // One inversion of the quote's contract at `target` (memory: Premium only), charged
  // to the fit's work and, when timing is armed, to the chain's fit_timing counters.
  [[nodiscard]] Result<double> invert(const QuoteSliceQuote &quote, double target,
                                      const DeAmQuoteIv &entry, IvPremiumMemory *memory);
  [[nodiscard]] Result<double> invert_premium(const QuoteSliceQuote &quote, double target,
                                              const DeAmQuoteIv &entry, IvPremiumMemory *memory,
                                              IvPremiumStats &stats);
  [[nodiscard]] Result<double> invert_search(const QuoteSliceQuote &quote, double target,
                                             const DeAmQuoteIv &entry);
  [[nodiscard]] DeAmKernel kernel() const noexcept;
  [[nodiscard]] bool premium() const noexcept;
  // Decides (once per cache) and builds the chain's sigma families under deam_family
  // Adaptive / Always: one box from the Black-76 IVs of the out-of-the-money two-sided mids
  // of both sides (lo = max(kAlSigmaFamilyMinSigma, 0.5 min), hi = 1.5 max + 2
  // kAlPackSigmaBump), a side built when its two-sided count passes the adaptive rule (or
  // Always); no-op otherwise. Charged to the fit's work and fit_timing counters.
  void ensure_families();

  // One edge price (kIvMin / kIvMax) of a band: its value, the sigma it was priced at,
  // and whether it is the accurate kernel's (decided) or the seed tier's.
  struct Edge {
    double value;
    double sigma;
    bool decided;
  };
  // The edge's value for a comparison with `x`: a seed-tier edge within its tested bound
  // of `x` is first re-priced on the accurate kernel (ambiguous edge). `failed` is set
  // when that re-price errs.
  [[nodiscard]] double settle(Edge &edge, double x, const QuoteSliceQuote &quote,
                              double q_effective, bool &failed);

  DeAmIvCache &cache_;
  const DeAmModel &model_;
  std::span<const QuoteSliceQuote> quotes_;
  QuoteSliceDeamProfile profile_;
  QuoteSliceDeamDiagnostics &work_;
  QuoteSliceDeamInversion inversion_;
  DeAmConfirmControls confirm_;
};

// The exact inputs a chain's cache depends on, as 64-bit words: two fits with
// equal keys compute bit-identical caches. `prepared` is a fit after carry and
// clock preparation (market, derived forward / discount / effective rate and
// yield, dividend model, variance-clock dividends, AlOpts, lattice controls);
// `config` supplies the inverter controls and the cache kernel (deam_profile).
struct DeAmIvKey {
  std::vector<std::uint64_t> words;
  std::uint64_t hash = 0; // quote_set_hash
};

[[nodiscard]] DeAmIvKey deam_iv_key(const QuoteSliceFit &prepared,
                                    std::span<const QuoteSliceQuote> quotes,
                                    const QuoteSliceConfig &config);

// Caches kept per thread by deam_iv_memo; the graded surface fits at most
// kQuoteSurfaceMaxExpiries chains, so one surface's chains all stay resident.
inline constexpr std::size_t kDeAmIvMemoCapacity = kQuoteSurfaceMaxExpiries;

// The calling thread's cache for `key` (ivs sized to `quote_count`), created
// empty on a miss after evicting the oldest entry at capacity. `reused` is set
// on a hit. Shared ownership: the cache outlives its eviction (or
// deam_iv_memo_clear) for as long as the caller holds it. Never null.
[[nodiscard]] std::shared_ptr<DeAmIvCache> deam_iv_memo(DeAmIvKey key, std::size_t quote_count,
                                                        bool &reused);

// Drops every cache of the calling thread (tests; bounded memory release).
void deam_iv_memo_clear() noexcept;

// Caches currently held by the calling thread's memo.
[[nodiscard]] std::size_t deam_iv_memo_size() noexcept;

} // namespace atx::vol
