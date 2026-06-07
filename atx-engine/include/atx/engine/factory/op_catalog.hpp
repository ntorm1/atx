#pragma once

// atx::engine::factory — OpCatalog: precomputed op-candidate buckets (S3-1, §0.4).
//
// As-built reconciliation (§0.4): `alpha::Library` exposes ONLY `find(name)` and
// `size()` — there is NO row iterator, NO commutative flag, and NO per-operand
// dtype vector. So op-swap cannot ask the Library "what else fits here?". The
// OpCatalog answers that question once, at construction, by walking the static
// built-in table (`alpha::detail::builtin_ops()`) plus any user rows the borrowed
// Library carries, grouping the NAMED Call ops by a bucket key
//   (result-shape category, out DType, materialized arity)
// so a Call node can be swapped for another op that yields the same result slot.
//
// It also tags the DECLARED-commutative opcode set
//   { Add, Mul, MinP, MaxP, And, Or, CmpEq, CmpNe }
// (the registry won't tell you — §0.4). This is consumed by op-swap (to avoid a
// no-op swap) and, in S3-2, by the canonical-hash commutative-operand sort.
//
// SAFETY: the Library is BORROWED (`const Library&`) and MUST outlive the
// catalog — every cached `const OpSig*` points into that one Library's rows.
// The Factory owns one Library for the whole run; the catalog (and every genome)
// borrows from it.
//
// Header-only; constructed once on a COLD path (vector allocation is fine).

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

namespace atx::engine::factory {

using atx::core::Xoshiro256pp;
using atx::engine::alpha::DType;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::OpSig;
using atx::engine::alpha::Shape;

// =========================================================================
//  Commutative tag — the S3-declared set (§0.4). The registry has no flag.
// =========================================================================

// True iff `op` is one of the eight DECLARED-commutative opcodes. Operand order
// is value-irrelevant for these, so op-swap may treat them interchangeably and
// (S3-2) the canonical hash sorts their operands.
[[nodiscard]] inline bool is_commutative(OpCode op) noexcept {
  switch (op) {
  case OpCode::Add:
  case OpCode::Mul:
  case OpCode::MinP:
  case OpCode::MaxP:
  case OpCode::And:
  case OpCode::Or:
  case OpCode::CmpEq:
  case OpCode::CmpNe:
    return true;
  default:
    return false;
  }
}

// =========================================================================
//  OpCatalog — named-Call op candidates bucketed by (shape-cat, dtype, arity).
// =========================================================================

class OpCatalog {
public:
  // Build the catalog from the run-wide Library.
  // SAFETY: `lib` is borrowed only for the duration of construction. The cached
  // `const OpSig*` candidates alias the static `builtin_ops()` table (program
  // lifetime), so they never dangle; the Library reference establishes which run
  // the catalog belongs to (the same one every genome borrows ops from — §0.4).
  explicit OpCatalog(const alpha::Library &lib) {
    static_cast<void>(lib); // built-ins enumerated from the static table below
    build();
  }

  // Sample a replacement op for a Call slot of result `(shape, dtype)` and the
  // given materialized `arity`, drawn uniformly from the same bucket but never
  // equal to `current`. Returns std::nullopt when the bucket has no alternative
  // (e.g. the only op producing that result type is `current` itself).
  //
  // Determinism (F1): the single draw is the sole entropy; the bucket is in a
  // fixed (build-order) sequence, so the same rng state yields the same pick.
  [[nodiscard]] std::optional<const OpSig *> sample_compatible(Shape shape, DType dtype,
                                                               atx::usize arity,
                                                               const OpSig *current,
                                                               Xoshiro256pp &rng) const {
    const Bucket *b = find_bucket(shape, dtype, arity);
    if (b == nullptr) {
      return std::nullopt;
    }
    // Count alternatives (!= current) without allocating; then pick the k-th.
    atx::usize alternatives = 0;
    for (const OpSig *sig : b->ops) {
      if (sig != current) {
        ++alternatives;
      }
    }
    if (alternatives == 0) {
      return std::nullopt;
    }
    const atx::usize k = static_cast<atx::usize>(rng.next_u64() % alternatives);
    atx::usize seen = 0;
    for (const OpSig *sig : b->ops) {
      if (sig == current) {
        continue;
      }
      if (seen == k) {
        return sig;
      }
      ++seen;
    }
    return std::nullopt; // unreachable: k < alternatives
  }

