#pragma once

// atx::engine::factory::pareto — engine-local NSGA-II primitives (S4.1, plan §4.1).
//
// ===========================================================================
//  WHAT THIS IS — multi-objective selection, made deterministic
// ===========================================================================
//  S3-5's SearchDriver maximizes a SCALAR `raw = wq * diversify * robust`. S4
//  replaces that collapse with a Pareto frontier over a VECTOR of objectives
//  (wq, diversify, robust; later novelty + cost), so a high-wq/low-diversify
//  candidate and a low-wq/high-diversify candidate can BOTH survive instead of
//  one strictly dominating the scalar. NSGA-II (Deb, Pratap, Agarwal & Meyarivan
//  2002, "A Fast and Elitist Multiobjective Genetic Algorithm: NSGA-II") is the
//  canonical selector: rank by non-dominated front, break ties within a front by
//  crowding distance (a density estimate that spreads survivors across the
//  frontier instead of clumping them).
//
//  THREE PRIMITIVES, all MAXIMIZATION (cost objectives arrive PRE-NEGATED so
//  higher == better universally — dominance stays a pure >= comparison):
//    * dominates(a, b)            — a Pareto-dominates b.
//    * fast_nondominated_sort(..) — front index per genome (Deb §III-A, O(N^2)).
//    * crowding_distance(..)      — per-front density (boundary +inf, interior gaps).
//
// ===========================================================================
//  DETERMINISM (load-bearing — same as the rest of the factory, F1/F2)
// ===========================================================================
//  Every iteration / sort here resolves in CANONICAL-ID ORDER (the caller passes
//  the precomputed canon_order, the value-based total order over genomes), BEFORE
//  any RNG draw downstream. Equal-objective genomes therefore land in a FIXED
//  front sequence and crowding ties break the same way every run — so the digest
//  is byte-identical across {1,2,4,8} DetPool workers. No RNG, no floating-point
//  order ambiguity (sorts use a strict-weak-ordering comparator with a canonical
//  final tie-break), no hidden global state.
//
// ===========================================================================
//  NaN POLICY — a NaN objective is treated as -inf
// ===========================================================================
//  A NaN objective (e.g. a degenerate-fold fitness) reads as -inf, so a genome
//  with any NaN objective is dominated by any finite genome on that axis and
//  NEVER poisons a front. (A raw `NaN >= x` is false both ways, which would make
//  a NaN genome spuriously non-dominated — the -inf projection fixes that.)
//
//  Header-only, alloc-light, every function inline. fast_nondominated_sort and
//  crowding_distance each allocate O(N) scratch ONCE per call (the cold per-
//  generation selection path, never the VM hot loop). dominates is allocation-
//  free and noexcept.

#include <algorithm> // std::sort, std::max, std::min
#include <cmath>     // std::isnan
#include <cstddef>   // (size types via atx/core)
#include <limits>    // std::numeric_limits
#include <span>      // std::span
#include <vector>    // std::vector (front scratch)

#include "atx/core/types.hpp" // atx::f64, atx::u16, atx::usize

namespace atx::engine::factory {

// =========================================================================
//  ObjMatrix — a non-owning row-major objective matrix view.
//
//  `data` is a flat [n * k] row-major span: genome i's objective vector is
//  data[i*k .. i*k + k). All three primitives read through `row(i)`.
//
//  SAFETY: ObjMatrix is a VIEW — `data` must outlive every call that reads it.
//  The caller owns the backing storage (the SearchDriver sizes one objectives
//  scratch buffer per generation and reuses it; pareto.hpp never allocates the
//  matrix). Trivial aggregate (Rule of Zero); copyable (it is just a span + two
//  sizes).
// =========================================================================
struct ObjMatrix {
  std::span<const atx::f64> data; // flat row-major [n * k]
  atx::usize n{0};                // number of genomes (rows)
  atx::usize k{0};                // number of objectives (columns)

