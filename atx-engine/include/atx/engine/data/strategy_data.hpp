#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"

namespace atx::engine::data {

// Explicit research snapshot, not verified historical publication/common-stock
// evidence. Panel mask is physical presence; membership is independent and uses
// strictly prior-session liquidity. Missing marks never become zero returns.
struct StrategyRoleData {
  alpha::Panel panel;
  std::vector<i64> session_keys{}, mark_times_ns{}, decision_times_ns{};
  std::vector<u64> instrument_ids{};
  std::vector<u8> decision_member{};
  usize score_begin{}, score_end{};
  std::string source_sha256{}, membership_recipe{}, clock_recipe{}, manifest_sha256{};
};

// Owns only one bounded role. SHA checks stream through the same captured file
// handle that fills each final vector; no full duplicate payload buffer. Caller
// pins the returned manifest SHA externally before treating a role as frozen.
[[nodiscard]] core::Result<StrategyRoleData> read_strategy_role(
    const std::string& manifest_path, u64 max_bytes);

// Ruling E-10 (platform v8 review B-3). A role built by prepare_recent_research.py with
// --delisting-returns (manifest `universe.delisting.returns_applied` true) carries imputed
// terminal returns: close, raw_close and present of every applied termination session come
// from a return whose cause may be classified after that session. Such a role may mark the
// NAV replay's books (the label role of Ruling E-25) and nothing else: no signal, field or
// IC score may read it.
//
// Whether `manifest_text` (a role manifest) declares applied delisting returns. No `universe`
// or no `universe.delisting` block: false. Err (InvalidArgument) when the text is not a JSON
// object, `universe` or `delisting` is not an object, or the delisting block lacks a boolean
// `returns_applied` (the builder always writes it, so its absence cannot be shown safe).
[[nodiscard]] core::Result<bool> role_delisting_returns_applied(std::string_view manifest_text);

// The signal-role refusal: Err (InvalidArgument) naming `manifest_path` and the manifest
// field when `manifest_text` declares applied delisting returns, or when the declaration is
// malformed. Every verb that evaluates or scores signals on a role calls it before reading
// any payload (the IC runner at admission, the marginal verb), and a NAV verb that binds a
// separate label role calls it on its signal role.
[[nodiscard]] core::Status refuse_delisting_returns_signal_role(std::string_view manifest_text,
                                                                std::string_view manifest_path);

} // namespace atx::engine::data
