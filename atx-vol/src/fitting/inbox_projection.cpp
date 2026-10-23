#include "fitting/inbox_projection.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

const char *to_string(QuoteSliceInboxOutcome value) noexcept {
  switch (value) {
  case QuoteSliceInboxOutcome::NotAttempted:
    return "not_attempted";
  case QuoteSliceInboxOutcome::Applied:
    return "applied";
  case QuoteSliceInboxOutcome::InfeasibleData:
    return "infeasible_data";
  case QuoteSliceInboxOutcome::NonConverged:
    return "nonconverged";
  case QuoteSliceInboxOutcome::NotCertified:
    return "not_certified";
  case QuoteSliceInboxOutcome::InvalidInput:
    return "invalid_input";
  }
  return "unknown";
}

const char *to_string(QuoteSliceInboxConflict value) noexcept {
  switch (value) {
  case QuoteSliceInboxConflict::None:
    return "none";
  case QuoteSliceInboxConflict::EmptyBox:
    return "empty_box";
  case QuoteSliceInboxConflict::NoConvexCurve:
    return "no_convex_curve";
  case QuoteSliceInboxConflict::AffineBox:
    return "affine_box";
  case QuoteSliceInboxConflict::Coupled:
    return "coupled";
  }
  return "unknown";
}

const char *to_string(QuoteSliceBoxRefusal value) noexcept {
  switch (value) {
  case QuoteSliceBoxRefusal::None:
    return "none";
  case QuoteSliceBoxRefusal::ExerciseBounds:
    return "exercise_bounds";
  case QuoteSliceBoxRefusal::NoConvexCurve:
    return "no_convex_curve";
  case QuoteSliceBoxRefusal::NotSettled:
    return "not_settled";
  case QuoteSliceBoxRefusal::CertificateRefused:
    return "certificate_refused";
  case QuoteSliceBoxRefusal::Other:
    return "other";
  }
  return "unknown";
}

namespace detail {
namespace {

// Interleaved variables: node i's call is 2i, its put 2i + 1. Every constraint row touches at
// most four variables within four positions of each other (a side's convexity triple spans
// 2(i - 1)..2(i + 1)), so the Newton system is banded with this half-bandwidth.
constexpr std::size_t kBand = 4;
constexpr double kBarrierGrowth = 16.0;      // t multiplier per outer iteration
constexpr double kCentred = 1.0e-10;         // lambda^2 / 2 that ends a centring
constexpr std::size_t kCentringSteps = 100;  // Newton steps per centring at most
constexpr double kPhase2Gap = 1.0e-5;        // m / t ending phase II, half-spread^2 units
constexpr double kWeightFloor = 5.0e-4;      // smallest half-width a weight uses (price units)
constexpr double kSnap = 1.0e-6;             // a move below this many half-widths is bias

[[nodiscard]] double allowance(double spot, double strike) noexcept {
  return 1.0e-8 + 1.0e-9 * std::max(spot, strike); // AmericanPriceSlice's fixed allowance
}

[[nodiscard]] bool valid_bounds(const AmericanPriceBounds &b) noexcept {
  return std::isfinite(b.spot) && b.spot > 0.0 && std::isfinite(b.discount_min) &&
         b.discount_min > 0.0 && std::isfinite(b.discount_max) &&
         b.discount_max >= b.discount_min && std::isfinite(b.prepaid_min) && b.prepaid_min >= 0.0 &&
         std::isfinite(b.prepaid_max) && b.prepaid_max >= b.prepaid_min && b.discount_min <= 1.0 &&
         b.discount_max >= 1.0 && b.prepaid_min <= b.spot && b.prepaid_max >= b.spot;
}

[[nodiscard]] std::size_t var(std::size_t node, Side side) noexcept {
  return 2u * node + (side == Side::Put ? 1u : 0u);
}

enum class RowKind : std::uint8_t {
  BoxLo,
  BoxHi,
  Convex,
  SlopeFirst,
  SlopeLast,
  AffineLo,
  AffineHi,
  Legendre
};

// One constraint g(x) = sum coef * x[index] - b >= 0 over at most four variables.
struct Row {
  std::array<std::size_t, 4> index{};
  std::array<double, 4> coef{};
  std::size_t count = 0;
  double b = 0.0;
  RowKind kind = RowKind::BoxLo;
  std::size_t node = 0;
  Side side = Side::Call;

