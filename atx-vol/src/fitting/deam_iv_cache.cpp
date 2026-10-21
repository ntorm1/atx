#include "fitting/deam_iv_cache.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "atx/vol/api/fitting/aggregate_arity.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "fitting/counters.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;

// The key below spells out every field of these inputs. A new field must be
// added to deam_iv_key, or two different inputs would share a cache.
static_assert(detail::aggregate_arity_is_v<AlOpts, 6>, "AlOpts changed: update deam_iv_key");
static_assert(detail::aggregate_arity_is_v<QuoteSliceMarket, 6>,
              "QuoteSliceMarket changed: update deam_iv_key");
static_assert(detail::aggregate_arity_is_v<QuoteSliceQuote, 5>,
              "QuoteSliceQuote changed: update deam_iv_key");
static_assert(detail::aggregate_arity_is_v<CashDividend, 2>,
              "CashDividend changed: update deam_iv_key");

bool positive(double value) noexcept { return value > 0.0 && std::isfinite(value); }

// FNV-1a over the words, then a splitmix64 finaliser: a filter only, equality
// of the words decides a hit.
std::uint64_t hash_words(std::span<const std::uint64_t> words) noexcept {
  std::uint64_t h = 0xcbf29ce484222325ull;
  for (const std::uint64_t word : words) {
    h = (h ^ word) * 0x100000001b3ull;
  }
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebull;
  h ^= h >> 31;
  return h;
}

struct MemoEntry {
  DeAmIvKey key;
  std::shared_ptr<DeAmIvCache> cache; // never null
};

// Per-thread ring of caches (FIFO eviction at capacity). The caches are shared
// with the fits reading them, so eviction never invalidates a cache in use.
struct Memo {
  std::vector<MemoEntry> entries;
  std::size_t next_eviction = 0;
};

thread_local Memo t_memo;

} // namespace

DeAmIvView::DeAmIvView(DeAmIvCache &cache, const DeAmModel &model,
                       std::span<const QuoteSliceQuote> quotes, QuoteSliceDeamProfile profile,
                       QuoteSliceDeamDiagnostics &work, QuoteSliceDeamInversion inversion,
                       DeAmConfirmControls confirm) noexcept
    : cache_(cache), model_(model), quotes_(quotes), profile_(profile), work_(work),
      inversion_(inversion), confirm_(confirm) {}

bool DeAmIvView::premium() const noexcept {
  return inversion_ == QuoteSliceDeamInversion::Premium && kernel() != DeAmKernel::Fast;
}

DeAmKernel DeAmIvView::kernel() const noexcept {
  if (profile_ == QuoteSliceDeamProfile::Fast && model_.has_fast_kernel()) {
    return DeAmKernel::Fast;
  }
  if (profile_ == QuoteSliceDeamProfile::Screen && model_.has_screen_kernel()) {
    return DeAmKernel::Screen;
  }
  return DeAmKernel::Accurate;
}

namespace {

// The calling thread's AL boundary solve count when timing is armed (0 otherwise).
[[nodiscard]] std::uint64_t al_solves(const counters::fit_timing::FitTimingCounters *timing) noexcept {
  namespace ledger = counters::ledger;
  return timing != nullptr ? ledger::this_thread(ledger::Solve::AlBoundarySolves) : 0u;
}

void charge_reason(counters::fit_timing::FitTimingCounters *timing,
                   counters::fit_timing::DeamReason reason, std::uint64_t solves) noexcept {
  if (timing == nullptr || solves == 0u) return;
  std::uint32_t &slot = timing->deam_al_by_reason[static_cast<std::size_t>(reason)];
  slot += counters::fit_timing::detail::saturate(solves);
}

} // namespace

DeAmQuoteIv &DeAmIvView::carry(std::size_t i) {
  DeAmQuoteIv &entry = cache_.ivs[i];
  if ((entry.computed & DeAmQuoteIv::kCarry) == 0u) {
    const QuoteSliceQuote &quote = quotes_[i];
    const double calendar_yield = quote.continuous_yield.value_or(model_.reference_yield());
    entry.q_effective = model_.effective_yield_of(calendar_yield);
    entry.local_forward = model_.local_forward_of(calendar_yield, entry.q_effective);
    entry.computed = static_cast<std::uint8_t>(entry.computed | DeAmQuoteIv::kCarry);
  }
  return entry;
}

