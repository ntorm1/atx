#include "atx/engine/factory/op_catalog.hpp"

#include <optional>

#include "atx/engine/alpha/registry.hpp"

namespace atx::engine::factory {

OpCatalog::OpCatalog(const alpha::Library &lib) {
  static_cast<void>(lib); // built-ins enumerated from the static table below
  build();
}

[[nodiscard]] std::optional<const OpSig *>
OpCatalog::sample_compatible(Shape shape, DType dtype, atx::usize arity, const OpSig *current,
                             Xoshiro256pp &rng) const {
  // The node satisfies its CURRENT op's group-classifier requirement, so only
  // ops with the same requirement keep its operands valid (S3.4 bucket key).
  const bool group =
      current != nullptr && alpha::detail::needs_group_arg(current->opcode);
  const Bucket *b = find_bucket(shape, dtype, arity, group);
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

[[nodiscard]] std::optional<OpCode>
OpCatalog::sample_compatible_opcode(Shape shape, DType dtype, atx::usize arity, OpCode current,
                                    Xoshiro256pp &rng) const {
  const OpcodeBucket *b = find_opcode_bucket(shape, dtype, arity);
  if (b == nullptr) {
    return std::nullopt;
  }
  atx::usize alternatives = 0;
  for (const OpCode op : b->ops) {
    if (op != current) {
      ++alternatives;
    }
  }
  if (alternatives == 0) {
    return std::nullopt;
  }
  const atx::usize k = static_cast<atx::usize>(rng.next_u64() % alternatives);
  atx::usize seen = 0;
  for (const OpCode op : b->ops) {
    if (op == current) {
      continue;
    }
    if (seen == k) {
      return op;
    }
    ++seen;
  }
  return std::nullopt; // unreachable: k < alternatives
}

void OpCatalog::build() {
  // Walk the static built-in table. Record ops are EXCLUDED (their result is a
  // named pin tuple, not a swappable single-value slot). Group-aware ops are
  // kept (their bucket is still (cat,dtype,arity)); analyze rejects a bad arg.
  for (const OpSig &sig : alpha::detail::builtin_ops()) {
    add_op(sig);
  }
  // User ops registered into the borrowed Library beyond the built-ins are not
  // enumerable (no iterator). Tests that need extra ops register them and rely
  // on find(); op-swap over built-ins is the spec'd surface (§0.4).

  // Bucket the bare infix/unary OpCode set (the half with no OpSig*, §0.4).
  for (const OpcodeRow &row : swappable_opcodes()) {
    add_opcode(row);
  }
}

} // namespace atx::engine::factory
