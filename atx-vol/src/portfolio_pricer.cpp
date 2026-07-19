// PortfolioPricer implementation — dedup, parallel Greeks fan-out, and the
// Taylor PnL-explain decomposition. See portfolio_pricer.hpp for the model.

#include "atx/vol/portfolio_pricer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <utility>

#include "atx/vol/american.hpp"           // AmericanGreeks
#include "atx/vol/counters.hpp"           // ATX_VOL_COUNT (opt-in P0.2; no-op when OFF)
#include "atx/vol/detail/adjoint_greeks.hpp" // WS-P P3: american_greeks_adjoint A/B route
#include "atx/vol/prepared_portfolio.hpp" // PreparedPortfolio (grouped exec substrate)
#include "atx/vol/pricing_executor.hpp"   // pricing_executor(): the persistent P1.4 pool
#include "atx/vol/simd/american_boundary_batch.hpp" // simd::avx2_boundary_selected (invariant tile-schedule gate)

namespace atx::vol {

namespace {

// "No value" sentinel for a non-Ok lane's numeric columns.
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

using ByteRange = std::span<const std::byte>;

template <class T>
[[nodiscard]] ByteRange byte_range(std::span<T> values, bool active = true) noexcept {
  return active ? std::as_bytes(values) : ByteRange{};
}

template <class T> [[nodiscard]] ByteRange byte_range(T &value) noexcept {
  return std::as_bytes(std::span<T>{&value, 1u});
}

[[nodiscard]] bool byte_ranges_overlap(ByteRange lhs, ByteRange rhs) noexcept {
  if (lhs.empty() || rhs.empty()) {
    return false;
  }
  const std::less<const std::byte *> less;
  const std::byte *const lhs_end = lhs.data() + lhs.size();
  const std::byte *const rhs_end = rhs.data() + rhs.size();
  return less(lhs.data(), rhs_end) && less(rhs.data(), lhs_end);
}

template <std::size_t N>
[[nodiscard]] bool any_byte_ranges_overlap(const std::array<ByteRange, N> &ranges) noexcept {
  for (std::size_t lhs = 0; lhs < N; ++lhs) {
    for (std::size_t rhs = lhs + 1u; rhs < N; ++rhs) {
      if (byte_ranges_overlap(ranges[lhs], ranges[rhs])) {
        return true;
      }
    }
  }
  return false;
}

[[nodiscard]] bool price_frame_view_overlaps(const PriceFrameView &out, bool want_greeks) noexcept {
  const std::array<ByteRange, 15> ranges{
      byte_range(out.id),
      byte_range(out.uid),
      byte_range(out.pv),
      byte_range(out.price),
      byte_range(out.iv),
      byte_range(out.delta, want_greeks),
      byte_range(out.gamma, want_greeks),
      byte_range(out.vega, want_greeks),
      byte_range(out.theta, want_greeks),
      byte_range(out.rho, want_greeks),
      byte_range(out.vanna, want_greeks),
      byte_range(out.volga, want_greeks),
      byte_range(out.charm, want_greeks),
      byte_range(out.status),
      byte_range(*out.total),
  };
  return any_byte_ranges_overlap(ranges);
}

[[nodiscard]] bool pnl_frame_view_overlaps(const PnlFrameView &out) noexcept {
  const std::array<ByteRange, 20> ranges{
      byte_range(out.id),        byte_range(out.uid),
      byte_range(out.pv_base),   byte_range(out.pv_target),
      byte_range(out.pnl_total), byte_range(out.pnl_delta),
      byte_range(out.pnl_gamma), byte_range(out.pnl_vega),
      byte_range(out.pnl_volga), byte_range(out.pnl_vanna),
      byte_range(out.pnl_theta), byte_range(out.pnl_rho),
      byte_range(out.pnl_charm), byte_range(out.pnl_unexplained),
      byte_range(out.d_spot),    byte_range(out.d_vol),
      byte_range(out.d_time),    byte_range(out.d_rate),
      byte_range(out.status),    byte_range(*out.total),
  };
  return any_byte_ranges_overlap(ranges);
}

[[nodiscard]] bool target_mark_view_overlaps(const TargetMarkView &out) noexcept {
  const std::array<ByteRange, 4> ranges{
      byte_range(out.id),
      byte_range(out.price_target),
      byte_range(out.status),
      byte_range(out.base_vega_proxy),
  };
  return any_byte_ranges_overlap(ranges);
}

[[nodiscard]] double timestamp_delta_ns(std::int64_t lhs, std::int64_t rhs) noexcept {
  if (lhs >= rhs) {
    const std::uint64_t magnitude =
        static_cast<std::uint64_t>(lhs) - static_cast<std::uint64_t>(rhs);
    return static_cast<double>(magnitude);
  }
  const std::uint64_t magnitude = static_cast<std::uint64_t>(rhs) - static_cast<std::uint64_t>(lhs);
  return -static_cast<double>(magnitude);
}

// Effective deliverable: non-finite or non-positive multiplier defaults to 100
// (matches the legacy portfolio dollar convention).
[[nodiscard]] double eff_multiplier(double m) noexcept {
  return (std::isfinite(m) && m > 0.0) ? m : 100.0;
}

// Bit-exact contract identity for dedup.
struct ContractKey {
  std::uint32_t uid;
  std::uint64_t kbits;
  std::uint64_t tbits;
  std::uint8_t side;
  bool operator==(const ContractKey &) const noexcept = default;
};

struct ContractKeyHash {
  [[nodiscard]] std::size_t operator()(const ContractKey &k) const noexcept {
    std::size_t h = std::hash<std::uint32_t>{}(k.uid);
    auto mix = [&h](std::uint64_t v) {
      h ^= std::hash<std::uint64_t>{}(v) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    mix(k.kbits);
    mix(k.tbits);
    mix(static_cast<std::uint64_t>(k.side));
    return h;
  }
};

[[nodiscard]] ContractKey key_of(const OptionContract &c) noexcept {
  return ContractKey{c.uid, std::bit_cast<std::uint64_t>(c.K), std::bit_cast<std::uint64_t>(c.T),
                     static_cast<std::uint8_t>(c.side)};
}

[[nodiscard]] ContractKey key_of(const FullGreekSeed &seed) noexcept {
  return ContractKey{seed.uid(), std::bit_cast<std::uint64_t>(seed.K()),
                     std::bit_cast<std::uint64_t>(seed.T()),
                     static_cast<std::uint8_t>(seed.side())};
}

[[nodiscard]] bool key_less(const ContractKey &a, const ContractKey &b) noexcept {
  if (a.uid != b.uid) {
    return a.uid < b.uid;
  }
  if (a.kbits != b.kbits) {
    return a.kbits < b.kbits;
  }
  if (a.tbits != b.tbits) {
    return a.tbits < b.tbits;
  }
  return a.side < b.side;
}

// A never-reused process-local identity for exact O(1) workspace invalidation.
// The compare/exchange refuses to wrap: exhausting 2^64-1 logical objects is a
// process-fatal invariant breach, never an excuse to reintroduce an ABA window.
[[nodiscard]] std::uint64_t allocate_logical_id() noexcept {
  static std::atomic<std::uint64_t> next{1};
  std::uint64_t candidate = next.load();
  for (;;) {
    if (candidate == std::numeric_limits<std::uint64_t>::max()) {
      std::terminate();
    }
    if (next.compare_exchange_weak(candidate, candidate + 1)) {
      return candidate;
    }
  }
}

} // namespace

// ── Portfolio ─────────────────────────────────────────────────────────────

Portfolio::Portfolio() noexcept : logical_id_(allocate_logical_id()) {}

Portfolio::Portfolio(const Portfolio &other)
    : positions_(other.positions_), contracts_(other.contracts_),
      pos_contract_ix_(other.pos_contract_ix_), first_position_ix_(other.first_position_ix_),
      uids_(other.uids_), logical_id_(allocate_logical_id()), revision_(0) {}

Portfolio::Portfolio(Portfolio &&other) noexcept
    : positions_(std::move(other.positions_)), contracts_(std::move(other.contracts_)),
      pos_contract_ix_(std::move(other.pos_contract_ix_)),
      first_position_ix_(std::move(other.first_position_ix_)), uids_(std::move(other.uids_)),
      logical_id_(std::exchange(other.logical_id_, allocate_logical_id())),
      revision_(std::exchange(other.revision_, 0)) {}

Portfolio &Portfolio::operator=(const Portfolio &other) {
  if (this == &other) {
    return *this;
  }
  Portfolio copy(other);
  *this = std::move(copy);
  return *this;
}

Portfolio &Portfolio::operator=(Portfolio &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  positions_ = std::move(other.positions_);
  contracts_ = std::move(other.contracts_);
  pos_contract_ix_ = std::move(other.pos_contract_ix_);
  first_position_ix_ = std::move(other.first_position_ix_);
  uids_ = std::move(other.uids_);
  logical_id_ = std::exchange(other.logical_id_, allocate_logical_id());
  revision_ = std::exchange(other.revision_, 0);
  return *this;
}

Result<Portfolio> Portfolio::create(std::span<const Position> positions,
                                    const PortfolioBuildOptions &options) {
  if (!detail::portfolio_index_count_is_representable(positions.size())) {
    return Err(ErrorCode::InvalidArgument,
               "Portfolio::create: position count exceeds uint32 index capacity");
  }
  Portfolio pf;
  pf.positions_.assign(positions.begin(), positions.end());
  pf.pos_contract_ix_.resize(positions.size());

  std::unordered_map<ContractKey, std::uint32_t, ContractKeyHash> seen;
  constexpr std::size_t kAutoUniqueReserveCap = 65'536u;
  // A hint above the position count cannot be right -- unique contracts are a
  // subset of positions -- so clamp it. An unclamped hint would let a caller
  // reinstate exactly the multi-million-bucket dedup table this option exists to
  // avoid, and a pathological value would throw length_error out of a factory
  // whose contract is to return a Result.
  const std::size_t expected = options.expected_unique_contracts > 0
                                   ? std::min(options.expected_unique_contracts, positions.size())
                                   : std::min(positions.size(), kAutoUniqueReserveCap);
  seen.reserve(expected);
  for (std::size_t i = 0; i < positions.size(); ++i) {
    const OptionContract &c = positions[i].contract;
    const ContractKey key = key_of(c);
    auto [it, inserted] = seen.try_emplace(key, static_cast<std::uint32_t>(pf.contracts_.size()));
    if (inserted) {
      pf.contracts_.push_back(c);
      pf.first_position_ix_.push_back(i);
    }
    pf.pos_contract_ix_[i] = it->second;
  }

  // Unique, sorted uid set (over the deduped contracts).
  pf.uids_.reserve(pf.contracts_.size());
  for (const OptionContract &c : pf.contracts_) {
    pf.uids_.push_back(c.uid);
  }
  std::sort(pf.uids_.begin(), pf.uids_.end());
  pf.uids_.erase(std::unique(pf.uids_.begin(), pf.uids_.end()), pf.uids_.end());

  return pf;
}

Status Portfolio::retime(std::span<const double> position_T) {
  if (position_T.size() != positions_.size()) {
    return Err(ErrorCode::InvalidArgument, "Portfolio::retime: tenor count mismatch");
  }

  // Phase 1: validate every position against its contract's retained first
  // position, without touching portfolio state or allocating scratch.
  for (std::size_t i = 0; i < positions_.size(); ++i) {
    const std::size_t contract_index = pos_contract_ix_[i];
    const std::size_t first_position = first_position_ix_[contract_index];
    if (std::bit_cast<std::uint64_t>(position_T[first_position]) !=
        std::bit_cast<std::uint64_t>(position_T[i])) {
      return Err(ErrorCode::InvalidArgument,
                 "Portfolio::retime: deduplicated positions have different tenors");
    }
  }

  bool changed = false;
  for (std::size_t contract_index = 0; contract_index < contracts_.size(); ++contract_index) {
    const double next_tenor = position_T[first_position_ix_[contract_index]];
    if (std::bit_cast<std::uint64_t>(contracts_[contract_index].T) !=
        std::bit_cast<std::uint64_t>(next_tenor)) {
      changed = true;
      break;
    }
  }
  if (!changed) {
    return Status{};
  }

  if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
    return Err(ErrorCode::Internal, "Portfolio::retime: logical-book revision exhausted");
  }

