// K2 (r5, 2026-10-22): the varying-parameter batch inversion (contract in
// american_iv_batch.hpp; design note docs/american-iv-batch.md).

#include "pricing/american_iv_batch.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

#include "atx/vol/api/simd/cpu.hpp"
#include "simd/american_iv_batch_avx2.hpp"

namespace atx::vol::iv_batch {

namespace {

namespace sq = sigma_q_family_detail;
namespace vx = simd::iv_batch_avx2;

constexpr double kInf = std::numeric_limits<double>::infinity();

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] double opt(std::span<const double> s, std::size_t i) noexcept {
  return s.empty() ? kNaN : s[i];
}

// The group invariants of one call.
struct Group {
  const sq::PremiumTable *table = nullptr;
  const AlSigmaQFamily *family = nullptr;
  Side side = Side::Put;
  double S = kNaN;
  double box_lo = kNaN;
  double box_hi = kNaN;
  double q_lo = kNaN;
  double q_hi = kNaN;
  double tol = 0.0;
  double floor_rel = 0.0;
  std::uint16_t cap = 0;
};

// One bracket end: σ and the family price there (NaN: not evaluated — the σ box edge or a
// caller's assumed bound).
struct End {
  double x = kNaN;
  double p = kNaN;
};

// One lane's iteration state.
struct Lane {
  std::size_t i = 0;
  double K = kNaN;
  double target = kNaN;
  double q = kNaN;
  double floor = 0.0;
  double x = kNaN; // the next point to evaluate
  End lo{};
  End hi{};
  double last_x = kNaN;
  double last_p = kNaN;
  double last_v = kNaN;
  double best_x = kNaN;
  double best_f = kInf;
  double dx = kInf;     // the last step
  double dx_old = kInf; // the one before (the rtsafe progress test)
  std::uint16_t evals = 0;
  std::uint8_t flags = 0;
  bool done = false;
  InverseStatus status = InverseStatus::NonConverged;
  double root = kNaN;
  AcceptanceKind kind = AcceptanceKind::None;
  double newton = kNaN;
};

void finish(Lane &L, InverseStatus s) noexcept {
  L.done = true;
  L.status = s;
}

void converge(Lane &L, double root, AcceptanceKind kind, double newton) noexcept {
  L.root = root;
  L.kind = kind;
  L.newton = newton;
  finish(L, InverseStatus::Converged);
}

// The bracket midpoint: geometric on a wide bracket (the σ box spans decades), else
// arithmetic.
[[nodiscard]] double mid(double lo, double hi) noexcept {
  return (hi > 2.0 * lo) ? std::sqrt(lo * hi) : 0.5 * (lo + hi);
}

// A family point (s, P) on the lane's map narrows the bracket on the side its price is
// on (P increasing in σ). A point at or beyond an UNEVALUATED end proves that end wrong
// (an assumed bound): it widens back to the box edge. A point contradicting an evaluated
// end (non-monotone rounding) is ignored.
void apply_point(Lane &L, const Group &G, double s, double P) noexcept {
  if (std::isnan(P) || std::isnan(s)) {
    return;
  }
  if (P < L.target) {
    if (s < L.lo.x || (s == L.lo.x && !std::isnan(L.lo.p))) {
      return;
    }
    if (s >= L.hi.x) {
      if (!std::isnan(L.hi.p)) {
        return;
      }
      L.hi = End{G.box_hi, kNaN};
    }
    L.lo = End{s, P};
  } else {
    if (s > L.hi.x || (s == L.hi.x && !std::isnan(L.hi.p))) {
      return;
    }
    if (s <= L.lo.x) {
      if (!std::isnan(L.lo.p)) {
        return;
      }
      L.lo = End{G.box_lo, kNaN};
    }
    L.hi = End{s, P};
  }
}

// Box misses and a bracket already within the tolerance, from the evaluated ends alone.
[[nodiscard]] bool settle(Lane &L, const Group &G) noexcept {
  if (!std::isnan(L.lo.p) && L.lo.x >= G.box_hi) {
    finish(L, InverseStatus::SigmaAboveBox);
    return true;
  }
  if (!std::isnan(L.hi.p) && L.hi.x <= G.box_lo) {
    finish(L, InverseStatus::SigmaBelowBox);
    return true;
  }
  if (!std::isnan(L.lo.p) && !std::isnan(L.hi.p) && L.hi.x - L.lo.x <= 2.0 * G.tol) {
    L.flags |= lane_flag::kBracketConverged;
    converge(L, 0.5 * (L.lo.x + L.hi.x), AcceptanceKind::FamilyBracket, kNaN);
    return true;
  }
  return false;
}

// The first point: the seed inside the bracket, else the end it points past (evaluated
// there when that end is unevaluated: a box-top seed is priced AT the box top), else the
// bracket midpoint.
void place_seed(Lane &L, double seed) noexcept {
  if (!std::isfinite(seed)) {
    L.flags |= lane_flag::kSeedFailed;
    L.x = mid(L.lo.x, L.hi.x);
  } else if (seed >= L.hi.x) {
    L.flags |= lane_flag::kSeedClamped;
    L.x = std::isnan(L.hi.p) ? L.hi.x : mid(L.lo.x, L.hi.x);
  } else if (seed <= L.lo.x) {
    L.flags |= lane_flag::kSeedClamped;
    L.x = std::isnan(L.lo.p) ? L.lo.x : mid(L.lo.x, L.hi.x);
  } else {
    L.x = seed;
  }
  L.dx = L.hi.x - L.lo.x;
  L.dx_old = L.dx;
}

// A lane's start: the box, the caller's retained bracket, the anchor's evaluated points
// (bid / ask behind their mid) and the seed. Returns with L.done when the start alone
// settles it (a box miss by monotonicity, a bracket already within tol).
void init_lane(Lane &L, const Group &G, const InverseBatchIn &in, std::size_t i,
               const Lane *anchor) noexcept {
  L = Lane{};
  L.i = i;
  L.K = in.K[i];
  L.target = in.target[i];
  L.q = in.q[i];
  L.floor = G.floor_rel * L.K;
  L.lo = End{G.box_lo, kNaN};
  L.hi = End{G.box_hi, kNaN};
  const double blo = opt(in.bracket_lo, i);
  const double bhi = opt(in.bracket_hi, i);
  if (std::isfinite(blo) || std::isfinite(bhi)) {
    L.flags |= lane_flag::kRetainedBracket;
  }
  if (std::isfinite(blo)) {
    const double plo = opt(in.price_lo, i);
    if (std::isfinite(plo)) {
      apply_point(L, G, blo, plo);
    } else if (blo > L.lo.x && blo < L.hi.x) {
      L.lo = End{blo, kNaN};
    }
  }
  if (std::isfinite(bhi)) {
    const double phi = opt(in.price_hi, i);
    if (std::isfinite(phi)) {
      apply_point(L, G, bhi, phi);
    } else if (bhi < L.hi.x && bhi > L.lo.x) {
      L.hi = End{bhi, kNaN};
    }
  }
  double seed = opt(in.seed_sigma, i);
  if (anchor != nullptr) {
    L.flags |= lane_flag::kAnchored;
    apply_point(L, G, anchor->lo.x, anchor->lo.p);
    apply_point(L, G, anchor->hi.x, anchor->hi.p);
    apply_point(L, G, anchor->last_x, anchor->last_p);
    if (std::isfinite(anchor->last_v) && anchor->last_v > anchor->floor &&
        std::isfinite(anchor->last_p)) {
      seed = anchor->last_x + (L.target - anchor->last_p) / anchor->last_v;
    }
  }
  if (settle(L, G)) {
    return;
  }
  place_seed(L, seed);
}

// One evaluated point: the typed exits, the bracket, then the next point (a Newton step
// kept inside the bracket and shrinking fast enough — the rtsafe test — else a probe of an
// unevaluated end the step points past, else bisection).
void step(Lane &L, const Group &G, double P, double V, bool exercised, bool degenerate) noexcept {
  const double x = L.x;
  ++L.evals;
  if (x == G.box_lo || x == G.box_hi) {
    L.flags |= lane_flag::kBoxEdgeEvaluated;
  }
  if (degenerate) {
    L.flags |= lane_flag::kDegenerateVega;
  }
  if (!std::isfinite(P)) {
    finish(L, InverseStatus::InvalidInput);
    return;
  }
  const double f = P - L.target;
  L.last_x = x;
  L.last_p = P;
  L.last_v = degenerate ? kNaN : V;
  if (std::fabs(f) < L.best_f) {
    L.best_f = std::fabs(f);
    L.best_x = x;
  }
  const bool usable = !degenerate && std::isfinite(V) && V > L.floor;
  // The exercise plateau: flat at intrinsic with the target at or below it.
  if (!usable && !degenerate && exercised && f >= 0.0) {
    apply_point(L, G, x, P);
    finish(L, InverseStatus::ExerciseFlat);
    return;
  }
  const double d = usable ? f / V : kNaN;
  if (usable && std::fabs(d) <= G.tol) {
    apply_point(L, G, x, P);
    converge(L, x - d, AcceptanceKind::NewtonDistance, std::fabs(d));
    return;
  }
  apply_point(L, G, x, P);
  if (settle(L, G)) {
    return;
  }
  if (L.evals >= G.cap) {
    finish(L, usable ? InverseStatus::NonConverged
                     : (exercised ? InverseStatus::ExerciseFlat : InverseStatus::LowVega));
    return;
  }
  const double cand = x - d;
  double next = kNaN;
  if (usable && cand > L.lo.x && cand < L.hi.x && std::fabs(d) <= 0.5 * std::fabs(L.dx_old)) {
    next = cand;
    L.dx_old = L.dx;
    L.dx = d;
  } else if (usable && cand >= L.hi.x && std::isnan(L.hi.p)) {
    next = L.hi.x;
    L.dx_old = L.dx;
    L.dx = next - x;
  } else if (usable && cand <= L.lo.x && std::isnan(L.lo.p)) {
    next = L.lo.x;
    L.dx_old = L.dx;
    L.dx = x - next;
  } else {
    next = mid(L.lo.x, L.hi.x);
    L.flags |= lane_flag::kBisected;
    L.dx_old = L.dx;
    L.dx = 0.5 * (L.hi.x - L.lo.x);
  }
  if (!(next >= G.box_lo && next <= G.box_hi) || next == x) {
    finish(L, usable ? InverseStatus::NonConverged : InverseStatus::LowVega);
    return;
  }
  L.x = next;
}

// The lane's outputs (contract in the header).
void write_lane(const Lane &L, const InverseBatchOut &out, InverseBatchWork &w) noexcept {
  const std::size_t i = L.i;
  const bool conv = L.status == InverseStatus::Converged;
  out.status[i] = L.status;
  out.evaluations[i] = L.evals;
  out.flags[i] = L.flags;
  out.sigma[i] = conv ? L.root : L.best_x;
  double lo = L.lo.x;
  double plo = L.lo.p;
  double hi = L.hi.x;
  double phi = L.hi.p;
  if (L.status == InverseStatus::SigmaAboveBox) {
    hi = kInf;
    phi = kNaN;
  } else if (L.status == InverseStatus::SigmaBelowBox) {
    lo = 0.0;
    plo = kNaN;
  }
  out.bracket_lo[i] = lo;
  out.bracket_hi[i] = hi;
  out.price_lo[i] = plo;
  out.price_hi[i] = phi;
  out.residual[i] = std::isnan(L.last_p) ? kNaN : L.last_p - L.target;
  out.vega[i] = L.last_v;
  out.last_sigma[i] = L.last_x;
  LaneAcceptance a;
  a.kind = conv ? L.kind : AcceptanceKind::None;
  a.newton_distance = L.newton;
  a.interval_lo = std::isnan(L.lo.p) ? kNaN : L.lo.x;
  a.interval_hi = std::isnan(L.hi.p) ? kNaN : L.hi.x;
  out.acceptance[i] = a;
  ++w.by_status[static_cast<std::size_t>(L.status)];
  w.evaluations += L.evals;
  if ((L.flags & lane_flag::kSeedFailed) != 0u) ++w.seed_failed;
  if ((L.flags & lane_flag::kSeedClamped) != 0u) ++w.seed_clamped;
  if ((L.flags & lane_flag::kAnchored) != 0u) ++w.anchored;
}

// A lane refused before any evaluation.
void write_refusal(std::size_t i, InverseStatus s, const InverseBatchOut &out,
                   InverseBatchWork &w) noexcept {
  out.status[i] = s;
  out.evaluations[i] = 0;
  out.flags[i] = 0;
  out.sigma[i] = kNaN;
  out.bracket_lo[i] = kNaN;
  out.bracket_hi[i] = kNaN;
  out.price_lo[i] = kNaN;
  out.price_hi[i] = kNaN;
  out.residual[i] = kNaN;
  out.vega[i] = kNaN;
  out.last_sigma[i] = kNaN;
  out.acceptance[i] = LaneAcceptance{};
  ++w.by_status[static_cast<std::size_t>(s)];
}

[[nodiscard]] bool sized(std::size_t got, std::size_t n, bool optional) noexcept {
  return got == n || (optional && got == 0);
}

// Lane roles (scratch.role): which lanes run, and in what chain.
enum : std::uint8_t { kSkip = 0, kRoot = 1, kDependent = 2 };

} // namespace

