#include "atx/engine/eval/pbo.hpp"

#include <span>   // std::span
#include <vector> // std::vector

namespace atx::engine::eval {

namespace detail {

PboResult pbo_cscv_core(std::span<const atx::f64> perf,
                        atx::usize n_candidates,
                        atx::usize n_splits,
                        atx::usize periods) {
  const atx::usize n = n_candidates;
  const atx::usize s = n_splits;
  const atx::usize half = s / 2U;       // |IS| == |OOS| == S/2
  const atx::usize width = periods / s; // periods per sub-period (T_used = width*S)

  // Lexicographic IS-combination walk over sub-period indices [0, S).
  std::vector<atx::usize> is_set(half);
  for (atx::usize i = 0U; i < half; ++i) {
    is_set[i] = i; // first combination: 0,1,...,half-1
  }

  // Reusable per-split scratch, sized once.
  SplitScratch sc;
  sc.oos_set.resize(s - half);
  sc.in_is.resize(s);
  sc.concat.reserve(half * width); // identical upper bound for IS and OOS gathers
  sc.is_sh.resize(n);
  sc.oos_sh.resize(n);
  sc.oos_rank.resize(n);

  PboResult result{0.0, {}, 0.0};
  result.split_logits.reserve(binomial(s, half)); // exact split count, no realloc churn

  atx::usize below_or_at = 0U; // count of splits with lambda <= 0
  atx::f64 logit_sum = 0.0;

  do {
    const atx::f64 lambda =
        split_logit(perf, n, periods, s, width, std::span<const atx::usize>{is_set}, sc);
    result.split_logits.push_back(lambda);
    logit_sum += lambda;
    if (lambda <= 0.0) {
      ++below_or_at;
    }
  } while (next_combination(std::span<atx::usize>{is_set}, s));

  const atx::usize n_split = result.split_logits.size();
  if (n_split != 0U) {
    result.pbo = static_cast<atx::f64>(below_or_at) / static_cast<atx::f64>(n_split);
    result.mean_logit = logit_sum / static_cast<atx::f64>(n_split);
  }
  return result;
}

} // namespace detail

} // namespace atx::engine::eval