  void term(std::size_t i, double c) noexcept {
    index[count] = i;
    coef[count] = c;
    ++count;
  }
  [[nodiscard]] double value(std::span<const double> x) const noexcept {
    double v = -b;
    for (std::size_t k = 0; k < count; ++k) v += coef[k] * x[index[k]];
    return v;
  }
};

struct Conflict {
  QuoteSliceInboxConflict kind = QuoteSliceInboxConflict::None;
  Side side = Side::Call;
  std::size_t node = 0;
  double excess = 0.0;
};

// Keeps the larger of two conflicts (the most violated constraint is the one named).
void keep_worst(std::optional<Conflict> &worst, const Conflict &c) {
  if (!worst || c.excess > worst->excess) worst = c;
}

// A symmetric positive definite matrix of half-bandwidth kBand, factored in place
// (Cholesky): entry (r, c), c <= r <= c + kBand, lives at r * (kBand + 1) + (r - c).
class BandSystem {
public:
  explicit BandSystem(std::size_t n) : n_(n), data_(n * (kBand + 1), 0.0) {}

  void clear() noexcept { std::fill(data_.begin(), data_.end(), 0.0); }
  // Adds v at (r, c) and, by symmetry, (c, r); requires |r - c| <= kBand.
  void add(std::size_t r, std::size_t c, double v) noexcept {
    if (r < c) std::swap(r, c);
    data_[r * (kBand + 1) + (r - c)] += v;
  }
  // Replaces row and column k by the identity's (a fixed variable: its step is zero).
  void pin(std::size_t k) noexcept {
    for (std::size_t o = 0; o <= kBand; ++o) {
      data_[k * (kBand + 1) + o] = 0.0;
      if (k + o < n_ && o > 0) data_[(k + o) * (kBand + 1) + o] = 0.0;
    }
    data_[k * (kBand + 1)] = 1.0;
  }
  [[nodiscard]] bool factor() noexcept {
    for (std::size_t j = 0; j < n_; ++j) {
      const std::size_t first = j >= kBand ? j - kBand : 0;
      double d = at(j, j);
      for (std::size_t k = first; k < j; ++k) d -= at(j, k) * at(j, k);
      if (!(d > 0.0) || !std::isfinite(d)) return false;
      const double pivot = std::sqrt(d);
      at(j, j) = pivot;
      const std::size_t last = std::min(n_ - 1, j + kBand);
      for (std::size_t i = j + 1; i <= last; ++i) {
        double v = at(i, j);
        for (std::size_t k = i >= kBand ? i - kBand : 0; k < j; ++k) v -= at(i, k) * at(j, k);
        at(i, j) = v / pivot;
      }
    }
    return true;
  }
  // Solves (L L^T) x = rhs in place after factor().
  void solve(std::span<double> rhs) const noexcept {
    for (std::size_t i = 0; i < n_; ++i) {
      double v = rhs[i];
      for (std::size_t k = i >= kBand ? i - kBand : 0; k < i; ++k) v -= get(i, k) * rhs[k];
      rhs[i] = v / get(i, i);
    }
    for (std::size_t i = n_; i-- > 0;) {
      double v = rhs[i];
      const std::size_t last = std::min(n_ - 1, i + kBand);
      for (std::size_t k = i + 1; k <= last; ++k) v -= get(k, i) * rhs[k];
      rhs[i] = v / get(i, i);
    }
  }

private:
  [[nodiscard]] double &at(std::size_t r, std::size_t c) noexcept {
    return data_[r * (kBand + 1) + (r - c)];
  }
  [[nodiscard]] double get(std::size_t r, std::size_t c) const noexcept {
    return data_[r * (kBand + 1) + (r - c)];
  }

  std::size_t n_;
  std::vector<double> data_;
};

// The projection problem after the exact tests: variable bounds, targets, weights, rows.
struct Problem {
  std::size_t nodes = 0;
  std::vector<double> strikes;
  std::vector<double> lo, hi;       // per variable, intersected with the certificate's bounds
  std::vector<double> target;       // the fitted prices
  std::vector<double> weight;       // 1 / h^2 for a free variable
  std::vector<double> centre, half; // the quote box's centre and half-width
  std::vector<std::uint8_t> free;   // 1: lo < hi, the projection may move it
  std::vector<Row> rows;
};

// Centred: lambda^2 / 2 <= kCentred (or the per-centring step cap was reached, the iterate
// interior either way). Interior: phase I reached s <= its target. Budget: the Newton budget
// is spent. Breakdown: a factorisation or line search failed in floating point.
enum class Centring : std::uint8_t { Centred, Interior, Budget, Breakdown };

// Newton centring of the barrier objective. Phase I (`phase1`): t s - sum log(g + s) over
// (x, s); phase II: t Q(x) - sum log g, Q = sum w (x - target)^2 / 2. The step is capped at
// 99 % of the exact distance to the nearest constraint (every row is affine), then
// backtracked (Armijo, 1 %, halving) on the barrier objective itself, so every iterate stays
// strictly inside every row. Every Newton system counts against `budget` (`used` is
// incremented).
class Centrer {
public:
  Centrer(const Problem &p, bool phase1) : p_(p), phase1_(phase1), band_(p.lo.size()) {
    const std::size_t n = p.lo.size();
    grad_.resize(n);
    hxs_.resize(n);
    u_.resize(n);
    v_.resize(n);
    dx_.resize(n);
    slack_.resize(p.rows.size());
    dslack_.resize(p.rows.size());
  }

