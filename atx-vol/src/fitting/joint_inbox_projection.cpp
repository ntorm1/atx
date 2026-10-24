#include "fitting/joint_inbox_projection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <Eigen/Core>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>

namespace atx::vol::detail {
namespace {

using Matrix = Eigen::SparseMatrix<double>;
using Vector = Eigen::VectorXd;

constexpr double kFeasibilityTolerance = 1.0e-10;

struct Row {
  std::array<int, 4> index{};
  std::array<double, 4> coef{};
  std::size_t count = 0;
  double b = 0.0;

  void term(int i, double a) {
    if (a == 0.0) return;
    for (std::size_t k = 0; k < count; ++k) {
      if (index[k] == i) { coef[k] += a; return; }
    }
    index[count] = i;
    coef[count++] = a;
  }

  [[nodiscard]] double value(const Vector &x) const {
    double v = -b;
    for (std::size_t k = 0; k < count; ++k) v += coef[k] * x[index[k]];
    return v;
  }
};

struct Problem {
  std::vector<std::size_t> offsets;
  std::vector<int> variable; // original coordinate -> free coordinate, -1 if fixed
  std::vector<double> fixed;
  std::vector<double> lower, upper, target;
  std::vector<Row> rows;
  bool infeasible = false;
};

[[nodiscard]] bool valid_bounds(const AmericanPriceBounds &b) {
  return std::isfinite(b.spot) && b.spot > 0.0 && std::isfinite(b.discount_min) &&
         b.discount_min > 0.0 && b.discount_min <= 1.0 && std::isfinite(b.discount_max) &&
         b.discount_max >= 1.0 && std::isfinite(b.prepaid_min) && b.prepaid_min >= 0.0 &&
         b.prepaid_min <= b.spot && std::isfinite(b.prepaid_max) && b.prepaid_max >= b.spot;
}

[[nodiscard]] bool nondividend(const AmericanPriceBounds &b) {
  return b.prepaid_min == b.spot && b.prepaid_max == b.spot && b.discount_max == 1.0;
}

[[nodiscard]] int coordinate(std::size_t offset, std::size_t node, bool call) {
  return static_cast<int>(offset + 2u * node + (call ? 0u : 1u));
}

// Substitute fixed coordinates, then discard constraints implied by the boxes.
// Directly contradictory boxes/constant rows are a genuine infeasibility witness.
void add_row(Problem &p, const Row &original) {
  Row row;
  row.b = original.b;
  for (std::size_t k = 0; k < original.count; ++k) {
    const auto old = static_cast<std::size_t>(original.index[k]);
    if (p.variable[old] < 0) row.b -= original.coef[k] * p.fixed[old];
    else row.term(p.variable[old], original.coef[k]);
  }
  double low = -row.b;
  double high = -row.b;
  double norm2 = 0.0;
  for (std::size_t k = 0; k < row.count; ++k) {
    const auto v = static_cast<std::size_t>(row.index[k]);
    const double a = row.coef[k];
    low += a * (a > 0.0 ? p.lower[v] : p.upper[v]);
    high += a * (a > 0.0 ? p.upper[v] : p.lower[v]);
    norm2 += a * a;
  }
  if (high < -kFeasibilityTolerance) p.infeasible = true;
  if (low >= 0.0 || norm2 == 0.0) return;
  const double scale = 1.0 / std::sqrt(norm2);
  for (std::size_t k = 0; k < row.count; ++k) row.coef[k] *= scale;
  row.b *= scale;
  p.rows.push_back(row);
}

[[nodiscard]] bool build_variables(std::span<const JointInboxSlice> slices, Problem &p) {
  std::size_t total = 0;
  for (const auto &s : slices) {
    if (s.nodes.size() < 2u || s.nodes.size() != s.boxes.size() || !valid_bounds(s.bounds)) {
      return false;
    }
    if (s.terminal_discount && (!std::isfinite(*s.terminal_discount) ||
        *s.terminal_discount < s.bounds.discount_min || *s.terminal_discount > s.bounds.discount_max)) {
      return false;
    }
    if (!s.terminal_prepaid.empty() &&
        (!s.terminal_discount || s.terminal_prepaid.size() != s.nodes.size())) return false;
    if (s.nodes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) / 2u - total) {
      return false;
    }
    total += s.nodes.size();
  }
  p.variable.reserve(2u * total);
  p.fixed.reserve(2u * total);
  for (const auto &s : slices) {
    p.offsets.push_back(p.variable.size());
    for (std::size_t i = 0; i < s.nodes.size(); ++i) {
      const auto &node = s.nodes[i];
      const auto &box = s.boxes[i];
      if (!std::isfinite(node.strike) || node.strike <= 0.0 ||
          !std::isfinite(node.call) || !std::isfinite(node.put) ||
          (i > 0u && node.strike <= s.nodes[i - 1u].strike)) return false;
      if (!s.terminal_prepaid.empty() && (!std::isfinite(s.terminal_prepaid[i]) ||
          s.terminal_prepaid[i] < 0.0)) return false;
      for (const bool call : {true, false}) {
        const double box_lo = call ? box.call_lo : box.put_lo;
        const double box_hi = call ? box.call_hi : box.put_hi;
        if (!std::isfinite(box_lo) || !std::isfinite(box_hi) || box_lo > box_hi) return false;
        double floor = std::max(call ? s.bounds.spot - node.strike : node.strike - s.bounds.spot, 0.0);
        const double terminal_df = s.terminal_discount.value_or(call ? s.bounds.discount_max
                                                                    : s.bounds.discount_min);
        const double prepaid = !s.terminal_prepaid.empty() ? s.terminal_prepaid[i]
                                                          : (call ? s.bounds.prepaid_min : s.bounds.prepaid_max);
        floor = std::max(floor, call ? prepaid - terminal_df * node.strike
                                    : terminal_df * node.strike - prepaid);
        if (call && nondividend(s.bounds)) {
          floor = std::max(floor, s.bounds.spot - s.bounds.discount_min * node.strike);
        }
        const double lo = std::max(box_lo, floor);
        const double hi = std::min(box_hi, call ? s.bounds.prepaid_max :
                                                   s.bounds.discount_max * node.strike);
        if (!std::isfinite(lo) || !std::isfinite(hi)) return false;
        if (lo > hi) p.infeasible = true;
        p.fixed.push_back(lo);
        if (lo == hi || lo > hi) p.variable.push_back(-1);
        else {
          p.variable.push_back(static_cast<int>(p.target.size()));
          p.target.push_back(call ? node.call : node.put);
          p.lower.push_back(lo);
          p.upper.push_back(hi);
        }
      }
    }
  }
  return true;
}

void add_interpolated(Row &r, std::span<const AmericanPriceNode> nodes, std::size_t offset,
                      bool call, double strike, double sign) {
  const auto it = std::lower_bound(nodes.begin(), nodes.end(), strike,
      [](const AmericanPriceNode &n, double k) { return n.strike < k; });
  const std::size_t right = static_cast<std::size_t>(it - nodes.begin());
  if (right < nodes.size() && nodes[right].strike == strike) {
    r.term(coordinate(offset, right, call), sign);
    return;
  }
  const std::size_t left = right - 1u;
  const double f = (strike - nodes[left].strike) / (nodes[right].strike - nodes[left].strike);
  r.term(coordinate(offset, left, call), sign * (1.0 - f));
  r.term(coordinate(offset, right, call), sign * f);
}

void add_shape(Problem &p, const JointInboxSlice &s, std::size_t offset) {
  const auto nodes = s.nodes;
  const auto b = s.bounds;
  for (const bool call : {true, false}) {
    for (std::size_t i = 1; i + 1u < nodes.size(); ++i) {
      const double left = nodes[i].strike - nodes[i - 1u].strike;
      const double right = nodes[i + 1u].strike - nodes[i].strike;
      Row row;
      row.term(coordinate(offset, i - 1u, call), right / (left + right));
      row.term(coordinate(offset, i, call), -1.0);
      row.term(coordinate(offset, i + 1u, call), left / (left + right));
      add_row(p, row);
    }
    Row front;
    front.term(coordinate(offset, 1, call), 1.0);
    front.term(coordinate(offset, 0, call), -1.0);
    const double call_min = -(nondividend(b) ? b.discount_min : b.discount_max);
    front.b = (call ? call_min : 0.0) * (nodes[1].strike - nodes[0].strike);
    add_row(p, front);
    Row back;
    const std::size_t n = nodes.size();
    back.term(coordinate(offset, n - 2u, call), 1.0);
    back.term(coordinate(offset, n - 1u, call), -1.0);
    back.b = -(call ? 0.0 : b.discount_max) * (nodes[n - 1u].strike - nodes[n - 2u].strike);
    add_row(p, back);
  }
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    Row lower;
    lower.term(coordinate(offset, i, true), 1.0);
    lower.term(coordinate(offset, i, false), -1.0);
    lower.b = b.prepaid_min - nodes[i].strike * b.discount_max;
    add_row(p, lower);
    Row upper;
    upper.term(coordinate(offset, i, true), -1.0);
    upper.term(coordinate(offset, i, false), 1.0);
    upper.b = -(b.prepaid_max - nodes[i].strike * b.discount_min);
    add_row(p, upper);
  }
}

