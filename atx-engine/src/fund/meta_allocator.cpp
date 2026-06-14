// meta_allocator.cpp — P2-S2-2 private implementation (USER DIRECTIVE: the unit's
// non-trivial numeric kernels live in the .cpp, not the header; .agents/cpp §6).
//
// Defines MetaAllocator::allocate + every private kernel: inverse-vol, the ERC
// log-barrier CCD solve, HRP (correlation-distance → single-linkage → seriation →
// recursive bisection), the order-fixed quad form, and the Kelly/cap composition.
// All reductions are ascending-index order-fixed; the CCD runs a FIXED sweep count
// with no early-exit (determinism R1). No RNG, no clock, no unordered containers.

#include "atx/engine/fund/meta_allocator.hpp"

#include <algorithm> // std::clamp, std::min
#include <cmath>     // std::sqrt, std::isfinite, std::fabs
#include <cstddef>   // std::size_t, std::ptrdiff_t
#include <limits>    // std::numeric_limits (single-linkage ∞ sentinel)
#include <utility>   // std::move, std::pair
#include <vector>    // std::vector (HRP cluster scratch)

namespace atx::engine::fund {

namespace {

// Equal weights 1/S (Σ=1). The ultimate fallback when even the vols are unusable.
[[nodiscard]] atx::core::linalg::VecX equal_weights(atx::usize s) {
  atx::core::linalg::VecX w(static_cast<Eigen::Index>(s));
  const atx::f64 inv = (s == 0U) ? 0.0 : 1.0 / static_cast<atx::f64>(s);
  for (Eigen::Index i = 0; i < w.size(); ++i) {
    w[i] = inv;
  }
  return w;
}

// Renormalize w to Σ=1 IN PLACE (order-fixed ascending sum). A non-positive sum
// (all-zero / degenerate) collapses to equal weights so the result stays a valid
// Σ=1, w>0 budget. Returns the normalized vector.
[[nodiscard]] atx::core::linalg::VecX normalize_sum1(atx::core::linalg::VecX w) {
  atx::f64 sum = 0.0;
  for (Eigen::Index i = 0; i < w.size(); ++i) {
    sum += w[i];
  }
  if (!(sum > 0.0) || !std::isfinite(sum)) {
    return equal_weights(static_cast<atx::usize>(w.size()));
  }
  for (Eigen::Index i = 0; i < w.size(); ++i) {
    w[i] /= sum;
  }
  return w;
}

} // namespace

// ---------------------------------------------------------------------------
//  inverse_vol — w_s ∝ 1/σ_s, normalized to Σ=1 (A1).
// ---------------------------------------------------------------------------
atx::core::linalg::VecX MetaAllocator::inverse_vol(std::span<const atx::f64> sleeve_vol) {
  const auto s = sleeve_vol.size();
  atx::core::linalg::VecX w(static_cast<Eigen::Index>(s));
  bool all_usable = true;
  for (std::size_t i = 0; i < s; ++i) {
    const atx::f64 sigma = sleeve_vol[i];
    if (!(sigma > 0.0) || !std::isfinite(sigma)) {
      all_usable = false; // a degenerate vol cannot define a 1/σ tilt
      break;
    }
  }
  if (!all_usable) {
    return equal_weights(s); // vols unusable ⇒ equal (§0.8 ultimate fallback)
  }
  for (std::size_t i = 0; i < s; ++i) {
    w[static_cast<Eigen::Index>(i)] = 1.0 / sleeve_vol[i];
  }
  return normalize_sum1(std::move(w));
}

// ---------------------------------------------------------------------------
//  erc_log_barrier — Spinu log-barrier ERC via cyclical coordinate descent (A1).
//
//  min ½wᵀΩw − Σ b_s ln(w_s) on w>0. Per-coordinate stationarity Ω_ss w_s + β = b_s/w_s
//  (β = Σ_{t≠s} w_t Ω_st) ⇒ the positive root
//      w_s = ( −β + sqrt(β² + 4 Ω_ss b_s) ) / (2 Ω_ss).
//  Sweep ascending s for EXACTLY `iters` full sweeps (NO residual early-exit, R1),
//  then renormalize to Σ=1. Warm-start at the inverse-vol point when available.
// ---------------------------------------------------------------------------
atx::core::linalg::VecX MetaAllocator::erc_log_barrier(const atx::core::linalg::MatX &Omega,
                                                       std::span<const atx::f64> b,
                                                       atx::usize iters) {
  const Eigen::Index s = Omega.rows();
  atx::core::linalg::VecX w(s);
  // Warm start: 1/σ_s (inverse-vol) from the diagonal; equal 1/√Ω_ss is positive and
  // cheap. Ω_ss > 0 is guaranteed by the caller's degeneracy gate.
  for (Eigen::Index i = 0; i < s; ++i) {
    const atx::f64 var = Omega(i, i);
    w[i] = (var > 0.0) ? 1.0 / std::sqrt(var) : 1.0;
  }

  // FIXED sweep count — the loop bound is solve_iters and S; no convergence test.
  for (atx::usize sweep = 0U; sweep < iters; ++sweep) {
    for (Eigen::Index i = 0; i < s; ++i) {
      // β = Σ_{t≠i} w_t·Ω_it (order-fixed ascending t).
      atx::f64 beta = 0.0;
      for (Eigen::Index t = 0; t < s; ++t) {
        if (t != i) {
          beta += w[t] * Omega(i, t);
        }
      }
      const atx::f64 diag = Omega(i, i);
      const atx::f64 bi = b[static_cast<std::size_t>(i)];
      // Positive root of Ω_ii w² + β w − b_i = 0.
      const atx::f64 disc = beta * beta + 4.0 * diag * bi;
      w[i] = (-beta + std::sqrt(disc)) / (2.0 * diag);
    }
  }
  return normalize_sum1(std::move(w));
}

// ---------------------------------------------------------------------------
//  HRP helpers (A2). Kept in this TU's anonymous namespace — pure scratch math.
// ---------------------------------------------------------------------------
namespace {

// Correlation-distance d_ij = sqrt(0.5·(1−ρ_ij)), ρ_ij = Ω_ij/(σ_i σ_j). Order-fixed.
[[nodiscard]] atx::core::linalg::MatX corr_distance(const atx::core::linalg::MatX &Omega) {
  const Eigen::Index s = Omega.rows();
  atx::core::linalg::MatX d(s, s);
  for (Eigen::Index i = 0; i < s; ++i) {
    const atx::f64 si = std::sqrt(Omega(i, i));
    for (Eigen::Index j = 0; j < s; ++j) {
      const atx::f64 sj = std::sqrt(Omega(j, j));
      const atx::f64 denom = si * sj;
      atx::f64 rho = (denom > 0.0) ? Omega(i, j) / denom : 0.0;
      rho = std::clamp(rho, -1.0, 1.0);
      d(i, j) = std::sqrt(0.5 * (1.0 - rho));
    }
  }
  return d;
}

// A single-linkage agglomerative merge step record: the two child clusters joined.
struct Merge {
  atx::usize a; // left child (an original leaf index < S, or a cluster id ≥ S)
  atx::usize b; // right child
};

// Single-linkage agglomerative clustering over the S leaves using the correlation-
// distance matrix `d`. Returns the merge list (S−1 merges); cluster ids ≥ S name the
// merges (id = S + merge_index), López de Prado's linkage indexing. Order-fixed: ties
// break on the lowest (a,b) index pair (ascending scan), so the tree is deterministic.
[[nodiscard]] std::vector<Merge> single_linkage(const atx::core::linalg::MatX &d) {
  const auto s = static_cast<atx::usize>(d.rows());
  // Active clusters: each carries the set of leaf members (for single-linkage min).
  std::vector<std::vector<atx::usize>> members; // leaf members per active cluster
  std::vector<atx::usize> ids;                  // the cluster id of each active slot
  members.reserve(s);
  ids.reserve(s);
  for (atx::usize i = 0U; i < s; ++i) {
    members.push_back({i});
    ids.push_back(i);
  }

  std::vector<Merge> merges;
  merges.reserve(s == 0U ? 0U : s - 1U);
  atx::usize next_id = s;
  // Each iteration merges the closest pair of active clusters. Bound: S−1 merges.
  while (members.size() > 1U) {
    atx::usize best_p = 0U;
    atx::usize best_q = 1U;
    atx::f64 best = std::numeric_limits<atx::f64>::infinity();
    // Ascending (p,q) scan; strict '<' keeps the FIRST (lowest-index) tie. Order-fixed.
    for (atx::usize p = 0U; p < members.size(); ++p) {
      for (atx::usize q = p + 1U; q < members.size(); ++q) {
        // Single-linkage distance = min over member cross-pairs (order-fixed).
        atx::f64 dist = std::numeric_limits<atx::f64>::infinity();
        for (const atx::usize mi : members[p]) {
          for (const atx::usize mj : members[q]) {
            const atx::f64 dij = d(static_cast<Eigen::Index>(mi), static_cast<Eigen::Index>(mj));
            if (dij < dist) {
              dist = dij;
            }
          }
        }
        if (dist < best) {
          best = dist;
          best_p = p;
          best_q = q;
        }
      }
    }
    merges.push_back(Merge{ids[best_p], ids[best_q]});
    // Replace slot best_p with the merged cluster; erase best_q.
    std::vector<atx::usize> merged = members[best_p];
    for (const atx::usize m : members[best_q]) {
      merged.push_back(m);
    }
    members[best_p] = std::move(merged);
    ids[best_p] = next_id;
    ++next_id;
    members.erase(members.begin() + static_cast<std::ptrdiff_t>(best_q));
    ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(best_q));
  }
  return merges;
}

// Quasi-diagonalize: expand the linkage tree into an ordered leaf list so correlated
// sleeves are adjacent (seriation). Replace each cluster id ≥ S by its two children
// (left then right), bottoming out at original leaves. EXPLICIT work-stack (NOT
// recursion) — a chained single-linkage tree has depth ~S, so recursion risks a deep
// call stack; this mirrors the bisection's deliberate de-recursion. Order-fixed:
// pushing m.b THEN m.a makes the LEFT child pop/emit first, preserving the exact
// left-then-right leaf ordering. Stack depth ≤ S (each push is a distinct tree node).
void seriate(atx::usize root, atx::usize s, const std::vector<Merge> &merges,
             std::vector<atx::usize> &order) {
  std::vector<atx::usize> stack;
  stack.push_back(root);
  while (!stack.empty()) {
    const atx::usize node = stack.back();
    stack.pop_back();
    if (node < s) {
      order.push_back(node); // original leaf
      continue;
    }
    const Merge &m = merges[node - s]; // cluster id `node` names merge (node − S)
    stack.push_back(m.b);              // push right first so the LEFT child pops first
    stack.push_back(m.a);
  }
}

// Inverse-variance cluster variance V = w̃ᵀ Ω_sub w̃ over the leaf subset `idx`, with
// w̃ = (1/diagΩ)/Σ(1/diagΩ) restricted to the subset. Order-fixed (ascending subset).
[[nodiscard]] atx::f64 cluster_variance(const atx::core::linalg::MatX &Omega,
                                        const std::vector<atx::usize> &idx) {
  const std::size_t n = idx.size();
  std::vector<atx::f64> w(n);
  atx::f64 inv_sum = 0.0;
  for (std::size_t k = 0; k < n; ++k) {
    const atx::f64 var = Omega(static_cast<Eigen::Index>(idx[k]), static_cast<Eigen::Index>(idx[k]));
    const atx::f64 iv = (var > 0.0) ? 1.0 / var : 0.0;
    w[k] = iv;
    inv_sum += iv;
  }
  if (inv_sum > 0.0) {
    for (std::size_t k = 0; k < n; ++k) {
      w[k] /= inv_sum;
    }
  }
  atx::f64 v = 0.0; // w̃ᵀ Ω_sub w̃, order-fixed ascending k then l
  for (std::size_t k = 0; k < n; ++k) {
    for (std::size_t l = 0; l < n; ++l) {
      v += w[k] *
           Omega(static_cast<Eigen::Index>(idx[k]), static_cast<Eigen::Index>(idx[l])) * w[l];
    }
  }
  return v;
}

} // namespace

