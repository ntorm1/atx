// THEO-3: breakeven-vol root-find — bounded bisection layered over
// `bev_replay_pnl` (breakeven.hpp / implemented at the end of backtest.cpp).
// This file does not touch the replay implementation; it only calls the
// public `bev_replay_pnl` entry point.

#include "atx/vol/breakeven.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "atx/vol/detail/parallel_for.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// JPL Rule 2: bounds the bisection loop in `solve_breakeven_vol` below to a
// statically-visible cap, independent of `sigma_tol` or path length.
constexpr std::uint8_t kBevMaxSolveIter = 40;

} // namespace

Result<BevLabel> solve_breakeven_vol(std::span<const BevDayState> path, const BevSpec &spec,
                                     std::span<const DividendEvent> dividends,
                                     const BevSolveConfig &cfg) {
  if (!(cfg.sigma_lo > 0.0) || !(cfg.sigma_hi > 0.0) || !(cfg.sigma_lo < cfg.sigma_hi) ||
      !(cfg.sigma_tol > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "solve_breakeven_vol: sigma_lo/sigma_hi/sigma_tol out of range");
  }

  ATX_TRY(const BevReplayResult r_lo,
          bev_replay_pnl(path, spec, cfg.sigma_lo, dividends, cfg.replay));
  ATX_TRY(const BevReplayResult r_hi,
          bev_replay_pnl(path, spec, cfg.sigma_hi, dividends, cfg.replay));
  std::uint8_t iters = 2;

  const auto label_from = [&iters](double sigma, const BevReplayResult &r, BevFlag flag) {
    return BevLabel{
        .sigma_be = sigma,
        .premium_at_be = r.premium,
        .vega_at_be = r.vega_entry,
        .pnl_residual = r.pnl,
        .n_days = r.n_days,
        .iters = iters,
        .flag = flag,
    };
  };

  // PnL(sigma) is monotone decreasing (PnlIsMonotoneDecreasingInEntrySigma):
  // a genuine bracket needs a STRICT sign change, pnl(sigma_lo) > 0 >
  // pnl(sigma_hi). A boundary pnl of exactly 0.0 is not treated as "already
  // the root": for a far-enough OTM wing at sigma_lo, the American price and
  // delta both underflow to exactly 0.0, so the replay never trades and pnl
  // is an exact 0.0 with no real gradient/vega information behind it --
  // that is wing ill-conditioning, not a resolved root. Anything failing the
  // strict bracket is reported as DATA via `flag`, not a solver defect, and
  // the batch layer filters on it.
  if (!(r_lo.pnl > 0.0 && r_hi.pnl < 0.0)) {
    const bool lo_closer = std::fabs(r_lo.pnl) <= std::fabs(r_hi.pnl);
    return Ok(lo_closer ? label_from(cfg.sigma_lo, r_lo, BevFlag::NoBracket)
                        : label_from(cfg.sigma_hi, r_hi, BevFlag::NoBracket));
  }

  double lo = cfg.sigma_lo;
  double hi = cfg.sigma_hi;
  double mid = lo;
  BevReplayResult r_mid = r_lo;
  bool converged = false;
  // JPL Rule 2: bounded by kBevMaxSolveIter; each step is one more
  // bev_replay_pnl evaluation, counted into `iters`.
  for (std::uint8_t i = 0; i < kBevMaxSolveIter; ++i) {
    mid = 0.5 * (lo + hi);
    ATX_TRY(r_mid, bev_replay_pnl(path, spec, mid, dividends, cfg.replay));
    ++iters;
    if (r_mid.pnl >= 0.0) {
      lo = mid;
    } else {
      hi = mid;
    }
    if (hi - lo < cfg.sigma_tol) {
      converged = true;
      break;
    }
  }

  const BevFlag flag = !converged              ? BevFlag::MaxIter
                       : r_mid.exercised_early ? BevFlag::ExercisedEarly
                                               : BevFlag::Ok;
  return Ok(label_from(mid, r_mid, flag));
}