void add_nondividend(Problem &p, const JointInboxSlice &s, std::size_t offset) {
  if (!nondividend(s.bounds)) return;
  const auto nodes = s.nodes;
  for (std::size_t i = 0; i + 1u < nodes.size(); ++i) {
    const double scale = nodes[i].strike / (nodes[i + 1u].strike - nodes[i].strike);
    Row row;
    row.term(coordinate(offset, i, true), scale + 1.0);
    row.term(coordinate(offset, i, false), -scale - 1.0);
    row.term(coordinate(offset, i + 1u, true), -scale);
    row.term(coordinate(offset, i + 1u, false), scale);
    row.b = s.bounds.spot;
    add_row(p, row);
  }
  const double df = s.bounds.discount_min;
  const double low = nodes.front().strike;
  const double high = df * nodes.back().strike;
  const auto upper = [&](double k) {
    if (k < low || k > high) return;
    Row row;
    // Clamp only the last-bit multiply/divide drift at the domain endpoint.
    add_interpolated(row, nodes, offset, true, std::min(k / df, nodes.back().strike), 1.0);
    add_interpolated(row, nodes, offset, false, k, -1.0);
    row.b = s.bounds.spot - k;
    add_row(p, row);
  };
  for (const auto &node : nodes) { upper(node.strike); upper(df * node.strike); }
}