  // Centres at `t`; phase I returns Interior as soon as s <= s_target.
  Centring centre(std::vector<double> &x, double &s, double t, std::size_t budget,
                  std::size_t &used, double s_target = 0.0) {
    for (std::size_t step = 0; step < kCentringSteps; ++step) {
      if (used >= budget) return Centring::Budget;
      ++used;
      double lambda2 = 0.0;
      double ds = 0.0;
      if (!newton(x, t, lambda2, ds)) return Centring::Breakdown;
      if (lambda2 / 2.0 <= kCentred) return Centring::Centred;
      const double alpha = line_search(x, t, lambda2, ds);
      if (!(alpha > 0.0)) return Centring::Breakdown;
      for (std::size_t k = 0; k < x.size(); ++k) x[k] += alpha * dx_[k];
      if (phase1_) s += alpha * ds;
      if (!refresh_slacks(x, s)) return Centring::Breakdown;
      if (phase1_ && s <= s_target) return Centring::Interior;
    }
    return Centring::Centred;
  }

  // Slacks at (x, s); false if any is not positive.
  bool refresh_slacks(std::span<const double> x, double s) {
    for (std::size_t j = 0; j < p_.rows.size(); ++j) {
      slack_[j] = p_.rows[j].value(x) + (phase1_ ? s : 0.0);
      if (!(slack_[j] > 0.0)) return false;
    }
    return true;
  }
  [[nodiscard]] std::span<const double> slacks() const noexcept { return slack_; }

private:
  // Assembles and solves the Newton system at the current slacks (refresh_slacks); lambda2 =
  // the Newton decrement^2, ds the phase I step of s.
  bool newton(std::span<const double> x, double t, double &lambda2, double &ds) {
    const std::size_t n = x.size();
    band_.clear();
    std::fill(grad_.begin(), grad_.end(), 0.0);
    std::fill(hxs_.begin(), hxs_.end(), 0.0);
    double hss = 0.0;
    double gs = phase1_ ? t : 0.0;
    for (std::size_t j = 0; j < p_.rows.size(); ++j) {
      const Row &r = p_.rows[j];
      const double inv = 1.0 / slack_[j];
      const double inv2 = inv * inv;
      for (std::size_t a = 0; a < r.count; ++a) {
        grad_[r.index[a]] -= r.coef[a] * inv;
        if (phase1_) hxs_[r.index[a]] += r.coef[a] * inv2;
        for (std::size_t b = 0; b <= a; ++b) {
          band_.add(r.index[a], r.index[b], r.coef[a] * r.coef[b] * inv2);
        }
      }
      if (phase1_) {
        gs -= inv;
        hss += inv2;
      }
    }
    if (!phase1_) {
      for (std::size_t k = 0; k < n; ++k) {
        if (!p_.free[k]) continue;
        grad_[k] += t * p_.weight[k] * (x[k] - p_.target[k]);
        band_.add(k, k, t * p_.weight[k]);
      }
    }
    for (std::size_t k = 0; k < n; ++k) {
      if (p_.free[k]) continue;
      band_.pin(k);
      grad_[k] = 0.0;
      hxs_[k] = 0.0;
    }
    if (!band_.factor()) return false;
    for (std::size_t k = 0; k < n; ++k) u_[k] = -grad_[k];
    band_.solve(u_);
    ds = 0.0;
    if (phase1_) {
      v_ = hxs_;
      band_.solve(v_);
      double hu = 0.0;
      double hv = 0.0;
      for (std::size_t k = 0; k < n; ++k) {
        hu += hxs_[k] * u_[k];
        hv += hxs_[k] * v_[k];
      }
      const double schur = hss - hv;
      if (!(schur > 0.0) || !std::isfinite(schur)) return false;
      ds = (-gs - hu) / schur;
      for (std::size_t k = 0; k < n; ++k) dx_[k] = u_[k] - v_[k] * ds;
    } else {
      dx_ = u_;
    }
    lambda2 = gs * -ds;
    for (std::size_t k = 0; k < n; ++k) lambda2 -= grad_[k] * dx_[k];
    return std::isfinite(lambda2) && lambda2 >= 0.0;
  }

