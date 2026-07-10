// PortfolioPricer implementation — dedup, parallel Greeks fan-out, and the
// Taylor PnL-explain decomposition. See portfolio_pricer.hpp for the model.

#include "atx/vol/portfolio_pricer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
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

} // namespace

// ── Portfolio ─────────────────────────────────────────────────────────────

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
  const std::size_t expected =
      options.expected_unique_contracts > 0
          ? std::min(options.expected_unique_contracts, positions.size())
          : std::min(positions.size(), kAutoUniqueReserveCap);
  seen.reserve(expected);
  for (std::size_t i = 0; i < positions.size(); ++i) {
    const OptionContract &c = positions[i].contract;
    const ContractKey key = key_of(c);
    auto [it, inserted] = seen.try_emplace(key, static_cast<std::uint32_t>(pf.contracts_.size()));
    if (inserted) {
      pf.contracts_.push_back(c);
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
  const EF fields = want_greeks ? (EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder)
                                : (EF::Iv | EF::Price);

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
        std::span<Status>(b_status).subspan(s, gsz)};
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
    auto git = std::upper_bound(
        groups.begin(), groups.end(), lo32,
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

// A cheap, allocation-free O(1) fingerprint of a book's first unique
// contract's exact bits (uid, K, T, side) -- one input to the
// PortfolioWorkspace ABA guard (see the contract doc on PortfolioWorkspace in
// the header). This is NOT a full content hash: it narrows, but does not
// close, the "same address, same unique-contract count, different
// middle-of-book content" reuse window. The real contract remains one
// PortfolioWorkspace per live book.
[[nodiscard]] std::uint64_t book_fingerprint(const Portfolio &pf) noexcept {
  const std::span<const OptionContract> contracts = pf.contracts();
  if (contracts.empty()) {
    return 0;
  }
  const OptionContract &c = contracts.front();
  std::uint64_t h = static_cast<std::uint64_t>(c.uid);
  h = h * 1099511628211ULL ^ std::bit_cast<std::uint64_t>(c.K);
  h = h * 1099511628211ULL ^ std::bit_cast<std::uint64_t>(c.T);
  h = h * 1099511628211ULL ^ static_cast<std::uint64_t>(c.side);
  return h;
}

// (Re)build the retained PreparedPortfolio only when the book identity
// changes: the owning Portfolio's address, its unique-contract count
// (pf.n_contracts()), AND the first-unique-contract fingerprint above must all
// match what the cache already holds -- closing the pointer-identity-only ABA
// hazard where a PortfolioPricer reconstructed at the same address as a prior
// one (e.g. a loop rebuilding a local PortfolioPricer per book) could
// otherwise reuse a stale, wrongly-sized substrate and drive solve_uniques()
// out of bounds (or silently mis-price if the stale unique count happened to
// be <= the new book's). The Greek route/mask no longer gates a rebuild:
// PreparedPortfolio's permutation, groups, oci, and aligned K/T/uid columns
// derive purely from (uid,side,T), so the substrate is byte-identical for
// Marks and FullGreeks -- only the `route` stamped on each ContractGroup
// differs, and nothing reads it yet (T13's AVX2 packer will own route
// correctness there). Emits PreparedBuilds on an actual build so the reuse is
// observable under counters.
[[nodiscard]] Status ensure_prepared(const Portfolio &pf, bool want_greeks, bool analytic,
                                     std::optional<PreparedPortfolio> &prepared,
                                     const Portfolio *&prepared_book,
                                     std::uint64_t &prepared_fingerprint) {
  const std::uint64_t fp = book_fingerprint(pf);
  if (prepared.has_value() && prepared_book == &pf && prepared->n_unique() == pf.n_contracts() &&
      prepared_fingerprint == fp) {
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
  prepared_book = &pf;
  prepared_fingerprint = fp;
  return atx::core::Ok();
}

} // namespace

// ── PortfolioWorkspace (retained substrate + reusable scratch) ─────────────

struct PortfolioWorkspace::Impl {
  std::vector<ContractPx> px;            // per unique contract, ORIGINAL-index order
  std::vector<double> b_iv;              // permuted-order batch-eval scratch
  std::vector<double> b_price;
  std::vector<AmericanGreeks> b_greeks;  // sized 0 under Marks
  std::vector<Status> b_status;
  std::optional<PreparedPortfolio> prepared; // retained across snapshots (built once)
  const Portfolio *prepared_book{nullptr};   // book identity the substrate is for
  std::uint64_t prepared_fingerprint{0};     // ABA guard: see book_fingerprint()
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
    const bool greeks_ok = out.delta.size() == n && out.gamma.size() == n &&
                           out.vega.size() == n && out.theta.size() == n && out.rho.size() == n &&
                           out.vanna.size() == n && out.volga.size() == n && out.charm.size() == n;
    if (!greeks_ok) {
      return Err(ErrorCode::InvalidArgument, "price_into: greek span/size mismatch");
    }
  }

  PortfolioWorkspace::Impl &w = *ws.impl_;
  const bool analytic = want_greeks ? opts.analytic_greeks : false;
  if (Status s = ensure_prepared(pf_, want_greeks, analytic, w.prepared, w.prepared_book,
                                 w.prepared_fingerprint);
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
  if (Status s = ensure_prepared(pf_, want_greeks, analytic, w.prepared, w.prepared_book,
                                 w.prepared_fingerprint);
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

  PriceFrameView view{f.id,    f.uid,   f.pv,    f.price, f.iv,     f.delta,  f.gamma, f.vega,
                      f.theta, f.rho,   f.vanna, f.volga, f.charm,  f.status, &f.total};
  PortfolioWorkspace ws; // one-shot local workspace (the wrapper accepts its alloc)
  if (Status s = price_into(surfaces, fields, view, ws, opts); !s.has_value()) {
    return Err(s.error());
  }
  return f;
}

// ── PnL explain ───────────────────────────────────────────────────────────

namespace {

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

} // namespace

Result<PnlFrame> PortfolioPricer::pnl_explain(const SurfaceSet &base, const SurfaceSet &shifted,
                                              const PriceOptions &opts) const {
  const std::span<const OptionContract> contracts = pf_.contracts();
  const std::span<const Position> positions = pf_.positions();

  // 1. Parallel: per unique contract, base Greeks + target reprice + state moves.
  std::vector<ContractPnl> pnl(contracts.size());
  pricing_executor().run_blocks(contracts.size(), opts.n_threads, [&](std::size_t i) {
    const OptionContract &c = contracts[i];
    ContractPnl &out = pnl[i];
    if (degenerate(c)) {
      out.status = PriceStatus::InvalidContract;
      return;
    }
    const PricedSurface *sb = base.find(c.uid);
    const PricedSurface *st = shifted.find(c.uid);
    if (sb == nullptr || st == nullptr) {
      out.status = PriceStatus::ModelUnavailable;
      return;
    }
    const double dt =
        static_cast<double>(st->pricing().now_ts_ns - sb->pricing().now_ts_ns) / kNsPerYear;
    const double T_b = c.T;
    const double T_t = T_b - dt;
    if (!(std::isfinite(T_t) && T_t > 0.0)) {
      out.status = PriceStatus::InvalidContract; // rolled past expiry
      return;
    }
    // One fused base resolution: base Greeks + base IV (sig_b) off a SINGLE sb
    // resolve at (K,T_b), killing the old separate sb->iv() that re-resolved sb at
    // the same (K,T_b) the base greeks already resolved.
    using EF = PricedSurface::EvalField;
    const PricedSurface::FusedResult fr_b =
        sb->evaluate(c.K, T_b, c.side, EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder,
                     opts.analytic_greeks); // base greeks + mark + iv
    auto pt = st->fair_value(c.K, T_t, c.side); // shifted mark at the rolled maturity
    if (!fr_b.status.has_value() || !std::isfinite(fr_b.greeks.price) || !pt.has_value() ||
        !std::isfinite(*pt)) {
      out.status = PriceStatus::NumericError;
      return;
    }
    const double sig_b = fr_b.iv;          // == sb->iv(c.K,T_b), reused from the base resolve
    const double sig_t = st->iv(c.K, T_b); // common maturity: term roll stays in theta
    if (!(std::isfinite(sig_b) && std::isfinite(sig_t))) {
      out.status = PriceStatus::NumericError;
      return;
    }
    out.gb = fr_b.greeks;
    out.price_base = fr_b.greeks.price; // == sb->fair_value(c.K,T_b,side), bit-identical, no dup solve
    out.price_target = *pt;
    out.dS = st->pricing().S - sb->pricing().S;
    out.dvol = sig_t - sig_b;
    out.dt = dt;
    out.dr = st->pricing().r - sb->pricing().r;
    out.status = PriceStatus::Ok;
  });

  // 2. Serial scatter (input order) + fixed-order total reduction.
  PnlFrame f;
  const std::size_t n = positions.size();
  // 19 per-row columns = 141 bytes/position (8+4+8*16+1).
  ATX_VOL_COUNT_N(FrameAllocations, 19);
  ATX_VOL_COUNT_N(FrameBytes, n * 141);
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

  for (std::size_t i = 0; i < n; ++i) {
    const Position &p = positions[i];
    const ContractPnl &c = pnl[pf_.contract_ix(i)];
    const double w = p.qty * eff_multiplier(p.multiplier);
    f.id[i] = p.id;
    f.uid[i] = p.contract.uid;
    f.status[i] = c.status;
    if (c.status != PriceStatus::Ok) {
      f.pv_base[i] = f.pv_target[i] = kNaN;
      f.pnl_total[i] = f.pnl_delta[i] = f.pnl_gamma[i] = kNaN;
      f.pnl_vega[i] = f.pnl_volga[i] = f.pnl_vanna[i] = kNaN;
      f.pnl_theta[i] = f.pnl_rho[i] = f.pnl_charm[i] = kNaN;
      f.pnl_unexplained[i] = kNaN;
      f.d_spot[i] = f.d_vol[i] = f.d_time[i] = f.d_rate[i] = kNaN;
      continue;
    }
    const AmericanGreeks &g = c.gb;
    // The full American PnL, decomposed by the base AMERICAN (cold-FD) Greeks. The
    // coefficients now carry the early-exercise premium (delta/gamma finite-
    // differenced through american_price), so `unexpl` is the pure higher-order
    // Taylor tail — small for a small move — not the early-exercise gap the old
    // European Black-76 Greeks left behind.
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

    f.pv_base[i] = w * c.price_base;
    f.pv_target[i] = w * c.price_target;
    f.pnl_total[i] = w * pnl_total_ps;
    f.pnl_delta[i] = w * pd;
    f.pnl_gamma[i] = w * pg;
    f.pnl_vega[i] = w * pv;
    f.pnl_volga[i] = w * pvol;
    f.pnl_vanna[i] = w * pvanna;
    f.pnl_theta[i] = w * pth;
    f.pnl_rho[i] = w * prho;
    f.pnl_charm[i] = w * pcharm;
    f.pnl_unexplained[i] = w * unexpl;
    f.d_spot[i] = c.dS;
    f.d_vol[i] = c.dvol;
    f.d_time[i] = c.dt;
    f.d_rate[i] = c.dr;

    f.total.pv_base += f.pv_base[i];
    f.total.pv_target += f.pv_target[i];
    f.total.pnl_total += f.pnl_total[i];
    f.total.pnl_delta += f.pnl_delta[i];
    f.total.pnl_gamma += f.pnl_gamma[i];
    f.total.pnl_vega += f.pnl_vega[i];
    f.total.pnl_volga += f.pnl_volga[i];
    f.total.pnl_vanna += f.pnl_vanna[i];
    f.total.pnl_theta += f.pnl_theta[i];
    f.total.pnl_rho += f.pnl_rho[i];
    f.total.pnl_charm += f.pnl_charm[i];
    f.total.pnl_unexplained += f.pnl_unexplained[i];
    ++f.total.n_ok;
  }
  return f;
}

} // namespace atx::vol