void DeAmIvView::ensure_families() {
  if (cache_.families_decided || confirm_.family == QuoteSliceDeamFamily::Off ||
      kernel() != DeAmKernel::Screen || !premium()) {
    return;
  }
  cache_.families_decided = true;
  // The box: the Black-76 IVs of the out-of-the-money two-sided mids of BOTH sides (the
  // strike-wise smile; an in-the-money mid's Black-76 IV carries its early-exercise premium
  // and only widens the box); the count: each side's two-sided quotes.
  double lo = std::numeric_limits<double>::infinity();
  double hi = 0.0;
  std::array<std::size_t, 2> count{0u, 0u};
  // The families' yield: the median effective yield of the two-sided quotes (per-strike
  // carry under funding-pv; the leg carries each contract's own q).
  std::vector<double> yields;
  yields.reserve(quotes_.size());
  for (std::size_t i = 0; i < quotes_.size(); ++i) {
    const QuoteSliceQuote &quote = quotes_[i];
    if (!(quote.bid > 0.0 && quote.ask > quote.bid) || !std::isfinite(quote.ask)) continue;
    const std::size_t s = quote.side == Side::Put ? 0u : 1u;
    ++count[s];
    const DeAmQuoteIv &entry = carry(i);
    if (std::isfinite(entry.q_effective)) yields.push_back(entry.q_effective);
    const bool otm = quote.side == Side::Put ? quote.strike <= entry.local_forward
                                             : quote.strike >= entry.local_forward;
    if (!otm) continue;
    const double iv = model_.black_iv(quote, 0.5 * (quote.bid + quote.ask), entry.local_forward);
    if (!(iv > 0.0) || !std::isfinite(iv)) continue;
    lo = std::min(lo, iv);
    hi = std::max(hi, iv);
  }
  if (!(hi >= lo) || yields.empty()) return; // no out-of-the-money two-sided mid: no box
  const auto mid = yields.begin() + static_cast<std::ptrdiff_t>(yields.size() / 2);
  std::nth_element(yields.begin(), mid, yields.end());
  const double family_q = *mid;
  const double needed = confirm_.family_c * 2.0 * static_cast<double>(kDeAmFamilyNodes);
  std::array<double, 2> box_lo{1.0, 1.0};
  std::array<double, 2> box_hi{0.0, 0.0}; // empty: no family of that side
  for (std::size_t s = 0; s < 2; ++s) {
    const bool wanted = count[s] > 0u && (confirm_.family == QuoteSliceDeamFamily::Always ||
                                          kDeAmFamilyCallsPerQuote * static_cast<double>(count[s]) >
                                              needed);
    if (wanted) {
      box_lo[s] = std::max(kAlSigmaFamilyMinSigma, 0.5 * lo);
      box_hi[s] = 1.5 * hi + 2.0 * kAlPackSigmaBump;
    }
  }
  if (!(box_hi[0] > box_lo[0]) && !(box_hi[1] > box_lo[1])) return;
  cache_.families = model_.sigma_families(family_q, box_lo[0], box_hi[0], box_lo[1], box_hi[1]);
  if (!cache_.families) return;
  const std::size_t built = (cache_.families->of(Side::Put) != nullptr ? 1u : 0u) +
                            (cache_.families->of(Side::Call) != nullptr ? 1u : 0u);
  work_.family_builds += built;
  namespace ft = counters::fit_timing;
  if (ft::FitTimingCounters *const timing = ft::current_counts()) {
    timing->deam_family_builds += static_cast<std::uint32_t>(built);
    timing->deam_family_build_us += static_cast<std::uint64_t>(cache_.families->build_us);
  }
}