// ---------------------------------------------------------------------------
//  hrp_weights — Hierarchical Risk Parity (A2, López de Prado 2016).
//
//  (a) correlation-distance; (b) single-linkage agglomerative tree (documented:
//  SINGLE linkage); (c) quasi-diagonal seriation; (d) recursive bisection over the
//  seriated order: split in half, weight each half by inverse-variance cluster
//  variance, split factor α = 1 − V₁/(V₁+V₂), recurse; final leaf weight = product
//  of split factors. NEVER inverts Ω. Normalized to Σ=1.
// ---------------------------------------------------------------------------
atx::core::linalg::VecX MetaAllocator::hrp_weights(const atx::core::linalg::MatX &Omega) {
  const auto s = static_cast<atx::usize>(Omega.rows());
  atx::core::linalg::VecX w(static_cast<Eigen::Index>(s));
  if (s == 0U) {
    return w;
  }
  if (s == 1U) {
    w[0] = 1.0;
    return w;
  }

  const atx::core::linalg::MatX d = corr_distance(Omega);
  const std::vector<Merge> merges = single_linkage(d);
  // The LAST merge's cluster id is S + (merges.size()−1) = the tree root. Seriate from
  // the root into the quasi-diagonal leaf order (correlated sleeves made adjacent).
  std::vector<atx::usize> order;
  order.reserve(s);
  seriate(s + merges.size() - 1U, s, merges, order);

  // Recursive bisection over the SERIATED order. Each leaf carries a running weight,
  // initialized to 1 and multiplied by the split factor of every bisection it falls
  // in. We index by POSITION in `order`; map back to leaf at the end.
  std::vector<atx::f64> alpha(s, 1.0); // per-position running weight
  // A work stack of [lo, hi) half-open position ranges to bisect (no recursion depth
  // surprise; bound is the tree size). Order-fixed: push right then left so left pops
  // first, but the math is split-symmetric so order only affects determinism, which
  // the fixed push order pins.
  std::vector<std::pair<std::size_t, std::size_t>> stack;
  stack.push_back({0U, s});
  while (!stack.empty()) {
    const auto [lo, hi] = stack.back();
    stack.pop_back();
    const std::size_t n = hi - lo;
    if (n < 2U) {
      continue;
    }
    const std::size_t mid = lo + n / 2U; // split in half (left gets the floor)
    // Leaf-index subsets for the two halves (positions → original leaves via `order`).
    std::vector<atx::usize> left;
    std::vector<atx::usize> right;
    left.reserve(mid - lo);
    right.reserve(hi - mid);
    for (std::size_t p = lo; p < mid; ++p) {
      left.push_back(order[p]);
    }
    for (std::size_t p = mid; p < hi; ++p) {
      right.push_back(order[p]);
    }
    const atx::f64 v1 = cluster_variance(Omega, left);
    const atx::f64 v2 = cluster_variance(Omega, right);
    const atx::f64 denom = v1 + v2;
    const atx::f64 a = (denom > 0.0) ? 1.0 - v1 / denom : 0.5; // split factor (left share)
    for (std::size_t p = lo; p < mid; ++p) {
      alpha[p] *= a; // left half scaled by α
    }
    for (std::size_t p = mid; p < hi; ++p) {
      alpha[p] *= (1.0 - a); // right half scaled by 1−α
    }
    stack.push_back({mid, hi});
    stack.push_back({lo, mid});
  }

  // Scatter the per-position weights back to their leaf indices.
  for (std::size_t p = 0; p < s; ++p) {
    w[static_cast<Eigen::Index>(order[p])] = alpha[p];
  }
  return normalize_sum1(std::move(w));
}