void add_calendars(Problem &p, std::span<const JointInboxSlice> slices) {
  for (std::size_t a = 0; a < slices.size(); ++a) {
    double bridge_low = -std::numeric_limits<double>::infinity();
    double bridge_high = std::numeric_limits<double>::infinity();
    for (std::size_t b = a + 1u; b < slices.size(); ++b) {
      if (b > a + 1u) {
        bridge_low = std::max(bridge_low, slices[b - 1u].nodes.front().strike);
        bridge_high = std::min(bridge_high, slices[b - 1u].nodes.back().strike);
      }
      const auto near = slices[a].nodes;
      const auto far = slices[b].nodes;
      const double low = std::max(near.front().strike, far.front().strike);
      const double high = std::min(near.back().strike, far.back().strike);
      const auto add = [&](double k) {
        if (k < low || k > high || (b > a + 1u && k >= bridge_low && k <= bridge_high)) return;
        for (const bool call : {true, false}) {
          Row row;
          add_interpolated(row, far, p.offsets[b], call, k, 1.0);
          add_interpolated(row, near, p.offsets[a], call, k, -1.0);
          add_row(p, row);
        }
      };
      std::size_t i = 0;
      std::size_t j = 0;
      for (; i < near.size() || j < far.size();) {
        const double k = j == far.size() || (i < near.size() && near[i].strike < far[j].strike)
                             ? near[i].strike : far[j].strike;
        if (i < near.size() && near[i].strike == k) ++i;
        if (j < far.size() && far[j].strike == k) ++j;
        add(k);
      }
    }
  }
}

