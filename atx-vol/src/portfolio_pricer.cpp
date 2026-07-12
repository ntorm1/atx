// PortfolioPricer implementation — dedup, parallel Greeks fan-out, and the
// Taylor PnL-explain decomposition. See portfolio_pricer.hpp for the model.

#include "atx/vol/portfolio_pricer.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>

#include "atx/vol/american.hpp"           // AmericanGreeks
#include "atx/vol/counters.hpp"           // ATX_VOL_COUNT (opt-in P0.2; no-op when OFF)
#include "atx/vol/prepared_portfolio.hpp" // PreparedPortfolio (grouped exec substrate)
#include "atx/vol/pricing_executor.hpp"   // pricing_executor(): the persistent P1.4 pool

namespace atx::vol {

namespace {

// "No value" sentinel for a non-Ok lane's numeric columns.
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

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

// A never-reused process-local identity for exact O(1) workspace invalidation.
// The compare/exchange refuses to wrap: exhausting 2^64-1 logical books is a
// process-fatal invariant breach, never an excuse to reintroduce an ABA window.
[[nodiscard]] std::uint64_t allocate_logical_book_id() noexcept {
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

Portfolio::Portfolio() noexcept : logical_id_(allocate_logical_book_id()) {}

Portfolio::Portfolio(const Portfolio &other)
    : positions_(other.positions_), contracts_(other.contracts_),
      pos_contract_ix_(other.pos_contract_ix_), first_position_ix_(other.first_position_ix_),
      uids_(other.uids_), logical_id_(allocate_logical_book_id()), revision_(0) {}

Portfolio::Portfolio(Portfolio &&other) noexcept
    : positions_(std::move(other.positions_)), contracts_(std::move(other.contracts_)),
      pos_contract_ix_(std::move(other.pos_contract_ix_)),
      first_position_ix_(std::move(other.first_position_ix_)), uids_(std::move(other.uids_)),
      logical_id_(std::exchange(other.logical_id_, allocate_logical_book_id())),
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
  logical_id_ = std::exchange(other.logical_id_, allocate_logical_book_id());
  revision_ = std::exchange(other.revision_, 0);
  return *this;
}

Result<Portfolio> Portfolio::create(std::span<const Position> positions,
                                    const PortfolioBuildOptions &options) {
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
struct ContractPx {
  double fair_value{0.0};
  AmericanGreeks g{};
  double iv{0.0};
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
                   unsigned n_threads, std::vector<ContractPx> &px, std::vector<double> &b_iv,
                   std::vector<double> &b_price, std::vector<AmericanGreeks> &b_greeks,
                   std::vector<Status> &b_status) {
  const std::size_t n_unique = pp.n_unique();
  using EF = PricedSurface::EvalField;
  const EF fields =
      want_greeks ? (EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder) : (EF::Iv | EF::Price);

  px.resize(contracts.size());
  b_iv.resize(n_unique);
  b_price.resize(n_unique);
  b_greeks.resize(want_greeks ? n_unique : 0);
  b_status.resize(n_unique);

  const std::span<const ContractGroup> groups = pp.groups();
  const std::span<const std::uint32_t> oci = pp.original_contract_index();
  const std::span<const double> kcol = pp.k();
  const std::span<const double> tcol = pp.t();
  const std::span<const Side> scol = pp.side();

  const auto solve_span = [&](const ContractGroup &g, std::uint32_t s, std::uint32_t e) {
    const std::size_t gsz = static_cast<std::size_t>(e - s);
    const PricedSurface *surf = surfaces.find(g.uid); // hoisted: one lower_bound / sub-span
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
    PricedSurface::EvaluationSoA soa{
        std::span<double>(b_iv).subspan(s, gsz), std::span<double>(b_price).subspan(s, gsz),
        want_greeks ? std::span<AmericanGreeks>(b_greeks).subspan(s, gsz)
                    : std::span<AmericanGreeks>{},
        std::span<Status>(b_status).subspan(s, gsz), {}, {}};
    (void)surf->evaluate_batch(kcol.subspan(s, gsz), tcol.subspan(s, gsz), scol.subspan(s, gsz),
                               fields, analytic, soa);
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
      if (!b_status[p].has_value() || !std::isfinite(b_greeks[p].price)) {
        out.status = PriceStatus::NumericError;
        continue;
      }
      // greeks().price IS the American fair_value (bit-identical, cold-FD invariant).
      out.fair_value = b_greeks[p].price;
      out.g = b_greeks[p];
      out.status = PriceStatus::Ok;
    }
  };

  // Fan out over the FLATTENED permuted unique-contract index [0, n_unique) so
  // the pool scales even on a single-uid book; each worker walks the group
  // boundaries its contiguous [lo,hi) overlaps (one find per touched group).
  pricing_executor().run_ranges(n_unique, n_threads, [&](std::size_t lo, std::size_t hi) {
    const std::uint32_t lo32 = static_cast<std::uint32_t>(lo);
    const std::uint32_t hi32 = static_cast<std::uint32_t>(hi);
    auto git =
        std::upper_bound(groups.begin(), groups.end(), lo32,
                         [](std::uint32_t v, const ContractGroup &grp) { return v < grp.end; });
    for (; git != groups.end() && git->begin < hi32; ++git) {
      const std::uint32_t s = std::max<std::uint32_t>(git->begin, lo32);
      const std::uint32_t e = std::min<std::uint32_t>(git->end, hi32);
      solve_span(*git, s, e);
    }
  });
}

// Cache-line-disjoint SoA scatter into the caller's output view. `want_greeks`
// gates the eight Greek columns; under Marks those spans are empty and are NEVER
// touched (the 64 B/pos saving). Bit-identical to price()'s scatter for the
// columns it writes.
void scatter_rows(std::span<const Position> positions, const Portfolio &pf,
                  std::span<const ContractPx> px, bool want_greeks, unsigned n_threads,
                  const PriceFrameView &out) {
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
      out.delta[i] = w * g.delta;
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
                         std::span<const ContractPx> px, bool want_greeks, PriceTotals &t) {
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
      t.delta += w * g.delta;
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
// PreparedPortfolio's permutation, groups, oci, and aligned K/T/uid columns
// derive purely from (uid,side,T), so the substrate is byte-identical for
// Marks and FullGreeks -- only the `route` stamped on each ContractGroup
// differs, and nothing reads it yet (T13's AVX2 packer will own route
// correctness there). Emits PreparedBuilds on an actual build so the reuse is
// observable under counters.
[[nodiscard]] Status ensure_prepared(const Portfolio &pf, bool want_greeks, bool analytic,
                                     std::optional<PreparedPortfolio> &prepared,
                                     std::uint64_t logical_id, std::uint64_t revision,
                                     std::uint64_t &prepared_logical_id,
                                     std::uint64_t &prepared_revision) {
  if (prepared.has_value() && prepared_logical_id == logical_id && prepared_revision == revision &&
      prepared->n_unique() == pf.n_contracts()) {
    return atx::core::Ok();
  }
  PriceOptions build_opts;
  build_opts.analytic_greeks = analytic;
  build_opts.prices_only = !want_greeks;
  Result<PreparedPortfolio> pp = PreparedPortfolio::create(pf, build_opts);
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
  std::vector<double> b_iv;   // permuted-order batch-eval scratch (base surface)
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
};

PortfolioWorkspace::PortfolioWorkspace() : impl_(std::make_unique<Impl>()) {}
PortfolioWorkspace::~PortfolioWorkspace() = default;
PortfolioWorkspace::PortfolioWorkspace(PortfolioWorkspace &&) noexcept = default;
PortfolioWorkspace &PortfolioWorkspace::operator=(PortfolioWorkspace &&) noexcept = default;

void PortfolioWorkspace::reserve(std::size_t n_unique, std::size_t n_positions) {
  // The per-row output frame is caller-owned, so only the unique-contract count
  // sizes the internal scratch; `n_positions` is advisory (kept for API symmetry
  // and forward use).
  (void)n_positions;
  impl_->px.reserve(n_unique);
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
}

// ── Pricing entry points ───────────────────────────────────────────────────

Status PortfolioPricer::price_into(const SurfaceSet &surfaces, PriceFieldMask fields,
                                   PriceFrameView out, PortfolioWorkspace &ws,
                                   const PriceOptions &opts) const {
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

  PortfolioWorkspace::Impl &w = *ws.impl_;
  const bool analytic = want_greeks ? opts.analytic_greeks : false;
  if (Status s = ensure_prepared(pf_, want_greeks, analytic, w.prepared, pf_.logical_id_,
                                 pf_.revision_, w.prepared_logical_id, w.prepared_revision);
      !s.has_value()) {
    return Err(s.error());
  }

  solve_uniques(*w.prepared, surfaces, contracts, want_greeks, analytic, opts.n_threads, w.px,
                w.b_iv, w.b_price, w.b_greeks, w.b_status);

  // FrameBytes reflects the mask (37 B/pos Marks, 101 B/pos FullGreeks). No
  // FrameAllocations: the output spans are caller-owned and the scratch was
  // reserved, so the hot path allocates no frame memory (n_threads>1 still
  // allocates a worker-thread vector; see PortfolioWorkspace).
  ATX_VOL_COUNT_N(FrameBytes, n * bytes_per_position(fields));

  scatter_rows(positions, pf_, w.px, want_greeks, opts.n_threads, out);
  *out.total = PriceTotals{};
  reduce_price_totals(positions, pf_, w.px, want_greeks, *out.total);
  return atx::core::Ok();
}

Result<PriceTotals> PortfolioPricer::price_totals(const SurfaceSet &surfaces, PriceFieldMask fields,
                                                  PortfolioWorkspace &ws,
                                                  const PriceOptions &opts) const {
  const std::span<const OptionContract> contracts = pf_.contracts();
  const std::span<const Position> positions = pf_.positions();
  const bool want_greeks = has_field(fields, PriceFieldMask::Greeks);

  PortfolioWorkspace::Impl &w = *ws.impl_;
  const bool analytic = want_greeks ? opts.analytic_greeks : false;
  if (Status s = ensure_prepared(pf_, want_greeks, analytic, w.prepared, pf_.logical_id_,
                                 pf_.revision_, w.prepared_logical_id, w.prepared_revision);
      !s.has_value()) {
    return Err(s.error());
  }

  solve_uniques(*w.prepared, surfaces, contracts, want_greeks, analytic, opts.n_threads, w.px,
                w.b_iv, w.b_price, w.b_greeks, w.b_status);

  // No scatter, no per-row frame: reduce weight*result over positions in fixed
  // input order straight off the unique-result SoA — bit-identical to
  // price(...).total.
  PriceTotals t{};
  reduce_price_totals(positions, pf_, w.px, want_greeks, t);
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
                       bool analytic, unsigned n_threads, std::vector<ContractPnl> &pnl,
                       std::vector<double> &b_iv, std::vector<double> &b_price,
                       std::vector<AmericanGreeks> &b_greeks, std::vector<Status> &b_status,
                       std::vector<double> &s_tt, std::vector<double> &s_iv,
                       std::vector<double> &s_price, std::vector<Status> &s_status,
                       std::vector<double> &s_junk, std::vector<Status> &s_junk_status) {
  const std::size_t n_unique = pp.n_unique();
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
  const std::span<const std::uint32_t> oci = pp.original_contract_index();
  const std::span<const double> kcol = pp.k();
  const std::span<const double> tcol = pp.t();
  const std::span<const Side> scol = pp.side();

  const auto solve_span = [&](const ContractGroup &g, std::uint32_t s, std::uint32_t e) {
    const std::size_t gsz = static_cast<std::size_t>(e - s);
    const PricedSurface *sb = base.find(g.uid);    // one base find per group
    const PricedSurface *st = shifted.find(g.uid); // one shifted find per group
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
    // Per-group-constant state moves (the surfaces, hence dt/dS/dr, are per-uid).
    const double dt =
        static_cast<double>(st->pricing().now_ts_ns - sb->pricing().now_ts_ns) / kNsPerYear;
    const double dS = st->pricing().S - sb->pricing().S;
    const double dr = st->pricing().r - sb->pricing().r;
    for (std::uint32_t p = s; p < e; ++p) {
      s_tt[p] = tcol[p] - dt; // T_t = T_b - dt (bit-identical to the ungrouped subtraction)
    }

    // Base surface at T_b: greeks + mark + iv (sig_b), analytic route as requested.
    PricedSurface::EvaluationSoA base_soa{std::span<double>(b_iv).subspan(s, gsz),
                                          std::span<double>(b_price).subspan(s, gsz),
                                          std::span<AmericanGreeks>(b_greeks).subspan(s, gsz),
                                          std::span<Status>(b_status).subspan(s, gsz), {}, {}};
    (void)sb->evaluate_batch(kcol.subspan(s, gsz), tcol.subspan(s, gsz), scol.subspan(s, gsz),
                             EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder, analytic,
                             base_soa);
    // Shifted surface at the COMMON base maturity T_b: iv only (sig_t).
    PricedSurface::EvaluationSoA sig_soa{
        std::span<double>(s_iv).subspan(s, gsz), std::span<double>(s_junk).subspan(s, gsz),
        std::span<AmericanGreeks>{}, std::span<Status>(s_junk_status).subspan(s, gsz), {}, {}};
    (void)st->evaluate_batch(kcol.subspan(s, gsz), tcol.subspan(s, gsz), scol.subspan(s, gsz),
                             EF::Iv, /*analytic=*/false, sig_soa);
    // Shifted surface at the rolled maturity T_t: American mark only (price_target).
    PricedSurface::EvaluationSoA px_soa{
        std::span<double>(s_junk).subspan(s, gsz), std::span<double>(s_price).subspan(s, gsz),
        std::span<AmericanGreeks>{}, std::span<Status>(s_status).subspan(s, gsz), {}, {}};
    (void)st->evaluate_batch(kcol.subspan(s, gsz), std::span<double>(s_tt).subspan(s, gsz),
                             scol.subspan(s, gsz), EF::Price, /*analytic=*/false, px_soa);

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
      if (!b_status[p].has_value() || !std::isfinite(b_greeks[p].price) ||
          !s_status[p].has_value() || !std::isfinite(s_price[p])) {
        out.status = PriceStatus::NumericError;
        continue;
      }
      const double sig_b = b_iv[p]; // == sb->iv(K,T_b), from the base resolve
      const double sig_t = s_iv[p]; // == st->iv(K,T_b), common maturity
      if (!(std::isfinite(sig_b) && std::isfinite(sig_t))) {
        out.status = PriceStatus::NumericError;
        continue;
      }
      out.gb = b_greeks[p];
      out.price_base = b_greeks[p].price; // greeks().price IS the American fair_value
      out.price_target = s_price[p];
      out.dS = dS;
      out.dvol = sig_t - sig_b;
      out.dt = dt;
      out.dr = dr;
      out.status = PriceStatus::Ok;
    }
  };

  // Fan out over the FLATTENED permuted unique-contract index [0, n_unique); each
  // worker walks the group boundaries its contiguous [lo,hi) overlaps (mirrors
  // solve_uniques). Disjoint slot writes + per-entry-bit-identical batches ⇒
  // bit-identical across worker counts.
  pricing_executor().run_ranges(n_unique, n_threads, [&](std::size_t lo, std::size_t hi) {
    const std::uint32_t lo32 = static_cast<std::uint32_t>(lo);
    const std::uint32_t hi32 = static_cast<std::uint32_t>(hi);
    auto git =
        std::upper_bound(groups.begin(), groups.end(), lo32,
                         [](std::uint32_t v, const ContractGroup &grp) { return v < grp.end; });
    for (; git != groups.end() && git->begin < hi32; ++git) {
      const std::uint32_t s = std::max<std::uint32_t>(git->begin, lo32);
      const std::uint32_t e = std::min<std::uint32_t>(git->end, hi32);
      solve_span(*git, s, e);
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

  PortfolioWorkspace::Impl &w = *ws.impl_;
  // P&L always wants the full base Greek bundle; the retained substrate is byte-
  // identical for Marks/FullGreeks (derived from (uid,side,T) only), so it is shared
  // with the price path — a warm price_into build is reused here and vice versa.
  if (Status s = ensure_prepared(pf_, /*want_greeks=*/true, opts.analytic_greeks, w.prepared,
                                 pf_.logical_id_, pf_.revision_, w.prepared_logical_id,
                                 w.prepared_revision);
      !s.has_value()) {
    return Err(s.error());
  }

  solve_pnl_uniques(*w.prepared, base, shifted, contracts, opts.analytic_greeks, opts.n_threads,
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
  if (Status s = ensure_prepared(pf_, /*want_greeks=*/true, opts.analytic_greeks, w.prepared,
                                 pf_.logical_id_, pf_.revision_, w.prepared_logical_id,
                                 w.prepared_revision);
      !s.has_value()) {
    return Err(s.error());
  }

  solve_pnl_uniques(*w.prepared, base, shifted, contracts, opts.analytic_greeks, opts.n_threads,
                    w.pnl, w.b_iv, w.b_price, w.b_greeks, w.b_status, w.pnl_tt, w.pnl_s_iv,
                    w.pnl_s_price, w.pnl_s_status, w.pnl_junk, w.pnl_junk_status);

  // No scatter, no per-row frame: reduce the weighted per-row decomposition over
  // positions in fixed input order — bit-identical to pnl_explain(...).total.
  PnlTotals t{};
  reduce_pnl_totals(positions, pf_, w.pnl, t);
  return t;
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
