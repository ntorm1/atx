// atx-vol backtest engine (Phase B0) — see backtest.hpp for the model.

#include "atx/vol/backtest.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/phase_profile.hpp"
#include "atx/vol/strategy.hpp"        // IStrategy
#include "atx/vol/surface_archive.hpp" // SurfaceArchive
#include "atx/vol/surface_db.hpp"      // SurfaceDb, DbPartitionInfo, kSurfaceDbPartitionDir/Ext
#include "atx/vol/universe.hpp"        // canonical_symbol

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Process-wide archive-open counter (test seam). Loads increment it exactly once.
std::atomic<std::uint64_t> g_open_count{0};

// A forward-only run owns base, shifted, and at most one prefetched future.
// Retaining three cache entries covers that working set without accumulating one
// mapped archive per date. Caller-supplied caches remain reusable and unbounded.
constexpr std::size_t kPrivateSnapshotCacheCapacity = 3u;

// Contract residual T on the snapshot dated `base_ts`: (expiry - base.ts)/year.
[[nodiscard]] double residual_T(std::int64_t expiry_ts_ns, std::int64_t base_ts_ns) noexcept {
  return (static_cast<double>(expiry_ts_ns) - static_cast<double>(base_ts_ns)) / kNsPerYear;
}

// Materialize the current book as `Position`s priced at `base_ts`'s residual T.
[[nodiscard]] std::vector<Position> positions_at(const std::vector<Lot> &lots,
                                                 std::int64_t base_ts_ns) {
  std::vector<Position> out;
  out.reserve(lots.size());
  for (const Lot &lot : lots) {
    const double T = residual_T(lot.expiry_ts_ns, base_ts_ns);
    out.push_back(Position{lot.id,
                           OptionContract{lot.contract.uid, lot.contract.K, T, lot.contract.side},
                           lot.qty, lot.multiplier});
  }
  return out;
}

class RetainedBookPricer {
public:
  [[nodiscard]] Result<PortfolioPricer *> prepare(const std::vector<Lot> &lots,
                                                  std::int64_t valuation_ts) {
    if (!same_book(lots)) {
      ATX_TRY(Portfolio portfolio, Portfolio::create(positions_at(lots, valuation_ts)));
      pricer_.emplace(std::move(portfolio));
      workspace_ = PortfolioWorkspace{};
      workspace_.reserve(pricer_->portfolio().n_contracts(), pricer_->portfolio().n_positions());
      key_ = lots;
    } else {
      tenors_.resize(lots.size());
      for (std::size_t i = 0; i < lots.size(); ++i) {
        tenors_[i] = residual_T(lots[i].expiry_ts_ns, valuation_ts);
      }
      ATX_TRY_VOID(pricer_->retime(tenors_));
    }
    return &*pricer_;
  }

  [[nodiscard]] PortfolioWorkspace &workspace() noexcept { return workspace_; }

private:
  [[nodiscard]] bool same_book(const std::vector<Lot> &lots) const noexcept {
    if (!pricer_.has_value() || key_.size() != lots.size()) {
      return false;
    }
    for (std::size_t i = 0; i < lots.size(); ++i) {
      const Lot &a = key_[i];
      const Lot &b = lots[i];
      if (a.id != b.id || a.contract.uid != b.contract.uid || a.contract.K != b.contract.K ||
          a.contract.side != b.contract.side || a.qty != b.qty || a.multiplier != b.multiplier ||
          a.expiry_ts_ns != b.expiry_ts_ns || a.cohort != b.cohort) {
        return false;
      }
    }
    return true;
  }

  std::vector<Lot> key_;
  std::vector<double> tenors_;
  std::optional<PortfolioPricer> pricer_;
  PortfolioWorkspace workspace_;
};

// Book greeks + the count of positions the pricer could not value on THIS
// snapshot's date. `total`'s `gross_*` sum only the Ok lanes; `n_unpriced`
// = n_pos - PriceTotals::n_ok is the number EXCLUDED from that sum (surface
// absent, degenerate contract, or numeric failure). `first_unpriced_uid` names
// the first such position (input order) for the Error-policy diagnostic; 0 when
// none. This is a single-date snapshot count — distinct from a STEP's
// completeness (which needs base AND shifted); see BacktestResult.
struct BookGreeks {
  PriceTotals total{};
  std::uint32_t n_unpriced{0};
  std::uint32_t first_unpriced_uid{0};
};

[[nodiscard]] BookGreeks summarize_price_frame(const PriceFrame &frame) {
  BookGreeks result;
  result.total = frame.total;
  const std::size_t n_pos = frame.size();
  const std::size_t n_ok = frame.total.n_ok;
  result.n_unpriced = static_cast<std::uint32_t>((n_pos >= n_ok) ? (n_pos - n_ok) : std::size_t{0});
  if (result.n_unpriced > 0) {
    for (std::size_t i = 0; i < frame.size(); ++i) {
      if (frame.status[i] != PriceStatus::Ok) {
        result.first_unpriced_uid = frame.uid[i];
        break;
      }
    }
  }
  return result;
}

struct ReusablePriceFrame {
  PriceFrame frame;

  void resize(std::size_t n) {
    frame.id.resize(n);
    frame.uid.resize(n);
    frame.pv.resize(n);
    frame.price.resize(n);
    frame.iv.resize(n);
    frame.delta.resize(n);
    frame.gamma.resize(n);
    frame.vega.resize(n);
    frame.theta.resize(n);
    frame.rho.resize(n);
    frame.vanna.resize(n);
    frame.volga.resize(n);
    frame.charm.resize(n);
    frame.status.resize(n);
  }