// Reuses the symbolic structure and coefficient positions at each iteration.
class NewtonSystem {
public:
  explicit NewtonSystem(const Problem &p) : p_(p), matrix_(static_cast<int>(p.target.size()),
                                                         static_cast<int>(p.target.size())) {
    std::vector<Eigen::Triplet<double>> entries;
    for (int i = 0; i < matrix_.rows(); ++i) entries.emplace_back(i, i, 1.0);
    for (const Row &r : p.rows) {
      for (std::size_t a = 0; a < r.count; ++a) {
        for (std::size_t b = 0; b <= a; ++b) {
          entries.emplace_back(std::max(r.index[a], r.index[b]),
                               std::min(r.index[a], r.index[b]), 1.0);
        }
      }
    }
    matrix_.setFromTriplets(entries.begin(), entries.end());
    matrix_.makeCompressed();
    for (int i = 0; i < matrix_.rows(); ++i) diagonal_.push_back(position(i, i));
    for (const Row &r : p.rows) {
      for (std::size_t a = 0; a < r.count; ++a) {
        for (std::size_t b = 0; b <= a; ++b) {
          terms_.push_back({position(std::max(r.index[a], r.index[b]),
                                    std::min(r.index[a], r.index[b])), r.coef[a] * r.coef[b]});
        }
      }
    }
    factor_.analyzePattern(matrix_);
  }

  [[nodiscard]] bool factor(const Vector &s, const Vector &z) {
    std::fill_n(matrix_.valuePtr(), matrix_.nonZeros(), 0.0);
    for (const int k : diagonal_) matrix_.valuePtr()[k] = 1.0;
    std::size_t t = 0;
    for (std::size_t j = 0; j < p_.rows.size(); ++j) {
      const auto row = static_cast<Eigen::Index>(j);
      const double weight = z[row] / s[row];
      const auto count = p_.rows[j].count;
      for (std::size_t k = 0; k < count * (count + 1u) / 2u; ++k, ++t) {
        matrix_.valuePtr()[terms_[t].position] += weight * terms_[t].value;
      }
    }
    factor_.factorize(matrix_);
    return factor_.info() == Eigen::Success;
  }

  [[nodiscard]] bool direction(const Vector &s, const Vector &z, const Vector &rp,
                                const Vector &rd, const Vector &rc,
                                Vector &dx, Vector &ds, Vector &dz) const {
    Vector rhs = -rd;
    for (std::size_t j = 0; j < p_.rows.size(); ++j) {
      const auto row = static_cast<Eigen::Index>(j);
      const double v = (rc[row] + z[row] * rp[row]) / s[row];
      const Row &r = p_.rows[j];
      for (std::size_t k = 0; k < r.count; ++k) rhs[r.index[k]] -= r.coef[k] * v;
    }
    dx = factor_.solve(rhs);
    if (!dx.allFinite()) return false;
    for (std::size_t j = 0; j < p_.rows.size(); ++j) {
      const auto row = static_cast<Eigen::Index>(j);
      double d = rp[row];
      const Row &r = p_.rows[j];
      for (std::size_t k = 0; k < r.count; ++k) d += r.coef[k] * dx[r.index[k]];
      ds[row] = d;
      dz[row] = (-rc[row] - z[row] * d) / s[row];
    }
    return ds.allFinite() && dz.allFinite();
  }

private:
  [[nodiscard]] int position(int row, int column) const {
    const int begin = matrix_.outerIndexPtr()[column];
    const int end = matrix_.outerIndexPtr()[column + 1];
    return static_cast<int>(std::lower_bound(matrix_.innerIndexPtr() + begin,
                                            matrix_.innerIndexPtr() + end, row) - matrix_.innerIndexPtr());
  }
  struct Term { int position; double value; };
  const Problem &p_;
  Matrix matrix_;
  Eigen::SimplicialLDLT<Matrix, Eigen::Lower> factor_;
  std::vector<int> diagonal_;
  std::vector<Term> terms_;
};