Result<double> DeAmIvView::invert(const QuoteSliceQuote &quote, double target,
                                  const DeAmQuoteIv &entry, IvPremiumMemory *memory) {
  namespace ft = counters::fit_timing;
  namespace ledger = counters::ledger;
  ensure_families(); // before the inversion's own solve count: builds are not its solves
  ft::FitTimingCounters *const timing = ft::current_counts();
  const std::uint64_t al0 = timing ? ledger::this_thread(ledger::Solve::AlBoundarySolves) : 0u;
  IvPremiumStats stats;
  Result<double> out = premium() ? invert_premium(quote, target, entry, memory, stats)
                                 : invert_search(quote, target, entry);
  if (timing == nullptr) {
    return out;
  }
  const std::uint64_t al = ledger::this_thread(ledger::Solve::AlBoundarySolves) - al0;
  const std::uint32_t spent = ft::detail::saturate(al);
  ++timing->deam_inversions;
  timing->deam_inversion_al += spent;
  timing->deam_inversion_al_max = std::max(timing->deam_inversion_al_max, spent);
  if (!premium()) {
    charge_reason(timing, ft::DeamReason::Search, al);
    return out;
  }
  // Reason split (gap report 3a): the Premium attempt's solves are iterates plus the
  // confirm plus the band gate's chord solve; whatever the inversion spent beyond them is
  // the Search route's (a fallback).
  const std::uint64_t premium_solves = stats.premium_solves;
  const std::uint64_t confirm_solves = std::min<std::uint64_t>(stats.confirm_solves, premium_solves);
  const std::uint64_t gate_solves =
      std::min<std::uint64_t>(stats.gate_solves, premium_solves - confirm_solves);
  charge_reason(timing, ft::DeamReason::Iterate, premium_solves - confirm_solves - gate_solves);
  charge_reason(timing, ft::DeamReason::Confirm, confirm_solves);
  charge_reason(timing, ft::DeamReason::Gate, gate_solves);
  charge_reason(timing, ft::DeamReason::Search, al > premium_solves ? al - premium_solves : 0u);
  timing->deam_confirms += stats.confirm_solves;
  if (stats.seed_routed) ++timing->deam_seed_routed;
  timing->deam_family_prices += stats.family_prices;
  if (stats.family_refused) ++timing->deam_family_refused;
  ++timing->deam_premium;
  const bool fell_back = stats.fallback != IvPremiumFallback::None;
  if (fell_back) {
    ++timing->deam_premium_fallbacks;
    const auto reason = static_cast<std::size_t>(stats.fallback);
    if (reason < timing->deam_premium_fallback_by_reason.size()) {
      ++timing->deam_premium_fallback_by_reason[reason];
    }
  } else {
    timing->deam_premium_max_residual_usd =
        std::max(timing->deam_premium_max_residual_usd, stats.last_residual_usd);
    if (stats.confirm_skipped) {
      ++timing->deam_confirms_skipped;
      if (std::isfinite(stats.gate_bound_vol)) {
        timing->deam_gate_max_bound_vol =
            std::max(timing->deam_gate_max_bound_vol, stats.gate_bound_vol);
      }
    }
  }
  if (ft::deam_audit_armed()) {
    // A fallback IS the Search answer (bit for bit, pinned by AmericanIvPremium tests):
    // a pair at 0 without a second inversion. An accepted root is compared with Search.
    double dsigma = 0.0;
    if (!fell_back) {
      const Result<double> search =
          model_.implied_vol(quote, target, entry.q_effective, DeAmKernel::Accurate);
      if (static_cast<bool>(search) != static_cast<bool>(out)) {
        ++timing->deam_audit_status_mismatch;
      } else if (search && out) {
        dsigma = std::fabs(*out - *search);
      }
      if (dsigma > 0.0) {
        const Result<double> pp = model_.price(quote, *out, entry.q_effective, DeAmKernel::Accurate);
        const Result<double> ps =
            model_.price(quote, *search, entry.q_effective, DeAmKernel::Accurate);
        if (pp && ps) {
          const double chord = std::fabs((*pp - *ps) / (*out - *search));
          if (chord > 0.0 && std::isfinite(chord)) {
            const double np = std::fabs(*pp - target) / chord;
            const double ns = std::fabs(*ps - target) / chord;
            timing->deam_audit_max_newton_premium =
                std::max(timing->deam_audit_max_newton_premium, np);
            timing->deam_audit_max_newton_search =
                std::max(timing->deam_audit_max_newton_search, ns);
            timing->deam_audit_search_farther += ns > np ? 1u : 0u;
            if (stats.confirm_skipped) {
              // The band gate's accuracy is MEASURED here: the accurate-map Newton
              // distance of a root accepted without a confirm.
              timing->deam_audit_max_newton_skipped =
                  std::max(timing->deam_audit_max_newton_skipped, np);
            }
          }
        }
      }
      if (stats.confirm_skipped) ++timing->deam_audit_skipped_pairs;
    }
    ++timing->deam_audit_pairs;
    timing->deam_audit_max_dsigma = std::max(timing->deam_audit_max_dsigma, dsigma);
    std::size_t bin = 0;
    if (dsigma > 0.0) {
      const double decade = std::floor(std::log10(dsigma)) + 17.0; // 1e-16 -> bin 1
      bin = static_cast<std::size_t>(std::clamp(decade, 1.0, 16.0));
    }
    ++timing->deam_audit_dsigma_decades[bin];
  }
  return out;
}