  // The step along (dx, ds): at most 99 % of the way to the nearest row, halved until the
  // barrier objective falls by 1 % of the first-order prediction (Armijo); 0 when no step of
  // at least 2^-60 of the cap qualifies.
  [[nodiscard]] double line_search(std::span<const double> x, double t, double lambda2,
                                   double ds) {
    double cap = 1.0;
    for (std::size_t j = 0; j < p_.rows.size(); ++j) {
      const Row &r = p_.rows[j];
      double d = phase1_ ? ds : 0.0;
      for (std::size_t a = 0; a < r.count; ++a) d += r.coef[a] * dx_[r.index[a]];
      dslack_[j] = d;
      if (d < 0.0) cap = std::min(cap, 0.99 * slack_[j] / -d);
    }
    // The objective term along the step: phase I t s (linear); phase II t Q (quadratic).
    double linear = phase1_ ? t * ds : 0.0;
    double quadratic = 0.0;
    if (!phase1_) {
      for (std::size_t k = 0; k < x.size(); ++k) {
        if (!p_.free[k]) continue;
        linear += t * p_.weight[k] * (x[k] - p_.target[k]) * dx_[k];
        quadratic += t * p_.weight[k] * dx_[k] * dx_[k];
      }
    }
    double alpha = cap;
    for (int halving = 0; halving < 60; ++halving, alpha *= 0.5) {
      double change = alpha * linear + 0.5 * alpha * alpha * quadratic;
      bool inside = true;
      for (std::size_t j = 0; j < p_.rows.size() && inside; ++j) {
        const double ratio = alpha * dslack_[j] / slack_[j];
        inside = ratio > -1.0;
        if (inside) change -= std::log1p(ratio);
      }
      if (inside && change <= -0.01 * alpha * lambda2) return alpha;
    }
    return 0.0;
  }

  const Problem &p_;
  bool phase1_;
  BandSystem band_;
  std::vector<double> grad_, hxs_, u_, v_, dx_, slack_, dslack_;
};

[[nodiscard]] const char *side_name(Side side) noexcept {
  return side == Side::Call ? "call" : "put";
}

// The greatest convex curve with slopes in [slope_min, slope_max] under `upper` (per node):
// first the tightest upper bound the slopes alone imply at every node (a running minimum in
// each direction), then that bound's greatest convex minorant (its lower hull). Every curve
// of the side's feasible set lies under it and it is itself in the set's closure, so a lower
// edge above it is an exact infeasibility certificate.
void greatest_curve(std::span<const double> strikes, std::span<const double> upper,
                    double slope_min, double slope_max, std::vector<double> &out) {
  const std::size_t n = strikes.size();
  std::vector<double> cap(upper.begin(), upper.end());
  // From the left: f(K_j) <= f(K_k) + slope_max (K_j - K_k) <= upper_k + slope_max (...).
  double best = std::numeric_limits<double>::infinity();
  for (std::size_t j = 0; j < n; ++j) {
    best = std::min(best, upper[j] - slope_max * strikes[j]);
    cap[j] = std::min(cap[j], best + slope_max * strikes[j]);
  }
  // From the right: f(K_j) <= f(K_k) - slope_min (K_k - K_j) <= upper_k - slope_min (...).
  best = std::numeric_limits<double>::infinity();
  for (std::size_t j = n; j-- > 0;) {
    best = std::min(best, upper[j] - slope_min * strikes[j]);
    cap[j] = std::min(cap[j], best + slope_min * strikes[j]);
  }
  std::vector<std::size_t> hull;
  hull.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    while (hull.size() >= 2u) {
      const std::size_t l = hull[hull.size() - 2u];
      const std::size_t m = hull.back();
      const double fraction = (strikes[m] - strikes[l]) / (strikes[i] - strikes[l]);
      if (cap[m] <= std::lerp(cap[l], cap[i], fraction)) break;
      hull.pop_back();
    }
    hull.push_back(i);
  }
  out.assign(n, 0.0);
  for (std::size_t h = 0; h < hull.size(); ++h) {
    out[hull[h]] = cap[hull[h]];
    if (h == 0u) continue;
    const std::size_t l = hull[h - 1u];
    const std::size_t r = hull[h];
    for (std::size_t i = l + 1u; i < r; ++i) {
      out[i] = std::lerp(cap[l], cap[r], (strikes[i] - strikes[l]) / (strikes[r] - strikes[l]));
    }
  }
}

// Lowest value of a row over the variables' bounds: a row whose lowest value is nonnegative
// holds wherever the boxes hold, so it adds nothing to the feasible set.
[[nodiscard]] double row_floor(const Row &r, const Problem &p) noexcept {
  double v = -r.b;
  for (std::size_t a = 0; a < r.count; ++a) {
    v += r.coef[a] * (r.coef[a] > 0.0 ? p.lo[r.index[a]] : p.hi[r.index[a]]);
  }
  return v;
}

// The exact projection keeps every node no active row touches at its fitted price; the
// barrier's iterate moves them by a bias of order 1 / t. Each free variable within kSnap of
// its half-width of its fitted price returns to that price; any row this leaves without a
// strictly positive slack has its snapped variables restored, until every row holds
// strictly again (at worst the barrier's own point). Returns the variables snapped.
std::size_t snap_to_fitted(const Problem &p, std::vector<double> &x) {
  const std::vector<double> interior = x;
  std::vector<std::uint8_t> snapped(x.size(), 0u);
  std::size_t count = 0;
  for (std::size_t v = 0; v < x.size(); ++v) {
    const double h = std::max(p.half[v], kWeightFloor);
    if (!p.free[v] || x[v] == p.target[v] || !(std::abs(x[v] - p.target[v]) <= kSnap * h) ||
        !(p.target[v] > p.lo[v] && p.target[v] < p.hi[v])) {
      continue;
    }
    x[v] = p.target[v];
    snapped[v] = 1u;
    ++count;
  }
  for (bool changed = count > 0u; changed;) {
    changed = false;
    for (const Row &r : p.rows) {
      if (r.value(x) > 0.0) continue;
      for (std::size_t a = 0; a < r.count; ++a) {
        const std::size_t v = r.index[a];
        if (!snapped[v]) continue;
        x[v] = interior[v];
        snapped[v] = 0u;
        --count;
        changed = true;
      }
    }
  }
  return count;
}

void add_row(Problem &p, Row r) {
  bool moves = false;
  for (std::size_t a = 0; a < r.count; ++a) moves = moves || p.free[r.index[a]] != 0u;
  // A row over fixed variables alone is a constant (the exact tests judged it); a row the
  // boxes imply is redundant. Box rows are never dropped: they keep the system definite.
  if (r.kind != RowKind::BoxLo && r.kind != RowKind::BoxHi &&
      (!moves || row_floor(r, p) >= 0.0)) {
    return;
  }
  p.rows.push_back(r);
}

} // namespace