[[nodiscard]] double step_bound(const Vector &positive, const Vector &delta) {
  double step = 1.0;
  for (Eigen::Index i = 0; i < positive.size(); ++i) {
    if (delta[i] < 0.0) step = std::min(step, -positive[i] / delta[i]);
  }
  return step;
}

[[nodiscard]] bool solve(const Problem &p, std::size_t budget, Vector &x, std::size_t &used,
                         JointInboxDiagnostics &diagnostics) {
  const auto n = static_cast<Eigen::Index>(p.target.size());
  const auto m = static_cast<Eigen::Index>(p.rows.size());
  x.resize(n);
  for (Eigen::Index i = 0; i < n; ++i) {
    const auto k = static_cast<std::size_t>(i);
    x[i] = std::clamp(p.target[k], p.lower[k], p.upper[k]);
  }
  if (m == 0) return true;
  if (std::all_of(p.rows.begin(), p.rows.end(), [&](const Row &r) {
        return r.value(x) >= -kFeasibilityTolerance;
      })) {
    return true;
  }
  Vector s(m), z = Vector::Ones(m), rp(m), rd(n), rc(m), dx(n), ds(m), dz(m);
  Vector ds_aff(m), dz_aff(m);
  for (Eigen::Index j = 0; j < m; ++j) s[j] = std::max(1.0, p.rows[static_cast<std::size_t>(j)].value(x));
  NewtonSystem system(p);
  for (std::size_t iteration = 0; iteration < budget; ++iteration) {
    used = iteration + 1u;
    for (Eigen::Index i = 0; i < n; ++i) rd[i] = x[i] - p.target[static_cast<std::size_t>(i)];
    double violation = 0.0;
    for (Eigen::Index j = 0; j < m; ++j) {
      const Row &r = p.rows[static_cast<std::size_t>(j)];
      const double slack = r.value(x);
      violation = std::max(violation, -slack);
      rp[j] = slack - s[j];
      for (std::size_t k = 0; k < r.count; ++k) rd[r.index[k]] -= r.coef[k] * z[j];
      rc[j] = s[j] * z[j];
    }
    const double mu = rc.mean();
    diagnostics.primal_residual = std::max(violation, rp.lpNorm<Eigen::Infinity>());
    diagnostics.dual_residual = rd.lpNorm<Eigen::Infinity>();
    diagnostics.duality_gap = mu;
    // The production contract is a certified feasible price surface. Tracking the
    // target regularizes the search; proving the least-squares optimum is not
    // required, and forcing complementarity to zero at a boundary-only feasible
    // face can needlessly destroy the Newton system's conditioning.
    if (diagnostics.primal_residual <= kFeasibilityTolerance) return true;
    if (!system.factor(s, z) || !system.direction(s, z, rp, rd, rc, dx, ds_aff, dz_aff)) return false;
    const double ap = step_bound(s, ds_aff);
    const double ad = step_bound(z, dz_aff);
    const double mu_aff = ((s + ap * ds_aff).array() * (z + ad * dz_aff).array()).mean();
    const double ratio = std::clamp(mu_aff / mu, 0.0, 1.0);
    const double sigma = ratio * ratio * ratio;
    rc.array() += ds_aff.array() * dz_aff.array() - sigma * mu;
    if (!system.direction(s, z, rp, rd, rc, dx, ds, dz)) return false;
    const double alpha_p = std::min(1.0, 0.995 * step_bound(s, ds));
    const double alpha_d = std::min(1.0, 0.995 * step_bound(z, dz));
    x += alpha_p * dx;
    s += alpha_p * ds;
    z += alpha_d * dz;
    if (!x.allFinite() || !s.allFinite() || !z.allFinite() || s.minCoeff() <= 0.0 || z.minCoeff() <= 0.0) {
      return false;
    }
  }
  return false;
}

