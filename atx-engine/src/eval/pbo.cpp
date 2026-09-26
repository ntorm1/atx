#include "atx/engine/eval/pbo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/stats/algo.hpp"
#include "atx/engine/eval/stats_ext.hpp"

namespace atx::engine::eval {
namespace {
using atx::f64;
using atx::usize;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

// Center on each candidate's first used value. No subtraction of whole-sample
// totals to form OOS: both sides combine their selected blocks directly.
struct BlockMoments {
  f64 sum{0.0};
  f64 square{0.0};
  f64 max_abs{0.0};
};
struct Score {
  f64 value{0.0};
  f64 error{std::numeric_limits<f64>::infinity()};
  bool exact{false};
};
struct Scratch {
  std::vector<usize> oos;
  std::vector<f64> gather;
  std::vector<f64> legacy_oos;
  std::vector<usize> legacy_rank;
  std::vector<Score> is_score, oos_score;
};

void complement(std::span<const usize> selected, usize splits, std::span<usize> out) {
  std::array<bool, 16> included{};
  for (const auto block : selected) included[block] = true;
  usize next = 0;
  for (usize block = 0; block < splits; ++block) if (!included[block]) out[next++] = block;
}

[[nodiscard]] atx::core::Result<f64>
reference_sharpe(std::span<const f64> row, std::span<const usize> subset, usize width,
                  std::vector<f64>& gather, PboResult& result) {
  ++result.reference_evaluations;
  gather.clear();
  for (const auto block : subset)
    for (usize offset = 0; offset < width; ++offset) gather.push_back(row[block * width + offset]);
  // Frozen V1 order/formula, including its exact zero-std convention.
  const MeanStd moments = mean_std_pop(gather);
  const f64 sharpe = moments.std == 0.0 ? 0.0 : moments.mean / moments.std;
  if (!std::isfinite(sharpe))
    return Err(ErrorCode::InvalidArgument, "pbo_cscv: nonfinite reference Sharpe");
  return Ok(sharpe);
}

[[nodiscard]] f64 rank_logit(usize rank, usize candidates) noexcept {
  const f64 relative = (static_cast<f64>(rank) + 1.0) / static_cast<f64>(candidates + 1U);
  return std::log(relative / (1.0 - relative));
}

[[nodiscard]] atx::core::Result<f64>
legacy_split(std::span<const f64> perf, usize n, usize periods, usize width,
              std::span<const usize> selected, Scratch& scratch, PboResult& result) {
  usize winner = 0;
  f64 best = 0.0;
  for (usize c = 0; c < n; ++c) {
    const auto row = perf.subspan(c * periods, periods);
    ATX_TRY(const f64 in, reference_sharpe(row, selected, width, scratch.gather, result));
    ATX_TRY(const f64 out, reference_sharpe(row, scratch.oos, width, scratch.gather, result));
    scratch.legacy_oos[c] = out;
    if (c == 0U || in > best) { winner = c; best = in; }
  }
  atx::core::stats::partial_rank<f64>(scratch.legacy_oos, scratch.legacy_rank);
  return Ok(rank_logit(scratch.legacy_rank[winner], n));
}

[[nodiscard]] std::vector<BlockMoments>
cache_blocks(std::span<const f64> perf, usize n, usize periods, usize splits, usize width) {
  std::vector<BlockMoments> blocks(n * splits);
  for (usize c = 0; c < n; ++c) {
    const auto row = perf.subspan(c * periods, periods);
    const f64 anchor = row.front();
    for (usize block = 0; block < splits; ++block) {
      auto& moments = blocks[c * splits + block];
      for (usize offset = 0; offset < width; ++offset) {
        const f64 value = row[block * width + offset];
        const f64 centered = value - anchor;
        moments.sum += centered;
        moments.square += centered * centered;
        moments.max_abs = std::max(moments.max_abs, std::abs(value));
      }
    }
  }
  return blocks;
}

[[nodiscard]] Score cached_score(std::span<const BlockMoments> blocks,
                                  std::span<const usize> selected, usize observations,
                                  f64 anchor) noexcept {
  f64 sum = 0.0, square = 0.0, scale = std::abs(anchor);
  for (const auto block : selected) {
    sum += blocks[block].sum;
    square += blocks[block].square;
    scale = std::max(scale, blocks[block].max_abs);
  }
  const f64 count = static_cast<f64>(observations);
  const f64 centered_mean = sum / count;
  const f64 mean = anchor + centered_mean;
  const f64 variance = (square - centered_mean * sum) / count;
  if (!std::isfinite(mean) || !std::isfinite(variance) ||
      variance < std::numeric_limits<f64>::min()) return {};
  const f64 deviation = std::sqrt(variance);
  const f64 value = mean / deviation;

  // Conservative roundoff envelope covering both the grouped moments and the
  // reference's ordered mean/deviation reductions. Large common offsets, tiny
  // variance, overflow/underflow and cancellation force the reference path.
  // This is a versioned numerical guard, not a proof of universal bit identity.
  constexpr f64 epsilon = std::numeric_limits<f64>::epsilon();
  const f64 gamma = 32.0 * epsilon * count;
  const f64 mean_error = 4.0 * gamma * scale;
  const f64 variance_error = 4.0 * gamma * (square / count + centered_mean * centered_mean) +
                             mean_error * mean_error + 4.0 * mean_error * deviation;
  if (gamma >= 0.01 || !std::isfinite(value) || !std::isfinite(variance_error) ||
      // Relative-error arithmetic does not bound subnormal rounding. A normal
      // input scale alone does not exclude a subnormal variance/error envelope.
      variance_error < std::numeric_limits<f64>::min() ||
      variance <= 8.0 * variance_error ||
      scale > std::sqrt(std::numeric_limits<f64>::max() / count) * 0.25 ||
      (scale != 0.0 && scale < std::sqrt(std::numeric_limits<f64>::min()))) return {};
  const f64 error = mean_error / deviation + std::abs(value) * variance_error / variance +
                    16.0 * epsilon * (1.0 + std::abs(value));
  if (!std::isfinite(error)) return {};
  return Score{value, error, false};
}

[[nodiscard]] bool ambiguous(const Score& a, const Score& b) noexcept {
  return !(a.exact && b.exact) && std::abs(a.value - b.value) <= a.error + b.error;
}

[[nodiscard]] atx::core::Status resolve(Score& score, usize c, std::span<const f64> perf,
                                       usize periods, usize width, std::span<const usize> subset,
                                       Scratch& scratch, PboResult& result) {
  if (score.exact) return Ok();
  ATX_TRY(const f64 value, reference_sharpe(perf.subspan(c * periods, periods), subset,
                                          width, scratch.gather, result));
  score = Score{value, 0.0, true};
  return Ok();
}

[[nodiscard]] atx::core::Result<f64>
cached_split(std::span<const f64> perf, usize n, usize periods, usize splits, usize width,
              std::span<const usize> selected, std::span<const BlockMoments> blocks,
              Scratch& scratch, PboResult& result) {
  const usize observations = selected.size() * width;
  for (usize c = 0; c < n; ++c) {
    const auto row_blocks = blocks.subspan(c * splits, splits);
    scratch.is_score[c] = cached_score(row_blocks, selected, observations, perf[c * periods]);
    scratch.oos_score[c] = cached_score(row_blocks, scratch.oos, observations, perf[c * periods]);
    result.cached_evaluations += 2U;
    if (!std::isfinite(scratch.is_score[c].error))
      ATX_TRY_VOID(resolve(scratch.is_score[c], c, perf, periods, width, selected, scratch, result));
    if (!std::isfinite(scratch.oos_score[c].error))
      ATX_TRY_VOID(resolve(scratch.oos_score[c], c, perf, periods, width, scratch.oos, scratch, result));
  }
  usize winner = 0;
  for (usize c = 1; c < n; ++c) {
    if (ambiguous(scratch.is_score[c], scratch.is_score[winner])) {
      ++result.ambiguous_comparisons;
      ATX_TRY_VOID(resolve(scratch.is_score[c], c, perf, periods, width, selected, scratch, result));
      ATX_TRY_VOID(resolve(scratch.is_score[winner], winner, perf, periods, width,
                           selected, scratch, result));
    }
    if (scratch.is_score[c].value > scratch.is_score[winner].value) winner = c;
  }
  usize rank = 0;
  for (usize c = 0; c < n; ++c) {
    if (c == winner) continue;
    if (ambiguous(scratch.oos_score[c], scratch.oos_score[winner])) {
      ++result.ambiguous_comparisons;
      ATX_TRY_VOID(resolve(scratch.oos_score[c], c, perf, periods, width,
                           scratch.oos, scratch, result));
      ATX_TRY_VOID(resolve(scratch.oos_score[winner], winner, perf, periods, width,
                           scratch.oos, scratch, result));
    }
    const f64 candidate = scratch.oos_score[c].value, best = scratch.oos_score[winner].value;
    if (candidate < best || (candidate == best && c < winner)) ++rank;
  }
  return Ok(rank_logit(rank, n));
}
} // namespace

atx::core::Result<PboResult>
pbo_cscv_checked(std::span<const atx::f64> perf, atx::usize n_candidates,
                 atx::usize n_splits, PboRule rule) {
  const usize n = n_candidates, splits = n_splits;
  if (n < 2U) return Err(ErrorCode::InvalidArgument, "pbo_cscv: n_candidates must be >= 2");
  if (perf.size() % n != 0U)
    return Err(ErrorCode::InvalidArgument, "pbo_cscv: performance matrix must have equal candidate lengths");
  if (splits == 0U || (splits % 2U) != 0U)
    return Err(ErrorCode::InvalidArgument, "pbo_cscv: n_splits must be a positive even number");
  if (splits > 16U)
    return Err(ErrorCode::InvalidArgument, "pbo_cscv: exhaustive CSCV supports at most 16 splits");
  const usize periods = perf.size() / n;
  if (splits > periods)
    return Err(ErrorCode::InvalidArgument, "pbo_cscv: n_splits must not exceed T (periods)");
  if (rule != PboRule::LegacyGatherV1 && rule != PboRule::CachedMomentsV2)
    return Err(ErrorCode::InvalidArgument, "pbo_cscv: unsupported numerical rule");
  const usize half = splits / 2U, width = periods / splits;
  const usize split_count = detail::binomial(splits, half);
  if (n > std::numeric_limits<atx::u64>::max() / (2U * split_count) ||
      n > std::vector<BlockMoments>{}.max_size() / splits)
    return Err(ErrorCode::InvalidArgument, "pbo_cscv: cache/counter size overflow");
  // Matrix shape was validated; each row stride remains the original untrimmed T.
  for (usize c = 0; c < n; ++c)
    for (usize t = 0; t < width * splits; ++t)
      if (!std::isfinite(perf[c * periods + t]))
        return Err(ErrorCode::InvalidArgument, "pbo_cscv: nonfinite used return");

  Scratch scratch;
  scratch.oos.resize(half); scratch.gather.reserve(half * width);
  std::vector<BlockMoments> blocks;
  if (rule == PboRule::LegacyGatherV1) {
    scratch.legacy_oos.resize(n); scratch.legacy_rank.resize(n);
  } else {
    blocks = cache_blocks(perf, n, periods, splits, width);
    scratch.is_score.resize(n); scratch.oos_score.resize(n);
  }
  std::vector<usize> selected(half);
  for (usize i = 0; i < half; ++i) selected[i] = i;
  PboResult result{0.0, {}, 0.0}; result.rule = rule;
  result.split_logits.reserve(split_count);
  usize below_or_at = 0;
  f64 logit_sum = 0.0;
  do {
    complement(selected, splits, scratch.oos);
    auto evaluated = rule == PboRule::LegacyGatherV1
        ? legacy_split(perf, n, periods, width, selected, scratch, result)
        : cached_split(perf, n, periods, splits, width, selected, blocks, scratch, result);
    if (!evaluated) return Err(evaluated.error());
    const f64 lambda = *evaluated;
    result.split_logits.push_back(lambda); logit_sum += lambda;
    if (lambda <= 0.0) ++below_or_at;
  } while (detail::next_combination(selected, splits));
  result.pbo = static_cast<f64>(below_or_at) / static_cast<f64>(split_count);
  result.mean_logit = logit_sum / static_cast<f64>(split_count);
  return Ok(std::move(result));
}
} // namespace atx::engine::eval