  [[nodiscard]] PriceFrameView view() noexcept {
    return PriceFrameView{frame.id,    frame.uid,   frame.pv,    frame.price,  frame.iv,
                          frame.delta, frame.gamma, frame.vega,  frame.theta,  frame.rho,
                          frame.vanna, frame.volga, frame.charm, frame.status, &frame.total};
  }
};

// Price the current lots against `snap` at its residual T and report how many
// could not be valued. An empty book yields zero totals and n_unpriced 0 (an
// empty portfolio prices to an empty frame).
[[nodiscard]] Result<BookGreeks> book_greeks(const MarketSnapshot &snap,
                                             const std::vector<Lot> &lots, const PriceOptions &opts,
                                             RetainedBookPricer &retained) {
  ATX_VOL_PROFILE_SCOPE(BookGreeks);
  ATX_TRY(PortfolioPricer * pricer, retained.prepare(lots, snap.ts_ns()));
  PortfolioWorkspace &workspace = retained.workspace();
  auto totals = pricer->price_totals(snap.set(), PriceFieldMask::FullGreeks, workspace, opts);
  if (!totals) {
    return Err(totals.error());
  }
  BookGreeks result;
  result.total = *totals;
  const std::size_t n_positions = pricer->portfolio().n_positions();
  result.n_unpriced = static_cast<std::uint32_t>(
      n_positions >= totals->n_ok ? n_positions - totals->n_ok : std::size_t{0});
  if (result.n_unpriced > 0) {
    // The common all-Ok path stays totals-only. Materialize a diagnostic frame
    // only when the caller needs the first failing uid for an error message.
    auto frame = pricer->price(snap.set(), opts);
    if (!frame) {
      return Err(frame.error());
    }
    for (std::size_t i = 0; i < frame->size(); ++i) {
      if (frame->status[i] != PriceStatus::Ok) {
        result.first_unpriced_uid = frame->uid[i];
        break;
      }
    }
  }
  return Ok(result);
}

// One priced step base -> shifted over `lots`: partition lots with an exact
// expiry-time observation (settled at intrinsic: qty*mult*(intrinsic(S_expiry) -
// base_mark)) from survivors, then Taylor PnL-explain the survivors. A step that
// skips across expiry fails closed because the shifted spot is not an expiry
// settlement observation. Shared by both backtest overloads.
struct StepPnl {
  PnlTotals totals{};
  double settlement{0.0};
  // Alive (non-expiring) positions the pricer could not value this step (surface
  // absent, rolled past expiry, or numeric failure) — i.e. alive.size() - n_ok.
  // Their PnL is excluded from `totals`. `first_unpriced_uid` names the first such
  // position (input order) for the Error-policy diagnostic; 0 when none.
  std::uint32_t n_unpriced{0};
  std::uint32_t first_unpriced_uid{0};
};

[[nodiscard]] Result<StepPnl> compute_step(const MarketSnapshot &base,
                                           const MarketSnapshot &shifted,
                                           const std::vector<Lot> &lots, const PriceOptions &opts,
                                           RetainedBookPricer &retained) {
  ATX_VOL_PROFILE_SCOPE(StepPnl);
  std::vector<Lot> alive;
  alive.reserve(lots.size());
  double settlement = 0.0;
  for (const Lot &lot : lots) {
    if (lot.expiry_ts_ns <= shifted.ts_ns()) {
      if (lot.expiry_ts_ns != shifted.ts_ns()) {
        return Err(ErrorCode::NotFound,
                   "run_backtest: no exact expiry observation for lot id=" +
                       std::to_string(lot.id) + " (expiry_ts_ns=" +
                       std::to_string(lot.expiry_ts_ns) + ", next_snapshot_ts_ns=" +
                       std::to_string(shifted.ts_ns()) + ")");
      }
      const double T_base = residual_T(lot.expiry_ts_ns, base.ts_ns());
      const PricedSurface *bs = base.find(lot.contract.uid);
      const PricedSurface *ss = shifted.find(lot.contract.uid);
      if (bs == nullptr || ss == nullptr) {
        return Err(ErrorCode::NotFound, "run_backtest: no surface for settling lot");
      }
      auto mark = bs->fair_value(lot.contract.K, T_base, lot.contract.side);
      if (!mark) {
        return Err(mark.error());
      }
      const double S = ss->pricing().S;
      const double K = lot.contract.K;
      const double intrinsic =
          (lot.contract.side == Side::Call) ? std::max(0.0, S - K) : std::max(0.0, K - S);
      settlement += lot.qty * lot.multiplier * (intrinsic - *mark);
    } else {
      alive.push_back(lot);
    }
  }

  PnlTotals t{};
  std::uint32_t n_unpriced = 0;
  std::uint32_t first_unpriced_uid = 0;
  if (!alive.empty()) {
    ATX_TRY(PortfolioPricer * pricer, retained.prepare(alive, base.ts_ns()));
    PortfolioWorkspace &workspace = retained.workspace();
    auto totals = pricer->pnl_totals(base.set(), shifted.set(), workspace, opts);
    if (!totals) {
      return Err(totals.error());
    }
    t = *totals;
    // Count the alive positions the reduction skipped. `n_ok` is produced by the
    // same serial-scatter reduction (bit-identical across thread counts) and can
    // never exceed the position count; guard the subtraction rather than underflow.
    const std::size_t n_pos = alive.size();
    const std::size_t n_ok = t.n_ok;
    n_unpriced = static_cast<std::uint32_t>((n_pos >= n_ok) ? (n_pos - n_ok) : std::size_t{0});
    if (n_unpriced > 0) {
      auto fr = pricer->pnl_explain(base.set(), shifted.set(), opts);
      if (!fr) {
        return Err(fr.error());
      }
      for (std::size_t i = 0; i < fr->size(); ++i) {
        if (fr->status[i] != PriceStatus::Ok) {
          first_unpriced_uid = fr->uid[i];
          break;
        }
      }
    }
  }
  return Ok(StepPnl{t, settlement, n_unpriced, first_unpriced_uid});
}