// ---------------------------------------------------------------------------
//  risk_budget_weights — dispatch on cfg.method.
// ---------------------------------------------------------------------------
atx::core::linalg::VecX MetaAllocator::risk_budget_weights(RiskBudgetMethod method,
                                                           const atx::core::linalg::MatX &Omega,
                                                           std::span<const atx::f64> sleeve_vol,
                                                           std::span<const atx::f64> b,
                                                           atx::usize iters) {
  switch (method) {
  case RiskBudgetMethod::InverseVol:
    return inverse_vol(sleeve_vol);
  case RiskBudgetMethod::EqualRiskContribution:
    return erc_log_barrier(Omega, b, iters);
  case RiskBudgetMethod::HierarchicalRiskParity:
    return hrp_weights(Omega);
  }
  return {}; // unreachable — switch is exhaustive over RiskBudgetMethod
}

// ---------------------------------------------------------------------------
//  quad_form — order-fixed wᵀΩw (ascending i then j).
// ---------------------------------------------------------------------------
atx::f64 MetaAllocator::quad_form(const atx::core::linalg::MatX &Omega,
                                  const atx::core::linalg::VecX &w) {
  const Eigen::Index s = w.size();
  atx::f64 q = 0.0;
  for (Eigen::Index i = 0; i < s; ++i) {
    for (Eigen::Index j = 0; j < s; ++j) {
      q += w[i] * Omega(i, j) * w[j];
    }
  }
  return q;
}

