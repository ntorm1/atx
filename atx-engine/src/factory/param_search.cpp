#include "atx/engine/factory/param_search.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

#include "atx/engine/alpha/parser.hpp"

namespace atx::engine::factory {

// A genome-free factory: one dim per `{lo, hi}` pair, no genome literal attached
// (`id == kNoExpr`). Used by the raw-optimizer tests against a closed-form
// objective. The dim `kind` is irrelevant for the raw path (no instantiation).
[[nodiscard]] ParamSpace box(const std::vector<std::pair<atx::f64, atx::f64>> &bounds) {
  ParamSpace sp;
  sp.dim.reserve(bounds.size());
  for (const auto &[lo, hi] : bounds) {
    sp.dim.push_back(ParamDim{kNoExpr, ConstKind::Scale, lo, hi});
  }
  return sp;
}

// Extract the free fractional constants of `g` as a search box (plan §4.5). The
// free dims are the Window and Scale literals (Hparam included best-effort, since
// a peeled hparam is still a tunable immediate). Reuses `classify_literals` (§0.3)
// so the dim order is canonical (ascending ExprId) and thus deterministic.
[[nodiscard]] ParamSpace extract_free_constants(const Genome &g) {
  ParamSpace sp;
  for (const ClassifiedConst &c : classify_literals(g)) {
    const atx::f64 v = g.ast.node(c.id).value;
    atx::f64 lo = 0.0;
    atx::f64 hi = 0.0;
    if (c.kind == ConstKind::Window) {
      lo = 1.0;
      hi = kDefaultMaxLookback;
    } else {
      // Scale / Hparam: a continuous range spanning [0, 2v] (signed), widened to
      // a fixed floor when |v| is too small to give the search any room.
      const atx::f64 a = kScaleSpan * v;
      lo = std::min(0.0, a);
      hi = std::max(0.0, a);
      if (hi - lo < kScaleFloor) {
        lo = v - kScaleFloor;
        hi = v + kScaleFloor;
      }
    }
    sp.dim.push_back(ParamDim{c.id, c.kind, lo, hi});
  }
  return sp;
}

[[nodiscard]] atx::core::Result<Genome>
instantiate(const Genome &template_genome, const ParamSpace &space, std::span<const atx::f64> x) {
  Genome cur = template_genome.clone();
  for (atx::usize k = 0; k < space.dim.size(); ++k) {
    const ExprId id = space.dim[k].id;
    if (id == kNoExpr) {
      continue; // box dim: no genome literal to set
    }
    const atx::f64 v = x[k];
    Ast rebuilt = rebuild_with(cur, id, [v](Expr &e, Ast & /*dst*/) { e.value = v; });
    ATX_TRY(Genome next, analyze_into(std::move(rebuilt)));
    cur = std::move(next);
  }
  return atx::core::Ok(std::move(cur));
}

namespace detail {

[[nodiscard]] atx::f64 clamp_dim(atx::f64 v, const ParamDim &d) noexcept {
  return std::min(d.hi, std::max(d.lo, v));
}

[[nodiscard]] SepCmaParams sep_cma_params(atx::usize K, atx::usize lambda) {
  SepCmaParams p;
  const atx::f64 n = static_cast<atx::f64>(K);
  const atx::f64 lam = static_cast<atx::f64>(lambda);
  p.mu = lambda / 2;
  if (p.mu == 0) {
    p.mu = 1;
  }
  p.w.resize(p.mu);
  // Hansen's log weights w_i ∝ ln((lambda+1)/2) - ln(i), normalized to sum 1.
  atx::f64 sum = 0.0;
  for (atx::usize i = 0; i < p.mu; ++i) {
    p.w[i] = std::log(0.5 * (lam + 1.0)) - std::log(static_cast<atx::f64>(i + 1));
    sum += p.w[i];
  }
  atx::f64 sum_sq = 0.0;
  for (atx::f64 &wi : p.w) {
    wi /= sum;
    sum_sq += wi * wi;
  }
  p.mu_eff = 1.0 / sum_sq;
  // Standard CMA-ES adaptation parameters, with the sep-CMA rank-mu boost
  // ((n+2)/3) from Ros & Hansen (2008): faster diagonal learning since C has
  // only n free entries, not n(n+1)/2.
  p.c_sigma = (p.mu_eff + 2.0) / (n + p.mu_eff + 5.0);
  p.d_sigma = 1.0 + 2.0 * std::max(0.0, std::sqrt((p.mu_eff - 1.0) / (n + 1.0)) - 1.0) + p.c_sigma;
  p.c_c = (4.0 + p.mu_eff / n) / (n + 4.0 + 2.0 * p.mu_eff / n);
  const atx::f64 c1_full = 2.0 / ((n + 1.3) * (n + 1.3) + p.mu_eff);
  const atx::f64 cmu_full =
      std::min(1.0 - c1_full,
               2.0 * (p.mu_eff - 2.0 + 1.0 / p.mu_eff) / ((n + 2.0) * (n + 2.0) + p.mu_eff));
  const atx::f64 sep_boost = (n + 2.0) / 3.0;
  p.c_1 = std::min(1.0, c1_full * sep_boost);
  p.c_mu = std::min(1.0 - p.c_1, cmu_full * sep_boost);
  p.chi_n = std::sqrt(n) * (1.0 - 1.0 / (4.0 * n) + 1.0 / (21.0 * n * n));
  return p;
}

} // namespace detail

} // namespace atx::engine::factory