// The Error-policy message for a step that has `n_unpriced` held lots with no
// surface. Kept next to `compute_step` so both run_backtest overloads word it the
// same. Non-empty precondition: callers only build this when n_unpriced > 0.
[[nodiscard]] std::string unpriced_error_message(std::uint32_t n_unpriced,
                                                 std::uint32_t first_uid) {
  return "run_backtest: " + std::to_string(n_unpriced) +
         " held lot(s) have no surface this step (first uid=" + std::to_string(first_uid) + ")";
}

// The Error-policy message for a recorded row whose book greeks could not value
// `n_unpriced` held lots on `date`. Distinct wording from the step message: this
// is a single-date snapshot (book_greeks), so it names the date rather than "this
// step". Non-empty precondition: callers only build this when n_unpriced > 0.
[[nodiscard]] std::string unpriced_greeks_error_message(std::uint32_t n_unpriced,
                                                        std::uint32_t first_uid,
                                                        const std::string &date) {
  return "run_backtest: " + std::to_string(n_unpriced) + " held lot(s) have no surface on " + date +
         " (first uid=" + std::to_string(first_uid) + ")";
}

} // namespace

// ── Clock ─────────────────────────────────────────────────────────────────

Result<Clock> Clock::from_manifest(const CorpusManifest &manifest) {
  Clock clock;
  // `manifest.dates` are unique + ascending; `entries` are sorted (date asc,
  // symbol asc) so the first Ok entry per date is deterministic.
  for (const std::string &d : manifest.dates) {
    for (const CorpusEntry &e : manifest.entries) {
      if (e.date == d && e.status == CorpusFitStatus::Ok && !e.archive_path.empty()) {
        clock.refs_.push_back(SnapshotRef{d, e.archive_path});
        break;
      }
    }
  }
  if (clock.refs_.empty()) {
    return Err(ErrorCode::InvalidArgument, "Clock::from_manifest: no Ok snapshots in manifest");
  }
  return Ok(std::move(clock));
}

Result<Clock> Clock::from_surface_db(const SurfaceDb &db) {
  auto parts = db.partitions();
  if (parts.empty()) {
    return Err(ErrorCode::InvalidArgument, "Clock::from_surface_db: surface db has no partitions");
  }
  std::sort(parts.begin(), parts.end(),
            [](const DbPartitionInfo &a, const DbPartitionInfo &b) { return a.key < b.key; });
  Clock clock;
  clock.refs_.reserve(parts.size());
  const std::filesystem::path dir = std::filesystem::path(db.root()) / kSurfaceDbPartitionDir;
  for (const auto &p : parts) {
    clock.refs_.push_back(
        SnapshotRef{p.key, (dir / (p.key + std::string(kSurfaceDbPartitionExt))).string()});
  }
  return Ok(std::move(clock));
}

// ── MarketSnapshot ──────────────────────────────────────────────────────────

MarketSnapshot::MarketSnapshot(std::vector<PricedSurface> &&surfaces, SurfaceSet &&set,
                               std::int64_t ts,
                               std::vector<std::pair<std::string, std::uint32_t>> &&syms) noexcept
    : surfaces_{std::move(surfaces)}, set_{std::move(set)}, ts_ns_{ts}, syms_{std::move(syms)} {}

std::uint64_t MarketSnapshot::open_count() noexcept { return g_open_count.load(); }
void MarketSnapshot::reset_open_count() noexcept { g_open_count.store(0); }

Result<MarketSnapshot> MarketSnapshot::load(std::string_view archive_path) {
  ATX_VOL_PROFILE_SCOPE(SnapshotLoad);
  auto arch = [&]() {
    ATX_VOL_PROFILE_SCOPE(ArchiveOpen);
    return SurfaceArchive::open_file(archive_path);
  }();
  if (!arch) {
    return Err(arch.error());
  }
  // One archive-open event (the load-once gate asserts N loads => N opens).
  g_open_count.fetch_add(1, std::memory_order_relaxed);

  auto mapped = [&]() {
    ATX_VOL_PROFILE_SCOPE(ArchiveMap);
    return arch->map_all();
  }();
  if (!mapped) {
    return Err(mapped.error());
  }
  std::vector<PricedSurface> surfaces = std::move(*mapped);
  if (surfaces.empty()) {
    return Err(ErrorCode::InvalidArgument, "MarketSnapshot::load: archive holds no surfaces");
  }

  // Valuation timestamp: the surfaces of one date agree on now_ts_ns.
  const std::int64_t ts = surfaces.front().pricing().now_ts_ns;
  for (const PricedSurface &s : surfaces) {
    if (s.pricing().now_ts_ns != ts) {
      return Err(ErrorCode::InvalidArgument,
                 "MarketSnapshot::load: surfaces disagree on now_ts_ns within a date");
    }
  }

  // Non-owning resolver over the owned surfaces' stable addresses.
  std::vector<const PricedSurface *> ptrs;
  ptrs.reserve(surfaces.size());
  for (const PricedSurface &s : surfaces) {
    ptrs.push_back(&s);
  }
  auto set = SurfaceSet::create(ptrs);
  if (!set) {
    return Err(set.error());
  }

  // symbol -> uid from the archive directory (canonical symbol bytes).
  std::vector<std::pair<std::string, std::uint32_t>> syms;
  const std::span<const ArchiveDirEntry> dir = arch->directory();
  syms.reserve(dir.size());
  for (const ArchiveDirEntry &e : dir) {
    syms.emplace_back(std::string(e.symbol, e.symbol_len), e.uid);
  }

  return MarketSnapshot{std::move(surfaces), std::move(*set), ts, std::move(syms)};
}