const char *inverse_status_name(InverseStatus s) noexcept {
  switch (s) {
  case InverseStatus::Inactive:
    return "inactive";
  case InverseStatus::Converged:
    return "converged";
  case InverseStatus::LowVega:
    return "low_vega";
  case InverseStatus::ExerciseFlat:
    return "exercise_flat";
  case InverseStatus::SigmaBelowBox:
    return "sigma_below_box";
  case InverseStatus::SigmaAboveBox:
    return "sigma_above_box";
  case InverseStatus::QOutsideBox:
    return "q_outside_box";
  case InverseStatus::NonConverged:
    return "nonconverged";
  case InverseStatus::InvalidInput:
    return "invalid_input";
  case InverseStatus::UnsupportedIsa:
    return "unsupported_isa";
  }
  return "unknown";
}

void InverseBatchWork::add(const InverseBatchWork &o) noexcept {
  lanes += o.lanes;
  active_lanes += o.active_lanes;
  evaluations += o.evaluations;
  vector_steps += o.vector_steps;
  vector_lane_slots += o.vector_lane_slots;
  scalar_evaluations += o.scalar_evaluations;
  q_contractions += o.q_contractions;
  tile_reuses += o.tile_reuses;
  seed_failed += o.seed_failed;
  seed_clamped += o.seed_clamped;
  anchored += o.anchored;
  for (std::size_t s = 0; s < kInverseStatusCount; ++s) {
    by_status[s] += o.by_status[s];
  }
  avx2 = o.avx2;
}

