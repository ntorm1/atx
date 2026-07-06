// atx-vol backtest engine (Phase B0) — see backtest.hpp for the model.

#include "atx/vol/backtest.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/strategy.hpp"         // IStrategy
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

// One priced step base -> shifted over `lots`: partition expiring lots (settled at
// intrinsic: qty*mult*(intrinsic(S_shifted) - base_mark)) from survivors, then
// Taylor PnL-explain the survivors. Byte-identical arithmetic to the fixed-book
// loop above; shared by the strategy overload.
struct StepPnl {
  PnlTotals totals{};
  double settlement{0.0};
};

[[nodiscard]] Result<StepPnl> compute_step(const MarketSnapshot& base, const MarketSnapshot& shifted,
                                           const std::vector<Lot>& lots, const PriceOptions& opts) {
  std::vector<Position> alive;
  alive.reserve(lots.size());
  double settlement = 0.0;
  for (const Lot& lot : lots) {
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

  PnlTotals t{};
  if (!alive.empty()) {
    auto pf = Portfolio::create(alive);
    if (!pf) {
      return Err(pf.error());
    }
    const PortfolioPricer pricer(std::move(*pf));
    auto fr = pricer.pnl_explain(base.set(), shifted.set(), opts);
    if (!fr) {
      return Err(fr.error());
    }
    t = fr->total;
  }
  return Ok(StepPnl{t, settlement});
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

// One step's realized trade accounting (frictions + turnover) computed by the
// engine executor over the book diff and the hedge overlay.
struct ExecResult {
  double cost{0.0};
  double turnover_notional{0.0};
  double turnover_vega{0.0};
};

Result<BacktestResult> run_backtest(const Clock& clock, IStrategy& strat, const RunConfig& cfg) {
  const std::span<const SnapshotRef> refs = clock.refs();
  if (refs.empty()) {
    return Err(ErrorCode::InvalidArgument, "run_backtest: empty clock");
  }
  const std::size_t stride = (cfg.record_every_n == 0) ? std::size_t{1} : cfg.record_every_n;

  BacktestResult out;
  PortfolioState book{};
  std::uint64_t next_id = 1;  // monotonic lot ids the strategy consumes

  // ── Engine-internal cash + per-uid share ledger (B2) ──────────────────────
  double cash = cfg.financing.initial_cash;
  std::vector<std::pair<std::uint32_t, double>> shares;  // per-uid delta-hedge share count
  const auto shares_get = [&shares](std::uint32_t uid) -> double {
    for (const auto& kv : shares) {
      if (kv.first == uid) {
        return kv.second;
      }
    }
    return 0.0;
  };
  const auto shares_add = [&shares](std::uint32_t uid, double dn) {
    for (auto& kv : shares) {
      if (kv.first == uid) {
        kv.second += dn;
        return;
      }
    }
    shares.push_back({uid, dn});
  };
  const auto shares_sum = [&shares]() -> double {
    double s = 0.0;
    for (const auto& kv : shares) {
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

  const auto push_row = [&out](const std::string& date, std::int64_t ts, double p_total,
                               double p_delta, double p_gamma, double p_vega, double p_vanna,
                               double p_volga, double p_theta, double p_rho, double p_charm,
                               double p_unexpl, double p_settle, double p_shares, double p_fin,
                               double p_cost, double nav_v, double cash_v, double g_delta,
                               const PriceTotals& g, double turn_notl, double turn_vega,
                               std::size_t n_lots) {
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
    out.gross_delta.push_back(g_delta);  // NET book delta = option delta + hedge shares
    out.gross_gamma.push_back(g.gamma);
    out.gross_vega.push_back(g.vega);
    out.gross_theta.push_back(g.theta);
    out.turnover_notional.push_back(turn_notl);
    out.turnover_vega.push_back(turn_vega);
    out.n_open_lots.push_back(static_cast<double>(n_lots));
  };

  // Signal series: names captured on the first recorded row, then one value per
  // recorded row per series (NaN when a name is absent that row).
  bool sig_init = false;
  const auto record_signals = [&out, &strat, &sig_init](const MarketSnapshot& snap) {
    const std::vector<std::pair<std::string, double>> s = strat.signals(snap);
    if (!sig_init) {
      for (const auto& kv : s) {
        out.signals.emplace_back(kv.first, std::vector<double>{});
      }
      sig_init = true;
    }
    for (auto& series : out.signals) {
      double v = std::numeric_limits<double>::quiet_NaN();
      for (const auto& kv : s) {
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
  const auto execute = [&](const MarketSnapshot& base_snap,
                           const std::vector<Lot>& before_lots) -> Result<ExecResult> {
    ExecResult ex;
    bool entry_happened = false;

    const auto in_before = [&before_lots](std::uint64_t id) {
      for (const Lot& l : before_lots) {
        if (l.id == id) {
          return true;
        }
      }
      return false;
    };
    const auto in_book = [&book](std::uint64_t id) {
      for (const Lot& l : book.lots) {
        if (l.id == id) {
          return true;
        }
      }
      return false;
    };

    // Entry trades: lots present now but absent from `before_lots`.
    for (const Lot& lot : book.lots) {
      if (in_before(lot.id)) {
        continue;
      }
      entry_happened = true;
      const double T_res = residual_T(lot.expiry_ts_ns, base_snap.ts_ns());
      const PricedSurface* s = base_snap.find(lot.contract.uid);
      double vega = 0.0;
      if (s != nullptr) {
        const Result<AmericanGreeks> gr = s->greeks(lot.contract.K, T_res, lot.contract.side);
        if (gr) {
          vega = gr->vega;
        }
      }
      const double mark = lot.entry_price;  // entry_mark (fill at mid)
      const double hs = half_spread(mark, vega);
      const double leg_cost = std::fabs(lot.qty) * lot.multiplier * hs +
                              cfg.frictions.per_contract_cost * std::fabs(lot.qty);
      ex.cost += leg_cost;
      cash -= lot.qty * lot.multiplier * mark;  // premium paid (long) / received (short)
      ex.turnover_notional += std::fabs(lot.qty * lot.multiplier * mark);
      ex.turnover_vega += std::fabs(lot.qty * lot.multiplier * vega);
    }

    // Roll-close trades: lots in `before_lots` gone now (expiries were already
    // settled + erased before on_step, so these are strategy-driven closes).
    for (const Lot& lot : before_lots) {
      if (in_book(lot.id)) {
        continue;
      }
      const double T_res = residual_T(lot.expiry_ts_ns, base_snap.ts_ns());
      const PricedSurface* s = base_snap.find(lot.contract.uid);
      double mark = 0.0;
      double vega = 0.0;
      if (s != nullptr) {
        const Result<double> m = s->fair_value(lot.contract.K, T_res, lot.contract.side);
        if (!m) {
          return Err(m.error());
        }
        mark = *m;
        const Result<AmericanGreeks> gr = s->greeks(lot.contract.K, T_res, lot.contract.side);
        if (gr) {
          vega = gr->vega;
        }
      }
      const double hs = half_spread(mark, vega);
      const double leg_cost = std::fabs(lot.qty) * lot.multiplier * hs +
                              cfg.frictions.per_contract_cost * std::fabs(lot.qty);
      ex.cost += leg_cost;
      cash += lot.qty * lot.multiplier * mark;  // proceeds from closing
      ex.turnover_notional += std::fabs(lot.qty * lot.multiplier * mark);
      ex.turnover_vega += std::fabs(lot.qty * lot.multiplier * vega);
    }

    // Hedge overlay: drive per-uid net book delta (option + shares) into [-band,band].
    const HedgeSpec spec = strat.hedge_spec();
    if (spec.kind == HedgeSpec::Kind::DeltaToZero) {
      const bool fires = (spec.cadence == HedgeSpec::Cadence::Daily) ||
                         (spec.cadence == HedgeSpec::Cadence::AtEntry && entry_happened);
      if (fires) {
        std::vector<std::uint32_t> uids;
        const auto add_uid = [&uids](std::uint32_t u) {
          for (std::uint32_t x : uids) {
            if (x == u) {
              return;
            }
          }
          uids.push_back(u);
        };
        for (const Lot& l : book.lots) {
          add_uid(l.contract.uid);
        }
        for (const auto& kv : shares) {
          add_uid(kv.first);
        }
        for (const std::uint32_t uid : uids) {
          // Option net delta for this uid = PriceTotals.delta of its lots on base.
          double opt_delta = 0.0;
          std::vector<Position> ps;
          for (const Lot& l : book.lots) {
            if (l.contract.uid != uid) {
              continue;
            }
            const double T_res = residual_T(l.expiry_ts_ns, base_snap.ts_ns());
            ps.push_back(Position{l.id, OptionContract{uid, l.contract.K, T_res, l.contract.side},
                                  l.qty, l.multiplier});
          }
          if (!ps.empty()) {
            auto pf = Portfolio::create(ps);
            if (!pf) {
              return Err(pf.error());
            }
            const PortfolioPricer pricer(std::move(*pf));
            auto fr = pricer.price(base_snap.set(), cfg.price);
            if (!fr) {
              return Err(fr.error());
            }
            opt_delta = fr->total.delta;
          }
          const double sh = shares_get(uid);
          const double net = opt_delta + sh;
          if (std::fabs(net) > spec.band) {
            const double dn = -net;  // shares to trade (delta 1 per share)
            const PricedSurface* s = base_snap.find(uid);
            const double S = (s != nullptr) ? s->pricing().S : 0.0;
            ex.cost += std::fabs(dn) * S * (cfg.frictions.hedge_slippage_bps / 1.0e4);
            cash -= dn * S;  // buying shares costs cash (mid); slippage hits cost below
            shares_add(uid, dn);
          }
        }
      }
    }

    cash -= ex.cost;  // realized frictions hit cash at fill
    return Ok(ex);
  };

  auto base_res = MarketSnapshot::load(refs[0].archive_path);
  if (!base_res) {
    return Err(base_res.error());
  }
  MarketSnapshot base = std::move(*base_res);

  double nav = 0.0;

  // Inception (row 0): open positions AS OF refs[0], book entry frictions + premium
  // + the opening hedge into cash; PnL columns are zero; record post-trade cash.
  {
    Status st = strat.on_step(base, 0, book, next_id);
    if (!st) {
      return Err(st.error());
    }
    const std::vector<Lot> before_lots;  // empty ⇒ every opened lot is a fresh entry
    auto ex = execute(base, before_lots);
    if (!ex) {
      return Err(ex.error());
    }
    auto g = book_greeks(base, book.lots, cfg.price);
    if (!g) {
      return Err(g.error());
    }
    const double g_delta = g->delta + shares_sum();
    push_row(refs[0].date, base.ts_ns(), 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
             0.0, ex->cost, 0.0, cash, g_delta, *g, ex->turnover_notional, ex->turnover_vega,
             book.lots.size());
    record_signals(base);
  }

  for (std::size_t i = 1; i < refs.size(); ++i) {
    auto shifted_res = MarketSnapshot::load(refs[i].archive_path);
    if (!shifted_res) {
      return Err(shifted_res.error());
    }
    MarketSnapshot shifted = std::move(*shifted_res);

    // 1. PnL of the current book (resolved on base) forward to shifted (unchanged B1).
    auto step = compute_step(base, shifted, book.lots, cfg.price);
    if (!step) {
      return Err(step.error());
    }
    const PnlTotals& t = step->totals;
    const double settlement = step->settlement;

    // 2. Shares PnL + financing over the step, from the ledger held over the step.
    const double dt =
        (static_cast<double>(shifted.ts_ns()) - static_cast<double>(base.ts_ns())) / kNsPerYear;
    double shares_pnl = 0.0;
    double financing = 0.0;
    if (cfg.financing.finance_premium) {
      const double r = base.surfaces().front().pricing().r;  // base-date rate
      const double growth = std::exp(r * dt);
      financing += cash * (growth - 1.0);  // cash carry on the pre-step balance
      cash *= growth;                      // apply to the ledger
    }
    for (const auto& [uid, n] : shares) {
      const PricedSurface* bs = base.find(uid);
      const PricedSurface* ss = shifted.find(uid);
      if (bs == nullptr || ss == nullptr) {
        continue;
      }
      const double Sb = bs->pricing().S;
      shares_pnl += n * (ss->pricing().S - Sb);          // shares held over the step
      const double short_amt = std::max(0.0, -n);        // |min(shares,0)|
      financing += -cfg.financing.borrow_rate * short_amt * Sb * dt;  // borrow (0 when rate 0)
      if (cfg.financing.shares_carry) {
        financing += n * (bs->q_eff_at(0.25) - bs->pricing().r) * Sb * dt;  // long div, pay finance
      }
    }

    // 3. Adopt shifted as the next base; settle expiries into cash; drop them.
    base = std::move(shifted);
    for (const Lot& lot : book.lots) {
      if (lot.expiry_ts_ns > base.ts_ns()) {
        continue;
      }
      const PricedSurface* bs = base.find(lot.contract.uid);
      if (bs == nullptr) {
        continue;
      }
      const double S = bs->pricing().S;
      const double K = lot.contract.K;
      const double intrinsic =
          (lot.contract.side == Side::Call) ? std::max(0.0, S - K) : std::max(0.0, K - S);
      cash += lot.qty * lot.multiplier * intrinsic;  // intrinsic settle proceeds
    }
    std::erase_if(book.lots, [&base](const Lot& l) { return l.expiry_ts_ns <= base.ts_ns(); });

    // 4-5. Strategy entries/rolls + hedge overlay on the new base.
    const std::vector<Lot> before_lots = book.lots;  // survivors before on_step
    Status st = strat.on_step(base, i, book, next_id);
    if (!st) {
      return Err(st.error());
    }
    auto ex = execute(base, before_lots);
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
      auto g = book_greeks(base, book.lots, cfg.price);
      if (!g) {
        return Err(g.error());
      }
      const double g_delta = g->delta + shares_sum();
      push_row(refs[i].date, base.ts_ns(), step_total, t.pnl_delta, t.pnl_gamma, t.pnl_vega,
               t.pnl_vanna, t.pnl_volga, t.pnl_theta, t.pnl_rho, t.pnl_charm, t.pnl_unexplained,
               settlement, shares_pnl, financing, ex->cost, nav, cash, g_delta, *g,
               ex->turnover_notional, ex->turnover_vega, book.lots.size());
      record_signals(base);
    }
  }

  return Ok(std::move(out));
}

}  // namespace atx::vol