Result<double> DeAmIvView::invert_premium(const QuoteSliceQuote &quote, double target,
                                          const DeAmQuoteIv &entry, IvPremiumMemory *memory,
                                          IvPremiumStats &stats) {
  ++cache_.inversions;
  ++work_.inversions;
  ++work_.premium_inversions;
  DeAmPremiumControls controls;
  controls.seed_kernel = kernel();
  controls.confirm = confirm_.policy == QuoteSliceDeamConfirm::Band    ? IvConfirmPolicy::Band
                     : confirm_.policy == QuoteSliceDeamConfirm::Never ? IvConfirmPolicy::Never
                                                                       : IvConfirmPolicy::Always;
  controls.k = confirm_.k;
  // The quote's half-spread in price; a bidless quote has no band and always confirms.
  controls.half_spread_usd =
      quote.bid > 0.0 && quote.ask > quote.bid ? 0.5 * (quote.ask - quote.bid) : 0.0;
  controls.confirm_fast = confirm_.kernel == QuoteSliceDeamConfirmKernel::Fast;
  controls.seed_gate_kappa = confirm_.seed_gate_kappa;
  if (cache_.families) {
    controls.family = cache_.families->of(quote.side);
    controls.family_q = cache_.families->q;
  }
  Result<double> iv =
      model_.implied_vol_premium(quote, target, entry.q_effective, memory, stats, controls);
  if (stats.fallback != IvPremiumFallback::None) {
    ++work_.premium_fallbacks;
  } else {
    work_.confirms += stats.confirm_solves;
    if (stats.confirm_skipped) ++work_.confirms_skipped;
  }
  if (stats.seed_routed) ++work_.seed_routed;
  work_.family_prices += stats.family_prices;
  if (stats.family_refused) ++work_.family_refused;
  return iv;
}

double DeAmIvView::settle(Edge &edge, double x, const QuoteSliceQuote &quote, double q_effective,
                          bool &failed) {
  if (edge.decided || failed) {
    return edge.value;
  }
  const DeAmKernel k = kernel();
  if (!(std::fabs(x - edge.value) <= model_.seed_price_bound(edge.value, k))) {
    return edge.value; // the quote clears the seed tier's bound: decided on the seed price
  }
  // Ambiguous edge (gap report 1a): the accurate kernel's edge price decides.
  namespace ft = counters::fit_timing;
  ft::FitTimingCounters *const timing = ft::current_counts();
  const std::uint64_t al0 = al_solves(timing);
  const auto exact = model_.price(quote, edge.sigma, q_effective, DeAmKernel::Accurate);
  charge_reason(timing, ft::DeamReason::AmbiguousEdge, al_solves(timing) - al0);
  if (timing != nullptr) ++timing->deam_ambiguous_edges;
  ++cache_.model_prices;
  ++work_.model_prices;
  ++work_.ambiguous_edges;
  if (!exact) {
    failed = true;
    return edge.value;
  }
  edge = Edge{*exact, edge.sigma, true};
  return edge.value;
}

