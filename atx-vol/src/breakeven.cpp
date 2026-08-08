// THEO-3: breakeven-vol root-find — bounded bisection layered over
// `bev_replay_pnl` (breakeven.hpp / implemented at the end of backtest.cpp).
// This file does not touch the replay implementation; it only calls the
// public `bev_replay_pnl` entry point.

#include "atx/vol/breakeven.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
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

} // namespace atx::vol