// ── scratch ──────────────────────────────────────────────────────────────────────────

struct InverseScratch::Impl {
  std::size_t cap = 0;
  std::vector<std::uint8_t> role;
  std::vector<std::int32_t> first_dep; // a root's first dependent (-1: none)
  std::vector<std::int32_t> next_dep;  // a dependent's next sibling (-1: none)
  sq::QTile tile{};                    // the scalar path's tile / the AVX2 load staging
  vx::Slots slots{};
  void reserve(std::size_t n) {
    if (n <= cap) {
      return;
    }
    role.resize(n);
    first_dep.resize(n);
    next_dep.resize(n);
    cap = n;
  }
};

InverseScratch::InverseScratch() : impl_(std::make_unique<Impl>()) {}
InverseScratch::~InverseScratch() = default;
InverseScratch::InverseScratch(InverseScratch &&) noexcept = default;
InverseScratch &InverseScratch::operator=(InverseScratch &&) noexcept = default;
void InverseScratch::reserve(std::size_t n) { impl_->reserve(n); }
std::size_t InverseScratch::capacity() const noexcept { return impl_->cap; }

namespace {

// The lane's family point from the scalar reference (K1's map).
struct Point {
  double price = kNaN;
  double vega = kNaN;
  bool exercised = false;
  bool degenerate = false;
};

[[nodiscard]] Point eval_scalar(const Group &G, const sq::QTile &tile, const Lane &L) noexcept {
  const sq::FusedPriceVega r = (G.table != nullptr)
                                   ? sq::price_vega_on(*G.table, tile, G.S, L.K, L.x)
                                   : sq::price_and_vega(*G.family, G.S, L.K, L.x, L.q);
  Point p;
  if (r.status == sq::FusedStatus::Ok || r.status == sq::FusedStatus::NearNodeDegenerate) {
    p.price = r.price;
    p.vega = r.vega;
    p.exercised = r.exercised;
    p.degenerate = r.status == sq::FusedStatus::NearNodeDegenerate;
  }
  return p;
}

// The scalar path: chains in root order, each lane to completion.
void run_scalar(const Group &G, const InverseBatchIn &in, const InverseBatchOut &out,
                InverseScratch::Impl &sc, InverseBatchWork &w) noexcept {
  const std::size_t n = in.K.size();
  bool tile_ok = false;
  double tile_q = kNaN;
  const auto run = [&](Lane &L) {
    if (G.table != nullptr && !L.done) {
      if (tile_ok && same_bits(tile_q, L.q)) {
        ++w.tile_reuses;
      } else {
        tile_ok = sq::contract_q(*G.table, L.q, sc.tile) == sq::FusedStatus::Ok;
        tile_q = L.q;
        ++w.q_contractions;
      }
    }
    while (!L.done) {
      const Point p = eval_scalar(G, sc.tile, L);
      ++w.scalar_evaluations;
      step(L, G, p.price, p.vega, p.exercised, p.degenerate);
    }
    write_lane(L, out, w);
  };
  for (std::size_t r = 0; r < n; ++r) {
    if (sc.role[r] != kRoot) {
      continue;
    }
    Lane root;
    init_lane(root, G, in, r, nullptr);
    run(root);
    for (std::int32_t d = sc.first_dep[r]; d >= 0; d = sc.next_dep[static_cast<std::size_t>(d)]) {
      Lane dep;
      init_lane(dep, G, in, static_cast<std::size_t>(d), &root);
      run(dep);
    }
  }
}

// The AVX2 path: four slots; a slot runs a chain (root, then its dependents on the same
// tile), refilled from the root queue as lanes finish; one kernel call per step evaluates
// every busy slot.
struct Slot {
  bool busy = false;
  bool tile_ok = false;
  double tile_q = kNaN;
  Lane lane{};
  Lane anchor{};             // the chain's finished root
  std::int32_t pending = -1; // the chain's next dependent
};

void run_avx2(const Group &G, const InverseBatchIn &in, const InverseBatchOut &out,
              InverseScratch::Impl &sc, InverseBatchWork &w) noexcept {
  const std::size_t n = in.K.size();
  vx::Slots &vs = sc.slots;
  std::array<Slot, vx::kLanes> slots{};
  std::size_t cursor = 0;
  const auto load = [&](Slot &s, std::size_t k) {
    if (s.tile_ok && same_bits(s.tile_q, s.lane.q)) {
      vx::load_strike(*G.table, G.S, s.lane.K, k, vs);
      ++w.tile_reuses;
    } else {
      static_cast<void>(sq::contract_q(*G.table, s.lane.q, sc.tile));
      vx::load_slot(*G.table, sc.tile, G.S, s.lane.K, k, vs);
      s.tile_ok = true;
      s.tile_q = s.lane.q;
      ++w.q_contractions;
    }
    vs.sigma[k] = s.lane.x;
    s.busy = true;
  };
  // Fill slot k with the next lane that needs evaluating; false when none is left.
  const auto refill = [&](Slot &s, std::size_t k) -> bool {
    for (;;) {
      if (s.pending >= 0) {
        const auto d = static_cast<std::size_t>(s.pending);
        s.pending = sc.next_dep[d];
        init_lane(s.lane, G, in, d, &s.anchor);
        if (s.lane.done) {
          write_lane(s.lane, out, w);
          continue;
        }
        load(s, k);
        return true;
      }
      while (cursor < n && sc.role[cursor] != kRoot) {
        ++cursor;
      }
      if (cursor >= n) {
        return false;
      }
      const std::size_t r = cursor++;
      init_lane(s.lane, G, in, r, nullptr);
      if (s.lane.done) {
        write_lane(s.lane, out, w);
        s.anchor = s.lane;
        s.pending = sc.first_dep[r];
        continue;
      }
      load(s, k);
      return true;
    }
  };
  for (;;) {
    std::size_t busy = 0;
    std::size_t any = vx::kLanes;
    for (std::size_t k = 0; k < vx::kLanes; ++k) {
      if (!slots[k].busy) {
        static_cast<void>(refill(slots[k], k));
      }
      if (slots[k].busy) {
        ++busy;
        any = k;
      }
    }
    if (busy == 0) {
      break;
    }
    for (std::size_t k = 0; k < vx::kLanes; ++k) {
      if (!slots[k].busy) {
        vs.sigma[k] = vs.sigma[any]; // an idle slot repeats a busy one's σ (ignored)
      }
    }
    vx::Result res;
    vx::price_vega4(*G.table, vs, res);
    ++w.vector_steps;
    w.vector_lane_slots += busy;
    for (std::size_t k = 0; k < vx::kLanes; ++k) {
      Slot &s = slots[k];
      if (!s.busy) {
        continue;
      }
      step(s.lane, G, res.price[k], res.vega[k], res.exercised[k] != 0u, res.degenerate[k] != 0u);
      if (!s.lane.done) {
        vs.sigma[k] = s.lane.x;
        continue;
      }
      write_lane(s.lane, out, w);
      s.busy = false;
      if (sc.role[s.lane.i] == kRoot) {
        s.anchor = s.lane;
        s.pending = sc.first_dep[s.lane.i];
      }
    }
  }
}

} // namespace