  // Number of distinct (shape, dtype, arity) buckets — diagnostics / tests.
  [[nodiscard]] atx::usize bucket_count() const noexcept { return buckets_.size(); }

private:
  // A coarse result-shape category: the named ops' shape rule is determined by
  // family (Ts*→Panel, Cs*→CrossSection, element-wise→broadcast). Two ops share
  // a bucket only if they yield the SAME family AND out dtype AND arity, so a
  // swap stays in the same result slot (the analyze backstop catches residue).
  enum class ShapeCat : atx::u8 { Panel, CrossSection, Elementwise };

  struct Key {
    ShapeCat cat{ShapeCat::Elementwise};
    DType dtype{DType::F64};
    atx::usize arity{0};
    [[nodiscard]] bool operator==(const Key &o) const noexcept {
      return cat == o.cat && dtype == o.dtype && arity == o.arity;
    }
  };

  struct Bucket {
    Key key;
    std::vector<const OpSig *> ops;
  };

  // Classify a named op's result-shape family from its shape_of function pointer
  // (the registry's pure shape rules are shared singletons we can compare to).
  [[nodiscard]] static ShapeCat shape_cat_of(const OpSig &sig) noexcept {
    if (sig.shape_of == &alpha::shape_panel) {
      return ShapeCat::Panel;
    }
    if (sig.shape_of == &alpha::shape_cross_section) {
      return ShapeCat::CrossSection;
    }
    return ShapeCat::Elementwise; // shape_elementwise / shape_unary
  }

  // Map a (result Shape) to the bucket category used at lookup time. A Ts* op
  // always yields Panel; a Cs* op always yields CrossSection; an element-wise op
  // broadcasts (so its result shape varies) — we file/look-up those by family.
  [[nodiscard]] static ShapeCat cat_for_lookup(Shape shape, ShapeCat fallback) noexcept {
    switch (shape) {
    case Shape::Panel:
      return ShapeCat::Panel;
    case Shape::CrossSection:
      return ShapeCat::CrossSection;
    case Shape::Scalar:
      return fallback;
    }
    return fallback;
  }

  void build() {
    // Walk the static built-in table. Record ops are EXCLUDED (their result is a
    // named pin tuple, not a swappable single-value slot). Group-aware ops are
    // kept (their bucket is still (cat,dtype,arity)); analyze rejects a bad arg.
    for (const OpSig &sig : alpha::detail::builtin_ops()) {
      add_op(sig);
    }
    // User ops registered into the borrowed Library beyond the built-ins are not
    // enumerable (no iterator). Tests that need extra ops register them and rely
    // on find(); op-swap over built-ins is the spec'd surface (§0.4).
  }

  void add_op(const OpSig &sig) {
    if (!sig.pins.empty()) {
      return; // record op: not a swappable single-value slot
    }
    const Key key{shape_cat_of(sig), sig.out_dtype, static_cast<atx::usize>(sig.min_arity)};
    for (Bucket &b : buckets_) {
      if (b.key == key) {
        b.ops.push_back(&sig);
        return;
      }
    }
    buckets_.push_back(Bucket{key, std::vector<const OpSig *>{&sig}});
  }

  [[nodiscard]] const Bucket *find_bucket(Shape shape, DType dtype, atx::usize arity) const {
    // Try the exact shape-derived category first, then the element-wise fallback
    // (an element-wise op produces Panel/CrossSection/Scalar by broadcast, so a
    // Panel-result element-wise node is filed under Elementwise, not Panel).
    for (const ShapeCat cat :
         {cat_for_lookup(shape, ShapeCat::Elementwise), ShapeCat::Elementwise}) {
      const Key key{cat, dtype, arity};
      for (const Bucket &b : buckets_) {
        if (b.key == key) {
          return &b;
        }
      }
    }
    return nullptr;
  }

  std::vector<Bucket> buckets_;
};

} // namespace atx::engine::factory