std::optional<std::uint32_t> MarketSnapshot::uid_of(std::string_view symbol) const {
  // The directory stores CANONICAL symbol bytes (ASCII-upper, truncated), so
  // canonicalize the query the same way before comparing. Without this a
  // lower-case or over-long query would miss a symbol that is present — and a
  // universe authored in lower case would fail to resolve. `canonical_symbol` is
  // the single source of truth shared with `uid_for_symbol` (the write side).
  const std::string query = canonical_symbol(symbol);
  for (const auto &[sym, uid] : syms_) {
    if (sym == query) {
      return uid;
    }
  }
  return std::nullopt;
}

// ── Driver ──────────────────────────────────────────────────────────────────

Result<BacktestResult> run_backtest(const Clock &clock, PortfolioState initial,
                                    const RunConfig &cfg) {
  ATX_VOL_PROFILE_SCOPE(BacktestTotal);
  const std::span<const SnapshotRef> refs = clock.refs();
  if (refs.empty()) {
    return Err(ErrorCode::InvalidArgument, "run_backtest: empty clock");
  }
  const std::size_t stride = (cfg.record_every_n == 0) ? std::size_t{1} : cfg.record_every_n;

  BacktestResult out;
  PortfolioState book = std::move(initial);
  RetainedBookPricer retained_pricer;

  // Append one row from fully-computed step totals.
  const auto push_row = [&out](const std::string &date, std::int64_t ts, double p_total,
                               double p_delta, double p_gamma, double p_vega, double p_vanna,
                               double p_volga, double p_theta, double p_rho, double p_charm,
                               double p_unexpl, double p_settle, double nav_v, const PriceTotals &g,
                               std::size_t n_lots, double n_unpriced, double n_unpriced_greeks) {
    out.date.push_back(date);
    out.ts_ns.push_back(ts);
    out.pnl_total.push_back(p_total);
    out.pnl_delta.push_back(p_delta);
    out.pnl_gamma.push_back(p_gamma);
    out.pnl_vega.push_back(p_vega);
    out.pnl_vanna.push_back(p_vanna);
    out.pnl_volga.push_back(p_volga);
    out.pnl_theta.push_back(p_theta);
    out.pnl_rho.push_back(p_rho);
    out.pnl_charm.push_back(p_charm);
    out.pnl_unexplained.push_back(p_unexpl);
    out.pnl_settlement.push_back(p_settle);
    // Fixed-book overload: no strategy trades, no ledger — zero-fill the B2 columns.
    out.pnl_shares.push_back(0.0);
    out.financing.push_back(0.0);
    out.cost.push_back(0.0);
    out.nav.push_back(nav_v);
    out.cash.push_back(0.0);
    out.gross_delta.push_back(g.delta);
    out.gross_gamma.push_back(g.gamma);
    out.gross_vega.push_back(g.vega);
    out.gross_theta.push_back(g.theta);
    out.turnover_notional.push_back(0.0);
    out.turnover_vega.push_back(0.0);
    out.n_open_lots.push_back(static_cast<double>(n_lots));
    out.n_unpriced_lots.push_back(n_unpriced);
    out.n_unpriced_greeks.push_back(n_unpriced_greeks);
  };

  // base = load(refs[0]) — the inception snapshot.
  const std::shared_ptr<SnapshotCache> snapshot_cache =
      cfg.snapshot_cache ? cfg.snapshot_cache
                         : std::make_shared<SnapshotCache>(kPrivateSnapshotCacheCapacity);
  auto base_res = snapshot_cache->load(refs[0].archive_path);
  if (!base_res) {
    return Err(base_res.error());
  }
  std::shared_ptr<const MarketSnapshot> base = std::move(*base_res);
  if (cfg.prefetch_snapshots && refs.size() > 1) {
    snapshot_cache->prefetch(refs[1].archive_path);
  }

  double nav = 0.0;

  // Row 0: inception (zero PnL, nav 0, book greeks on the first date). Even though
  // no step has run, book_greeks is a real measurement here — an inception book with
  // an unpriced held lot aborts under the Error policy (an empty book prices to 0).
  {
    auto g = book_greeks(*base, book.lots, cfg.price, retained_pricer);
    if (!g) {
      return Err(g.error());
    }
    if (cfg.unpriced == UnpricedLotPolicy::Error && g->n_unpriced > 0) {
      return Err(ErrorCode::NotFound,
                 unpriced_greeks_error_message(g->n_unpriced, g->first_unpriced_uid, refs[0].date));
    }
    push_row(refs[0].date, base->ts_ns(), 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
             0.0, g->total, book.lots.size(), 0.0, static_cast<double>(g->n_unpriced));
  }

  for (std::size_t i = 1; i < refs.size(); ++i) {
    auto shifted_res = snapshot_cache->load(refs[i].archive_path);
    if (!shifted_res) {
      return Err(shifted_res.error());
    }
    std::shared_ptr<const MarketSnapshot> shifted = std::move(*shifted_res);
    if (cfg.prefetch_snapshots && i + 1 < refs.size()) {
      snapshot_cache->prefetch(refs[i + 1].archive_path);
    }

    // Partition + Taylor PnL-explain: byte-identical arithmetic to the strategy
    // overload's step (shared `compute_step`), which now also reports the count of
    // held lots the pricer could not value this step.
    auto step = compute_step(*base, *shifted, book.lots, cfg.price, retained_pricer);
    if (!step) {
      return Err(step.error());
    }
    if (cfg.unpriced == UnpricedLotPolicy::Error && step->n_unpriced > 0) {
      return Err(ErrorCode::NotFound,
                 unpriced_error_message(step->n_unpriced, step->first_unpriced_uid));
    }
    const PnlTotals &t = step->totals;
    const double settlement = step->settlement;

    const double step_total = t.pnl_total + settlement;
    nav += step_total; // cumulative every step, regardless of recording

    // Adopt the shifted snapshot as the next base (no reload) and drop expiries.
    base = std::move(shifted);
    std::erase_if(book.lots, [&base](const Lot &l) { return l.expiry_ts_ns <= base->ts_ns(); });

    const bool is_last = (i + 1 == refs.size());
    const bool record = ((i % stride) == 0) || is_last;
    if (record) {
      auto g = book_greeks(*base, book.lots, cfg.price, retained_pricer);
      if (!g) {
        return Err(g.error());
      }
      if (cfg.unpriced == UnpricedLotPolicy::Error && g->n_unpriced > 0) {
        return Err(ErrorCode::NotFound, unpriced_greeks_error_message(
                                            g->n_unpriced, g->first_unpriced_uid, refs[i].date));
      }
      push_row(refs[i].date, base->ts_ns(), step_total, t.pnl_delta, t.pnl_gamma, t.pnl_vega,
               t.pnl_vanna, t.pnl_volga, t.pnl_theta, t.pnl_rho, t.pnl_charm, t.pnl_unexplained,
               settlement, nav, g->total, book.lots.size(), static_cast<double>(step->n_unpriced),
               static_cast<double>(g->n_unpriced));
    }
  }

  return Ok(std::move(out));
}