[[nodiscard]] bool certify(std::span<const JointInboxSlice> slices, const Problem &p,
                            const Vector &x, std::vector<std::vector<AmericanPriceNode>> &nodes) {
  std::vector<AmericanPriceSlice> certified;
  certified.reserve(slices.size());
  for (std::size_t s = 0; s < slices.size(); ++s) {
    nodes.emplace_back(slices[s].nodes.begin(), slices[s].nodes.end());
    for (std::size_t i = 0; i < nodes.back().size(); ++i) {
      for (const bool call : {true, false}) {
        const auto old = static_cast<std::size_t>(coordinate(p.offsets[s], i, call));
        const int v = p.variable[old];
        const double value = v < 0 ? p.fixed[old] : std::clamp(x[v], p.lower[static_cast<std::size_t>(v)],
                                                                  p.upper[static_cast<std::size_t>(v)]);
        (call ? nodes.back()[i].call : nodes.back()[i].put) = value;
      }
    }
    auto created = AmericanPriceSlice::create(nodes.back(), slices[s].bounds);
    if (!created) return false;
    if (nondividend(slices[s].bounds) &&
        !check_nondividend_american(*created, slices[s].bounds.discount_min)) return false;
    certified.push_back(std::move(*created));
  }
  for (std::size_t a = 0; a < certified.size(); ++a) {
    for (std::size_t b = a + 1u; b < certified.size(); ++b) {
      if (!check_american_calendar(certified[a], certified[b])) return false;
    }
  }
  return true;
}

} // namespace

JointProjectionResult project_joint_in_box(std::span<const JointInboxSlice> slices,
                                           std::size_t max_iterations,
                                           JointInboxDiagnostics *diagnostics) {
  JointProjectionResult result;
  JointInboxDiagnostics local;
  JointInboxDiagnostics &record = diagnostics ? *diagnostics : local;
  record = {};
  if (max_iterations == 0u || slices.empty()) return result;
  Problem p;
  if (!build_variables(slices, p)) return result;
  if (!p.infeasible) {
    for (std::size_t s = 0; s < slices.size(); ++s) {
      add_shape(p, slices[s], p.offsets[s]);
      add_nondividend(p, slices[s], p.offsets[s]);
    }
    add_calendars(p, slices);
  }
  if (p.infeasible) {
    result.outcome = JointProjectionOutcome::InfeasibleData;
    return result;
  }
  // Box rows are retained even though the bound test regards them as redundant;
  // they supply each free variable's interior barrier and keep the iterates bounded.
  for (std::size_t i = 0; i < p.target.size(); ++i) {
    Row lo;
    lo.term(static_cast<int>(i), 1.0);
    lo.b = p.lower[i];
    p.rows.push_back(lo);
    Row hi;
    hi.term(static_cast<int>(i), -1.0);
    hi.b = -p.upper[i];
    p.rows.push_back(hi);
  }
  Vector x;
  std::vector<std::vector<AmericanPriceNode>> nodes;
  record.variables = p.target.size();
  record.constraints = p.rows.size();
  if (!solve(p, max_iterations, x, result.sweeps, record) || !certify(slices, p, x, nodes)) {
    result.outcome = JointProjectionOutcome::NonConverged;
    return result;
  }
  record.certificate_passed = true;
  for (std::size_t s = 0; s < slices.size(); ++s) {
    std::copy(nodes[s].begin(), nodes[s].end(), slices[s].nodes.begin());
  }
  result.outcome = JointProjectionOutcome::Feasible;
  return result;
}

} // namespace atx::vol::detail