Result<double> DeAmIvView::invert_search(const QuoteSliceQuote &quote, double target,
                                         const DeAmQuoteIv &entry) {
  ++cache_.inversions;
  ++work_.inversions;
  if (kernel() == DeAmKernel::Accurate) {
    return model_.implied_vol(quote, target, entry.q_effective, DeAmKernel::Accurate);
  }
  auto fast = model_.implied_vol(quote, target, entry.q_effective, DeAmKernel::Fast);
  if (fast && *fast > kIvMin && *fast < kIvMax &&
      model_.black_vega(quote, *fast, entry.local_forward) >=
          kDeAmFastMinRelativeVega * std::max(target, 1.0)) {
    return fast;
  }
  ++cache_.accurate_fallbacks;
  ++work_.accurate_fallbacks;
  ++cache_.inversions;
  ++work_.inversions;
  return model_.implied_vol(quote, target, entry.q_effective, DeAmKernel::Accurate);
}

Result<double> DeAmIvView::mid_iv(std::size_t i) {
  DeAmQuoteIv &entry = carry(i);
  if ((entry.computed & DeAmQuoteIv::kMid) == 0u) {
    const QuoteSliceQuote &quote = quotes_[i];
    const double mid = 0.5 * quote.bid + 0.5 * quote.ask; // admission's own arithmetic
    // Premium: the mid from scratch; its cold points seed the band edges.
    const auto iv = invert(quote, mid, entry, premium() ? &entry.mid_memory : nullptr);
    entry.mid_iv = iv && positive(*iv) ? *iv : std::numeric_limits<double>::quiet_NaN();
    entry.computed = static_cast<std::uint8_t>(entry.computed | DeAmQuoteIv::kMid);
  }
  if (!positive(entry.mid_iv)) {
    return Err(ErrorCode::Unavailable, "de-Am IV cache: no identifiable mid volatility");
  }
  return Ok(entry.mid_iv);
}

