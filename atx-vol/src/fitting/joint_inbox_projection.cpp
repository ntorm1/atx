#include "fitting/joint_inbox_projection.hpp"

#if defined(ATX_VOL_JOINT_ROW_CENSUS_RESEARCH)
#include "fitting/joint_row_census.hpp"
#endif

#if defined(ATX_VOL_JOINT_CENTRALITY_RESEARCH)
#include "../../bench/joint-centrality/qp_control.hpp"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <new>
#include <optional>
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

class ProfileSpan {
public:
  explicit ProfileSpan(std::uint64_t *target) : target_(target) {
    if (target_) start_ = Clock::now();
  }
  ~ProfileSpan() { finish(); }
  ProfileSpan(const ProfileSpan &) = delete;
  ProfileSpan &operator=(const ProfileSpan &) = delete;
  void finish() {
    if (!target_) return;
    *target_ += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count());
    target_ = nullptr;
  }
private:
  using Clock = std::chrono::steady_clock;
  std::uint64_t *target_;
  Clock::time_point start_{};
};

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

// Feasible rows need only numerical data in every residual/direction scan.
// Contradiction provenance belongs to the temporary add_row input, not this
// repeatedly streamed storage. Preserve the coefficient and accumulation order.
static_assert(sizeof(Row) == 64u);

