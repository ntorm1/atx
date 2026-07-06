// atx-vol backtest engine (Phase B0) — see backtest.hpp for the model.

#include "atx/vol/backtest.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/surface_archive.hpp"  // SurfaceArchive

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Process-wide archive-open counter (test seam). Loads increment it exactly once.
std::atomic<std::uint64_t> g_open_count{0};

// Contract residual T on the snapshot dated `base_ts`: (expiry - base.ts)/year.
[[nodiscard]] double residual_T(std::int64_t expiry_ts_ns, std::int64_t base_ts_ns) noexcept {
  return (static_cast<double>(expiry_ts_ns) - static_cast<double>(base_ts_ns)) / kNsPerYear;
}

// Materialize the current book as `Position`s priced at `base_ts`'s residual T.
[[nodiscard]] std::vector<Position> positions_at(const std::vector<Lot>& lots,
                                                 std::int64_t base_ts_ns) {
  std::vector<Position> out;
  out.reserve(lots.size());
  for (const Lot& lot : lots) {
    const double T = residual_T(lot.expiry_ts_ns, base_ts_ns);
    out.push_back(Position{lot.id,
                           OptionContract{lot.contract.uid, lot.contract.K, T, lot.contract.side},
                           lot.qty, lot.multiplier});
  }
  return out;
}

// Book greeks: price the current lots against `snap` at its residual T. An empty
// book yields zero totals (an empty portfolio prices to an empty frame).
[[nodiscard]] Result<PriceTotals> book_greeks(const MarketSnapshot& snap,
                                              const std::vector<Lot>& lots,
                                              const PriceOptions& opts) {
  const std::vector<Position> ps = positions_at(lots, snap.ts_ns());
  auto pf = Portfolio::create(ps);
  if (!pf) {
    return Err(pf.error());
  }
  const PortfolioPricer pricer(std::move(*pf));
  auto fr = pricer.price(snap.set(), opts);
  if (!fr) {
    return Err(fr.error());
  }
  return Ok(fr->total);
}

}  // namespace

// ── Clock ─────────────────────────────────────────────────────────────────

