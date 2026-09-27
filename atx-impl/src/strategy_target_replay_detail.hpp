#pragma once

// PRIVATE to atx-impl. Shared seams of the saved-blend target replay, used only by
// strategy_target_replay.cpp (definitions), strategy_nav_replay.cpp and their tests.
// The definitions forward to the target replay's file-local implementations, so the
// NAV replay reuses the exact loader, validation and target arithmetic rather than a
// copy. No nlohmann/SHA dependency here; the manifest travels as its JSON text.

#include <span>
#include <string>
#include <utility>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "strategy_target_replay.hpp"

namespace atx::impl::strategy::detail {
// One externally pinned saved blend plus its bound price role, owned. Date-major.
// volume is empty unless requested; present => finite >= 0 raw shares, absent => NaN.
struct LoadedSavedBlend {
  atx::usize dates{}, names{}, begin{}, end{};
  std::vector<atx::f64> signal, close, raw, volume;
  std::vector<atx::u8> member, present;
  std::vector<atx::i64> sessions;
  std::vector<atx::u64> ids;
  std::string manifest_json; // the pinned combined manifest, re-serialized
  // Borrowed view; valid while *this is alive and unmodified.
  [[nodiscard]] TargetReplayInput view() const;
};
// Pinned load exactly as run_target_replay performs it (combined SHA, role SHA ==
// manifest.role_manifest_sha256, source SHA, axes, member == member&present&close>0).
// Admits equal-family/equal-within and pinned-candidate-weights blends; the latter's
// composition_weights_sha256 stays in manifest_json (published as source_bindings).
// with_volume additionally requires the role, volume_basis "raw-share-volume" and the
// volume contract, and charges 8 more admitted bytes per cell.
[[nodiscard]] atx::core::Result<LoadedSavedBlend> load_saved_blend(
    const TargetReplayRunConfig& cfg, bool with_volume);
// The target replay's own recipe/geometry/axes/support validation and budget.
[[nodiscard]] atx::core::Status validate_replay_input(const TargetReplayInput& in,
                                                      const TargetReplayConfig& cfg);
// Centered tied rank of members, demeaned, gross 1; an all-tie row stays flat.
void desired_target(std::span<const atx::f64> signal, std::span<const atx::u8> member,
                    std::vector<std::pair<atx::f64, atx::usize>>& row,
                    std::vector<atx::f64>& target);
// Forced exits to zero every decision; partial move by the (v2 budget-capped)
// fraction on rebalance decisions. `current` is updated in place to the plan and
// `out` accumulates the planned turnover/exposure fields.
void update_weights(const TargetReplayInput& in, const TargetReplayConfig& cfg, atx::usize d,
                    bool rebalance, atx::f64 spent, const std::vector<atx::f64>& desired,
                    std::vector<atx::f64>& current, TargetReplayDay& out);
// YYYYMM of a UTC-midnight session key.
[[nodiscard]] atx::u32 calendar_month(atx::i64 session_ns);
} // namespace atx::impl::strategy::detail