struct Problem {
  std::vector<std::size_t> offsets;
  std::vector<int> variable; // original coordinate -> free coordinate, -1 if fixed
  std::vector<double> fixed;
  std::vector<double> lower, upper, target;
  std::vector<Row> rows;
  bool infeasible = false;
  JointProjectionConflict conflict{};
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
void add_row(Problem &p, const Row &original,
             const JointProjectionConflict &conflict = {JointConflictKind::LinearConstraint}) {
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
  if (high < -kFeasibilityTolerance) {
    p.infeasible = true;
    if (-high > p.conflict.excess) {
      p.conflict = conflict;
      p.conflict.excess = -high;
    }
  }
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
        *s.terminal_discount < s.bounds.discount_min ||
        *s.terminal_discount > s.bounds.discount_max)) {
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
  for (std::size_t expiry = 0; expiry < slices.size(); ++expiry) {
    const auto &s = slices[expiry];
    p.offsets.push_back(p.variable.size());
    for (std::size_t i = 0; i < s.nodes.size(); ++i) {
      const auto &node = s.nodes[i];
      const auto &box = s.boxes[i];
      if (!std::isfinite(node.strike) || node.strike <= 0.0 ||
          !std::isfinite(node.call) || !std::isfinite(node.put) ||
          (i > 0u && node.strike <= s.nodes[i - 1u].strike)) return false;
      if (!s.terminal_prepaid.empty()) {
        const double value = s.terminal_prepaid[i];
        const double roundoff = 64.0 * std::numeric_limits<double>::epsilon() *
                                std::max(1.0, s.bounds.prepaid_max);
        if (!std::isfinite(value) || value < 0.0 ||
            value < s.bounds.prepaid_min - roundoff ||
            value > s.bounds.prepaid_max + roundoff) return false;
      }
      for (const bool call : {true, false}) {
        const double box_lo = call ? box.call_lo : box.put_lo;
        const double box_hi = call ? box.call_hi : box.put_hi;
        if (!std::isfinite(box_lo) || !std::isfinite(box_hi) || box_lo > box_hi) return false;
        double floor = std::max(call ? s.bounds.spot - node.strike
                                    : node.strike - s.bounds.spot, 0.0);
        const double terminal_df = s.terminal_discount.value_or(call ? s.bounds.discount_max
                                                                    : s.bounds.discount_min);
        const double prepaid = !s.terminal_prepaid.empty()
                                   ? s.terminal_prepaid[i]
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
        if (lo > hi) {
          p.infeasible = true;
          if (lo - hi > p.conflict.excess) {
            p.conflict = {JointConflictKind::EmptyBox, expiry, i, expiry,
                          node.strike, lo - hi};
          }
        }
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
          add_row(p, row, {JointConflictKind::CalendarBox, a, 0, b, k, 0.0});
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
  NewtonSystem(const Problem &p, JointInboxProfile *profile)
      : p_(p), profile_(profile), matrix_(static_cast<int>(p.target.size()),
                                       static_cast<int>(p.target.size())) {
    ProfileSpan timer(profile_ ? &profile_->symbolic_ns : nullptr);
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
    if (profile_) profile_->matrix_nonzeros = static_cast<std::size_t>(matrix_.nonZeros());
  }

  [[nodiscard]] bool factor(const Vector &s, const Vector &z) {
    ProfileSpan assembly(profile_ ? &profile_->assembly_ns : nullptr);
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
    assembly.finish();
    // Finite inputs can overflow A^T diag(z/s) A. Eigen may still report a
    // successful factor and a finite but invalid direction for an infinite H.
    if (!std::all_of(matrix_.valuePtr(), matrix_.valuePtr()+matrix_.nonZeros(),
                     [](double value) { return std::isfinite(value); })) return false;
    ProfileSpan numeric(profile_ ? &profile_->factor_ns : nullptr);
    factor_.factorize(matrix_);
    if (profile_) {
      ++profile_->factorizations;
      if (factor_.info() == Eigen::Success) {
        profile_->factor_nonzeros =
            static_cast<std::size_t>(factor_.matrixL().nestedExpression().nonZeros());
      }
    }
    return factor_.info() == Eigen::Success;
  }

  [[nodiscard]] bool direction(const Vector &s, const Vector &z, const Vector &rp,
                                const Vector &rd, const Vector &rc,
                                Vector &dx, Vector &ds, Vector &dz,
                                Vector *rhs_scratch = nullptr) const {
    ProfileSpan rhs_timer(profile_ ? &profile_->rhs_ns : nullptr);
    if (profile_) ++profile_->directions;
    Vector owned_rhs;
    Vector &rhs = rhs_scratch ? *rhs_scratch : owned_rhs;
    rhs = -rd;
    for (std::size_t j = 0; j < p_.rows.size(); ++j) {
      const auto row = static_cast<Eigen::Index>(j);
      const double v = (rc[row] + z[row] * rp[row]) / s[row];
      const Row &r = p_.rows[j];
      for (std::size_t k = 0; k < r.count; ++k) rhs[r.index[k]] -= r.coef[k] * v;
    }
    rhs_timer.finish();
    ProfileSpan solve_timer(profile_ ? &profile_->linear_solve_ns : nullptr);
    dx = factor_.solve(rhs);
    solve_timer.finish();
    if (!dx.allFinite()) return false;
    ProfileSpan slack_timer(profile_ ? &profile_->slack_direction_ns : nullptr);
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
    const auto *found = std::lower_bound(matrix_.innerIndexPtr() + begin,
                                         matrix_.innerIndexPtr() + end, row);
    return static_cast<int>(found - matrix_.innerIndexPtr());
  }
  struct Term { int position; double value; };
  const Problem &p_;
  JointInboxProfile *profile_;
  Matrix matrix_;
  Eigen::SimplicialLDLT<Matrix, Eigen::Lower> factor_;
  std::vector<int> diagonal_;
  std::vector<Term> terms_;
};

#if defined(ATX_VOL_JOINT_CENTRALITY_RESEARCH)
std::span<double> values(Vector &vector) noexcept {
  return {vector.data(), static_cast<std::size_t>(vector.size())};
}

// All trial directions live here until the primitive accepts a complete triple.
// Iterates are borrowed read-only; the factor belongs to the original solve.
struct CentralityWorkspace {
  const NewtonSystem &system;
  const Vector &s, &z;
  JointInboxProfile *profile;
  Vector zero_rp, zero_rd, residual, dx, ds, dz, rhs;

  CentralityWorkspace(const NewtonSystem &factor, const Vector &slack, const Vector &dual,
                      Eigen::Index variables, JointInboxProfile *observation)
      : system(factor), s(slack), z(dual), profile(observation),
        zero_rp(Vector::Zero(slack.size())), zero_rd(Vector::Zero(variables)),
        residual(slack.size()), dx(variables), ds(slack.size()), dz(slack.size()), rhs(variables) {}

  research::CentralityDirection direction() noexcept {
    return {values(dx), values(ds), values(dz)};
  }

  static bool homogeneous(void *context, std::span<const double> rc,
                          research::CentralityDirection output) {
    auto &work = *static_cast<CentralityWorkspace *>(context);
    if (rc.data() != work.residual.data() || rc.size() != values(work.residual).size() ||
        output.x.data() != work.dx.data() || output.x.size() != values(work.dx).size() ||
        output.s.data() != work.ds.data() || output.s.size() != values(work.ds).size() ||
        output.z.data() != work.dz.data() || output.z.size() != values(work.dz).size()) {
      return false;
    }
    try {
      return work.system.direction(work.s, work.z, work.zero_rp, work.zero_rd, work.residual,
                                   work.dx, work.ds, work.dz, &work.rhs);
    } catch (const std::bad_alloc &) {
      if (work.profile) ++work.profile->centrality_allocation_refusals;
      return false;
    }
  }

  void improve(Vector &primal, Vector &slack, Vector &dual, double mu_target) {
    ProfileSpan timer(profile ? &profile->centrality_ns : nullptr);
    research::CentralityWork work;
    const bool valid = research::improve_centrality(
        {s.data(), static_cast<std::size_t>(s.size())},
        {z.data(), static_cast<std::size_t>(z.size())}, mu_target,
        {values(primal), values(slack), values(dual)},
        {values(residual), direction()}, homogeneous, this, work);
    if (!profile) return;
    profile->centrality_attempts += work.attempts;
    profile->centrality_solves += work.solves;
    profile->centrality_accepted += work.accepted;
    profile->centrality_zero_target += work.zero_target;
    profile->centrality_nonfinite += work.nonfinite;
    profile->centrality_nonimproving += work.nonimproving;
    profile->centrality_solve_refused += work.solve_refused;
    profile->centrality_invalid_target += work.invalid_target;
    if (!valid) ++profile->centrality_input_refusals;
  }
};

void prepare_centrality(std::optional<CentralityWorkspace> &scratch,
                        const NewtonSystem &system, const Vector &s, const Vector &z,
                        Eigen::Index variables, JointInboxProfile *profile) {
  ProfileSpan timer(profile ? &profile->centrality_setup_ns : nullptr);
  const auto n = static_cast<std::size_t>(variables), m = static_cast<std::size_t>(s.size());
  constexpr auto limit = (std::numeric_limits<std::size_t>::max()-sizeof(CentralityWorkspace)) /
      sizeof(double);
  if (m > limit/4u || n > (limit-4u*m)/3u) {
    if (profile) ++profile->centrality_setup_refusals;
    return;
  }
  if (profile) {
    profile->centrality_workspace_bytes = sizeof(CentralityWorkspace)+(4u*m+3u*n)*sizeof(double);
  }
  try {
    scratch.emplace(system, s, z, variables, profile);
  } catch (const std::bad_alloc &) {
    if (profile) {
      ++profile->centrality_setup_refusals;
      ++profile->centrality_allocation_refusals;
    }
  }
}
#endif

[[nodiscard]] double step_bound(const Vector &positive, const Vector &delta) {
  double step = 1.0;
  for (Eigen::Index i = 0; i < positive.size(); ++i) {
    if (delta[i] < 0.0) step = std::min(step, -positive[i] / delta[i]);
  }
  return step;
}

#if defined(ATX_VOL_JOINT_ROW_CENSUS_RESEARCH)
struct CensusContext {
  research::JointRowObserver *observer;
  std::span<const JointInboxSlice> slices;
  const research::JointRowRawSource *raw;
};
void observe_built(const Problem &p, const Vector &x, std::size_t budget,
                   const CensusContext &census) noexcept {
  if (!census.observer) return;
  struct RowSource { const Problem &problem; const Vector &initial; } rows{p, x};
  const auto at = [](const void *context, std::size_t i) noexcept {
    const auto &source = *static_cast<const RowSource *>(context);
    const auto &row = source.problem.rows[i];
    return research::JointRowView{row.index, row.coef, row.count, row.b,
                                 row.value(source.initial)};
  };
  const auto slice_at = [](const void *context, std::size_t i) noexcept {
    const auto slices = *static_cast<const std::span<const JointInboxSlice> *>(context);
    const auto &slice = slices[i];
    return research::JointRowSliceView{slice.nodes, slice.boxes, slice.bounds,
                                      slice.terminal_discount, slice.terminal_prepaid};
  };
  census.observer->built({census.slices.size(), &census.slices, slice_at,
      p.offsets, p.variable, p.fixed,
      p.lower, p.upper, p.target, std::span(x.data(), static_cast<std::size_t>(x.size())),
      &rows, at, p.rows.size(), budget, kFeasibilityTolerance,
      census.raw ? *census.raw : research::JointRowRawSource{}});
}
#endif

[[nodiscard]] bool solve(const Problem &p, std::size_t budget, Vector &x, std::size_t &used,
                         JointInboxDiagnostics &diagnostics, JointInboxProfile *profile,
                          bool centrality
#if defined(ATX_VOL_JOINT_ROW_CENSUS_RESEARCH)
                          , const CensusContext &census
#endif
                          ) {
  const auto n = static_cast<Eigen::Index>(p.target.size());
  const auto m = static_cast<Eigen::Index>(p.rows.size());
  x.resize(n);
  for (Eigen::Index i = 0; i < n; ++i) {
    const auto k = static_cast<std::size_t>(i);
    x[i] = std::clamp(p.target[k], p.lower[k], p.upper[k]);
  }
#if defined(ATX_VOL_JOINT_ROW_CENSUS_RESEARCH)
  observe_built(p, x, budget, census);
#endif
  if (m == 0) return true;
  if (std::all_of(p.rows.begin(), p.rows.end(), [&](const Row &r) {
        return r.value(x) >= -kFeasibilityTolerance;
      })) {
    return true;
  }
  Vector s(m), z = Vector::Ones(m), rp(m), rd(n), rc(m), dx(n), ds(m), dz(m);
  Vector ds_aff(m), dz_aff(m);
  for (Eigen::Index j = 0; j < m; ++j) {
    s[j] = std::max(1.0, p.rows[static_cast<std::size_t>(j)].value(x));
  }
  NewtonSystem system(p, profile);
#if defined(ATX_VOL_JOINT_CENTRALITY_RESEARCH)
  std::optional<CentralityWorkspace> correction;
  if (centrality) prepare_centrality(correction, system, s, z, n, profile);
#else
  static_cast<void>(centrality);
#endif
  for (std::size_t iteration = 0; iteration < budget; ++iteration) {
    ProfileSpan iteration_timer(profile ? &profile->iteration_ns : nullptr);
    ProfileSpan residual_timer(profile ? &profile->residual_ns : nullptr);
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
    residual_timer.finish();
    // The production contract is a certified feasible price surface. Tracking the
    // target regularizes the search; proving the least-squares optimum is not
    // required, and forcing complementarity to zero at a boundary-only feasible
    // face can needlessly destroy the Newton system's conditioning.
    if (diagnostics.primal_residual <= kFeasibilityTolerance) return true;
    if (!system.factor(s, z) ||
        !system.direction(s, z, rp, rd, rc, dx, ds_aff, dz_aff)) return false;
    const double ap = step_bound(s, ds_aff);
    const double ad = step_bound(z, dz_aff);
    const double mu_aff = ((s + ap * ds_aff).array() * (z + ad * dz_aff).array()).mean();
    const double ratio = std::clamp(mu_aff / mu, 0.0, 1.0);
    const double sigma = ratio * ratio * ratio;
    rc.array() += ds_aff.array() * dz_aff.array() - sigma * mu;
    if (!system.direction(s, z, rp, rd, rc, dx, ds, dz)) return false;
#if defined(ATX_VOL_JOINT_CENTRALITY_RESEARCH)
    if (correction) correction->improve(dx, ds, dz, sigma * mu);
#endif
    ProfileSpan update_timer(profile ? &profile->update_ns : nullptr);
    const double alpha_p = std::min(1.0, 0.995 * step_bound(s, ds));
    const double alpha_d = std::min(1.0, 0.995 * step_bound(z, dz));
    x += alpha_p * dx;
    s += alpha_p * ds;
    z += alpha_d * dz;
    if (!x.allFinite() || !s.allFinite() || !z.allFinite() ||
        s.minCoeff() <= 0.0 || z.minCoeff() <= 0.0) {
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
        const auto free = static_cast<std::size_t>(v);
        const double value = v < 0 ? p.fixed[old] : std::clamp(x[v], p.lower[free], p.upper[free]);
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

bool joint_centrality_compiled() noexcept {
#if defined(ATX_VOL_JOINT_CENTRALITY_RESEARCH)
  return true;
#else
  return false;
#endif
}

bool recover_terminal_floor_asks(const JointInboxSlice &slice,
                                 std::span<const std::uint8_t> quoted,
                                 std::span<AmericanPriceBox> boxes,
                                 TerminalFloorRecovery &record) {
  record = {};
  if (!slice.terminal_discount || slice.terminal_prepaid.size() != slice.nodes.size() ||
      quoted.size() != slice.nodes.size() || boxes.size() != slice.nodes.size()) return false;
  Problem checked;
  if (!build_variables(std::span(&slice, 1), checked)) return false;
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    const double strike = slice.nodes[i].strike;
    const double prepaid = slice.terminal_prepaid[i];
    const double df = *slice.terminal_discount;
    const double allowance = 1e-8 + 1e-9 * std::max(slice.bounds.spot, strike);
    for (const bool call : {true, false}) {
      if ((quoted[i] & (call ? 1u : 2u)) == 0u) continue;
      const double intrinsic = call ? slice.bounds.spot - strike : strike - slice.bounds.spot;
      const double european = call ? prepaid - df * strike : df * strike - prepaid;
      const double floor = std::max({0.0, intrinsic, european});
      double &ask = call ? boxes[i].call_hi : boxes[i].put_hi;
      if (ask >= floor) continue;
      const double raised = floor + allowance;
      record.max_ask_raise = std::max(record.max_ask_raise, raised - ask);
      ask = raised;
      ++record.raised_asks;
    }
  }
  return true;
}

JointProjectionResult project_joint_in_box(std::span<const JointInboxSlice> slices,
                                           std::size_t max_iterations,
                                           JointInboxDiagnostics *diagnostics,
                                           JointInboxProfile *profile,
                                            JointDirectionMode mode,
                                            research::JointRowObserver *observer,
                                            const research::JointRowRawSource *raw_census) {
#if defined(ATX_VOL_JOINT_ROW_CENSUS_RESEARCH)
  // Resolve only the calling thread's live owner. An arbitrary supplied pointer
  // never grants permission to dereference another thread's recorder.
  auto *current_observer = research::joint_row_census_observer();
  if (observer && observer != current_observer) {
    if (current_observer) current_observer->invalidate(research::JointRowCaptureError::WrongPhase);
    current_observer = nullptr;
  }
  observer = current_observer;
  if (observer) observer->attempt();
#else
  static_cast<void>(observer);
  static_cast<void>(raw_census);
#endif
  if (profile) *profile = {};
  ProfileSpan total_timer(profile ? &profile->total_ns : nullptr);
  ProfileSpan build_timer(profile ? &profile->build_ns : nullptr);
  JointProjectionResult result;
  JointInboxDiagnostics local;
  JointInboxDiagnostics &record = diagnostics ? *diagnostics : local;
  record = {};
  bool centrality = false;
  switch (mode) {
    case JointDirectionMode::Compiled: centrality = joint_centrality_compiled(); break;
    case JointDirectionMode::Original: break;
    case JointDirectionMode::Centrality:
      if (!joint_centrality_compiled()) return result;
      centrality = true;
      break;
    default: return result;
  }
  if (profile) profile->centrality_enabled = centrality;
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
    result.conflict = p.conflict;
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
  if (profile) {
    profile->bound_rows = 2u * p.target.size();
    profile->row_storage_bytes = sizeof(Row) * p.rows.capacity();
    for (const Row &row : p.rows) ++profile->row_arities[row.count];
  }
  build_timer.finish();
  if (!solve(p, max_iterations, x, result.sweeps, record, profile, centrality
#if defined(ATX_VOL_JOINT_ROW_CENSUS_RESEARCH)
             , CensusContext{observer, slices, raw_census}
#endif
             )) {
    result.outcome = JointProjectionOutcome::NonConverged;
    return result;
  }
  ProfileSpan certificate_timer(profile ? &profile->certificate_ns : nullptr);
  if (!certify(slices, p, x, nodes)) {
    result.outcome = JointProjectionOutcome::NonConverged;
    return result;
  }
  certificate_timer.finish();
  record.certificate_passed = true;
#if defined(ATX_VOL_JOINT_ROW_CENSUS_RESEARCH)
  if (observer) observer->published({nodes,
      std::span(x.data(), static_cast<std::size_t>(x.size())), result.sweeps, &record, profile});
#endif
  for (std::size_t s = 0; s < slices.size(); ++s) {
    std::copy(nodes[s].begin(), nodes[s].end(), slices[s].nodes.begin());
  }
  result.outcome = JointProjectionOutcome::Feasible;
  return result;
}

} // namespace atx::vol::detail

#if defined(ATX_VOL_JOINT_CENTRALITY_RESEARCH)
namespace atx::vol::research {
bool solve_joint_homogeneous_control(const JointHomogeneousInput &input,
    CentralityDirection output, JointHomogeneousWork &work) {
  work = {};
  const auto n = input.variables, m = input.s.size();
  if (n == 0 || n > 4 || m == 0 || m > 32 || input.a.size() != m*n ||
      input.z.size() != m || input.rc.size() != m || output.x.size() != n ||
      output.s.size() != m || output.z.size() != m) return false;
  const auto finite = [](double v) { return std::isfinite(v); };
  const auto positive = [](double v) { return v > 0. && std::isfinite(v); };
  if (!std::all_of(input.a.begin(), input.a.end(), finite) ||
      !std::all_of(input.rc.begin(), input.rc.end(), finite) ||
      !std::all_of(input.s.begin(), input.s.end(), positive) ||
      !std::all_of(input.z.begin(), input.z.end(), positive)) return false;
  try {
    detail::Problem problem;
    problem.target.resize(n);
    problem.rows.resize(m);
    for (std::size_t row = 0; row < m; ++row) {
      for (std::size_t col = 0; col < n; ++col) {
        problem.rows[row].term(static_cast<int>(col), input.a[row*n+col]);
      }
    }
    const Eigen::Index rows = static_cast<Eigen::Index>(m);
    const detail::Vector s = Eigen::Map<const detail::Vector>(input.s.data(), rows);
    const detail::Vector z = Eigen::Map<const detail::Vector>(input.z.data(), rows);
    detail::NewtonSystem system(problem, nullptr);
    detail::CentralityWorkspace scratch(system, s, z, static_cast<Eigen::Index>(n), nullptr);
    std::copy(input.rc.begin(), input.rc.end(), scratch.residual.data());
    ++work.factors;
    if (!system.factor(s, z)) return false;
    ++work.directions;
    if (!detail::CentralityWorkspace::homogeneous(&scratch, detail::values(scratch.residual),
                                                  scratch.direction())) return false;
    std::copy_n(scratch.dx.data(), n, output.x.begin());
    std::copy_n(scratch.ds.data(), m, output.s.begin());
    std::copy_n(scratch.dz.data(), m, output.z.begin());
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}
} // namespace atx::vol::research
#endif