// THEO-4: deterministic parallel fan-out of solve_breakeven_vol over `jobs`.
// This function does not touch the solver or the replay; it only calls the
// public solve_breakeven_vol entry point once per job, from inside
// `parallel_for`'s disjoint-write worker body.
Result<BevLabelFrame> solve_breakeven_batch(std::span<const BevJob> jobs, const BevSolveConfig &cfg,
                                            unsigned n_threads) {
  // `cfg` is shared by every job, so an invalid cfg is a batch-wide
  // configuration error (mirrors solve_breakeven_vol's own upfront
  // validation) rather than a per-job rejection: fail closed before touching
  // the frame or spawning any worker.
  if (!(cfg.sigma_lo > 0.0) || !(cfg.sigma_hi > 0.0) || !(cfg.sigma_lo < cfg.sigma_hi) ||
      !(cfg.sigma_tol > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "solve_breakeven_batch: sigma_lo/sigma_hi/sigma_tol out of range");
  }

  const std::size_t n = jobs.size();
  BevLabelFrame frame;
  // All frame vectors sized up front, before the parallel region: workers
  // below write existing slots by index only (no push_back/resize inside the
  // parallel region), which is what makes the disjoint-write contract hold.
  frame.sigma_be.resize(n);
  frame.premium_at_be.resize(n);
  frame.vega_at_be.resize(n);
  frame.pnl_residual.resize(n);
  frame.n_days.resize(n);
  frame.iters.resize(n);
  frame.flag.resize(n);
  frame.status_ok.resize(n);

  // Contiguous block partition (parallel_for's contract, detail/
  // parallel_for.hpp): worker j only ever touches slot j of each vector
  // above, after pure reads of jobs[j]/cfg, so the frame is bit-identical for
  // any n_threads.
  parallel_for(n, n_threads, [&](std::size_t j) {
    const BevJob &job = jobs[j];
    const Result<BevLabel> lab = solve_breakeven_vol(job.path, job.spec, job.dividends, cfg);
    if (lab.has_value()) {
      frame.sigma_be[j] = lab->sigma_be;
      frame.premium_at_be[j] = lab->premium_at_be;
      frame.vega_at_be[j] = lab->vega_at_be;
      frame.pnl_residual[j] = lab->pnl_residual;
      frame.n_days[j] = lab->n_days;
      frame.iters[j] = lab->iters;
      frame.flag[j] = static_cast<std::uint8_t>(lab->flag);
      frame.status_ok[j] = 1u;
      return;
    }
    // Per-job rejection (bad path/spec/etc.): neutral values, does not sink
    // the batch -- BevLabelFrame's contract comment documents the
    // flag/status_ok discriminator neighbors rely on.
    frame.sigma_be[j] = 0.0;
    frame.premium_at_be[j] = 0.0;
    frame.vega_at_be[j] = 0.0;
    frame.pnl_residual[j] = 0.0;
    frame.n_days[j] = 0u;
    frame.iters[j] = 0u;
    frame.flag[j] = 0u;
    frame.status_ok[j] = 0u;
  });

  return Ok(std::move(frame));
}

