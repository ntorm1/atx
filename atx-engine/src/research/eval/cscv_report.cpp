#include "atx/engine/research/eval/cscv_report.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include "atx/engine/eval/pbo.hpp"
#include "atx/engine/research/eval/house_moments.hpp"
#include "atx/engine/research/numpy_reduce.hpp"

namespace atx::engine::research::eval {

core::Result<CscvReport> cscv_report(std::span<const f64> matrix, usize sessions,
                                     usize n_candidates, usize blocks) {
  const usize n = n_candidates;
  if (n < 2U || matrix.size() != sessions * n) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "cscv_pbo: need a (T, N) matrix with N >= 2");
  }
  if (blocks < 2U || blocks % 2U != 0U || blocks > sessions) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "cscv_pbo: blocks must be even, >= 2 and <= T");
  }
  // Ruling B2-R2: a candidate whose used returns are all equal has Sharpe 0 in every split, the
  // Python's rule (_subset_sr: mean / sd where the variance is > 0, else 0). Its centred block
  // sums there are exact small multiples of one rounding residue, so the Python variance is
  // exactly 0 -- the same condition, no tolerance. The engine CSCV would divide by the reference
  // gather's rounding residue instead, so such a column goes to it as zeros, whose Sharpe is
  // exactly 0 by the engine's own zero-std rule.
  const usize used = (sessions / blocks) * blocks;
  std::vector<usize> constant;
  for (usize c = 0; c < n; ++c) {
    bool same = true;
    for (usize t = 1; t < used && same; ++t) {
      same = matrix[t * n + c] == matrix[c];
    }
    if (same && std::isfinite(matrix[c])) {
      constant.push_back(c);
    }
  }
  // Candidate-major for the engine (M[c * T + t]).
  std::vector<f64> perf(matrix.size());
  for (usize t = 0; t < sessions; ++t) {
    for (usize c = 0; c < n; ++c) {
      perf[c * sessions + t] = matrix[t * n + c];
    }
  }
  for (const usize c : constant) {
    std::fill_n(perf.begin() + static_cast<std::ptrdiff_t>(c * sessions), sessions, 0.0);
  }
  std::vector<atx::engine::eval::PboSplitTrace> trace;
  ATX_TRY(atx::engine::eval::PboResult res,
          atx::engine::eval::pbo_cscv_traced(perf, n, blocks,
                                             atx::engine::eval::PboRule::CachedMomentsV2, trace));
  const usize splits = res.split_logits.size();
  if (splits == 0U || trace.size() != splits) {
    return core::Err(core::ErrorCode::Internal, "cscv_pbo: the engine returned no split");
  }

  CscvReport out;
  out.pbo = res.pbo;
  out.n_candidates = n;
  out.sessions = sessions;
  out.blocks = blocks;
  out.block_width = sessions / blocks;
  out.dropped_tail_sessions = sessions - out.block_width * blocks;
  out.splits = splits;
  out.splits_total = splits;
  out.logits = std::move(res.split_logits);
  out.cached_evaluations = res.cached_evaluations;
  out.reference_evaluations = res.reference_evaluations;
  out.ambiguous_comparisons = res.ambiguous_comparisons;
  out.constant_candidates = std::move(constant);
  out.logit_mean = numpy::mean(out.logits);

  std::vector<f64> sorted = out.logits;
  std::sort(sorted.begin(), sorted.end());
  for (usize q = 0; q < kLogitQuantiles.size(); ++q) {
    out.logit_quantiles[q] = numpy::quantile_linear(sorted, kLogitQuantiles[q]);
  }

  out.rank_logits.resize(n);
  out.rank_splits.assign(n, 0U);
  out.winner_counts.assign(n, 0U);
  for (usize k = 0; k < n; ++k) {
    out.rank_logits[k] = std::log((static_cast<f64>(k) + 1.0) / static_cast<f64>(n - k));
  }
  std::vector<f64> is_sr(splits);
  std::vector<f64> oos_sr(splits);
  usize losses = 0;
  for (usize s = 0; s < splits; ++s) {
    const atx::engine::eval::PboSplitTrace &step = trace[s];
    if (step.winner >= n || step.rank >= n) {
      return core::Err(core::ErrorCode::Internal, "cscv_pbo: split trace out of range");
    }
    ++out.winner_counts[step.winner];
    ++out.rank_splits[step.rank];
    is_sr[s] = step.is_sharpe;
    oos_sr[s] = step.oos_sharpe;
    losses += step.oos_sharpe < 0.0 ? 1U : 0U;
  }
  const f64 root = std::sqrt(kAnnualization);
  out.winner_is_sr_annual_mean = numpy::mean(is_sr) * root;
  out.winner_oos_sr_annual_mean = numpy::mean(oos_sr) * root;
  out.prob_winner_oos_loss = static_cast<f64>(losses) / static_cast<f64>(splits);

  // np.cov(iw, ow, ddof=1)[0, 1] / np.var(iw, ddof=1), when there is more than one split and
  // the IS series is not constant.
  if (splits > 1U && numpy::var(is_sr, 0U) > 0.0) {
    const f64 mi = numpy::mean(is_sr);
    const f64 mo = numpy::mean(oos_sr);
    std::vector<f64> cross(splits);
    for (usize s = 0; s < splits; ++s) {
      cross[s] = (is_sr[s] - mi) * (oos_sr[s] - mo);
    }
    const f64 cov = numpy::pairwise_sum(cross) / static_cast<f64>(splits - 1U);
    out.degradation_slope = cov / numpy::var(is_sr, 1U);
  }
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::eval