InverseBatchError invert_many(const InverseBatchIn &in, const InverseBatchOut &out,
                              InverseScratch &scratch, InverseBatchWork *work) noexcept {
  const std::size_t n = in.K.size();
  if (!sized(in.target.size(), n, false) || !sized(in.q.size(), n, false) ||
      !sized(in.is_call.size(), n, false) || !sized(in.active.size(), n, true) ||
      !sized(in.seed_sigma.size(), n, true) || !sized(in.bracket_lo.size(), n, true) ||
      !sized(in.bracket_hi.size(), n, true) || !sized(in.price_lo.size(), n, true) ||
      !sized(in.price_hi.size(), n, true) || !sized(in.anchor.size(), n, true) ||
      out.sigma.size() != n || out.bracket_lo.size() != n || out.bracket_hi.size() != n ||
      out.price_lo.size() != n || out.price_hi.size() != n || out.residual.size() != n ||
      out.vega.size() != n || out.last_sigma.size() != n || out.evaluations.size() != n ||
      out.status.size() != n || out.flags.size() != n || out.acceptance.size() != n) {
    return InverseBatchError::SpanMismatch;
  }
  Group G;
  G.table = in.table;
  G.family = in.family;
  if ((G.table == nullptr) == (G.family == nullptr)) {
    return InverseBatchError::NoFamily;
  }
  if (G.table != nullptr) {
    if (!G.table->ok) return InverseBatchError::NoFamily;
    G.side = G.table->side;
    G.box_lo = G.table->sigma_lo;
    G.box_hi = G.table->sigma_hi;
    G.q_lo = G.table->q_lo;
    G.q_hi = G.table->q_hi;
  } else {
    if (!G.family->prepared()) return InverseBatchError::NoFamily;
    G.side = G.family->side();
    G.box_lo = G.family->sigma_lo();
    G.box_hi = G.family->sigma_hi();
    G.q_lo = G.family->q_lo();
    G.q_hi = G.family->q_hi();
  }
  try {
    scratch.reserve(n);
  } catch (const std::bad_alloc &) {
    return InverseBatchError::ScratchFailed;
  }
  G.S = in.S;
  G.tol = in.tol;
  G.floor_rel = in.vega_floor_rel;
  G.cap = in.max_evaluations;
  InverseBatchWork w;
  w.lanes = n;
  InverseScratch::Impl &sc = scratch.impl();
  const bool group_ok = std::isfinite(in.S) && in.S > 0.0 && in.tol > 0.0 &&
                        std::isfinite(in.tol) && in.max_evaluations > 0 &&
                        std::isfinite(in.vega_floor_rel) && in.vega_floor_rel >= 0.0;
  const std::uint8_t side_call = G.side == Side::Call ? 1u : 0u;
  // Pass 1: each lane's own validity.
  for (std::size_t i = 0; i < n; ++i) {
    sc.role[i] = kSkip;
    sc.first_dep[i] = -1;
    sc.next_dep[i] = -1;
    if (!in.active.empty() && in.active[i] == 0u) {
      out.status[i] = InverseStatus::Inactive;
      ++w.by_status[static_cast<std::size_t>(InverseStatus::Inactive)];
      continue;
    }
    ++w.active_lanes;
    const double K = in.K[i];
    const double t = in.target[i];
    const double q = in.q[i];
    const double blo = opt(in.bracket_lo, i);
    const double bhi = opt(in.bracket_hi, i);
    const std::int32_t a = in.anchor.empty() ? -1 : in.anchor[i];
    const bool lane_ok = group_ok && std::isfinite(K) && K > 0.0 && std::isfinite(t) && t > 0.0 &&
                         std::isfinite(q) && (in.is_call[i] != 0u ? 1u : 0u) == side_call &&
                         !(blo > bhi) && !std::isinf(blo) && !(bhi <= 0.0) &&
                         a < static_cast<std::int32_t>(n) && a != static_cast<std::int32_t>(i);
    if (!lane_ok) {
      write_refusal(i, InverseStatus::InvalidInput, out, w);
      continue;
    }
    if (q < G.q_lo || q > G.q_hi) {
      write_refusal(i, InverseStatus::QOutsideBox, out, w);
      continue;
    }
    sc.role[i] = (a >= 0) ? kDependent : kRoot;
  }
  // Pass 2: the chains. An anchor must be an anchor-free lane of the same contract; an
  // anchor that is masked or itself refused leaves its dependents to run from scratch.
  for (std::size_t i = n; i-- > 0;) {
    if (sc.role[i] != kDependent) {
      continue;
    }
    const auto a = static_cast<std::size_t>(in.anchor[i]);
    const bool anchor_lane_ok = in.anchor.empty() || in.anchor[a] < 0;
    if (!anchor_lane_ok || !same_bits(in.K[a], in.K[i]) || !same_bits(in.q[a], in.q[i])) {
      sc.role[i] = kSkip;
      write_refusal(i, InverseStatus::InvalidInput, out, w);
      continue;
    }
    if (sc.role[a] != kRoot) {
      sc.role[i] = kRoot; // no runnable anchor: from scratch
      continue;
    }
    sc.next_dep[i] = sc.first_dep[a]; // prepend: walking i downward keeps index order
    sc.first_dep[a] = static_cast<std::int32_t>(i);
  }
  const bool table_source = G.table != nullptr;
  const bool avx2 = resolve_avx2(in.isa, table_source, simd::have_avx2(), simd::use_avx2());
  if (in.isa == InverseIsa::Avx2 && !avx2) {
    for (std::size_t i = 0; i < n; ++i) {
      if (sc.role[i] != kSkip) {
        sc.role[i] = kSkip;
        write_refusal(i, InverseStatus::UnsupportedIsa, out, w);
      }
    }
  } else if (avx2) {
    run_avx2(G, in, out, sc, w);
  } else {
    run_scalar(G, in, out, sc, w);
  }
  w.avx2 = avx2;
  if (work != nullptr) {
    work->add(w);
  }
  return InverseBatchError::None;
}

} // namespace atx::vol::iv_batch