  // Phase 2: commit. Assigning doubles cannot fail, so after all validation and
  // revision-capacity checks above, every write and the final revision bump are
  // one strong-guarantee transaction from the caller's perspective.
  for (std::size_t contract_index = 0; contract_index < contracts_.size(); ++contract_index) {
    contracts_[contract_index].T = position_T[first_position_ix_[contract_index]];
  }
  for (std::size_t i = 0; i < positions_.size(); ++i) {
    positions_[i].contract.T = position_T[i];
  }
  ++revision_;
  return Status{};
}

// ── SurfaceSet ────────────────────────────────────────────────────────────

SurfaceSet::SurfaceSet() noexcept : logical_id_(allocate_logical_id()) {}

SurfaceSet::SurfaceSet(SurfaceSet &&other) noexcept
    : by_uid_(std::move(other.by_uid_)),
      logical_id_(std::exchange(other.logical_id_, allocate_logical_id())) {
  other.by_uid_.clear();
}

SurfaceSet &SurfaceSet::operator=(SurfaceSet &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  by_uid_ = std::move(other.by_uid_);
  logical_id_ = std::exchange(other.logical_id_, allocate_logical_id());
  other.by_uid_.clear();
  return *this;
}

Result<SurfaceSet> SurfaceSet::create(std::span<const PricedSurface *const> surfaces) {
  SurfaceSet ss;
  ss.by_uid_.reserve(surfaces.size());
  for (const PricedSurface *s : surfaces) {
    if (s == nullptr) {
      return Err(ErrorCode::InvalidArgument, "SurfaceSet: null surface pointer");
    }
    ss.by_uid_.emplace_back(s->uid(), s);
  }
  std::sort(ss.by_uid_.begin(), ss.by_uid_.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
  for (std::size_t i = 1; i < ss.by_uid_.size(); ++i) {
    if (ss.by_uid_[i].first == ss.by_uid_[i - 1].first) {
      return Err(ErrorCode::InvalidArgument, "SurfaceSet: duplicate uid");
    }
  }
  return ss;
}

const PricedSurface *SurfaceSet::find(std::uint32_t uid) const noexcept {
  auto it = std::lower_bound(by_uid_.begin(), by_uid_.end(), uid,
                             [](const auto &e, std::uint32_t u) { return e.first < u; });
  if (it != by_uid_.end() && it->first == uid) {
    return it->second;
  }
  return nullptr;
}

// ── Pricing ───────────────────────────────────────────────────────────────

namespace {

// Per-unique-contract price result. `fair_value` is the American Andersen-Lake
// mark (the accurate served theo); `g` are the American Greeks (cold finite
// differences on american_price, so g.price == fair_value bit-identical).
// `vega_slope` is the (already omega-blended) SpiderRock VegaSlope from
// `priced_surface_skew_slope` + `vega_slope_from_skew_slope`, computed once
// per unique contract in `solve_span` (where the served surface + this
// contract's (K, T) are both in scope) and carried alongside so `scatter_rows`
// / `reduce_price_totals` -- which do NOT hold a SurfaceSet -- can each apply
// `delta + vega_slope * vega` independently under `PriceOptions::skew_adjusted_delta`,
// matching this file's existing pattern of two independent, bit-identical
// per-position reductions off one shared per-unique solve. Left at its 0.0
// default (a no-op multiplier) whenever the flag is off or the lane never
// reaches Ok status.
struct ContractPx {
  double fair_value{0.0};
  AmericanGreeks g{};
  double iv{0.0};
  double vega_slope{0.0};
  PriceStatus status{PriceStatus::ModelUnavailable};
};

// Per-unique-contract P&L solve result (indexed by the ORIGINAL Portfolio contract
// index). `gb` are the base American (cold-FD) Greeks — the Taylor coefficients;
// `price_base`/`price_target` are the base/shifted American marks; the `d*` are the
// per-share state moves the decomposition is taken over. Defined here (above
// PortfolioWorkspace::Impl) so the workspace can retain a per-unique vector of it.
struct ContractPnl {
  AmericanGreeks gb{};      // base American (cold-FD) Greeks (the Taylor coefficients)
  double price_base{0.0};   // base American mark (fair_value)
  double price_target{0.0}; // shifted American mark (fair_value)
  double dS{0.0};
  double dvol{0.0};
  double dt{0.0};
  double dr{0.0};
  PriceStatus status{PriceStatus::ModelUnavailable};
};

[[nodiscard]] bool degenerate(const OptionContract &c) noexcept {
  return !(std::isfinite(c.K) && c.K > 0.0 && std::isfinite(c.T) && c.T > 0.0);
}

// dSigma/dk at k_log = ln(K / F(T)) off a served `PricedSurface`, central FD
// (h = 1e-4) -- the same scheme adjusted_greeks.hpp's `surface_skew_slope`
// applies to a `VolSurface`. Adapted here because `PricedSurface` is
// K-parameterized (`total_variance(K, T)`, absolute strike) rather than
// k_log-parameterized like `VolSurface::w`: bump k_log by +/- h and convert
// back to an absolute strike through the surface's OWN forward, so
// `total_variance`'s internal `k_log = ln(K / F(T))` re-derivation lands
// exactly on `k_log +/- h` (mod the log/exp round-trip's ~1 ULP noise,
// negligible next to the FD's own 1e-4 step) -- not exposed on
// adjusted_greeks.hpp's public surface because that header's `surface_skew_slope`
// is scoped to `VolSurface` only (see the I6 task report for why a second
// public PricedSurface overload was not added). NaN under the same
// conditions `surface_skew_slope` documents: T <= 0, non-positive/non-finite
// sigma at k_log, a non-finite FD stencil point, or (the PricedSurface-
// specific addition) a non-positive/non-finite forward at T.
[[nodiscard]] double priced_surface_skew_slope(const PricedSurface &surf, double K,
                                               double T) noexcept {
  // Source of truth: adjusted_greeks.cpp's TU-local kFdStep -- the h = 1e-4
  // documented on curve_skew_slope/surface_skew_slope in adjusted_greeks.hpp.
  // Kept as a local mirror (not exposed in that header) because the value is
  // part of those functions' documented CONTRACT; a drift here would be caught
  // by Backtest.HedgeTradesOnAdjustedDelta's independent h=1e-4 oracle.
  constexpr double kFdStep = 1e-4;
  const double F = surf.forward_at(T);
  if (!(F > 0.0) || !std::isfinite(F)) {
    return kNaN;
  }
  const double k_log = std::log(K / F);
  const double w0 = surf.total_variance(K, T);
  const double sigma = std::sqrt(w0 / T);
  if (!(T > 0.0) || !(sigma > 0.0) || !std::isfinite(sigma)) {
    return kNaN;
  }
  const double K_plus = F * std::exp(k_log + kFdStep);
  const double K_minus = F * std::exp(k_log - kFdStep);
  const double w_plus = surf.total_variance(K_plus, T);
  const double w_minus = surf.total_variance(K_minus, T);
  const double dw_dk = (w_plus - w_minus) / (2.0 * kFdStep);
  if (!std::isfinite(dw_dk)) {
    return kNaN;
  }
  return dw_dk / (2.0 * sigma * T);
}

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] bool same_greeks(const AmericanGreeks &a, const AmericanGreeks &b) noexcept {
  return same_bits(a.delta, b.delta) && same_bits(a.gamma, b.gamma) && same_bits(a.vega, b.vega) &&
         same_bits(a.theta, b.theta) && same_bits(a.rho, b.rho) && same_bits(a.vanna, b.vanna) &&
         same_bits(a.volga, b.volga) && same_bits(a.charm, b.charm) && same_bits(a.price, b.price);
}

[[nodiscard]] bool same_seed_payload(const FullGreekSeed &a, const FullGreekSeed &b) noexcept {
  return key_of(a) == key_of(b) && a.surface_instance_id() == b.surface_instance_id() &&
         a.analytic_greeks() == b.analytic_greeks() && same_bits(a.iv(), b.iv()) &&
         same_greeks(a.greeks(), b.greeks());
}

// Configured and forced-cold evaluations can differ in harmless solver roundoff
// even when a cold-compatible surface maps both requests to the same economic
// route. The caller separately validates both candidates' surface provenance and
// route before this comparison; same-enum duplicates remain bit-strict.
[[nodiscard]] bool same_seed_semantics(const FullGreekSeed &a, const FullGreekSeed &b) noexcept {
  if (a.query_execution() == b.query_execution()) {
    return same_seed_payload(a, b);
  }
  return (a.query_execution() == QueryExecution::Configured &&
          b.query_execution() == QueryExecution::ColdReference) ||
         (a.query_execution() == QueryExecution::ColdReference &&
          b.query_execution() == QueryExecution::Configured);
}

[[nodiscard]] bool seed_route_matches(const FullGreekSeed &seed, const OptionContract &contract,
                                      const SurfaceSet &surfaces, bool analytic,
                                      QueryExecution query_execution) noexcept {
  const PricedSurface *const surface = surfaces.find(contract.uid);
  if (surface == nullptr) {
    return false;
  }
  const QueryPricingTier tier = surface->query_pricing_tier();
  const bool configured_is_cold =
      tier == QueryPricingTier::LegacyCompatible || tier == QueryPricingTier::ColdReference;
  const bool cold_alias =
      configured_is_cold && ((seed.query_execution() == QueryExecution::Configured &&
                              query_execution == QueryExecution::ColdReference) ||
                             (seed.query_execution() == QueryExecution::ColdReference &&
                              query_execution == QueryExecution::Configured));
  const bool execution_matches = seed.query_execution() == query_execution || cold_alias;
  return seed.surface_instance_id() == surface->instance_id() &&
         seed.analytic_greeks() == analytic && execution_matches;
}

struct SeedStageCounts {
  std::uint64_t accepted_unique{0};
  std::uint64_t rejected_candidates{0};
};

[[nodiscard]] SeedStageCounts
stage_full_greek_seeds(const SurfaceSet &surfaces, std::span<const OptionContract> contracts,
                       bool analytic, QueryExecution query_execution, bool skew_adjusted_delta,
                       const StickyParams &sticky, std::span<const FullGreekSeed> seeds,
                       std::vector<ContractPx> &staged, std::vector<std::uint8_t> &accepted,
                       std::vector<std::uint32_t> &seed_order,
                       std::vector<std::uint8_t> &candidate_matched) {
  staged.resize(contracts.size());
  accepted.assign(contracts.size(), std::uint8_t{0});
  seed_order.resize(seeds.size());
  std::iota(seed_order.begin(), seed_order.end(), std::uint32_t{0});
  std::sort(seed_order.begin(), seed_order.end(), [&seeds](std::uint32_t a, std::uint32_t b) {
    return key_less(key_of(seeds[a]), key_of(seeds[b]));
  });
  candidate_matched.assign(seeds.size(), std::uint8_t{0});

  SeedStageCounts counts;
  for (std::size_t contract_index = 0; contract_index < contracts.size(); ++contract_index) {
    const OptionContract &contract = contracts[contract_index];
    const ContractKey contract_key = key_of(contract);
    const auto first = std::lower_bound(seed_order.begin(), seed_order.end(), contract_key,
                                        [&seeds](std::uint32_t seed_index, const ContractKey &key) {
                                          return key_less(key_of(seeds[seed_index]), key);
                                        });
    auto last = first;
    while (last != seed_order.end() && key_of(seeds[*last]) == contract_key) {
      candidate_matched[*last] = std::uint8_t{1};
      ++last;
    }
    if (first == last) {
      continue;
    }

    const auto exact_execution =
        std::find_if(first, last, [&seeds, query_execution](std::uint32_t candidate) {
          return seeds[candidate].query_execution() == query_execution;
        });
    const FullGreekSeed &representative =
        exact_execution != last ? seeds[*exact_execution] : seeds[*first];
    bool consistent = true;
    for (auto candidate = first; candidate != last; ++candidate) {
      const bool candidate_route_matches =
          seed_route_matches(seeds[*candidate], contract, surfaces, analytic, query_execution);
      const bool candidate_is_consistent =
          candidate_route_matches && same_seed_semantics(representative, seeds[*candidate]);
      consistent = consistent && candidate_is_consistent;
    }
    if (!consistent) {
      counts.rejected_candidates += static_cast<std::uint64_t>(std::distance(first, last));
      continue;
    }

    ContractPx &out = staged[contract_index];
    out = ContractPx{};
    out.fair_value = representative.greeks().price;
    out.g = representative.greeks();
    out.iv = representative.iv();
    out.status = PriceStatus::Ok;
    if (skew_adjusted_delta) {
      const PricedSurface *const surface = surfaces.find(contract.uid);
      if (surface != nullptr) {
        const double slope = priced_surface_skew_slope(*surface, contract.K, contract.T);
        out.vega_slope = vega_slope_from_skew_slope(slope, surface->pricing().S, sticky);
      }
    }
    accepted[contract_index] = std::uint8_t{1};
    ++counts.accepted_unique;
  }

  for (const std::uint8_t matched : candidate_matched) {
    if (matched == std::uint8_t{0}) {
      ++counts.rejected_candidates;
    }
  }
  return counts;
}

// Solve every UNIQUE contract into `px` (indexed by the ORIGINAL Portfolio
// contract index), reusing the caller's batch-eval SoA scratch (`b_*`). This is
// the T5 grouped, permuted, evaluate_batch fan-out factored verbatim so that
// price()/price_into()/price_totals() share ONE bit-identical solve. `resize`
// to the exact needed size is a no-op that keeps a reserved workspace
// allocation-free.
//
// THE BIT-IDENTITY ARGUMENT (unchanged from T5): each unique result is written
// into px[original_contract_index()[p]] — the SAME slot the ungrouped loop used
// — so the downstream scatter (px[contract_ix(i)]) and the fixed-order totals
// reduction are byte-for-byte unchanged. The solve writes DISJOINT slots, so
// permuting the compute order cannot change any output; grouping only hoists the
// surface find per (uid,side) and lets evaluate_batch reuse a T-bracket carry
// across a raw-bit-equal-T ladder (bit-identical to per-contract evaluate).
void solve_uniques(const PreparedPortfolio &pp, const SurfaceSet &surfaces,
                   std::span<const OptionContract> contracts, bool want_greeks, bool analytic,
                   bool adjoint_greeks, bool skew_adjusted_delta, const StickyParams &sticky,
                   simd::SimdIsa resolved_price_isa, QueryExecution query_execution,
                   unsigned n_threads, std::vector<ContractPx> &px, std::vector<double> &b_iv,
                   std::vector<double> &b_price, std::vector<AmericanGreeks> &b_greeks,
                   std::vector<Status> &b_status, std::span<const ContractPx> staged_seeds = {},
                   std::span<const std::uint8_t> accepted_seeds = {}) {
  const std::size_t n_unique = pp.n_unique();
  using EF = PricedSurface::EvalField;
  // WS-P P3 adjoint A/B: request IV ONLY from evaluate_batch (priced_surface.cpp:
  // "Iv-only: no pricer solve"). The FD greek bundle is never computed, AND the mark's
  // boundary solve is skipped — american_greeks_adjoint's ONE taped AL solve provides
  // BOTH delta..charm AND the served mark (its al_put_price_from_boundary == the
  // andersen_lake mark), so each unique contract pays a SINGLE boundary solve instead
  // of two (I-2 fuse). Non-adjoint FullGreeks stays the FD bundle; Marks stays Iv|Price.
  const EF fields = adjoint_greeks
                        ? EF::Iv
                        : ((want_greeks) ? (EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder)
                                         : (EF::Iv | EF::Price));

  const bool have_seed_stage =
      staged_seeds.size() == contracts.size() && accepted_seeds.size() == contracts.size();
  px.resize(contracts.size());
  for (std::size_t i = 0; i < contracts.size(); ++i) {
    px[i] =
        have_seed_stage && accepted_seeds[i] != std::uint8_t{0} ? staged_seeds[i] : ContractPx{};
  }
  b_iv.resize(n_unique);
  b_price.resize(n_unique);
  b_greeks.resize(want_greeks ? n_unique : 0);
  b_status.resize(n_unique);

  const std::span<const ContractGroup> groups = pp.groups();
  const std::span<const PreparedPriceTile> tiles = pp.price_tiles();
  const std::span<const std::uint32_t> oci = pp.original_contract_index();
  const std::span<const double> kcol = pp.k();
  const std::span<const double> tcol = pp.t();
  const std::span<const Side> scol = pp.side();

  const auto is_seeded = [&](std::uint32_t original_index) noexcept {
    return have_seed_stage && accepted_seeds[original_index] != std::uint8_t{0};
  };

  const auto solve_span = [&](std::uint32_t uid, std::uint32_t s, std::uint32_t e) {
    const std::size_t gsz = static_cast<std::size_t>(e - s);
    const PricedSurface *surf = surfaces.find(uid);
    if (surf == nullptr) {
      // Degenerate is checked FIRST (an invalid contract is InvalidContract even
      // when its uid has no surface) — matching the ungrouped precedence.
      for (std::uint32_t p = s; p < e; ++p) {
        const std::uint32_t orig = oci[p];
        px[orig].status = degenerate(contracts[orig]) ? PriceStatus::InvalidContract
                                                      : PriceStatus::ModelUnavailable;
      }
      return;
    }
    // V1 solve ledger: one full-Greek bundle per unique in this group, split by route.
    // Marks-only (!want_greeks) spends no bundle. Boundary solves themselves are
    // counted always-on at al_seed_boundary (american.cpp); this attributes the route.
    if (want_greeks) {
      using counters::ledger::Solve;
      counters::ledger::bump(adjoint_greeks ? Solve::GreeksBundlesAdjoint
                             : analytic     ? Solve::GreeksBundlesAnalytic
                                            : Solve::GreeksBundlesFd,
                             gsz);
    }
    PricedSurface::EvaluationSoA soa{std::span<double>(b_iv).subspan(s, gsz),
                                     std::span<double>(b_price).subspan(s, gsz),
                                     (want_greeks && !adjoint_greeks)
                                         ? std::span<AmericanGreeks>(b_greeks).subspan(s, gsz)
                                         : std::span<AmericanGreeks>{},
                                     std::span<Status>(b_status).subspan(s, gsz),
                                     {},
                                     {}};
    const Status batch_status =
        surf->evaluate_batch(kcol.subspan(s, gsz), tcol.subspan(s, gsz), scol.subspan(s, gsz),
                             fields, analytic, soa, resolved_price_isa, query_execution);
    if (!batch_status.has_value()) {
      for (std::uint32_t p = s; p < e; ++p) {
        b_iv[p] = kNaN;
        b_price[p] = kNaN;
        if (want_greeks) {
          b_greeks[p].price = kNaN;
        }
        b_status[p] = Err(batch_status.error());
      }
    }
    for (std::uint32_t p = s; p < e; ++p) {
      const std::uint32_t orig = oci[p];
      ContractPx &out = px[orig];
      if (degenerate(contracts[orig])) {
        out.status = PriceStatus::InvalidContract;
        continue;
      }
      out.iv = b_iv[p]; // set before the finite check, exactly as the ungrouped path
      if (!want_greeks) {
        if (!b_status[p].has_value() || !std::isfinite(b_price[p])) {
          out.status = PriceStatus::NumericError;
          continue;
        }
        out.fair_value = b_price[p];
        out.status = PriceStatus::Ok;
        continue;
      }
      if (adjoint_greeks) {
        // WS-P P3+I-2 adjoint A/B: evaluate_batch computed IV ONLY (no boundary solve).
        // american_greeks_adjoint's single taped Andersen-Lake solve provides BOTH the
        // risk bundle AND the mark (its price IS the andersen_lake mark), resolved at
        // the SAME (sigma,rate,q) — so each contract pays ONE boundary solve, not two.
        // delta/gamma stay bit-identical to the FD path (spot-independent boundary),
        // vega/rho match the served mark on the wide domain, FD fallback inside the
        // kernel elsewhere. The served mark is the kernel's AL price (price ==
        // fair_value); it equals the FD-route mark to the andersen_lake tolerance.
        if (!b_status[p].has_value()) {
          out.status = PriceStatus::NumericError;
          continue;
        }
        const auto rp = surf->resolve(kcol[p], tcol[p]);
        bool took = false;
        const Result<AmericanGreeks> ga = detail::american_greeks_adjoint(
            surf->pricing().S, kcol[p], tcol[p], rp.sigma, rp.rate, rp.q_eff, scol[p],
            std::optional<AlOpts>{surf->pricing().al_opts}, &took);
        if (!ga.has_value() || !std::isfinite(ga->price)) {
          out.status = PriceStatus::NumericError;
          continue;
        }
        b_greeks[p] = *ga;
        out.fair_value = ga->price;
        out.g = *ga;
        out.status = PriceStatus::Ok;
        if (skew_adjusted_delta) {
          const double slope = priced_surface_skew_slope(*surf, kcol[p], tcol[p]);
          out.vega_slope = vega_slope_from_skew_slope(slope, surf->pricing().S, sticky);
        }
        continue;
      }
      if (!b_status[p].has_value() || !std::isfinite(b_greeks[p].price)) {
        out.status = PriceStatus::NumericError;
        continue;
      }
      // greeks().price IS the American fair_value (bit-identical, cold-FD invariant).
      out.fair_value = b_greeks[p].price;
      out.g = b_greeks[p];
      out.status = PriceStatus::Ok;
      if (skew_adjusted_delta) {
        const double slope = priced_surface_skew_slope(*surf, kcol[p], tcol[p]);
        out.vega_slope = vega_slope_from_skew_slope(slope, surf->pricing().S, sticky);
      }
    }
  };

  const auto solve_unseeded = [&](std::uint32_t uid, std::uint32_t s, std::uint32_t e) {
    std::uint32_t p = s;
    while (p < e) {
      while (p < e && is_seeded(oci[p])) {
        ++p;
      }
      const std::uint32_t begin = p;
      while (p < e && !is_seeded(oci[p])) {
        ++p;
      }
      if (begin < p) {
        solve_span(uid, begin, p);
      }
    }
  };

  if (have_seed_stage &&
      std::all_of(accepted_seeds.begin(), accepted_seeds.end(),
                  [](std::uint8_t accepted) { return accepted != std::uint8_t{0}; })) {
    return;
  }

  if (!want_greeks && simd::avx2_boundary_selected(resolved_price_isa)) {
    // Any AVX2 Marks route (ForceAvx2 OR Auto→AVX2 now that WS-K ships it by
    // default) needs invariant pack membership. Each immutable tile (up to
    // kPreparedPriceTileLanes, a multiple of the four-lane pack) is one work unit;
    // evaluate_batch packs whole four-lane groups within it, so changing n_threads
    // changes only tile ownership, never the packs or final tail. Gating on
    // avx2_boundary_selected (the SAME predicate the boundary dispatch uses) closes
    // the latent Auto→AVX2 thread-count non-determinism the run_ranges split left.
    pricing_executor().run_blocks(tiles.size(), n_threads, [&](std::size_t i) {
      const PreparedPriceTile &tile = tiles[i];
      solve_span(tile.uid, tile.begin, tile.end);
    });
    return;
  }

  // Preserve the established flattened-unique schedule for Auto/ForceScalar and
  // every full-Greek route. This keeps default scalar scheduling and performance
  // unchanged while still sharing the exact same solve body.
  pricing_executor().run_ranges(n_unique, n_threads, [&](std::size_t lo, std::size_t hi) {
    const std::uint32_t lo32 = static_cast<std::uint32_t>(lo);
    const std::uint32_t hi32 = static_cast<std::uint32_t>(hi);
    auto git =
        std::upper_bound(groups.begin(), groups.end(), lo32,
                         [](std::uint32_t v, const ContractGroup &grp) { return v < grp.end; });
    for (; git != groups.end() && git->begin < hi32; ++git) {
      const std::uint32_t s = std::max<std::uint32_t>(git->begin, lo32);
      const std::uint32_t e = std::min<std::uint32_t>(git->end, hi32);
      solve_unseeded(git->uid, s, e);
    }
  });
}

// Cache-line-disjoint SoA scatter into the caller's output view. `want_greeks`
// gates the eight Greek columns; under Marks those spans are empty and are NEVER
// touched (the 64 B/pos saving). Bit-identical to price()'s scatter for the
// columns it writes.
void scatter_rows(std::span<const Position> positions, const Portfolio &pf,
                  std::span<const ContractPx> px, bool want_greeks, bool skew_adjusted_delta,
                  unsigned n_threads, const PriceFrameView &out) {
  const std::size_t n = positions.size();
  pricing_executor().run_blocks(n, n_threads, [&](std::size_t i) {
    const Position &p = positions[i];
    const ContractPx &c = px[pf.contract_ix(i)];
    const double w = p.qty * eff_multiplier(p.multiplier);
    out.id[i] = p.id;
    out.uid[i] = p.contract.uid;
    out.status[i] = c.status;
    out.iv[i] = c.iv;
    if (c.status != PriceStatus::Ok) {
      out.pv[i] = kNaN;
      out.price[i] = kNaN;
      if (want_greeks) {
        out.delta[i] = out.gamma[i] = out.vega[i] = out.theta[i] = out.rho[i] = kNaN;
        out.vanna[i] = out.volga[i] = out.charm[i] = kNaN;
      }
      return;
    }
    const AmericanGreeks &g = c.g;
    out.price[i] = c.fair_value; // American per-share mark
    out.pv[i] = w * c.fair_value;
    if (want_greeks) {
      // Skew-adjusted (SpiderRock) delta: delta + VegaSlope * vega, VegaSlope
      // precomputed per unique contract in solve_span (c.vega_slope). Off by
      // default -> delta_ps == g.delta, bit-identical to the pre-I6 path.
      const double delta_ps = skew_adjusted_delta ? (g.delta + c.vega_slope * g.vega) : g.delta;
      out.delta[i] = w * delta_ps;
      out.gamma[i] = w * g.gamma;
      out.vega[i] = w * g.vega;
      out.theta[i] = w * g.theta;
      out.rho[i] = w * g.rho;
      out.vanna[i] = w * g.vanna;
      out.volga[i] = w * g.volga;
      out.charm[i] = w * g.charm;
    }
  });
}

// Fixed-input-order totals reduction over the Ok lanes — the deterministic sum
// (same order, same operand association) price() has always used, so totals are
// bit-identical across thread counts AND across price()/price_into/price_totals.
// Reducing `w * c.fair_value` here yields the same bits the scatter stored into
// the pv column (IEEE double round-trips losslessly). Under !want_greeks the
// Greek sums stay NaN (a clean 0.0 would read as a genuinely vega-flat book).
// `t` must be zero-initialized by the caller.
void reduce_price_totals(std::span<const Position> positions, const Portfolio &pf,
                         std::span<const ContractPx> px, bool want_greeks, bool skew_adjusted_delta,
                         PriceTotals &t) {
  if (!want_greeks) {
    t.delta = t.gamma = t.vega = t.theta = t.rho = kNaN;
    t.vanna = t.volga = t.charm = kNaN;
  }
  const std::size_t n = positions.size();
  for (std::size_t i = 0; i < n; ++i) {
    const Position &p = positions[i];
    const ContractPx &c = px[pf.contract_ix(i)];
    if (c.status != PriceStatus::Ok) {
      continue;
    }
    const double w = p.qty * eff_multiplier(p.multiplier);
    t.pv += w * c.fair_value;
    if (want_greeks) {
      const AmericanGreeks &g = c.g;
      // Same skew adjustment as scatter_rows, applied independently (this
      // reduction never reads the scattered frame) so both stay bit-identical
      // to each other and to the pre-I6 path when the flag is off.
      const double delta_ps = skew_adjusted_delta ? (g.delta + c.vega_slope * g.vega) : g.delta;
      t.delta += w * delta_ps;
      t.gamma += w * g.gamma;
      t.vega += w * g.vega;
      t.theta += w * g.theta;
      t.rho += w * g.rho;
      t.vanna += w * g.vanna;
      t.volga += w * g.volga;
      t.charm += w * g.charm;
    }
    ++t.n_ok;
  }
}

// (Re)build the retained PreparedPortfolio only when the exact logical-book
// version changes. The process-unique identity closes same-address ABA; the
// revision detects every successful in-place retime with an O(1), allocation-
// free comparison. The unique count is retained as a defensive invariant check.
// The Greek route/mask does not gate a rebuild:
// PreparedPortfolio's permutation, groups, fixed raw-T tiles, reverse mapping,
// and aligned columns derive purely from book metadata. Marks/Greeks, method,
// preset, and ISA changes therefore reuse the same substrate. Emits
// PreparedBuilds on an actual build so reuse is observable under counters.
[[nodiscard]] Status ensure_prepared(const Portfolio &pf,
                                     std::optional<PreparedPortfolio> &prepared,
                                     std::uint64_t logical_id, std::uint64_t revision,
                                     std::uint64_t &prepared_logical_id,
                                     std::uint64_t &prepared_revision) {
  if (prepared.has_value() && prepared_logical_id == logical_id && prepared_revision == revision &&
      prepared->n_unique() == pf.n_contracts()) {
    return atx::core::Ok();
  }
  Result<PreparedPortfolio> pp = PreparedPortfolio::create(pf, PriceOptions{});
  if (!pp.has_value()) {
    return Err(pp.error());
  }
  ATX_VOL_COUNT(PreparedBuilds);
  prepared.emplace(std::move(*pp));
  prepared_logical_id = logical_id;
  prepared_revision = revision;
  return atx::core::Ok();
}

} // namespace

// ── PortfolioWorkspace (retained substrate + reusable scratch) ─────────────

struct PortfolioWorkspace::Impl {
  std::vector<ContractPx> px; // per unique contract, ORIGINAL-index order
  // Full-Greek seed validation is staged separately so stale/conflicting
  // candidates cannot partially overwrite the live retained-risk bundle.
  std::vector<ContractPx> seed_px;
  std::vector<std::uint8_t> seed_accepted;
  std::vector<std::uint32_t> seed_order;
  std::vector<std::uint8_t> seed_candidate_matched;
  std::vector<double> b_iv; // permuted-order batch-eval scratch (base surface)
  std::vector<double> b_price;
  std::vector<AmericanGreeks> b_greeks; // sized 0 under Marks
  std::vector<Status> b_status;
  // P&L solve scratch (permuted order): the per-unique result plus the shifted-
  // surface batch buffers the grouped P&L solve fills (base batch reuses b_* above).
  std::vector<ContractPnl> pnl;              // per unique contract, ORIGINAL-index order
  std::vector<double> pnl_tt;                // shifted maturity column T_t = T_b - dt
  std::vector<double> pnl_s_iv;              // shifted iv at the common base maturity (sig_t)
  std::vector<double> pnl_s_price;           // shifted American mark at T_t (price_target)
  std::vector<Status> pnl_s_status;          // shifted-price batch status
  std::vector<double> pnl_junk;              // throwaway span for the batch's unused output
  std::vector<Status> pnl_junk_status;       // throwaway status span
  std::optional<PreparedPortfolio> prepared; // retained across snapshots (built once)
  std::uint64_t prepared_logical_id{0};      // exact book identity the substrate is for
  std::uint64_t prepared_revision{0};        // exact retime revision the substrate is for
  // Exact provenance for `px` when it holds a FullGreeks result. P&L may reuse
  // this base bundle only when every field matches; marks-only pricing
  // overwrites `px` and invalidates the stamp before solving.
  bool base_risk_valid{false};
  std::uint64_t base_surface_logical_id{0};
  std::vector<std::uint64_t> base_surface_instance_ids;
  std::uint64_t base_book_logical_id{0};
  std::uint64_t base_book_revision{0};
  bool base_analytic_greeks{false};
  QueryExecution base_query_execution{QueryExecution::Configured};
};

PortfolioWorkspace::PortfolioWorkspace() : impl_(std::make_unique<Impl>()) {}
PortfolioWorkspace::~PortfolioWorkspace() = default;
PortfolioWorkspace::PortfolioWorkspace(PortfolioWorkspace &&) noexcept = default;
PortfolioWorkspace &PortfolioWorkspace::operator=(PortfolioWorkspace &&) noexcept = default;

void PortfolioWorkspace::reserve(std::size_t n_unique, std::size_t n_positions) {
  // The per-row output frame is caller-owned. Unique-contract scratch follows
  // `n_unique`; seed-order validation follows the candidate upper bound supplied
  // by the prepared portfolio position count.
  impl_->px.reserve(n_unique);
  impl_->seed_px.reserve(n_unique);
  impl_->seed_accepted.reserve(n_unique);
  impl_->seed_order.reserve(n_positions);
  impl_->seed_candidate_matched.reserve(n_positions);
  impl_->b_iv.reserve(n_unique);
  impl_->b_price.reserve(n_unique);
  impl_->b_greeks.reserve(n_unique);
  impl_->b_status.reserve(n_unique);
  // P&L solve scratch (sized alongside the price scratch so pnl_explain_into /
  // pnl_totals are allocation-free after this).
  impl_->pnl.reserve(n_unique);
  impl_->pnl_tt.reserve(n_unique);
  impl_->pnl_s_iv.reserve(n_unique);
  impl_->pnl_s_price.reserve(n_unique);
  impl_->pnl_s_status.reserve(n_unique);
  impl_->pnl_junk.reserve(n_unique);
  impl_->pnl_junk_status.reserve(n_unique);
  impl_->base_surface_instance_ids.reserve(n_unique);
}

// ── Pricing entry points ───────────────────────────────────────────────────

Status PortfolioPricer::price_into(const SurfaceSet &surfaces, PriceFieldMask fields,
                                   PriceFrameView out, PortfolioWorkspace &ws,
                                   const PriceOptions &opts) const {
  return price_into(surfaces, fields, out, ws, opts, {});
}

Status PortfolioPricer::price_into(const SurfaceSet &surfaces, PriceFieldMask fields,
                                   PriceFrameView out, PortfolioWorkspace &ws,
                                   const PriceOptions &opts,
                                   std::span<const FullGreekSeed> seeds) const {
  const std::span<const OptionContract> contracts = pf_.contracts();
  const std::span<const Position> positions = pf_.positions();
  const std::size_t n = positions.size();
  const bool want_greeks = has_field(fields, PriceFieldMask::Greeks);

  // Validate the caller's view: marks spans (+ totals sink) are always required;
  // the eight greek spans are required only when the mask asks for them.
  const bool marks_ok = out.id.size() == n && out.uid.size() == n && out.pv.size() == n &&
                        out.price.size() == n && out.iv.size() == n && out.status.size() == n &&
                        out.total != nullptr;
  if (!marks_ok) {
    return Err(ErrorCode::InvalidArgument, "price_into: marks span/size mismatch");
  }
  if (want_greeks) {
    const bool greeks_ok = out.delta.size() == n && out.gamma.size() == n && out.vega.size() == n &&
                           out.theta.size() == n && out.rho.size() == n && out.vanna.size() == n &&
                           out.volga.size() == n && out.charm.size() == n;
    if (!greeks_ok) {
      return Err(ErrorCode::InvalidArgument, "price_into: greek span/size mismatch");
    }
  }
  if (price_frame_view_overlaps(out, want_greeks)) {
    return Err(ErrorCode::InvalidArgument, "price_into: output ranges overlap");
  }

  PortfolioWorkspace::Impl &w = *ws.impl_;
  w.base_risk_valid = false;
  const bool analytic = want_greeks ? opts.analytic_greeks : false;
  // WS-P P3 adjoint A/B (compute-only): route risk through american_greeks_adjoint.
  const bool use_adjoint = want_greeks && opts.adjoint_greeks;
  if (Status s = ensure_prepared(pf_, w.prepared, pf_.logical_id_, pf_.revision_,
                                 w.prepared_logical_id, w.prepared_revision);
      !s.has_value()) {
    return Err(s.error());
  }

  std::span<const ContractPx> staged_seeds;
  std::span<const std::uint8_t> accepted_seeds;
  // Seeds carry FD-computed greeks; never mix them into an adjoint-greeks frame.
  if (fields == PriceFieldMask::FullGreeks && !seeds.empty() && !use_adjoint) {
    // Candidate indices are uint32_t throughout the prepared substrate. One
    // genuine seed per position is a valid producer shape even when the book
    // deduplicates contracts; a larger set fails closed so seed-order scratch
    // stays within the reserve(n_unique, n_positions) warm-allocation bound.
    const bool indices_representable =
        seeds.size() <= static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
    const bool within_candidate_cap = seeds.size() <= positions.size();
    if (!indices_representable || !within_candidate_cap) {
      ATX_VOL_COUNT_N(FullGreekSeedRejectedCandidates, seeds.size());
    } else {
      const SeedStageCounts counts = stage_full_greek_seeds(
          surfaces, contracts, analytic, opts.query_execution, opts.skew_adjusted_delta,
          opts.sticky, seeds, w.seed_px, w.seed_accepted, w.seed_order, w.seed_candidate_matched);
      (void)counts;
      ATX_VOL_COUNT_N(FullGreekSeedReuseLanes, counts.accepted_unique);
      ATX_VOL_COUNT_N(FullGreekSeedRejectedCandidates, counts.rejected_candidates);
      staged_seeds = w.seed_px;
      accepted_seeds = w.seed_accepted;
    }
  }

  solve_uniques(*w.prepared, surfaces, contracts, want_greeks, analytic, use_adjoint,
                opts.skew_adjusted_delta, opts.sticky, opts.resolved_price_isa,
                opts.query_execution, opts.n_threads, w.px, w.b_iv, w.b_price, w.b_greeks,
                w.b_status, staged_seeds, accepted_seeds);

  // FrameBytes reflects the mask (37 B/pos Marks, 101 B/pos FullGreeks). No
  // FrameAllocations: the output spans are caller-owned and the scratch was
  // reserved, so the hot path allocates no frame memory (n_threads>1 still
  // allocates a worker-thread vector; see PortfolioWorkspace).
  ATX_VOL_COUNT_N(FrameBytes, n * bytes_per_position(fields));

  scatter_rows(positions, pf_, w.px, want_greeks, opts.skew_adjusted_delta, opts.n_threads, out);
  *out.total = PriceTotals{};
  reduce_price_totals(positions, pf_, w.px, want_greeks, opts.skew_adjusted_delta, *out.total);
  // Adjoint mode is compute-only: leave base_risk_valid false so a later pnl_totals
  // cannot reuse adjoint-computed base risk under an FD assumption (it recomputes).
  if (want_greeks && !use_adjoint) {
    w.base_surface_logical_id = surfaces.logical_id_;
    w.base_surface_instance_ids.resize(pf_.uids().size());
    for (std::size_t i = 0; i < pf_.uids().size(); ++i) {
      const PricedSurface *const surface = surfaces.find(pf_.uids()[i]);
      w.base_surface_instance_ids[i] = surface != nullptr ? surface->instance_id() : 0u;
    }
    w.base_book_logical_id = pf_.logical_id_;
    w.base_book_revision = pf_.revision_;
    w.base_analytic_greeks = analytic;
    w.base_query_execution = opts.query_execution;
    w.base_risk_valid = true;
  }
  return atx::core::Ok();
}

Result<PriceTotals> PortfolioPricer::price_totals(const SurfaceSet &surfaces, PriceFieldMask fields,
                                                  PortfolioWorkspace &ws,
                                                  const PriceOptions &opts) const {
  const std::span<const OptionContract> contracts = pf_.contracts();
  const std::span<const Position> positions = pf_.positions();
  const bool want_greeks = has_field(fields, PriceFieldMask::Greeks);

  PortfolioWorkspace::Impl &w = *ws.impl_;
  w.base_risk_valid = false;
  const bool analytic = want_greeks ? opts.analytic_greeks : false;
  const bool use_adjoint = want_greeks && opts.adjoint_greeks; // WS-P P3 adjoint A/B
  if (Status s = ensure_prepared(pf_, w.prepared, pf_.logical_id_, pf_.revision_,
                                 w.prepared_logical_id, w.prepared_revision);
      !s.has_value()) {
    return Err(s.error());
  }

  solve_uniques(*w.prepared, surfaces, contracts, want_greeks, analytic, use_adjoint,
                opts.skew_adjusted_delta, opts.sticky, opts.resolved_price_isa,
                opts.query_execution, opts.n_threads, w.px, w.b_iv, w.b_price, w.b_greeks,
                w.b_status);

  // No scatter, no per-row frame: reduce weight*result over positions in fixed
  // input order straight off the unique-result SoA — bit-identical to
  // price(...).total.
  PriceTotals t{};
  reduce_price_totals(positions, pf_, w.px, want_greeks, opts.skew_adjusted_delta, t);
  if (want_greeks && !use_adjoint) {
    w.base_surface_logical_id = surfaces.logical_id_;
    w.base_surface_instance_ids.resize(pf_.uids().size());
    for (std::size_t i = 0; i < pf_.uids().size(); ++i) {
      const PricedSurface *const surface = surfaces.find(pf_.uids()[i]);
      w.base_surface_instance_ids[i] = surface != nullptr ? surface->instance_id() : 0u;
    }
    w.base_book_logical_id = pf_.logical_id_;
    w.base_book_revision = pf_.revision_;
    w.base_analytic_greeks = analytic;
    w.base_query_execution = opts.query_execution;
    w.base_risk_valid = true;
  }
  return t;
}

Result<PriceFrame> PortfolioPricer::price(const SurfaceSet &surfaces,
                                          const PriceOptions &opts) const {
  const std::size_t n = pf_.positions().size();
  const PriceFieldMask fields =
      opts.prices_only ? PriceFieldMask::Marks : PriceFieldMask::FullGreeks;
  const bool want_greeks = has_field(fields, PriceFieldMask::Greeks);

  // The returning API is a thin wrapper over price_into over a locally-owned
  // frame + workspace. Allocate EXACTLY the columns the mask materializes: the
  // six marks columns always, the eight Greek columns only under FullGreeks.
  // FrameAllocations counts the ACTUAL frame column allocations here (not a
  // hard-coded 14); the in-place price_into path emits none.
  PriceFrame f;
  // Six marks columns always; the eight Greek columns only under FullGreeks. The
  // count is inlined (not a local) so it does not read as unused when the OFF
  // build expands ATX_VOL_COUNT_N to ((void)0).
  ATX_VOL_COUNT_N(FrameAllocations, n > 0 ? (want_greeks ? 14u : 6u) : 0u);
  f.id.resize(n);
  f.uid.resize(n);
  f.pv.resize(n);
  f.price.resize(n);
  f.iv.resize(n);
  f.status.resize(n);
  if (want_greeks) {
    f.delta.resize(n);
    f.gamma.resize(n);
    f.vega.resize(n);
    f.theta.resize(n);
    f.rho.resize(n);
    f.vanna.resize(n);
    f.volga.resize(n);
    f.charm.resize(n);
  }

  PriceFrameView view{f.id,    f.uid, f.pv,    f.price, f.iv,    f.delta,  f.gamma, f.vega,
                      f.theta, f.rho, f.vanna, f.volga, f.charm, f.status, &f.total};
  PortfolioWorkspace ws; // one-shot local workspace (the wrapper accepts its alloc)
  if (Status s = price_into(surfaces, fields, view, ws, opts); !s.has_value()) {
    return Err(s.error());
  }
  return f;
}

// ── PnL explain ───────────────────────────────────────────────────────────

namespace {

// Solve every UNIQUE contract's P&L decomposition into `pnl` (indexed by the
// ORIGINAL Portfolio contract index), routing the base/shifted resolves through
// the grouped PreparedPortfolio substrate — the §4 hoist. Mirrors solve_uniques:
// one base.find + one shifted.find per (uid,side) GROUP (vs. per contract), and
// `evaluate_batch` reuses the T-bracket across each group's equal-T ladder.
//
// THE BIT-IDENTITY ARGUMENT (the acceptance gate): each unique result is written
// into pnl[original_contract_index()[p]] — the SAME slot the ungrouped per-contract
// loop used — so the downstream scatter/reduction are byte-unchanged. Per entry the
// grouped path reproduces the ungrouped resolves EXACTLY:
//   * base greeks/mark/iv  = sb->evaluate(K,T_b,side,Iv|Price|First|Second,analytic)
//       via sb->evaluate_batch over the group at T_b — evaluate_batch is bit-identical
//       to per-entry evaluate (T compared by raw bits, carry reused only within an
//       equal-T run);
//   * sig_t = st->iv(K,T_b)          via st->evaluate_batch(Iv) over the group at T_b;
//   * price_target = st->fair_value(K,T_t,side) via st->evaluate_batch(Price) at T_t,
//       where T_t = T_b - dt and dt is CONSTANT within a (uid,side) group, so a raw-
//       bit-equal-T_b run maps to a raw-bit-equal-T_t run (same ladder reuse).
// The status/precedence sequence (degenerate → surface-missing → rolled-past-expiry
// → numeric → sigma-finite) matches the ungrouped path exactly. Splitting a group
// across run_ranges workers cannot change any per-entry result (evaluate_batch is
// per-entry bit-identical regardless of where a sub-call begins).
void solve_pnl_uniques(const PreparedPortfolio &pp, const SurfaceSet &base,
                       const SurfaceSet &shifted, std::span<const OptionContract> contracts,
                       bool analytic, simd::SimdIsa resolved_price_isa,
                       QueryExecution query_execution, unsigned n_threads,
                       std::span<const ContractPx> cached_base, std::vector<ContractPnl> &pnl,
                       std::vector<double> &b_iv, std::vector<double> &b_price,
                       std::vector<AmericanGreeks> &b_greeks, std::vector<Status> &b_status,
                       std::vector<double> &s_tt, std::vector<double> &s_iv,
                       std::vector<double> &s_price, std::vector<Status> &s_status,
                       std::vector<double> &s_junk, std::vector<Status> &s_junk_status) {
  const std::size_t n_unique = pp.n_unique();
  const bool reuse_base = cached_base.size() == contracts.size();
  using EF = PricedSurface::EvalField;

  pnl.resize(contracts.size());
  b_iv.resize(n_unique);
  b_price.resize(n_unique);
  b_greeks.resize(n_unique);
  b_status.resize(n_unique);
  s_tt.resize(n_unique);
  s_iv.resize(n_unique);
  s_price.resize(n_unique);
  s_status.resize(n_unique);
  s_junk.resize(n_unique);
  s_junk_status.resize(n_unique);

  const std::span<const ContractGroup> groups = pp.groups();
  const std::span<const PreparedPriceTile> tiles = pp.price_tiles();
  const std::span<const std::uint32_t> oci = pp.original_contract_index();
  const std::span<const double> kcol = pp.k();
  const std::span<const double> tcol = pp.t();
  const std::span<const Side> scol = pp.side();

  const auto solve_span = [&](std::uint32_t uid, std::uint32_t s, std::uint32_t e) {
    const std::size_t gsz = static_cast<std::size_t>(e - s);
    const PricedSurface *sb = base.find(uid);    // one base find per (uid,side) span
    const PricedSurface *st = shifted.find(uid); // one shifted find per (uid,side) span
    if (sb == nullptr || st == nullptr) {
      // Degenerate is checked FIRST (an invalid contract is InvalidContract even when
      // its uid has no surface) — matching the ungrouped precedence.
      for (std::uint32_t p = s; p < e; ++p) {
        const std::uint32_t orig = oci[p];
        pnl[orig].status = degenerate(contracts[orig]) ? PriceStatus::InvalidContract
                                                       : PriceStatus::ModelUnavailable;
      }
      return;
    }
    if (reuse_base) {
      ATX_VOL_COUNT_N(BaseGreekReuseLanes, gsz);
    }
    // Per-group-constant state moves (the surfaces, hence dt/dS/dr, are per-uid).
    const double dt =
        timestamp_delta_ns(st->pricing().now_ts_ns, sb->pricing().now_ts_ns) / kNsPerYear;
    const double dS = st->pricing().S - sb->pricing().S;
    const double dr = st->pricing().r - sb->pricing().r;
    for (std::uint32_t p = s; p < e; ++p) {
      s_tt[p] = tcol[p] - dt; // T_t = T_b - dt (bit-identical to the ungrouped subtraction)
    }

    if (!reuse_base) {
      // V1 solve ledger: the pnl base solve is a full-Greek bundle per unique. Route by
      // the analytic flag (the pnl path never takes the adjoint route). When the base
      // risk stamp survives (reuse_base) NO bundle is spent — this is the 11 (expiry
      // day) vs 6 (no-churn) solve-economy split the L1 gate moves.
      counters::ledger::bump(analytic ? counters::ledger::Solve::GreeksBundlesAnalytic
                                      : counters::ledger::Solve::GreeksBundlesFd,
                             gsz);
      // Base surface at T_b: greeks + mark + iv (sig_b), analytic route as requested.
      PricedSurface::EvaluationSoA base_soa{std::span<double>(b_iv).subspan(s, gsz),
                                            std::span<double>(b_price).subspan(s, gsz),
                                            std::span<AmericanGreeks>(b_greeks).subspan(s, gsz),
                                            std::span<Status>(b_status).subspan(s, gsz),
                                            {},
                                            {}};
      (void)sb->evaluate_batch(kcol.subspan(s, gsz), tcol.subspan(s, gsz), scol.subspan(s, gsz),
                               EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder, analytic,
                               base_soa, resolved_price_isa, query_execution);
    }
    // Shifted surface at the COMMON base maturity T_b: iv only (sig_t).
    PricedSurface::EvaluationSoA sig_soa{std::span<double>(s_iv).subspan(s, gsz),
                                         std::span<double>(s_junk).subspan(s, gsz),
                                         std::span<AmericanGreeks>{},
                                         std::span<Status>(s_junk_status).subspan(s, gsz),
                                         {},
                                         {}};
    (void)st->evaluate_batch(kcol.subspan(s, gsz), tcol.subspan(s, gsz), scol.subspan(s, gsz),
                             EF::Iv, /*analytic=*/false, sig_soa, resolved_price_isa,
                             query_execution);
    // Shifted surface at the rolled maturity T_t: American mark only (price_target).
    PricedSurface::EvaluationSoA px_soa{std::span<double>(s_junk).subspan(s, gsz),
                                        std::span<double>(s_price).subspan(s, gsz),
                                        std::span<AmericanGreeks>{},
                                        std::span<Status>(s_status).subspan(s, gsz),
                                        {},
                                        {}};
    (void)st->evaluate_batch(kcol.subspan(s, gsz), std::span<double>(s_tt).subspan(s, gsz),
                             scol.subspan(s, gsz), EF::Price, /*analytic=*/false, px_soa,
                             resolved_price_isa, query_execution);

    for (std::uint32_t p = s; p < e; ++p) {
      const std::uint32_t orig = oci[p];
      ContractPnl &out = pnl[orig];
      if (degenerate(contracts[orig])) {
        out.status = PriceStatus::InvalidContract;
        continue;
      }
      const double T_t = s_tt[p];
      if (!(std::isfinite(T_t) && T_t > 0.0)) {
        out.status = PriceStatus::InvalidContract; // rolled past expiry
        continue;
      }
      const ContractPx *const cached = reuse_base ? &cached_base[orig] : nullptr;
      if (cached != nullptr && cached->status != PriceStatus::Ok) {
        out.status = cached->status;
        continue;
      }
      const bool base_ok = cached != nullptr
                               ? std::isfinite(cached->fair_value)
                               : b_status[p].has_value() && std::isfinite(b_greeks[p].price);
      if (!base_ok || !s_status[p].has_value() || !std::isfinite(s_price[p])) {
        out.status = PriceStatus::NumericError;
        continue;
      }
      const double sig_b = cached != nullptr ? cached->iv : b_iv[p];
      const double sig_t = s_iv[p]; // == st->iv(K,T_b), common maturity
      if (!(std::isfinite(sig_b) && std::isfinite(sig_t))) {
        out.status = PriceStatus::NumericError;
        continue;
      }
      out.gb = cached != nullptr ? cached->g : b_greeks[p];
      out.price_base = cached != nullptr ? cached->fair_value : b_greeks[p].price;
      out.price_target = s_price[p];
      out.dS = dS;
      out.dvol = sig_t - sig_b;
      out.dt = dt;
      out.dr = dr;
      out.status = PriceStatus::Ok;
    }
  };

  // H0/H5: the shifted-price leg (EF::Price) rides the SAME cold-American marks
  // route price_into uses, so when AVX2 is selected (ForceAvx2 OR Auto→AVX2 now
  // that WS-K ships it) it must schedule by IMMUTABLE tiles, not the run_ranges
  // split. Each tile is a 4-lane-pack multiple within a single (uid,side) group,
  // so pack membership — hence the ~1e-13 AVX2-vs-scalar per-mark delta — is fixed
  // regardless of worker count. This restores thread-count bit-identity of the P&L
  // decomposition (PnlExplain*/PortfolioPricerTargetMarks) after the WS-K flip.
  // solve_span reads dt/dS/dr per (uid) surface pair, so a per-tile call over a
  // split group reproduces the whole-group result exactly.
  if (simd::avx2_boundary_selected(resolved_price_isa)) {
    pricing_executor().run_blocks(tiles.size(), n_threads, [&](std::size_t ti) {
      const PreparedPriceTile &tile = tiles[ti];
      solve_span(tile.uid, tile.begin, tile.end);
    });
    return;
  }

  // Scalar (ForceScalar / non-AVX2 host): keep the established flattened-unique
  // schedule. Fan out over the FLATTENED permuted unique-contract index
  // [0, n_unique); each worker walks the group boundaries its contiguous [lo,hi)
  // overlaps (mirrors solve_uniques). Disjoint slot writes + per-entry-bit-identical
  // scalar batches ⇒ bit-identical across worker counts.
  pricing_executor().run_ranges(n_unique, n_threads, [&](std::size_t lo, std::size_t hi) {
    const std::uint32_t lo32 = static_cast<std::uint32_t>(lo);
    const std::uint32_t hi32 = static_cast<std::uint32_t>(hi);
    auto git =
        std::upper_bound(groups.begin(), groups.end(), lo32,
                         [](std::uint32_t v, const ContractGroup &grp) { return v < grp.end; });
    for (; git != groups.end() && git->begin < hi32; ++git) {
      const std::uint32_t s = std::max<std::uint32_t>(git->begin, lo32);
      const std::uint32_t e = std::min<std::uint32_t>(git->end, hi32);
      solve_span(git->uid, s, e);
    }
  });
}

// Cache-line-disjoint SoA scatter of the P&L decomposition into the caller's view.
// Each `i` writes only its own row slots from pnl[pf.contract_ix(i)] (disjoint
// writes, pure const reads → bit-identical for any worker count). The per-row math
// is the ungrouped fused loop's, verbatim (no reassociation).
void scatter_pnl_rows(std::span<const Position> positions, const Portfolio &pf,
                      std::span<const ContractPnl> pnl, unsigned n_threads,
                      const PnlFrameView &out) {
  const std::size_t n = positions.size();
  pricing_executor().run_blocks(n, n_threads, [&](std::size_t i) {
    const Position &p = positions[i];
    const ContractPnl &c = pnl[pf.contract_ix(i)];
    const double w = p.qty * eff_multiplier(p.multiplier);
    out.id[i] = p.id;
    out.uid[i] = p.contract.uid;
    out.status[i] = c.status;
    if (c.status != PriceStatus::Ok) {
      out.pv_base[i] = out.pv_target[i] = kNaN;
      out.pnl_total[i] = out.pnl_delta[i] = out.pnl_gamma[i] = kNaN;
      out.pnl_vega[i] = out.pnl_volga[i] = out.pnl_vanna[i] = kNaN;
      out.pnl_theta[i] = out.pnl_rho[i] = out.pnl_charm[i] = kNaN;
      out.pnl_unexplained[i] = kNaN;
      out.d_spot[i] = out.d_vol[i] = out.d_time[i] = out.d_rate[i] = kNaN;
      return;
    }
    const AmericanGreeks &g = c.gb;
    // The full American PnL, decomposed by the base AMERICAN (cold-FD) Greeks. The
    // coefficients carry the early-exercise premium (delta/gamma finite-differenced
    // through american_price), so `unexpl` is the pure higher-order Taylor tail.
    const double pnl_total_ps = c.price_target - c.price_base;
    const double pd = g.delta * c.dS;
    const double pg = 0.5 * g.gamma * c.dS * c.dS;
    const double pv = g.vega * c.dvol;
    const double pvol = 0.5 * g.volga * c.dvol * c.dvol;
    const double pvanna = g.vanna * c.dS * c.dvol;
    const double pth = g.theta * c.dt;
    const double prho = g.rho * c.dr;
    const double pcharm = g.charm * c.dS * c.dt;
    const double explained = pd + pg + pv + pvol + pvanna + pth + prho + pcharm;
    const double unexpl = pnl_total_ps - explained;

    out.pv_base[i] = w * c.price_base;
    out.pv_target[i] = w * c.price_target;
    out.pnl_total[i] = w * pnl_total_ps;
    out.pnl_delta[i] = w * pd;
    out.pnl_gamma[i] = w * pg;
    out.pnl_vega[i] = w * pv;
    out.pnl_volga[i] = w * pvol;
    out.pnl_vanna[i] = w * pvanna;
    out.pnl_theta[i] = w * pth;
    out.pnl_rho[i] = w * prho;
    out.pnl_charm[i] = w * pcharm;
    out.pnl_unexplained[i] = w * unexpl;
    out.d_spot[i] = c.dS;
    out.d_vol[i] = c.dvol;
    out.d_time[i] = c.dt;
    out.d_rate[i] = c.dr;
  });
}

// Minimal exact-mark scatter from the ContractPnl bundle produced by the totals
// solve. Position weights are intentionally absent: the downstream ledger gets
// one raw target mark for each input row, including zero-quantity duplicates.
void scatter_target_mark_rows(std::span<const Position> positions, const Portfolio &pf,
                              std::span<const ContractPnl> pnl, unsigned n_threads,
                              const TargetMarkView &out) {
  pricing_executor().run_blocks(positions.size(), n_threads, [&](std::size_t i) {
    const Position &position = positions[i];
    const ContractPnl &contract = pnl[pf.contract_ix(i)];
    out.id[i] = position.id;
    out.status[i] = contract.status;
    out.price_target[i] = contract.status == PriceStatus::Ok ? contract.price_target : kNaN;
    out.base_vega_proxy[i] = contract.status == PriceStatus::Ok ? contract.gb.vega : kNaN;
  });
}

// Fixed-input-order totals reduction over the Ok lanes — the deterministic sum
// (same order, same operand association) the ungrouped fused loop used, so totals
// are bit-identical across thread counts AND across pnl_explain/pnl_explain_into/
// pnl_totals. Recomputing `w * pd` etc. here yields the same bits the scatter stored
// (IEEE double round-trips losslessly), so pnl_totals (no frame) reduces identically.
// `t` must be zero-initialized by the caller. Keep `i` ascending: IEEE add is
// association-order-sensitive.
void reduce_pnl_totals(std::span<const Position> positions, const Portfolio &pf,
                       std::span<const ContractPnl> pnl, PnlTotals &t) {
  const std::size_t n = positions.size();
  for (std::size_t i = 0; i < n; ++i) {
    const Position &p = positions[i];
    const ContractPnl &c = pnl[pf.contract_ix(i)];
    if (c.status != PriceStatus::Ok) {
      continue;
    }
    const double w = p.qty * eff_multiplier(p.multiplier);
    const AmericanGreeks &g = c.gb;
    const double pnl_total_ps = c.price_target - c.price_base;
    const double pd = g.delta * c.dS;
    const double pg = 0.5 * g.gamma * c.dS * c.dS;
    const double pv = g.vega * c.dvol;
    const double pvol = 0.5 * g.volga * c.dvol * c.dvol;
    const double pvanna = g.vanna * c.dS * c.dvol;
    const double pth = g.theta * c.dt;
    const double prho = g.rho * c.dr;
    const double pcharm = g.charm * c.dS * c.dt;
    const double explained = pd + pg + pv + pvol + pvanna + pth + prho + pcharm;
    const double unexpl = pnl_total_ps - explained;
    t.pv_base += w * c.price_base;
    t.pv_target += w * c.price_target;
    t.pnl_total += w * pnl_total_ps;
    t.pnl_delta += w * pd;
    t.pnl_gamma += w * pg;
    t.pnl_vega += w * pv;
    t.pnl_volga += w * pvol;
    t.pnl_vanna += w * pvanna;
    t.pnl_theta += w * pth;
    t.pnl_rho += w * prho;
    t.pnl_charm += w * pcharm;
    t.pnl_unexplained += w * unexpl;
    ++t.n_ok;
  }
}

} // namespace

Status PortfolioPricer::pnl_explain_into(const SurfaceSet &base, const SurfaceSet &shifted,
                                         PnlFrameView out, PortfolioWorkspace &ws,
                                         const PriceOptions &opts) const {
  const std::span<const OptionContract> contracts = pf_.contracts();
  const std::span<const Position> positions = pf_.positions();
  const std::size_t n = positions.size();

  // P&L has NO field mask — all 19 columns (and the totals sink) must be present and
  // sized to the position count.
  const bool ok =
      out.id.size() == n && out.uid.size() == n && out.pv_base.size() == n &&
      out.pv_target.size() == n && out.pnl_total.size() == n && out.pnl_delta.size() == n &&
      out.pnl_gamma.size() == n && out.pnl_vega.size() == n && out.pnl_volga.size() == n &&
      out.pnl_vanna.size() == n && out.pnl_theta.size() == n && out.pnl_rho.size() == n &&
      out.pnl_charm.size() == n && out.pnl_unexplained.size() == n && out.d_spot.size() == n &&
      out.d_vol.size() == n && out.d_time.size() == n && out.d_rate.size() == n &&
      out.status.size() == n && out.total != nullptr;
  if (!ok) {
    return Err(ErrorCode::InvalidArgument, "pnl_explain_into: span/size mismatch");
  }
  if (pnl_frame_view_overlaps(out)) {
    return Err(ErrorCode::InvalidArgument, "pnl_explain_into: output ranges overlap");
  }

  PortfolioWorkspace::Impl &w = *ws.impl_;
  // P&L always wants the full base Greek bundle; the retained substrate is byte-
  // identical for Marks/FullGreeks (derived from (uid,side,T) only), so it is shared
  // with the price path — a warm price_into build is reused here and vice versa.
  if (Status s = ensure_prepared(pf_, w.prepared, pf_.logical_id_, pf_.revision_,
                                 w.prepared_logical_id, w.prepared_revision);
      !s.has_value()) {
    return Err(s.error());
  }

  const auto surface_instances_match = [&]() noexcept {
    const std::span<const std::uint32_t> uids = pf_.uids();
    if (w.base_surface_instance_ids.size() != uids.size()) {
      return false;
    }
    for (std::size_t i = 0; i < uids.size(); ++i) {
      const PricedSurface *const surface = base.find(uids[i]);
      const std::uint64_t instance_id = surface != nullptr ? surface->instance_id() : 0u;
      if (w.base_surface_instance_ids[i] != instance_id) {
        return false;
      }
    }
    return true;
  };
  const bool reuse_base = w.base_risk_valid && w.base_surface_logical_id == base.logical_id_ &&
                          w.base_book_logical_id == pf_.logical_id_ &&
                          w.base_book_revision == pf_.revision_ &&
                          w.base_analytic_greeks == opts.analytic_greeks &&
                          w.base_query_execution == opts.query_execution &&
                          w.px.size() == contracts.size() && surface_instances_match();
  const std::span<const ContractPx> cached_base =
      reuse_base ? std::span<const ContractPx>{w.px} : std::span<const ContractPx>{};
  solve_pnl_uniques(*w.prepared, base, shifted, contracts, opts.analytic_greeks,
                    opts.resolved_price_isa, opts.query_execution, opts.n_threads, cached_base,
                    w.pnl, w.b_iv, w.b_price, w.b_greeks, w.b_status, w.pnl_tt, w.pnl_s_iv,
                    w.pnl_s_price, w.pnl_s_status, w.pnl_junk, w.pnl_junk_status);

  // 19 per-row columns = 141 bytes/position (8 + 4 + 16*8 + 1). No FrameAllocations:
  // the output spans are caller-owned and the scratch was reserved.
  ATX_VOL_COUNT_N(FrameBytes, n * 141);

  scatter_pnl_rows(positions, pf_, w.pnl, opts.n_threads, out);
  *out.total = PnlTotals{};
  reduce_pnl_totals(positions, pf_, w.pnl, *out.total);
  return atx::core::Ok();
}

Result<PnlTotals> PortfolioPricer::pnl_totals(const SurfaceSet &base, const SurfaceSet &shifted,
                                              PortfolioWorkspace &ws,
                                              const PriceOptions &opts) const {
  const std::span<const OptionContract> contracts = pf_.contracts();
  const std::span<const Position> positions = pf_.positions();

  PortfolioWorkspace::Impl &w = *ws.impl_;
  if (Status s = ensure_prepared(pf_, w.prepared, pf_.logical_id_, pf_.revision_,
                                 w.prepared_logical_id, w.prepared_revision);
      !s.has_value()) {
    return Err(s.error());
  }

  const auto surface_instances_match = [&]() noexcept {
    const std::span<const std::uint32_t> uids = pf_.uids();
    if (w.base_surface_instance_ids.size() != uids.size()) {
      return false;
    }
    for (std::size_t i = 0; i < uids.size(); ++i) {
      const PricedSurface *const surface = base.find(uids[i]);
      const std::uint64_t instance_id = surface != nullptr ? surface->instance_id() : 0u;
      if (w.base_surface_instance_ids[i] != instance_id) {
        return false;
      }
    }
    return true;
  };
  const bool reuse_base = w.base_risk_valid && w.base_surface_logical_id == base.logical_id_ &&
                          w.base_book_logical_id == pf_.logical_id_ &&
                          w.base_book_revision == pf_.revision_ &&
                          w.base_analytic_greeks == opts.analytic_greeks &&
                          w.base_query_execution == opts.query_execution &&
                          w.px.size() == contracts.size() && surface_instances_match();
  const std::span<const ContractPx> cached_base =
      reuse_base ? std::span<const ContractPx>{w.px} : std::span<const ContractPx>{};
  solve_pnl_uniques(*w.prepared, base, shifted, contracts, opts.analytic_greeks,
                    opts.resolved_price_isa, opts.query_execution, opts.n_threads, cached_base,
                    w.pnl, w.b_iv, w.b_price, w.b_greeks, w.b_status, w.pnl_tt, w.pnl_s_iv,
                    w.pnl_s_price, w.pnl_s_status, w.pnl_junk, w.pnl_junk_status);

  // No scatter, no per-row frame: reduce the weighted per-row decomposition over
  // positions in fixed input order — bit-identical to pnl_explain(...).total.
  PnlTotals t{};
  reduce_pnl_totals(positions, pf_, w.pnl, t);
  return t;
}

Result<PnlTotals> PortfolioPricer::pnl_totals_with_target_marks_into(
    const SurfaceSet &base, const SurfaceSet &shifted, TargetMarkView out, PortfolioWorkspace &ws,
    const PriceOptions &opts) const {
  const std::size_t n = pf_.positions().size();
  if (out.id.size() != n || out.price_target.size() != n || out.status.size() != n ||
      out.base_vega_proxy.size() != n) {
    return Err(ErrorCode::InvalidArgument, "pnl_totals_with_target_marks_into: span/size mismatch");
  }
  if (target_mark_view_overlaps(out)) {
    return Err(ErrorCode::InvalidArgument, "pnl_totals_with_target_marks_into: spans overlap");
  }

  // pnl_totals performs the sole base/shifted solve and leaves its unique
  // ContractPnl bundle in the caller's workspace. Scatter only after that solve
  // succeeds, so an invalid view or substrate error cannot partially publish a
  // target-mark frame.
  Result<PnlTotals> totals = pnl_totals(base, shifted, ws, opts);
  if (!totals.has_value()) {
    return Err(totals.error());
  }
  const PortfolioWorkspace::Impl &w = *ws.impl_;
  scatter_target_mark_rows(pf_.positions(), pf_, w.pnl, opts.n_threads, out);
  // uint64 id + double mark + uint8 status + double prior-date vega proxy.
  ATX_VOL_COUNT_N(FrameBytes, n * 25u);
  return totals;
}

Result<PnlFrame> PortfolioPricer::pnl_explain(const SurfaceSet &base, const SurfaceSet &shifted,
                                              const PriceOptions &opts) const {
  const std::size_t n = pf_.positions().size();

  // The returning API is a thin wrapper over pnl_explain_into over a locally-owned
  // frame + workspace. FrameAllocations counts the ACTUAL 19 frame column
  // allocations here (the in-place pnl_explain_into path emits none).
  PnlFrame f;
  ATX_VOL_COUNT_N(FrameAllocations, n > 0 ? 19u : 0u);
  f.id.resize(n);
  f.uid.resize(n);
  f.pv_base.resize(n);
  f.pv_target.resize(n);
  f.pnl_total.resize(n);
  f.pnl_delta.resize(n);
  f.pnl_gamma.resize(n);
  f.pnl_vega.resize(n);
  f.pnl_volga.resize(n);
  f.pnl_vanna.resize(n);
  f.pnl_theta.resize(n);
  f.pnl_rho.resize(n);
  f.pnl_charm.resize(n);
  f.pnl_unexplained.resize(n);
  f.d_spot.resize(n);
  f.d_vol.resize(n);
  f.d_time.resize(n);
  f.d_rate.resize(n);
  f.status.resize(n);

  PnlFrameView view{f.id,        f.uid,       f.pv_base,   f.pv_target,       f.pnl_total,
                    f.pnl_delta, f.pnl_gamma, f.pnl_vega,  f.pnl_volga,       f.pnl_vanna,
                    f.pnl_theta, f.pnl_rho,   f.pnl_charm, f.pnl_unexplained, f.d_spot,
                    f.d_vol,     f.d_time,    f.d_rate,    f.status,          &f.total};
  PortfolioWorkspace ws; // one-shot local workspace (the wrapper accepts its alloc)
  if (Status s = pnl_explain_into(base, shifted, view, ws, opts); !s.has_value()) {
    return Err(s.error());
  }
  return f;
}

} // namespace atx::vol