// Task 5: surface-corpus path loader. Walks `clock`'s sessions ascending,
// resolving `uid` against each loaded `MarketSnapshot`'s own SurfaceSet, and
// assembles the per-day carry (S, r, q_eff) `bev_replay_pnl` / the solvers on
// top of THEO-2/3/4 need. Does not touch the replay or solver; it only
// produces their `path` input.
Result<BevPath> load_bev_path(const Clock &clock, std::string_view uid, std::int64_t entry_ts_ns,
                              std::int64_t expiry_ns, double tenor_probe_years,
                              BevExpirySnap snap) {
  if (!(entry_ts_ns < expiry_ns)) {
    return Err(ErrorCode::InvalidArgument,
               "load_bev_path: entry_ts_ns must be strictly before expiry_ns");
  }
  if (!(tenor_probe_years > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "load_bev_path: tenor_probe_years must be positive");
  }

  std::vector<BevDayState> days;
  bool found_entry = false;
  // True once a session observes expiry_ns EXACTLY -- that session is always
  // the terminal one for either snap mode, and it is never "snapped".
  bool found_exact_settle = false;
  std::int64_t settle_ts_ns = 0;
  bool snapped = false;

  // JPL Rule 2: bounded by clock.refs().size(), the corpus timeline's own
  // (already-materialized) length. Breaks early once a session strictly past
  // expiry_ns is seen -- refs are ascending, so nothing further can qualify.
  for (const SnapshotRef &ref : clock.refs()) {
    ATX_TRY(const MarketSnapshot session, MarketSnapshot::load(ref.archive_path));
    const std::int64_t ts = session.ts_ns();

    if (ts < entry_ts_ns) {
      continue; // strictly before the requested window
    }
    if (!found_entry) {
      if (ts != entry_ts_ns) {
        return Err(ErrorCode::InvalidArgument,
                   "load_bev_path: entry_ts_ns " + std::to_string(entry_ts_ns) +
                       " is not a clock observation (nearest at-or-after is " + std::to_string(ts) +
                       ")");
      }
      found_entry = true;
    }
    if (ts > expiry_ns) {
      break; // past the requested window; nothing further can qualify
    }
    if (!days.empty() && ts <= days.back().ts_ns) {
      return Err(ErrorCode::InvalidArgument,
                 "load_bev_path: clock observations are not strictly increasing at ts_ns=" +
                     std::to_string(ts));
    }

    const std::optional<std::uint32_t> resolved_uid = session.uid_of(uid);
    const SurfaceRef surf = resolved_uid.has_value() ? session.find(*resolved_uid) : SurfaceRef{};
    if (surf == nullptr) {
      return Err(ErrorCode::NotFound, "load_bev_path: no surface for uid '" + std::string(uid) +
                                          "' at ts_ns=" + std::to_string(ts));
    }

    // Per-day carry: remaining tenor from THIS session to the REQUESTED
    // expiry_ns (not the possibly-snapped settle), ACT/365.25 -- the "carry
    // errors masquerade as skew" guard, re-probed every session rather than
    // once at entry.
    const double t_rem = static_cast<double>(expiry_ns - ts) / kNsPerYear;
    const double q_eff = surf.q_eff_at(std::fmax(t_rem, tenor_probe_years));
    days.push_back(BevDayState{ts, surf.pricing().S, surf.pricing().r, q_eff});

    if (ts == expiry_ns) {
      settle_ts_ns = ts;
      snapped = false;
      found_exact_settle = true;
      break;
    }
    settle_ts_ns = ts; // tentative LastSessionAtOrBefore candidate
    snapped = true;
  }

  if (!found_entry) {
    return Err(ErrorCode::InvalidArgument, "load_bev_path: entry_ts_ns " +
                                               std::to_string(entry_ts_ns) +
                                               " is not a clock observation");
  }
  // Exhaustive switch (no default): a future third BevExpirySnap enumerator
  // must add a case here or this turns into a compile error under /WX,
  // rather than silently falling into whichever branch happened to be "else".
  switch (snap) {
  case BevExpirySnap::Exact:
    if (!found_exact_settle) {
      return Err(ErrorCode::InvalidArgument,
                 "load_bev_path: expiry_ns " + std::to_string(expiry_ns) +
                     " is not a clock observation (Exact requires an exact match)");
    }
    break;
  case BevExpirySnap::LastSessionAtOrBefore:
    if (days.empty()) {
      return Err(ErrorCode::InvalidArgument, "load_bev_path: no session in [" +
                                                 std::to_string(entry_ts_ns) + ", " +
                                                 std::to_string(expiry_ns) + "] to snap to");
    }
    break;
  }
  if (days.size() < 2) {
    return Err(ErrorCode::InvalidArgument, "load_bev_path: assembled path has " +
                                               std::to_string(days.size()) +
                                               " session(s); need at least 2 (entry + settlement)");
  }

  return Ok(BevPath{std::move(days), settle_ts_ns, snapped});
}

} // namespace atx::vol