// ---------------------------------------------------------------------------
//  apply_kelly_caps — fractional-Kelly × vol-target, gross-cap, per-sleeve box (A3/A5).
//
//  k_vol = (target_vol>0 && σ_fund>0) ? target_vol/σ_fund : 1; k = fractional_kelly·k_vol.
//  Gross cap FIRST: k_eff = min(k, max_gross/‖w_rb‖₁) so Σ|c| ≤ max_gross. THEN the
//  per-sleeve box: c_s = clip(k_eff·w_rb_s, 0, caps_s) — the box only shrinks an entry,
//  so it preserves Σ|c| ≤ max_gross while enforcing each c_s ≤ caps_s. (Documented order.)
// ---------------------------------------------------------------------------
std::vector<atx::f64> MetaAllocator::apply_kelly_caps(const atx::core::linalg::VecX &w_rb,
                                                      atx::f64 sigma_fund,
                                                      std::span<const atx::f64> caps,
                                                      const MetaAllocatorConfig &cfg) {
  const auto s = static_cast<std::size_t>(w_rb.size());
  const atx::f64 k_vol =
      (cfg.target_vol > 0.0 && sigma_fund > 0.0) ? cfg.target_vol / sigma_fund : 1.0;
  atx::f64 k = cfg.fractional_kelly * k_vol;
  if (!(k > 0.0) || !std::isfinite(k)) {
    k = 0.0; // a non-positive / non-finite Kelly never levers (mirrors size_book)
  }

  // ‖w_rb‖₁ (order-fixed). w_rb is Σ=1, w>0 ⇒ L1 == 1 for the budget kernels, but we
  // compute it generally so the gross cap is correct for any w_rb.
  atx::f64 l1 = 0.0;
  for (std::size_t i = 0; i < s; ++i) {
    l1 += std::fabs(w_rb[static_cast<Eigen::Index>(i)]);
  }
  // Gross cap applied to the SCALE: k_eff = min(k, max_gross/‖w_rb‖₁).
  atx::f64 k_eff = k;
  if (l1 > 0.0 && cfg.max_gross >= 0.0) {
    const atx::f64 k_gross = cfg.max_gross / l1;
    k_eff = std::min(k_eff, k_gross);
  }

  std::vector<atx::f64> c(s);
  for (std::size_t i = 0; i < s; ++i) {
    const atx::f64 raw = k_eff * w_rb[static_cast<Eigen::Index>(i)];
    const atx::f64 cap = caps[i] > 0.0 ? caps[i] : 0.0; // a negative cap clips to 0
    c[i] = std::clamp(raw, 0.0, cap); // per-sleeve capacity box [0, caps_s] (§0.3)
  }
  return c;
}