QuoteSliceBoxRefusal classify_box_refusal(std::string_view message) noexcept {
  if (message.find("box outside the exercise bounds") != std::string_view::npos) {
    return QuoteSliceBoxRefusal::ExerciseBounds;
  }
  if (message.find("no convex curve inside the boxes") != std::string_view::npos) {
    return QuoteSliceBoxRefusal::NoConvexCurve;
  }
  if (message.find("projection did not settle") != std::string_view::npos) {
    return QuoteSliceBoxRefusal::NotSettled;
  }
  if (message.find("in-box curve refused") != std::string_view::npos) {
    return QuoteSliceBoxRefusal::CertificateRefused;
  }
  return QuoteSliceBoxRefusal::Other;
}

Result<AmericanBoxCertificate> project_american_in_box(std::span<const AmericanPriceNode> fitted,
                                                       std::span<const AmericanPriceBox> boxes,
                                                       const AmericanPriceBounds &bounds,
                                                       std::string_view box_refusal,
                                                       QuoteSliceInboxRecord &record,
                                                       std::size_t max_newton_steps) {
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  record = QuoteSliceInboxRecord{};
  record.box_refusal = classify_box_refusal(box_refusal);
  const auto finish = [&](QuoteSliceInboxOutcome outcome) {
    record.outcome = outcome;
    record.elapsed_us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
  };
  const auto invalid = [&](const char *why) -> Result<AmericanBoxCertificate> {
    finish(QuoteSliceInboxOutcome::InvalidInput);
    return Err(ErrorCode::InvalidArgument,
               std::string("AmericanPriceSlice in-box projection: ") + why);
  };
  const std::size_t n = fitted.size();
  if (!valid_bounds(bounds)) return invalid("invalid carry bounds");
  if (n < 2u || boxes.size() != n) {
    return invalid("at least two nodes and one box per node required");
  }
  if (max_newton_steps == 0u) return invalid("the Newton budget must be positive");
  for (std::size_t i = 0; i < n; ++i) {
    const AmericanPriceNode &node = fitted[i];
    const AmericanPriceBox &box = boxes[i];
    const auto finite_box = [](double lo, double hi) noexcept {
      return std::isfinite(lo) && std::isfinite(hi) && lo <= hi;
    };
    if (!std::isfinite(node.strike) || !(node.strike > 0.0) || !std::isfinite(node.call) ||
        !std::isfinite(node.put) || (i > 0u && !(node.strike > fitted[i - 1u].strike)) ||
        !finite_box(box.call_lo, box.call_hi) || !finite_box(box.put_lo, box.put_hi)) {
      return invalid("finite increasing strikes, finite prices and boxes with lo <= hi "
                     "required");
    }
  }
  const double spot = bounds.spot;
  // The non-dividend regime of certify_and_serve's joint check (no carry, no dividends, a
  // nonnegative rate): its European call bounds join the certificate's own.
  const bool nondividend = bounds.prepaid_min == spot && bounds.prepaid_max == spot &&
                           bounds.discount_max == 1.0;
  Problem p;
  p.nodes = n;
  p.strikes.resize(n);
  for (std::size_t i = 0; i < n; ++i) p.strikes[i] = fitted[i].strike;
  const std::size_t vars = 2u * n;
  p.lo.resize(vars);
  p.hi.resize(vars);
  p.target.resize(vars);
  p.weight.assign(vars, 0.0);
  p.centre.resize(vars);
  p.half.resize(vars);
  p.free.assign(vars, 0u);
  const auto infeasible = [&](const Conflict &c) -> Result<AmericanBoxCertificate> {
    record.conflict = c.kind;
    record.conflict_side = c.side;
    record.conflict_node = c.node;
    record.conflict_strike = p.strikes[c.node];
    record.conflict_excess = c.excess;
    finish(QuoteSliceInboxOutcome::InfeasibleData);
    return Err(ErrorCode::Unavailable,
               "AmericanPriceSlice in-box projection: node " + std::to_string(c.node) + " (" +
                   side_name(c.side) + "): no arbitrage-free curve inside the boxes (" +
                   to_string(c.kind) + ", excess " + std::to_string(c.excess) + ")");
  };
  // ── Per-variable bounds: the box intersected with the certificate's node bounds ──
  std::optional<Conflict> worst;
  for (std::size_t i = 0; i < n; ++i) {
    const double k = p.strikes[i];
    for (const Side side : {Side::Call, Side::Put}) {
      const bool call = side == Side::Call;
      const std::size_t v = var(i, side);
      const double box_lo = call ? boxes[i].call_lo : boxes[i].put_lo;
      const double box_hi = call ? boxes[i].call_hi : boxes[i].put_hi;
      double cert_lo = std::max(call ? spot - k : k - spot, 0.0);
      if (call && nondividend) cert_lo = std::max(cert_lo, spot - k * bounds.discount_min);
      const double cert_hi = call ? bounds.prepaid_max : k * bounds.discount_max;
      double lo = std::max(box_lo, cert_lo);
      double hi = std::min(box_hi, cert_hi);
      if (lo > hi) {
        if (lo - hi > allowance(spot, k)) {
          keep_worst(worst, {QuoteSliceInboxConflict::EmptyBox, side, i, lo - hi});
          continue;
        }
        // Within the allowance the certificate accepts the quote's own edge.
        lo = hi = box_hi < cert_lo ? box_hi : box_lo;
      }
      if (!(hi - lo > 1.0e-12 * std::max(1.0, std::abs(hi)))) lo = hi = 0.5 * (lo + hi);
      p.lo[v] = lo;
      p.hi[v] = hi;
      p.target[v] = call ? fitted[i].call : fitted[i].put;
      p.centre[v] = 0.5 * (box_lo + box_hi);
      p.half[v] = 0.5 * (box_hi - box_lo);
      p.free[v] = hi > lo ? 1u : 0u;
      if (p.free[v]) {
        const double h = std::max(p.half[v], kWeightFloor);
        p.weight[v] = 1.0 / (h * h);
        ++record.free_sides;
        if (p.target[v] < lo || p.target[v] > hi) ++record.fitted_outside;
      }
    }
  }
  if (worst) return infeasible(*worst);
  // ── Exact tests: no convex curve on one side; no affine pair at one node ──
  const double call_slope_min = -(nondividend ? bounds.discount_min : bounds.discount_max);
  std::vector<double> side_hi(n);
  std::array<std::vector<double>, 2> greatest;
  for (const Side side : {Side::Call, Side::Put}) {
    const bool call = side == Side::Call;
    for (std::size_t i = 0; i < n; ++i) side_hi[i] = p.hi[var(i, side)];
    auto &g = greatest[call ? 0 : 1];
    greatest_curve(p.strikes, side_hi, call ? call_slope_min : 0.0,
                   call ? 0.0 : bounds.discount_max, g);
    for (std::size_t i = 0; i < n; ++i) {
      const double excess = p.lo[var(i, side)] - g[i];
      if (excess > allowance(spot, p.strikes[i])) {
        keep_worst(worst, {QuoteSliceInboxConflict::NoConvexCurve, side, i, excess});
      }
    }
  }
  if (worst) return infeasible(*worst);
  for (std::size_t i = 0; i < n; ++i) {
    const double k = p.strikes[i];
    const double affine_lo = bounds.prepaid_min - k * bounds.discount_max;
    const double affine_hi = bounds.prepaid_max - k * bounds.discount_min;
    const double reach_lo = p.lo[var(i, Side::Call)] - greatest[1][i];
    const double reach_hi = greatest[0][i] - p.lo[var(i, Side::Put)];
    const double excess = std::max(affine_lo - reach_hi, reach_lo - affine_hi);
    if (excess > allowance(spot, k)) {
      keep_worst(worst, {QuoteSliceInboxConflict::AffineBox, Side::Call, i, excess});
    }
  }
  if (worst) return infeasible(*worst);
  // ── Constraint rows ──
  for (std::size_t v = 0; v < vars; ++v) {
    if (!p.free[v]) continue;
    const std::size_t node = v / 2u;
    const Side side = v % 2u == 0u ? Side::Call : Side::Put;
    Row lo_row{};
    lo_row.term(v, 1.0);
    lo_row.b = p.lo[v];
    lo_row.kind = RowKind::BoxLo;
    lo_row.node = node;
    lo_row.side = side;
    add_row(p, lo_row);
    Row hi_row{};
    hi_row.term(v, -1.0);
    hi_row.b = -p.hi[v];
    hi_row.kind = RowKind::BoxHi;
    hi_row.node = node;
    hi_row.side = side;
    add_row(p, hi_row);
  }
  for (const Side side : {Side::Call, Side::Put}) {
    const bool call = side == Side::Call;
    for (std::size_t i = 1; i + 1 < n; ++i) {
      // The node at or under the chord of its neighbours (price units).
      const double wl = p.strikes[i] - p.strikes[i - 1u];
      const double wr = p.strikes[i + 1u] - p.strikes[i];
      Row r{};
      r.term(var(i - 1u, side), wr / (wl + wr));
      r.term(var(i, side), -1.0);
      r.term(var(i + 1u, side), wl / (wl + wr));
      r.kind = RowKind::Convex;
      r.node = i;
      r.side = side;
      add_row(p, r);
    }
    const double first = p.strikes[1] - p.strikes[0];
    const double last = p.strikes[n - 1u] - p.strikes[n - 2u];
    Row lower{}; // first slope >= slope_min
    lower.term(var(1u, side), 1.0);
    lower.term(var(0u, side), -1.0);
    lower.b = (call ? call_slope_min : 0.0) * first;
    lower.kind = RowKind::SlopeFirst;
    lower.node = 0u;
    lower.side = side;
    add_row(p, lower);
    Row upper{}; // last slope <= slope_max
    upper.term(var(n - 2u, side), 1.0);
    upper.term(var(n - 1u, side), -1.0);
    upper.b = -(call ? 0.0 : bounds.discount_max) * last;
    upper.kind = RowKind::SlopeLast;
    upper.node = n - 1u;
    upper.side = side;
    add_row(p, upper);
  }
  for (std::size_t i = 0; i < n; ++i) {
    const double k = p.strikes[i];
    Row lo_row{}; // call - put >= prepaid_min - K discount_max
    lo_row.term(var(i, Side::Call), 1.0);
    lo_row.term(var(i, Side::Put), -1.0);
    lo_row.b = bounds.prepaid_min - k * bounds.discount_max;
    lo_row.kind = RowKind::AffineLo;
    lo_row.node = i;
    add_row(p, lo_row);
    Row hi_row{}; // call - put <= prepaid_max - K discount_min
    hi_row.term(var(i, Side::Call), -1.0);
    hi_row.term(var(i, Side::Put), 1.0);
    hi_row.b = -(bounds.prepaid_max - k * bounds.discount_min);
    hi_row.kind = RowKind::AffineHi;
    hi_row.node = i;
    add_row(p, hi_row);
  }
  if (nondividend) {
    // check_nondividend_american's segment condition, linear in the segment's four prices:
    // K_l (put slope - call slope) - (P_l - C_l + S) >= 0.
    for (std::size_t i = 0; i + 1 < n; ++i) {
      const double scale = p.strikes[i] / (p.strikes[i + 1u] - p.strikes[i]);
      Row r{};
      r.term(var(i, Side::Call), scale + 1.0);
      r.term(var(i, Side::Put), -scale - 1.0);
      r.term(var(i + 1u, Side::Call), -scale);
      r.term(var(i + 1u, Side::Put), scale);
      r.b = spot;
      r.kind = RowKind::Legendre;
      r.node = i;
      add_row(p, r);
    }
  }
  record.constraints = p.rows.size();
  // ── Phase I: an interior point, or a certificate that none exists ──
  std::vector<double> x(vars);
  for (std::size_t v = 0; v < vars; ++v) {
    const double inset = 0.1 * (p.hi[v] - p.lo[v]);
    x[v] = p.free[v] ? std::clamp(p.target[v], p.lo[v] + inset, p.hi[v] - inset) : p.lo[v];
  }
  double worst_violation = -std::numeric_limits<double>::infinity();
  for (const Row &r : p.rows) worst_violation = std::max(worst_violation, -r.value(x));
  const double m = static_cast<double>(p.rows.size());
  std::size_t used = 0;
  const auto nonconverged = [&](int phase) -> Result<AmericanBoxCertificate> {
    finish(QuoteSliceInboxOutcome::NonConverged);
    return Err(ErrorCode::OutOfRange, "AmericanPriceSlice in-box projection: phase " +
                                          std::to_string(phase) + " spent the Newton budget (" +
                                          std::to_string(max_newton_steps) + " steps)");
  };
  if (!p.rows.empty() && !(worst_violation < 0.0)) {
    double s = worst_violation + std::max(0.1 * std::abs(worst_violation), 1.0e-6);
    // Phase I ends as soon as every row has a slack of at least 1e-9 of the price scale
    // (phase II recentres from there), or at a centred point with s < 0.
    double scale = 1.0;
    for (std::size_t v = 0; v < vars; ++v) scale = std::max(scale, std::abs(p.hi[v]));
    const double s_target = -1.0e-9 * scale;
    Centrer centrer(p, true);
    if (!centrer.refresh_slacks(x, s)) return nonconverged(1);
    double t = 0.0;
    for (const double z : centrer.slacks()) t += 1.0 / z;
    for (;;) {
      const Centring c = centrer.centre(x, s, t, max_newton_steps, used, s_target);
      record.phase1_steps = used;
      if (c == Centring::Interior) break;
      if (c != Centring::Centred) return nonconverged(1);
      if (s < 0.0) break;
      const double gap = m / t;
      if (s - gap > 0.0) {
        // Certified: no point violates every row by less than s - m / t. Name the row with
        // the largest dual weight 1 / (t (g + s)), i.e. the smallest phase I slack.
        const auto slacks = centrer.slacks();
        const std::size_t j = static_cast<std::size_t>(
            std::min_element(slacks.begin(), slacks.end()) - slacks.begin());
        return infeasible(
            {QuoteSliceInboxConflict::Coupled, p.rows[j].side, p.rows[j].node, s - gap});
      }
      if (gap < 1.0e-14 * std::max(1.0, std::abs(s))) {
        // Degenerate: the set is at best a boundary (s = 0 to rounding). Nothing interior
        // exists to project from; the weakest violation is typed as the conflict.
        return infeasible({QuoteSliceInboxConflict::Coupled, Side::Call, 0u, std::max(s, 0.0)});
      }
      t *= kBarrierGrowth;
    }
  }
  // ── Phase II: the projection from the interior point ──
  if (!p.rows.empty()) {
    Centrer centrer(p, false);
    double unused = 0.0;
    if (!centrer.refresh_slacks(x, unused)) return nonconverged(2);
    double q = 0.0;
    for (std::size_t v = 0; v < vars; ++v) {
      if (p.free[v]) q += 0.5 * p.weight[v] * (x[v] - p.target[v]) * (x[v] - p.target[v]);
    }
    double t = m / std::max(q, 1.0);
    const std::size_t phase1 = used;
    for (;;) {
      const Centring c = centrer.centre(x, unused, t, max_newton_steps, used);
      record.phase2_steps = used - phase1;
      if (c != Centring::Centred) return nonconverged(2);
      if (m / t <= kPhase2Gap) break;
      t *= kBarrierGrowth;
    }
  }
  snap_to_fitted(p, x);
  // ── Certification by the unchanged certificate ──
  std::vector<AmericanPriceNode> nodes(n);
  for (std::size_t i = 0; i < n; ++i) {
    nodes[i] = {p.strikes[i], x[var(i, Side::Call)], x[var(i, Side::Put)]};
  }
  auto certified = AmericanPriceSlice::create(nodes, bounds);
  if (!certified) {
    finish(QuoteSliceInboxOutcome::NotCertified);
    return Err(certified.error().code(), "AmericanPriceSlice in-box projection: projected "
                                         "curve refused: " +
                                             certified.error().message());
  }
  AmericanBoxCertificate out{std::move(*certified), std::vector<std::uint8_t>(n, 0u), 0u, 0.0};
  for (std::size_t i = 0; i < n; ++i) {
    const double call = std::abs(nodes[i].call - fitted[i].call);
    const double put = std::abs(nodes[i].put - fitted[i].put);
    if (call > 0.0) out.moved[i] |= 1u;
    if (put > 0.0) out.moved[i] |= 2u;
    if (out.moved[i] != 0u) ++out.moved_nodes;
    out.max_price_move = std::max({out.max_price_move, call, put});
  }
  for (std::size_t v = 0; v < vars; ++v) {
    if (!(p.half[v] > 0.0)) continue;
    record.max_move_half_spreads =
        std::max(record.max_move_half_spreads, std::abs(x[v] - p.target[v]) / p.half[v]);
    record.max_mid_half_spreads =
        std::max(record.max_mid_half_spreads, std::abs(x[v] - p.centre[v]) / p.half[v]);
  }
  finish(QuoteSliceInboxOutcome::Applied);
  return Ok(std::move(out));
}

} // namespace detail
} // namespace atx::vol
