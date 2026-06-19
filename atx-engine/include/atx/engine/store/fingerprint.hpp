#pragma once
// atx::engine::store::fingerprint — content hash over all run inputs. Equal fingerprint
// ⇒ identical inputs ⇒ a re-launch is a replay. FNV-1a 64 fold over a canonical byte
// stream (length-prefixed fields so concatenation is unambiguous). No compile-time
// seeds / wall-clock — the same inputs hash the same on every platform and run.

#include <string>
#include <string_view>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::store {

struct RunInputs {
  std::string engine_git_sha;
  std::string config_normalized;
  atx::u64 universe_content_hash{0};
  atx::u64 snapshot_content_hash{0};
  atx::u64 master_seed{0};
  std::string gate_config;
};

namespace fingerprint {

inline constexpr atx::u64 kFnvOffset = 1469598103934665603ull;
inline constexpr atx::u64 kFnvPrime  = 1099511628211ull;

[[nodiscard]] inline atx::u64 fold_bytes(atx::u64 h, std::string_view bytes) noexcept {
  for (const char c : bytes) {
    h ^= static_cast<atx::u8>(c);
    h *= kFnvPrime;
  }
  return h;
}

// Length-prefix each string field (8 raw length bytes) so "ab"+"c" != "a"+"bc".
[[nodiscard]] inline atx::u64 fold_string(atx::u64 h, std::string_view s) noexcept {
  atx::u64 n = s.size();
  for (int i = 0; i < 8; ++i) { h ^= static_cast<atx::u8>(n & 0xFF); h *= kFnvPrime; n >>= 8; }
  return fold_bytes(h, s);
}

[[nodiscard]] inline atx::u64 fold_u64(atx::u64 h, atx::u64 v) noexcept {
  for (int i = 0; i < 8; ++i) { h ^= static_cast<atx::u8>(v & 0xFF); h *= kFnvPrime; v >>= 8; }
  return h;
}

[[nodiscard]] inline atx::u64 compute(const RunInputs& in) noexcept {
  atx::u64 h = kFnvOffset;
  h = fold_string(h, in.engine_git_sha);
  h = fold_string(h, in.config_normalized);
  h = fold_u64(h, in.universe_content_hash);
  h = fold_u64(h, in.snapshot_content_hash);
  h = fold_u64(h, in.master_seed);
  h = fold_string(h, in.gate_config);
  return h;
}

// True iff a COMMITTED run already carries this fingerprint (a replay).
[[nodiscard]] inline atx::core::Result<bool>
is_replay(atx::core::db::Database& db, atx::u64 fp_value) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "SELECT 1 FROM run WHERE run_fingerprint = ?1 AND status = 'committed' LIMIT 1"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(fp_value)));
  ATX_TRY(const auto step, stmt->step());
  return atx::core::Ok(step == atx::core::db::Statement::Step::Row);
}

}  // namespace fingerprint
}  // namespace atx::engine::store