// ---------------------------------------------------------------------------
//  is_degenerate — non-finite entry or non-positive diagonal ⇒ inverse-vol fallback.
// ---------------------------------------------------------------------------
bool MetaAllocator::is_degenerate(const atx::core::linalg::MatX &Omega) noexcept {
  const Eigen::Index s = Omega.rows();
  for (Eigen::Index i = 0; i < s; ++i) {
    if (!(Omega(i, i) > 0.0) || !std::isfinite(Omega(i, i))) {
      return true; // a zero/negative/non-finite variance — ERC/HRP cannot proceed
    }
    for (Eigen::Index j = 0; j < s; ++j) {
      if (!std::isfinite(Omega(i, j))) {
        return true; // a non-finite off-diagonal poisons every reduction
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
//  allocate — the public entry point.
// ---------------------------------------------------------------------------
atx::core::Result<CapitalWeights>
MetaAllocator::allocate(const atx::core::linalg::MatX &Omega, std::span<const atx::f64> sleeve_vol,
                        std::span<const atx::f64> caps) const {
  // --- boundary validation (§ shape / feasibility) ---
  if (Omega.rows() != Omega.cols()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "MetaAllocator::allocate: Omega must be square (S×S)");
  }
  const auto s = static_cast<atx::usize>(Omega.rows());
  if (sleeve_vol.size() != s || caps.size() != s) {
    return atx::core::Err(
        atx::core::ErrorCode::InvalidArgument,
        "MetaAllocator::allocate: sleeve_vol / caps length must equal S (Omega.rows())");
  }

  // Config-scalar boundary validation. A negative max_gross would silently skip the
  // gross cap (apply_kelly_caps gates on max_gross >= 0), breaking the documented
  // Σ|c| ≤ max_gross invariant; the rest guard the Kelly/vol-target/iter math. Each is
  // a contract violation ⇒ Err(InvalidArgument), not a silent degenerate result.
  if (!(cfg.max_gross >= 0.0)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "MetaAllocator::allocate: max_gross must be >= 0");
  }
  if (!(cfg.fractional_kelly >= 0.0)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "MetaAllocator::allocate: fractional_kelly must be >= 0");
  }
  if (!(cfg.target_vol >= 0.0)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "MetaAllocator::allocate: target_vol must be >= 0");
  }
  if (cfg.solve_iters < 1U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "MetaAllocator::allocate: solve_iters must be >= 1");
  }

  // S=0 ⇒ Ok with empty weights (§0.8).
  if (s == 0U) {
    return atx::core::Ok(CapitalWeights{});
  }

  // Resolve the per-sleeve budget b: empty ⇒ equal 1/S; non-empty ⇒ validate size S,
  // all > 0, Σ ≈ 1 (A1). The validation is order-fixed.
  std::vector<atx::f64> budget;
  if (cfg.risk_budget.empty()) {
    budget.assign(s, 1.0 / static_cast<atx::f64>(s));
  } else {
    if (cfg.risk_budget.size() != s) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "MetaAllocator::allocate: risk_budget size must equal S");
    }
    atx::f64 sum = 0.0;
    for (const atx::f64 bi : cfg.risk_budget) {
      if (!(bi > 0.0) || !std::isfinite(bi)) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "MetaAllocator::allocate: risk_budget entries must be > 0");
      }
      sum += bi;
    }
    if (std::fabs(sum - 1.0) > 1e-9) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "MetaAllocator::allocate: risk_budget must sum to 1");
    }
    budget = cfg.risk_budget;
  }

  // --- risk-budget stage (with §0.8 degeneracy fallback) ---
  // A degenerate Ω cannot drive ERC/HRP (zero/negative/non-finite covariance) — fall
  // back to inverse-vol (or equal if the vols are also unusable). NEVER throw, NEVER
  // peek. InverseVol is already pure-vol so it skips the degeneracy gate.
  atx::core::linalg::VecX w_rb;
  if (cfg.method == RiskBudgetMethod::InverseVol || !is_degenerate(Omega)) {
    w_rb = risk_budget_weights(cfg.method, Omega, sleeve_vol, budget, cfg.solve_iters);
  } else {
    w_rb = inverse_vol(sleeve_vol); // degenerate Ω ⇒ inverse-vol fallback (§0.8)
  }

  // --- Kelly/cap composition (A3/A5) ---
  // σ_fund uses the SAME Ω; a degenerate Ω that fell back to inverse-vol still yields a
  // finite σ_fund here only if Ω's quad form is finite — guard target_vol against a
  // non-finite σ_fund by treating it as 0 (Kelly-only scale).
  atx::f64 sigma2 = quad_form(Omega, w_rb);
  if (!(sigma2 > 0.0) || !std::isfinite(sigma2)) {
    sigma2 = 0.0; // degenerate fund variance ⇒ no vol-target scaling (k_vol = 1)
  }
  const atx::f64 sigma_fund = std::sqrt(sigma2);

  CapitalWeights out;
  out.c = apply_kelly_caps(w_rb, sigma_fund, caps, cfg);
  return atx::core::Ok(std::move(out));
}

} // namespace atx::engine::fund
