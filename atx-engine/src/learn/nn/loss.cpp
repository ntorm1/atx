#include "atx/engine/learn/nn/loss.hpp"

#include <algorithm> // std::stable_sort (row groups)
#include <cmath>     // std::fabs, std::sqrt
#include <cstddef>   // std::size_t
#include <span>      // std::span
#include <utility>   // std::pair
#include <vector>    // std::vector

#include <Eigen/Dense> // Eigen::Index

#include "atx/core/macro.hpp" // ATX_ASSERT, ATX_CHECK
#include "atx/core/types.hpp" // f64, u32, usize

namespace atx::engine::learn::nn {

namespace {
[[nodiscard]] atx::f64 sign(atx::f64 x) noexcept { return (x > 0.0) - (x < 0.0); }
} // namespace

// =====================================================================
//  MseLoss — mean of (p - t)² over all elements.
// =====================================================================

atx::f64 MseLoss::value(const lin::MatX &pred, const lin::MatX &target) {
  ATX_ASSERT(pred.rows() == target.rows() && pred.cols() == target.cols());
  const Eigen::Index B = pred.rows();
  const Eigen::Index C = pred.cols();
  const atx::f64 n = static_cast<atx::f64>(B) * static_cast<atx::f64>(C);
  atx::f64 acc = 0.0; // ascending scalar fold (R1)
  for (Eigen::Index c = 0; c < C; ++c) {
    for (Eigen::Index r = 0; r < B; ++r) {
      const atx::f64 d = pred(r, c) - target(r, c);
      acc += d * d;
    }
  }
  return acc / n;
}

lin::MatX MseLoss::grad(const lin::MatX &pred, const lin::MatX &target) {
  ATX_ASSERT(pred.rows() == target.rows() && pred.cols() == target.cols());
  const atx::f64 n = static_cast<atx::f64>(pred.rows()) * static_cast<atx::f64>(pred.cols());
  return (2.0 / n) * (pred - target); // dL/dp = (2/N)(p - t)
}

// =====================================================================
//  HuberLoss — quadratic near 0, linear in the tails.
// =====================================================================

atx::f64 HuberLoss::value(const lin::MatX &pred, const lin::MatX &target) {
  ATX_ASSERT(pred.rows() == target.rows() && pred.cols() == target.cols());
  const Eigen::Index B = pred.rows();
  const Eigen::Index C = pred.cols();
  const atx::f64 n = static_cast<atx::f64>(B) * static_cast<atx::f64>(C);
  atx::f64 acc = 0.0; // ascending scalar fold (R1)
  for (Eigen::Index c = 0; c < C; ++c) {
    for (Eigen::Index r = 0; r < B; ++r) {
      const atx::f64 res = pred(r, c) - target(r, c);
      const atx::f64 a = std::fabs(res);
      acc += (a <= delta_) ? 0.5 * res * res : delta_ * (a - 0.5 * delta_);
    }
  }
  return acc / n;
}

lin::MatX HuberLoss::grad(const lin::MatX &pred, const lin::MatX &target) {
  ATX_ASSERT(pred.rows() == target.rows() && pred.cols() == target.cols());
  const Eigen::Index B = pred.rows();
  const Eigen::Index C = pred.cols();
  const atx::f64 n = static_cast<atx::f64>(B) * static_cast<atx::f64>(C);
  lin::MatX g(B, C);
  for (Eigen::Index c = 0; c < C; ++c) {
    for (Eigen::Index r = 0; r < B; ++r) {
      const atx::f64 res = pred(r, c) - target(r, c);
      const atx::f64 a = std::fabs(res);
      g(r, c) = ((a <= delta_) ? res : delta_ * sign(res)) / n;
    }
  }
  return g;
}

// =====================================================================
//  IcLoss — L = 1 - Pearson(pred, target), flattened column-major.
//
//  With centred pc = p - p̄, tc = t - t̄ and Spt = Σ pc·tc, Spp = Σ pc²,
//  Stt = Σ tc², the correlation is rho = Spt / sqrt(Spp·Stt) and
//    d(rho)/dp_i = tc_i / sqrt(Spp·Stt) - rho · pc_i / Spp.
//  Since L = 1 - rho, dL/dp_i = -d(rho)/dp_i.
//
//  PRECONDITION: inputs are expected centered / O(1) in magnitude (the normalised
//  forward-return targets and standardised model outputs this loss trains on). On
//  pathologically large un-normalised inputs the unscaled product Spp·Stt can
//  overflow to +inf; sqrt(inf)=inf then makes rho 0 (loss 1, no signal) — a benign
//  degraded result, never UB. The zero-variance case is caught by the eps guard.
// =====================================================================

namespace {

// Accumulate the Pearson moments of pred/target as ascending column-major scalar
// folds (R1). Returns rho and the moment scalars the gradient needs.
struct IcMoments {
  atx::f64 rho;
  atx::f64 pbar;
  atx::f64 tbar;
  atx::f64 spp;
  atx::f64 root; // sqrt(Spp·Stt)
  bool degenerate;
};

[[nodiscard]] IcMoments ic_moments(const lin::MatX &pred, const lin::MatX &target, atx::f64 eps) {
  const Eigen::Index B = pred.rows();
  const Eigen::Index C = pred.cols();
  const atx::f64 n = static_cast<atx::f64>(B) * static_cast<atx::f64>(C);
  atx::f64 sp = 0.0;
  atx::f64 st = 0.0;
  for (Eigen::Index c = 0; c < C; ++c) {
    for (Eigen::Index r = 0; r < B; ++r) {
      sp += pred(r, c);
      st += target(r, c);
    }
  }
  const atx::f64 pbar = sp / n;
  const atx::f64 tbar = st / n;
  atx::f64 spt = 0.0;
  atx::f64 spp = 0.0;
  atx::f64 stt = 0.0;
  for (Eigen::Index c = 0; c < C; ++c) {
    for (Eigen::Index r = 0; r < B; ++r) {
      const atx::f64 pc = pred(r, c) - pbar;
      const atx::f64 tc = target(r, c) - tbar;
      spt += pc * tc;
      spp += pc * pc;
      stt += tc * tc;
    }
  }
  IcMoments m{};
  m.pbar = pbar;
  m.tbar = tbar;
  m.spp = spp;
  if (spp <= eps || stt <= eps) {
    m.degenerate = true;
    m.rho = 0.0;
    m.root = 0.0;
    return m;
  }
  m.root = std::sqrt(spp * stt);
  m.rho = spt / m.root;
  m.degenerate = false;
  return m;
}

// Pearson moments over the rows `rows` of one group (all columns), as ascending
// (column, then listed-row) scalar folds (R1). Same contract as ic_moments.
[[nodiscard]] IcMoments group_moments(const lin::MatX &pred, const lin::MatX &target,
                                      std::span<const Eigen::Index> rows, atx::f64 eps) {
  const Eigen::Index C = pred.cols();
  const atx::f64 n = static_cast<atx::f64>(rows.size()) * static_cast<atx::f64>(C);
  atx::f64 sp = 0.0;
  atx::f64 st = 0.0;
  for (Eigen::Index c = 0; c < C; ++c) {
    for (const Eigen::Index r : rows) {
      sp += pred(r, c);
      st += target(r, c);
    }
  }
  IcMoments m{};
  m.pbar = sp / n;
  m.tbar = st / n;
  atx::f64 spt = 0.0;
  atx::f64 spp = 0.0;
  atx::f64 stt = 0.0;
  for (Eigen::Index c = 0; c < C; ++c) {
    for (const Eigen::Index r : rows) {
      const atx::f64 pc = pred(r, c) - m.pbar;
      const atx::f64 tc = target(r, c) - m.tbar;
      spt += pc * tc;
      spp += pc * pc;
      stt += tc * tc;
    }
  }
  m.spp = spp;
  if (n < 2.0 || spp <= eps || stt <= eps) {
    m.degenerate = true;
    m.rho = 0.0;
    m.root = 0.0;
    return m;
  }
  m.root = std::sqrt(spp * stt);
  m.rho = spt / m.root;
  m.degenerate = false;
  return m;
}

// The batch rows partitioned by group label: rows sorted by (group, row) with a stable
// sort, then cut into runs. Deterministic for any label values (R1).
struct GroupRuns {
  std::vector<Eigen::Index> rows;               // all rows, grouped
  std::vector<std::pair<std::size_t, std::size_t>> runs; // [lo, hi) into rows, ascending group
};

[[nodiscard]] GroupRuns group_runs(std::span<const atx::u32> groups) {
  GroupRuns g;
  g.rows.resize(groups.size());
  for (std::size_t i = 0; i < groups.size(); ++i) {
    g.rows[i] = static_cast<Eigen::Index>(i);
  }
  std::stable_sort(g.rows.begin(), g.rows.end(), [&groups](Eigen::Index a, Eigen::Index b) {
    return groups[static_cast<std::size_t>(a)] < groups[static_cast<std::size_t>(b)];
  });
  std::size_t lo = 0;
  while (lo < g.rows.size()) {
    const atx::u32 id = groups[static_cast<std::size_t>(g.rows[lo])];
    std::size_t hi = lo;
    while (hi < g.rows.size() && groups[static_cast<std::size_t>(g.rows[hi])] == id) {
      ++hi;
    }
    g.runs.emplace_back(lo, hi);
    lo = hi;
  }
  return g;
}

} // namespace

void IcLoss::set_row_groups(std::span<const atx::u32> groups) {
  groups_.assign(groups.begin(), groups.end());
}

atx::f64 IcLoss::value(const lin::MatX &pred, const lin::MatX &target) {
  ATX_ASSERT(pred.rows() == target.rows() && pred.cols() == target.cols());
  if (reduction_ == IcReduction::PerGroupMeanV2 && !groups_.empty()) {
    ATX_CHECK(groups_.size() == static_cast<std::size_t>(pred.rows()));
    const GroupRuns g = group_runs(std::span<const atx::u32>{groups_});
    atx::f64 sum = 0.0;
    atx::usize n_groups = 0;
    for (const auto &[lo, hi] : g.runs) {
      const std::span<const Eigen::Index> rows{g.rows.data() + lo, hi - lo};
      const IcMoments m = group_moments(pred, target, rows, eps_);
      if (!m.degenerate) {
        sum += m.rho;
        ++n_groups;
      }
    }
    return (n_groups == 0U) ? 1.0 : 1.0 - sum / static_cast<atx::f64>(n_groups);
  }
  const IcMoments m = ic_moments(pred, target, eps_);
  return 1.0 - m.rho; // degenerate => rho 0 => loss 1 (no signal)
}

lin::MatX IcLoss::grad(const lin::MatX &pred, const lin::MatX &target) {
  ATX_ASSERT(pred.rows() == target.rows() && pred.cols() == target.cols());
  const Eigen::Index B = pred.rows();
  const Eigen::Index C = pred.cols();
  if (reduction_ == IcReduction::PerGroupMeanV2 && !groups_.empty()) {
    ATX_CHECK(groups_.size() == static_cast<std::size_t>(B));
    const GroupRuns gr = group_runs(std::span<const atx::u32>{groups_});
    std::vector<IcMoments> mom;
    mom.reserve(gr.runs.size());
    atx::usize n_groups = 0;
    for (const auto &[lo, hi] : gr.runs) {
      const std::span<const Eigen::Index> rows{gr.rows.data() + lo, hi - lo};
      mom.push_back(group_moments(pred, target, rows, eps_));
      n_groups += mom.back().degenerate ? 0U : 1U;
    }
    lin::MatX g = lin::MatX::Zero(B, C);
    if (n_groups == 0U) {
      return g; // no group carries a defined correlation
    }
    const atx::f64 inv_g = 1.0 / static_cast<atx::f64>(n_groups);
    for (std::size_t k = 0; k < gr.runs.size(); ++k) {
      const IcMoments &m = mom[k];
      if (m.degenerate) {
        continue;
      }
      for (std::size_t i = gr.runs[k].first; i < gr.runs[k].second; ++i) {
        const Eigen::Index r = gr.rows[i];
        for (Eigen::Index c = 0; c < C; ++c) {
          const atx::f64 pc = pred(r, c) - m.pbar;
          const atx::f64 tc = target(r, c) - m.tbar;
          g(r, c) = -inv_g * (tc / m.root - m.rho * pc / m.spp);
        }
      }
    }
    return g;
  }
  const IcMoments m = ic_moments(pred, target, eps_);
  lin::MatX g(B, C);
  if (m.degenerate) {
    g.setZero(); // no defined gradient direction
    return g;
  }
  // dL/dp_i = -[ tc_i / root - rho * pc_i / Spp ].
  for (Eigen::Index c = 0; c < C; ++c) {
    for (Eigen::Index r = 0; r < B; ++r) {
      const atx::f64 pc = pred(r, c) - m.pbar;
      const atx::f64 tc = target(r, c) - m.tbar;
      const atx::f64 drho = tc / m.root - m.rho * pc / m.spp;
      g(r, c) = -drho;
    }
  }
  return g;
}

} // namespace atx::engine::learn::nn