  // Objective vector of genome i (a length-k subspan). Precondition: i < n and
  // data.size() >= n * k (the caller's invariant; asserted via the subspan).
  [[nodiscard]] std::span<const atx::f64> row(atx::usize i) const noexcept {
    return data.subspan(i * k, k);
  }
};

namespace detail {

// Project a raw objective to its dominance value: NaN -> -inf (a NaN-objective
// genome is dominated by any finite one and never wins a front). Finite values
// pass through unchanged. noexcept, allocation-free.
[[nodiscard]] inline atx::f64 dom_value(atx::f64 x) noexcept {
  return std::isnan(x) ? -std::numeric_limits<atx::f64>::infinity() : x;
}

} // namespace detail

// =========================================================================
//  dominates — Pareto dominance for MAXIMIZATION.
//
//  `a` dominates `b` iff a[k] >= b[k] for ALL k AND a[k] > b[k] for SOME k.
//  NaN objectives are projected to -inf (detail::dom_value) so a NaN axis can
//  never make a genome spuriously non-dominated. Precondition: a.size()==b.size().
//  Pure, noexcept, allocation-free.
// =========================================================================
[[nodiscard]] inline bool dominates(std::span<const atx::f64> a,
                                    std::span<const atx::f64> b) noexcept {
  bool strictly_better_somewhere = false;
  const atx::usize k = a.size();
  for (atx::usize i = 0; i < k; ++i) {
    const atx::f64 av = detail::dom_value(a[i]);
    const atx::f64 bv = detail::dom_value(b[i]);
    if (av < bv) {
      return false; // a is worse on objective i -> cannot dominate
    }
    if (av > bv) {
      strictly_better_somewhere = true;
    }
  }
  return strictly_better_somewhere;
}

// =========================================================================
//  fast_nondominated_sort — front index per genome (Deb et al. 2002 §III-A).
//
//  O(N^2): for each genome p compute its domination count n_p (how many genomes
//  dominate it) and its dominated set S_p. Front 0 = {p : n_p == 0}; peel it,
//  decrement n_q for every q in the S_p of a just-peeled p, and the newly-zeroed
//  genomes form the next front. Returns the front index per genome (front 0 is
//  best). Every genome is placed exactly once; the loop is bounded by N fronts.
//
//  DETERMINISM: genomes are iterated in `canon_order` when building each front,
//  so equal-objective genomes land in a fixed front SEQUENCE. The returned vector
//  is indexed by genome id (NOT canon position), so it is canon-order-independent
//  in value (an all-equal panel yields the same per-id fronts for any canon_order).
//
//  Precondition: canon_order is a permutation of [0, obj.n); obj.data.size() >=
//  obj.n * obj.k. Allocates O(N + sum|S_p|) scratch once.
// =========================================================================
[[nodiscard]] inline std::vector<atx::u16>
fast_nondominated_sort(const ObjMatrix &obj, std::span<const atx::usize> canon_order) {
  const atx::usize n = obj.n;
  std::vector<atx::u16> front_of(n, 0);
  if (n == 0) {
    return front_of;
  }
  std::vector<atx::usize> dom_count(n, 0);           // n_p: how many dominate p
  std::vector<std::vector<atx::usize>> dominated(n); // S_p: who p dominates

  // Build n_p and S_p. Iterate ordered pairs in canon_order so S_p lists are in a
  // fixed sequence (the peel below then visits them deterministically).
  for (const atx::usize p : canon_order) {
    for (const atx::usize q : canon_order) {
      if (p == q) {
        continue;
      }
      if (dominates(obj.row(p), obj.row(q))) {
        dominated[p].push_back(q);
      } else if (dominates(obj.row(q), obj.row(p))) {
        ++dom_count[p];
      }
    }
  }

  // Peel fronts. `current` holds the present front in canon_order; `next` the one
  // being assembled. Bounded by N iterations (each genome peeled exactly once).
  std::vector<atx::usize> current;
  current.reserve(n);
  for (const atx::usize p : canon_order) {
    if (dom_count[p] == 0) {
      front_of[p] = 0;
      current.push_back(p);
    }
  }
  atx::u16 front = 0;
  while (!current.empty()) {
    std::vector<atx::usize> next;
    for (const atx::usize p : current) {
      for (const atx::usize q : dominated[p]) {
        --dom_count[q];
        if (dom_count[q] == 0) {
          front_of[q] = static_cast<atx::u16>(front + 1);
          next.push_back(q);
        }
      }
    }
    current = std::move(next);
    ++front;
  }
  return front_of;
}

// =========================================================================
//  crowding_distance — per-front density estimate (Deb et al. 2002 §III-B).
//
//  For the genomes in `front_members`, returns a distance per GENOME ID (a full
//  length-n vector; non-members and unreferenced ids stay 0). Per objective:
//  sort the front members by that objective's value (canonical-id tie-break),
//  give the two boundary members +inf, and add to each interior member the
//  normalized gap between its two neighbours (|f_{i+1} - f_{i-1}| / range). A
//  degenerate objective (range 0, constant across the front) contributes 0 — no
//  division by zero. A single-member or two-member front is all-boundary (+inf).
//
//  DETERMINISM: the per-objective sort breaks value ties by canon_order position,
//  so the boundary assignment and neighbour gaps are identical regardless of the
//  order `front_members` is listed in (the result is permutation-invariant in the
//  member listing). Allocates O(|front|) scratch once.
//
//  Precondition: every id in front_members is < obj.n; canon_order maps id ->
//  canonical rank (canon_order is a permutation of [0, obj.n)).
// =========================================================================
[[nodiscard]] inline std::vector<atx::f64>
crowding_distance(const ObjMatrix &obj, std::span<const atx::usize> front_members,
                  std::span<const atx::usize> canon_order) {
  const atx::usize n = obj.n;
  std::vector<atx::f64> distance(n, 0.0);
  const atx::usize m = front_members.size();
  if (m == 0) {
    return distance;
  }
  constexpr atx::f64 kInf = std::numeric_limits<atx::f64>::infinity();
  if (m <= 2) {
    for (const atx::usize id : front_members) {
      distance[id] = kInf; // every member is a boundary
    }
    return distance;
  }

  // canon_rank[id] -> canonical position, for the value-tie tie-break. Sized n
  // (cheap, cold path); only the member ids are read.
  std::vector<atx::usize> canon_rank(n, 0);
  for (atx::usize r = 0; r < canon_order.size(); ++r) {
    canon_rank[canon_order[r]] = r;
  }

  std::vector<atx::usize> order(front_members.begin(), front_members.end());
  for (atx::usize o = 0; o < obj.k; ++o) {
    // Sort the front by objective o ascending; canonical-id tie-break so equal
    // objective values resolve in a fixed (replayable) sequence.
    std::sort(order.begin(), order.end(), [&](atx::usize a, atx::usize b) {
      const atx::f64 fa = obj.row(a)[o];
      const atx::f64 fb = obj.row(b)[o];
      if (fa != fb) {
        return fa < fb;
      }
      return canon_rank[a] < canon_rank[b];
    });
    const atx::f64 lo = obj.row(order.front())[o];
    const atx::f64 hi = obj.row(order.back())[o];
    const atx::f64 range = hi - lo;
    // Boundaries -> +inf (always selected; they anchor the frontier extent).
    distance[order.front()] = kInf;
    distance[order.back()] = kInf;
    if (range <= 0.0) {
      continue; // degenerate objective: 0 contribution, no division by zero
    }
    for (atx::usize i = 1; i + 1 < m; ++i) {
      // Skip a member already pinned to +inf by another objective's boundary.
      if (distance[order[i]] == kInf) {
        continue;
      }
      const atx::f64 prev = obj.row(order[i - 1])[o];
      const atx::f64 nextv = obj.row(order[i + 1])[o];
      distance[order[i]] += (nextv - prev) / range;
    }
  }
  return distance;
}

} // namespace atx::engine::factory