// One step's realized trade accounting (frictions + turnover) computed by the
// engine executor over the book diff and the hedge overlay.
struct ExecResult {
  double cost{0.0};
  double turnover_notional{0.0};
  double turnover_vega{0.0};
  std::optional<BookGreeks> book_greeks{};
};

Result<BacktestResult> run_backtest(const Clock &clock, IStrategy &strat, const RunConfig &cfg) {
  ATX_VOL_PROFILE_SCOPE(BacktestTotal);
  const std::span<const SnapshotRef> refs = clock.refs();
  if (refs.empty()) {
    return Err(ErrorCode::InvalidArgument, "run_backtest: empty clock");
  }
  const std::size_t stride = (cfg.record_every_n == 0) ? std::size_t{1} : cfg.record_every_n;

  BacktestResult out;
  PortfolioState book{};
  std::uint64_t next_id = 1; // monotonic lot ids the strategy consumes
  ReusablePriceFrame risk_frame;
  RetainedBookPricer retained_pricer;

  // ── Engine-internal cash + per-uid share ledger (B2) ──────────────────────
  double cash = cfg.financing.initial_cash;
  std::vector<std::pair<std::uint32_t, double>> shares; // per-uid delta-hedge share count
  const auto shares_get = [&shares](std::uint32_t uid) -> double {
    for (const auto &kv : shares) {
      if (kv.first == uid) {
        return kv.second;
      }
    }
    return 0.0;
  };
  const auto shares_add = [&shares](std::uint32_t uid, double dn) {
    for (auto &kv : shares) {
      if (kv.first == uid) {
        kv.second += dn;
        return;
      }
    }
    shares.push_back({uid, dn});
  };
  const auto shares_sum = [&shares]() -> double {
    double s = 0.0;
    for (const auto &kv : shares) {
      s += kv.second;
    }
    return s;
  };

  // Per-share half-spread under the friction model (0 when SpreadKind::None).
  const auto half_spread = [&cfg](double mark, double vega) -> double {
    switch (cfg.frictions.spread_kind) {
    case FrictionModel::SpreadKind::None:
      return 0.0;
    case FrictionModel::SpreadKind::PriceBps:
      return mark * (cfg.frictions.half_spread_bps / 1.0e4);
    case FrictionModel::SpreadKind::VolTicks:
      return vega * cfg.frictions.vol_tick;
    }
    return 0.0;
  };

  const auto push_row = [&out](const std::string &date, std::int64_t ts, double p_total,
                               double p_delta, double p_gamma, double p_vega, double p_vanna,
                               double p_volga, double p_theta, double p_rho, double p_charm,
                               double p_unexpl, double p_settle, double p_shares, double p_fin,
                               double p_cost, double nav_v, double cash_v, double g_delta,
                               const PriceTotals &g, double turn_notl, double turn_vega,
                               std::size_t n_lots, double n_unpriced, double n_unpriced_greeks) {
    out.date.push_back(date);
    out.ts_ns.push_back(ts);
    out.pnl_total.push_back(p_total);
    out.pnl_delta.push_back(p_delta);
    out.pnl_gamma.push_back(p_gamma);
    out.pnl_vega.push_back(p_vega);
    out.pnl_vanna.push_back(p_vanna);
    out.pnl_volga.push_back(p_volga);
    out.pnl_theta.push_back(p_theta);
    out.pnl_rho.push_back(p_rho);
    out.pnl_charm.push_back(p_charm);
    out.pnl_unexplained.push_back(p_unexpl);
    out.pnl_settlement.push_back(p_settle);
    out.pnl_shares.push_back(p_shares);
    out.financing.push_back(p_fin);
    out.cost.push_back(p_cost);
    out.nav.push_back(nav_v);
    out.cash.push_back(cash_v);
    out.gross_delta.push_back(g_delta); // NET book delta = option delta + hedge shares
    out.gross_gamma.push_back(g.gamma);
    out.gross_vega.push_back(g.vega);
    out.gross_theta.push_back(g.theta);
    out.turnover_notional.push_back(turn_notl);
    out.turnover_vega.push_back(turn_vega);
    out.n_open_lots.push_back(static_cast<double>(n_lots));
    out.n_unpriced_lots.push_back(n_unpriced);
    out.n_unpriced_greeks.push_back(n_unpriced_greeks);
  };

  // Signal series: names captured on the first recorded row, then one value per
  // recorded row per series (NaN when a name is absent that row).
  bool sig_init = false;
  const auto record_signals = [&out, &strat, &sig_init](const MarketSnapshot &snap) {
    ATX_VOL_PROFILE_SCOPE(Signals);
    const std::vector<std::pair<std::string, double>> s = strat.signals(snap);
    if (!sig_init) {
      for (const auto &kv : s) {
        out.signals.emplace_back(kv.first, std::vector<double>{});
      }
      sig_init = true;
    }
    for (auto &series : out.signals) {
      double v = std::numeric_limits<double>::quiet_NaN();
      for (const auto &kv : s) {
        if (kv.first == series.first) {
          v = kv.second;
          break;
        }
      }
      series.second.push_back(v);
    }
  };

  // Book this step's trades (entries + roll-closes diffed against `before_lots`,
  // then the DeltaToZero hedge overlay), updating `cash`/`shares` and returning the
  // step's realized friction `cost` + option turnover. Frictionless + hedge-None
  // ⇒ cost/turnover 0 and cash/shares untouched.
  const auto execute = [&](const MarketSnapshot &base_snap,
                           const std::vector<Lot> &before_lots) -> Result<ExecResult> {
    ATX_VOL_PROFILE_SCOPE(Execution);
    ExecResult ex;
    bool entry_happened = false;

    const auto in_before = [&before_lots](std::uint64_t id) {
      for (const Lot &l : before_lots) {
        if (l.id == id) {
          return true;
        }
      }
      return false;
    };
    const auto in_book = [&book](std::uint64_t id) {
      for (const Lot &l : book.lots) {
        if (l.id == id) {
          return true;
        }
      }
      return false;
    };

    for (const Lot &lot : book.lots) {
      if (!in_before(lot.id)) {
        entry_happened = true;
        break;
      }
    }
    const HedgeSpec hedge_spec = strat.hedge_spec();
    const bool hedge_fires =
        hedge_spec.kind == HedgeSpec::Kind::DeltaToZero &&
        ((hedge_spec.cadence == HedgeSpec::Cadence::Daily) ||
         (hedge_spec.cadence == HedgeSpec::Cadence::AtEntry && entry_happened));

    // One full-book price supplies entry vega, per-uid hedge delta, and the row
    // Greek totals. This replaces N per-uid portfolios plus the later row pass.
    const PriceFrame *current_risk = nullptr;
    if (entry_happened || hedge_fires) {
      ATX_TRY(PortfolioPricer * pricer, retained_pricer.prepare(book.lots, base_snap.ts_ns()));
      risk_frame.resize(pricer->portfolio().n_positions());
      ATX_TRY_VOID(pricer->price_into(base_snap.set(), PriceFieldMask::FullGreeks,
                                      risk_frame.view(), retained_pricer.workspace(), cfg.price));
      ex.book_greeks = summarize_price_frame(risk_frame.frame);
      current_risk = &risk_frame.frame;
    }

    // Entry trades: lots present now but absent from `before_lots`.
    for (const Lot &lot : book.lots) {
      if (in_before(lot.id)) {
        continue;
      }
      ATX_VOL_PROFILE_SCOPE(EntryRisk);
      double vega = 0.0;
      if (current_risk != nullptr) {
        for (std::size_t i = 0; i < current_risk->size(); ++i) {
          if (current_risk->id[i] == lot.id && current_risk->status[i] == PriceStatus::Ok) {
            const double weight = lot.qty * lot.multiplier;
            vega = weight != 0.0 ? current_risk->vega[i] / weight : 0.0;
            break;
          }
        }
      }
      const double mark = lot.entry_price; // entry_mark (fill at mid)
      const double hs = half_spread(mark, vega);
      const double leg_cost = std::fabs(lot.qty) * lot.multiplier * hs +
                              cfg.frictions.per_contract_cost * std::fabs(lot.qty);
      ex.cost += leg_cost;
      cash -= lot.qty * lot.multiplier * mark; // premium paid (long) / received (short)
      ex.turnover_notional += std::fabs(lot.qty * lot.multiplier * mark);
      ex.turnover_vega += std::fabs(lot.qty * lot.multiplier * vega);
    }

    // Roll-close trades: lots in `before_lots` gone now (expiries were already
    // settled + erased before on_step, so these are strategy-driven closes).
    for (const Lot &lot : before_lots) {
      if (in_book(lot.id)) {
        continue;
      }
      const double T_res = residual_T(lot.expiry_ts_ns, base_snap.ts_ns());
      const PricedSurface *s = base_snap.find(lot.contract.uid);
      double mark = 0.0;
      double vega = 0.0;
      if (s != nullptr) {
        const Result<AmericanGreeks> risk =
            s->greeks_analytic(lot.contract.K, T_res, lot.contract.side);
        if (!risk) {
          return Err(risk.error());
        }
        mark = risk->price;
        vega = risk->vega;
      }
      const double hs = half_spread(mark, vega);
      const double leg_cost = std::fabs(lot.qty) * lot.multiplier * hs +
                              cfg.frictions.per_contract_cost * std::fabs(lot.qty);
      ex.cost += leg_cost;
      cash += lot.qty * lot.multiplier * mark; // proceeds from closing
      ex.turnover_notional += std::fabs(lot.qty * lot.multiplier * mark);
      ex.turnover_vega += std::fabs(lot.qty * lot.multiplier * vega);
    }

    // Hedge overlay: aggregate every uid from the one full-book frame above.
    if (hedge_fires) {
      std::vector<std::uint32_t> uids;
      const auto add_uid = [&uids](std::uint32_t u) {
        for (std::uint32_t x : uids) {
          if (x == u) {
            return;
          }
        }
        uids.push_back(u);
      };
      for (const Lot &lot : book.lots) {
        add_uid(lot.contract.uid);
      }
      for (const auto &item : shares) {
        add_uid(item.first);
      }
      for (const std::uint32_t uid : uids) {
        ATX_VOL_PROFILE_SCOPE(HedgeRisk);
        double option_delta = 0.0;
        if (current_risk != nullptr) {
          for (std::size_t i = 0; i < current_risk->size(); ++i) {
            if (current_risk->uid[i] == uid && current_risk->status[i] == PriceStatus::Ok) {
              option_delta += current_risk->delta[i];
            }
          }
        }
        const double net = option_delta + shares_get(uid);
        if (std::fabs(net) > hedge_spec.band) {
          const double shares_to_trade = -net;
          const PricedSurface *surface = base_snap.find(uid);
          const double spot = surface != nullptr ? surface->pricing().S : 0.0;
          ex.cost += std::fabs(shares_to_trade) * spot * (cfg.frictions.hedge_slippage_bps / 1.0e4);
          cash -= shares_to_trade * spot;
          shares_add(uid, shares_to_trade);
        }
      }
    }

    cash -= ex.cost; // realized frictions hit cash at fill
    return Ok(ex);
  };

  const std::shared_ptr<SnapshotCache> snapshot_cache =
      cfg.snapshot_cache ? cfg.snapshot_cache
                         : std::make_shared<SnapshotCache>(kPrivateSnapshotCacheCapacity);
  auto base_res = snapshot_cache->load(refs[0].archive_path);
  if (!base_res) {
    return Err(base_res.error());
  }
  std::shared_ptr<const MarketSnapshot> base = std::move(*base_res);
  if (cfg.prefetch_snapshots && refs.size() > 1) {
    snapshot_cache->prefetch(refs[1].archive_path);
  }

  double nav = 0.0;

  // Inception (row 0): open positions AS OF refs[0], book entry frictions + premium
  // + the opening hedge into cash; PnL columns are zero; record post-trade cash.
  {
    Status st = [&]() {
      ATX_VOL_PROFILE_SCOPE(StrategyStep);
      return strat.on_step(*base, 0, book, next_id);
    }();
    if (!st) {
      return Err(st.error());
    }
    const std::vector<Lot> before_lots; // empty ⇒ every opened lot is a fresh entry
    auto ex = execute(*base, before_lots);
    if (!ex) {
      return Err(ex.error());
    }
    Result<BookGreeks> g = ex->book_greeks.has_value()
                               ? Ok(*ex->book_greeks)
                               : book_greeks(*base, book.lots, cfg.price, retained_pricer);
    if (!g) {
      return Err(g.error());
    }
    // Inception book greeks are a real measurement (the strategy has already opened
    // its entries): under the Error policy an unpriced held lot here aborts, exactly
    // as a later row would. The strategy never opens a lot in an absent name, so this
    // is 0 for a normally-opened basket and never fires on an empty book.
    if (cfg.unpriced == UnpricedLotPolicy::Error && g->n_unpriced > 0) {
      return Err(ErrorCode::NotFound,
                 unpriced_greeks_error_message(g->n_unpriced, g->first_unpriced_uid, refs[0].date));
    }
    const double g_delta = g->total.delta + shares_sum();
    // Opening fills are the first economic event of the run. execute() already
    // deducted their friction from cash; stamp the same loss into row-0 PnL/NAV
    // so total return and attribution include every paid dollar.
    nav = -ex->cost;
    push_row(refs[0].date, base->ts_ns(), nav, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
             0.0, 0.0, ex->cost, nav, cash, g_delta, g->total, ex->turnover_notional,
             ex->turnover_vega, book.lots.size(), 0.0, static_cast<double>(g->n_unpriced));
    record_signals(*base);
  }

  for (std::size_t i = 1; i < refs.size(); ++i) {
    auto shifted_res = snapshot_cache->load(refs[i].archive_path);
    if (!shifted_res) {
      return Err(shifted_res.error());
    }
    std::shared_ptr<const MarketSnapshot> shifted = std::move(*shifted_res);
    if (cfg.prefetch_snapshots && i + 1 < refs.size()) {
      snapshot_cache->prefetch(refs[i + 1].archive_path);
    }

    // 1. PnL of the current book (resolved on base) forward to shifted (unchanged B1).
    auto step = compute_step(*base, *shifted, book.lots, cfg.price, retained_pricer);
    if (!step) {
      return Err(step.error());
    }
    if (cfg.unpriced == UnpricedLotPolicy::Error && step->n_unpriced > 0) {
      return Err(ErrorCode::NotFound,
                 unpriced_error_message(step->n_unpriced, step->first_unpriced_uid));
    }
    const PnlTotals &t = step->totals;
    const double settlement = step->settlement;

    // 2. Shares PnL + financing over the step, from the ledger held over the step.
    const double dt =
        (static_cast<double>(shifted->ts_ns()) - static_cast<double>(base->ts_ns())) / kNsPerYear;
    double shares_pnl = 0.0;
    double financing = 0.0;
    if (cfg.financing.finance_premium) {
      const double r = base->surfaces().front().pricing().r; // base-date rate
      const double growth = std::exp(r * dt);
      financing += cash * (growth - 1.0); // cash carry on the pre-step balance
      cash *= growth;                     // apply to the ledger
    }
    for (const auto &[uid, n] : shares) {
      const PricedSurface *bs = base->find(uid);
      const PricedSurface *ss = shifted->find(uid);
      if (bs == nullptr || ss == nullptr) {
        continue;
      }
      const double Sb = bs->pricing().S;
      shares_pnl += n * (ss->pricing().S - Sb);                      // shares held over the step
      const double short_amt = std::max(0.0, -n);                    // |min(shares,0)|
      financing += -cfg.financing.borrow_rate * short_amt * Sb * dt; // borrow (0 when rate 0)
      if (cfg.financing.shares_carry) {
        // Buying shares has already reduced the financed cash balance, so cash
        // carry owns the funding cost when enabled. Charging r here too would
        // count it twice. Without cash financing, retain the standalone (q-r)
        // total-carry shortcut.
        const double funding_rate = cfg.financing.finance_premium ? 0.0 : bs->pricing().r;
        financing += n * (bs->q_eff_at(0.25) - funding_rate) * Sb * dt;
      }
    }

    // 3. Adopt shifted as the next base; settle expiries into cash; drop them.
    base = std::move(shifted);
    for (const Lot &lot : book.lots) {
      if (lot.expiry_ts_ns > base->ts_ns()) {
        continue;
      }
      const PricedSurface *bs = base->find(lot.contract.uid);
      if (bs == nullptr) {
        continue;
      }
      const double S = bs->pricing().S;
      const double K = lot.contract.K;
      const double intrinsic =
          (lot.contract.side == Side::Call) ? std::max(0.0, S - K) : std::max(0.0, K - S);
      cash += lot.qty * lot.multiplier * intrinsic; // intrinsic settle proceeds
    }
    std::erase_if(book.lots, [&base](const Lot &l) { return l.expiry_ts_ns <= base->ts_ns(); });

    // 4-5. Strategy entries/rolls + hedge overlay on the new base.
    const std::vector<Lot> before_lots = book.lots; // survivors before on_step
    Status st = [&]() {
      ATX_VOL_PROFILE_SCOPE(StrategyStep);
      return strat.on_step(*base, i, book, next_id);
    }();
    if (!st) {
      return Err(st.error());
    }
    auto ex = execute(*base, before_lots);
    if (!ex) {
      return Err(ex.error());
    }

    // 6. Running NAV increment — EXACT add order; collapses to B1's
    //    (pnl_total + settlement) bit-for-bit when features are off.
    double step_total = t.pnl_total;
    step_total += settlement;
    step_total += shares_pnl;
    step_total += financing;
    step_total -= ex->cost;
    nav += step_total;

    // 7. Record @ granularity: book greeks (net delta incl. shares) + B2 columns.
    const bool is_last = (i + 1 == refs.size());
    const bool record = ((i % stride) == 0) || is_last;
    if (record) {
      Result<BookGreeks> g = ex->book_greeks.has_value()
                                 ? Ok(*ex->book_greeks)
                                 : book_greeks(*base, book.lots, cfg.price, retained_pricer);
      if (!g) {
        return Err(g.error());
      }
      // The step-level Error guard above already fired for i>=1 whenever a held lot
      // is unpriced across this step (book_greeks under-counts only when this row's
      // surface is absent, which also breaks the step's pnl_explain), so this check
      // is a consistent belt-and-braces here; it is the sole guard only at inception.
      if (cfg.unpriced == UnpricedLotPolicy::Error && g->n_unpriced > 0) {
        return Err(ErrorCode::NotFound, unpriced_greeks_error_message(
                                            g->n_unpriced, g->first_unpriced_uid, refs[i].date));
      }
      const double g_delta = g->total.delta + shares_sum();
      push_row(refs[i].date, base->ts_ns(), step_total, t.pnl_delta, t.pnl_gamma, t.pnl_vega,
               t.pnl_vanna, t.pnl_volga, t.pnl_theta, t.pnl_rho, t.pnl_charm, t.pnl_unexplained,
               settlement, shares_pnl, financing, ex->cost, nav, cash, g_delta, g->total,
               ex->turnover_notional, ex->turnover_vega, book.lots.size(),
               static_cast<double>(step->n_unpriced), static_cast<double>(g->n_unpriced));
      record_signals(*base);
    }
  }

  return Ok(std::move(out));
}

} // namespace atx::vol