Result<Clock> Clock::from_manifest(const CorpusManifest& manifest) {
  Clock clock;
  // `manifest.dates` are unique + ascending; `entries` are sorted (date asc,
  // symbol asc) so the first Ok entry per date is deterministic.
  for (const std::string& d : manifest.dates) {
    for (const CorpusEntry& e : manifest.entries) {
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

// ── MarketSnapshot ──────────────────────────────────────────────────────────

MarketSnapshot::MarketSnapshot(std::vector<PricedSurface>&& surfaces, SurfaceSet&& set,
                               std::int64_t ts,
                               std::vector<std::pair<std::string, std::uint32_t>>&& syms) noexcept
    : surfaces_{std::move(surfaces)}, set_{std::move(set)}, ts_ns_{ts}, syms_{std::move(syms)} {}

std::uint64_t MarketSnapshot::open_count() noexcept { return g_open_count.load(); }
void MarketSnapshot::reset_open_count() noexcept { g_open_count.store(0); }

Result<MarketSnapshot> MarketSnapshot::load(std::string_view archive_path) {
  auto arch = SurfaceArchive::open_file(archive_path);
  if (!arch) {
    return Err(arch.error());
  }
  // One archive-open event (the load-once gate asserts N loads => N opens).
  g_open_count.fetch_add(1, std::memory_order_relaxed);

  auto mapped = arch->map_all();
  if (!mapped) {
    return Err(mapped.error());
  }
  std::vector<PricedSurface> surfaces = std::move(*mapped);
  if (surfaces.empty()) {
    return Err(ErrorCode::InvalidArgument, "MarketSnapshot::load: archive holds no surfaces");
  }

  // Valuation timestamp: the surfaces of one date agree on now_ts_ns.
  const std::int64_t ts = surfaces.front().pricing().now_ts_ns;
  for (const PricedSurface& s : surfaces) {
    if (s.pricing().now_ts_ns != ts) {
      return Err(ErrorCode::InvalidArgument,
                 "MarketSnapshot::load: surfaces disagree on now_ts_ns within a date");
    }
  }

  // Non-owning resolver over the owned surfaces' stable addresses.
  std::vector<const PricedSurface*> ptrs;
  ptrs.reserve(surfaces.size());
  for (const PricedSurface& s : surfaces) {
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
  for (const ArchiveDirEntry& e : dir) {
    syms.emplace_back(std::string(e.symbol, e.symbol_len), e.uid);
  }

  return MarketSnapshot{std::move(surfaces), std::move(*set), ts, std::move(syms)};
}

std::optional<std::uint32_t> MarketSnapshot::uid_of(std::string_view symbol) const {
  for (const auto& [sym, uid] : syms_) {
    if (sym == symbol) {
      return uid;
    }
  }
  return std::nullopt;
}

// ── Driver ──────────────────────────────────────────────────────────────────

Result<BacktestResult> run_backtest(const Clock& clock, PortfolioState initial,
                                    const RunConfig& cfg) {
  const std::span<const SnapshotRef> refs = clock.refs();
  if (refs.empty()) {
    return Err(ErrorCode::InvalidArgument, "run_backtest: empty clock");
  }
  const std::size_t stride = (cfg.record_every_n == 0) ? std::size_t{1} : cfg.record_every_n;

  BacktestResult out;
  PortfolioState book = std::move(initial);

  // Append one row from fully-computed step totals.
  const auto push_row = [&out](const std::string& date, std::int64_t ts, double p_total,
                               double p_delta, double p_gamma, double p_vega, double p_vanna,
                               double p_volga, double p_theta, double p_rho, double p_charm,
                               double p_unexpl, double p_settle, double nav_v,
                               const PriceTotals& g, std::size_t n_lots) {
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
    out.nav.push_back(nav_v);
    out.gross_delta.push_back(g.delta);
    out.gross_gamma.push_back(g.gamma);
    out.gross_vega.push_back(g.vega);
    out.gross_theta.push_back(g.theta);
    out.n_open_lots.push_back(static_cast<double>(n_lots));
  };

  // base = load(refs[0]) — the inception snapshot.
  auto base_res = MarketSnapshot::load(refs[0].archive_path);
  if (!base_res) {
    return Err(base_res.error());
  }
  MarketSnapshot base = std::move(*base_res);

  double nav = 0.0;

  // Row 0: inception (zero PnL, nav 0, book greeks on the first date).
  {
    auto g = book_greeks(base, book.lots, cfg.price);
    if (!g) {
      return Err(g.error());
    }
    push_row(refs[0].date, base.ts_ns(), 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
             0.0, *g, book.lots.size());
  }

  for (std::size_t i = 1; i < refs.size(); ++i) {
    auto shifted_res = MarketSnapshot::load(refs[i].archive_path);
    if (!shifted_res) {
      return Err(shifted_res.error());
    }
    MarketSnapshot shifted = std::move(*shifted_res);

    // Partition: expiring lots settle at intrinsic; the rest are Taylor-explained.
    std::vector<Position> alive;
    alive.reserve(book.lots.size());
    double settlement = 0.0;
    for (const Lot& lot : book.lots) {
      if (lot.expiry_ts_ns <= shifted.ts_ns()) {
        const double T_base = residual_T(lot.expiry_ts_ns, base.ts_ns());
        const PricedSurface* bs = base.find(lot.contract.uid);
        const PricedSurface* ss = shifted.find(lot.contract.uid);
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
        const double T_base = residual_T(lot.expiry_ts_ns, base.ts_ns());
        alive.push_back(
            Position{lot.id, OptionContract{lot.contract.uid, lot.contract.K, T_base, lot.contract.side},
                     lot.qty, lot.multiplier});
      }
    }

    // Taylor PnL-explain of the surviving book: base -> shifted (ages T by the ts gap).
    PnlTotals t{};
    if (!alive.empty()) {
      auto pf = Portfolio::create(alive);
      if (!pf) {
        return Err(pf.error());
      }
      const PortfolioPricer pricer(std::move(*pf));
      auto fr = pricer.pnl_explain(base.set(), shifted.set(), cfg.price);
      if (!fr) {
        return Err(fr.error());
      }
      t = fr->total;
    }

    const double step_total = t.pnl_total + settlement;
    nav += step_total;  // cumulative every step, regardless of recording

    // Adopt the shifted snapshot as the next base (no reload) and drop expiries.
    base = std::move(shifted);
    std::erase_if(book.lots,
                  [&base](const Lot& l) { return l.expiry_ts_ns <= base.ts_ns(); });

    const bool is_last = (i + 1 == refs.size());
    const bool record = ((i % stride) == 0) || is_last;
    if (record) {
      auto g = book_greeks(base, book.lots, cfg.price);
      if (!g) {
        return Err(g.error());
      }
      push_row(refs[i].date, base.ts_ns(), step_total, t.pnl_delta, t.pnl_gamma, t.pnl_vega,
               t.pnl_vanna, t.pnl_volga, t.pnl_theta, t.pnl_rho, t.pnl_charm, t.pnl_unexplained,
               settlement, nav, *g, book.lots.size());
    }
  }

  return Ok(std::move(out));
}

}  // namespace atx::vol