const DeAmQuoteIv &DeAmIvView::band(std::size_t i) {
  DeAmQuoteIv &entry = carry(i);
  if ((entry.computed & DeAmQuoteIv::kBand) != 0u) {
    return entry;
  }
  // Filled into a local copy and published, with the kBand bit, only once complete:
  // an exception mid-fill (a model price or inversion throwing std::bad_alloc) leaves
  // the entry without the bit, so a later call refills it instead of serving a
  // half-filled band as final (review-L2a minor, fixed in lane L2c).
  DeAmQuoteIv filled = entry;
  const QuoteSliceQuote &quote = quotes_[i];
  const auto publish = [&entry, &filled]() -> const DeAmQuoteIv & {
    filled.computed = static_cast<std::uint8_t>(filled.computed | DeAmQuoteIv::kBand);
    entry = filled;
    return entry;
  };
  if (!positive(filled.local_forward) || !std::isfinite(filled.q_effective)) {
    return publish();
  }
  const DeAmKernel k = kernel();
  namespace ft = counters::fit_timing;
  ft::FitTimingCounters *const timing = ft::current_counts();
  const std::uint64_t al0 = al_solves(timing);
  const auto floor_price = model_.price(quote, kIvMin, filled.q_effective, k);
  const auto cap_price = model_.price(quote, kIvMax, filled.q_effective, k);
  charge_reason(timing, ft::DeamReason::RangeEdge, al_solves(timing) - al0);
  cache_.model_prices += 2;
  work_.model_prices += 2;
  if (!floor_price || !cap_price) {
    return publish();
  }
  // Bound-decided edges (gap report 1a, SPY1s Phase 1): on the Screen seed tier every
  // comparison a quote makes with an edge price - the bid and the ask against both edges
  // here, the mid against both (the builder's own test, vol_space_channel_observations)
  // - is decided only when the quote clears the edge by more than the tier's tested
  // bound; otherwise that edge is first re-priced on the accurate kernel (settle). The
  // settled edges are what the entry publishes, so every later comparison agrees. On the
  // accurate and Fast kernels every edge is decided as priced: the historical path.
  const bool decided = k != DeAmKernel::Screen;
  Edge floor_edge{*floor_price, kIvMin, decided};
  Edge cap_edge{*cap_price, kIvMax, decided};
  bool failed = false;
  const double q_eff = filled.q_effective;
  const bool bid_below_cap = quote.bid < settle(cap_edge, quote.bid, quote, q_eff, failed);
  const bool ask_above_floor = quote.ask > settle(floor_edge, quote.ask, quote, q_eff, failed);
  const bool bid_above_floor = quote.bid > settle(floor_edge, quote.bid, quote, q_eff, failed);
  const bool ask_below_cap = quote.ask < settle(cap_edge, quote.ask, quote, q_eff, failed);
  const double mid = 0.5 * quote.bid + 0.5 * quote.ask; // admission's own arithmetic
  const bool mid_inside = quote.bid > 0.0 && mid > settle(floor_edge, mid, quote, q_eff, failed) &&
                          mid < settle(cap_edge, mid, quote, q_eff, failed);
  if (failed) {
    return publish(); // an accurate edge re-price erred: the band stays undecided
  }
  filled.floor_price = floor_edge.value;
  filled.cap_price = cap_edge.value;
  if (!bid_below_cap || !ask_above_floor) {
    return publish();
  }
  // Premium: the mid first (exactly as the channel builder would ask for it: a
  // two-sided quote whose mid lies inside the identifiable range), then each edge from
  // a copy of the mid's cold points; without a mid, the edges from scratch.
  bool seeded = false;
  if (premium() && mid_inside) {
    static_cast<void>(mid_iv(i));
    // mid_iv wrote the slot (entry); carry its mid fields into the local copy so
    // that publish() keeps the mid and its memory (W2b merge of L2c x L2e).
    filled.mid_iv = entry.mid_iv;
    filled.mid_memory = entry.mid_memory;
    filled.computed = entry.computed;
    seeded = entry.mid_memory.points > 0u;
  }
  IvPremiumMemory bid_memory = filled.mid_memory;
  IvPremiumMemory ask_memory = filled.mid_memory;
  // A bid at or below the minimum-volatility price carries no lower
  // information; an ask at or above the maximum-volatility price no upper.
  if (bid_above_floor) {
    const auto lo = invert(quote, quote.bid, filled, seeded ? &bid_memory : nullptr);
    filled.bid_iv = lo && positive(*lo) ? *lo : 0.0;
  }
  if (ask_below_cap) {
    const auto hi = invert(quote, quote.ask, filled, seeded ? &ask_memory : nullptr);
    if (hi && positive(*hi)) {
      filled.ask_iv = *hi;
    }
  }
  filled.well_posed = filled.ask_iv > filled.bid_iv &&
                      (filled.bid_iv > 0.0 || std::isfinite(filled.ask_iv));
  return publish();
}

DeAmIvKey deam_iv_key(const QuoteSliceFit &prepared, std::span<const QuoteSliceQuote> quotes,
                      const QuoteSliceConfig &config) {
  DeAmIvKey key;
  std::vector<std::uint64_t> &words = key.words;
  // 34 fixed words (market 6, derived carry 4, model + lattice + AlOpts flag 4, AlOpts 6,
  // inverter + kernel 4, confirm 3, seed gate 1, family 4, two counts), then 2 per dividend
  // and 6 per quote.
  words.reserve(34 + 2 * prepared.variance_clock_dividends.size() + 6 * quotes.size());
  const auto real = [&words](double value) { words.push_back(std::bit_cast<std::uint64_t>(value)); };
  const auto word = [&words](std::uint64_t value) { words.push_back(value); };
  const QuoteSliceMarket &m = prepared.market;
  real(m.spot);
  real(m.variance_years);
  real(m.calendar_years);
  real(m.rate);
  real(m.continuous_yield);
  real(m.dividend_pv);
  // The derived carry the model reads (prepare_fit): functions of the words
  // above and the dividends, keyed directly so no rounding path is left out.
  const QuoteSliceDiagnostics &d = prepared.diagnostics;
  real(d.forward);
  real(d.discount);
  real(d.effective_rate);
  real(d.effective_yield);
  word(static_cast<std::uint64_t>(prepared.dividend_model));
  word(static_cast<std::uint64_t>(static_cast<std::int64_t>(prepared.discrete_div_steps)));
  word(static_cast<std::uint64_t>(prepared.discrete_div_scheme));
  word(prepared.al_opts ? 1u : 0u);
  if (prepared.al_opts) {
    const AlOpts &opts = *prepared.al_opts;
    word(opts.n_collocation);
    word(opts.n_quadrature);
    word(opts.n_quad_price);
    word(opts.max_newton_iter);
    real(opts.tol);
    word(static_cast<std::uint64_t>(opts.profile));
  }
  real(config.iv_tolerance);
  word(config.max_iv_iterations);
  word(static_cast<std::uint64_t>(config.channel.deam_profile));
  word(static_cast<std::uint64_t>(config.channel.deam_inversion)); // a Premium cache never
                                                                   // answers a Search fit
  // The confirm controls of a seed-tier root (review-SPY1s-p1 M1): a cache filled under
  // confirm band / never holds unconfirmed seed roots and never answers a confirm-always
  // fit, nor a cache confirmed on one kernel a fit asking for the other.
  word(static_cast<std::uint64_t>(config.channel.deam_confirm));
  real(config.channel.deam_confirm_k);
  word(static_cast<std::uint64_t>(config.channel.deam_confirm_kernel));
  // The seed-tier routing gate (SPY1s Phase 2): it decides which kernel runs the iterates,
  // so a root (within tol) depends on it.
  real(config.channel.deam_seed_gate_kappa);
  // The sigma families (SPY1s Phase 2): mode, the adaptive constant, nodes and axis decide
  // whether and on which family the iterates ran.
  word(static_cast<std::uint64_t>(config.channel.deam_family));
  real(config.channel.deam_family_c);
  word(kDeAmFamilyNodes);
  word(static_cast<std::uint64_t>(kDeAmFamilyAxis));
  word(prepared.variance_clock_dividends.size());
  for (const CashDividend &event : prepared.variance_clock_dividends) {
    real(event.tau);
    real(event.amount);
  }
  word(quotes.size());
  for (const QuoteSliceQuote &quote : quotes) {
    real(quote.strike);
    word(static_cast<std::uint64_t>(quote.side));
    real(quote.bid);
    real(quote.ask);
    word(quote.continuous_yield ? 1u : 0u);
    real(quote.continuous_yield.value_or(0.0));
  }
  key.hash = hash_words(words);
  return key;
}

std::shared_ptr<DeAmIvCache> deam_iv_memo(DeAmIvKey key, std::size_t quote_count,
                                           bool &reused) {
  Memo &memo = t_memo;
  for (const MemoEntry &entry : memo.entries) {
    if (entry.key.hash == key.hash && entry.key.words == key.words &&
        entry.cache->ivs.size() == quote_count) {
      reused = true;
      return entry.cache;
    }
  }
  reused = false;
  MemoEntry fresh{std::move(key), std::make_shared<DeAmIvCache>()};
  fresh.cache->quote_set_hash = fresh.key.hash;
  fresh.cache->ivs.assign(quote_count, DeAmQuoteIv{});
  std::shared_ptr<DeAmIvCache> out = fresh.cache;
  if (memo.entries.size() < kDeAmIvMemoCapacity) {
    memo.entries.push_back(std::move(fresh));
    return out;
  }
  memo.entries[memo.next_eviction] = std::move(fresh);
  memo.next_eviction = (memo.next_eviction + 1) % kDeAmIvMemoCapacity;
  return out;
}

void deam_iv_memo_clear() noexcept {
  t_memo.entries.clear();
  t_memo.next_eviction = 0;
}

std::size_t deam_iv_memo_size() noexcept { return t_memo.entries.size(); }

} // namespace atx::vol
