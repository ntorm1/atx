#pragma once
// atx::impl::strategy — the mining campaign's cycle-ledger line (platform v8 H-3, Ruling E-33;
// review MINE-1). The C++ twin of atx-impl/tools/backtest_integrity.py `campaign_line`: the
// verb builds its OUTPUT/ledger_line.json here and refuses to write a line that campaign_line
// would refuse, so `research_cycle.py ledger-campaign` can append every line the verb writes.
//
// The line is one compact, sorted-key atx.trial-ledger/v1 JSON object:
//   {"budget": N, "campaign": ID, "confirm": {"begin": D0, "end": D1}, "count": 0,
//    "kind": "mining-campaign", "origin": "mined", "recipe_sha256": R,
//    "registry": {"bytes": B, "chain_head": SHA, "count": C, "path": P, "total": T},
//    "rule": "mined-v1", "schema": "atx.trial-ledger/v1", "trial_id": TID, "window_id": W}
// N is the campaign's --budget (pre-registration rule 10, Ruling E-32a), at least C and at most
// kMinedMaxBudget (Ruling PM4-13).
// chain_head is the SHA-256 (64 lowercase hex) of the campaign registry's first B bytes, the
// whole append-only log as the campaign left it. Ruling E-33a: C is the number of records this
// campaign added (>= 1), T the registry's cumulative size (>= C), so a registry shared by
// campaigns never counts an earlier campaign's trials twice. Review MINE-3: R is the SHA-256 of
// the campaign's trial recipe, which binds the confirm window [D0, D1) with the role, fields,
// library and pool digests; trial_id is the first 16 hex digits of SHA-256 of the compact
// ["mining-campaign", R, chain_head], and the ledger refuses a second line on R (a second
// confirm read on the same identity).
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy {

struct MineLedgerLine {
  std::string campaign_id;
  std::string registry_path; // as given; written with '/' separators
  std::string registry_head; // SHA-256 of the registry's first registry_bytes bytes
  atx::u64 registry_bytes{};
  atx::u64 registry_count{}; // records this campaign added (Ruling E-33a)
  atx::u64 registry_total{}; // the registry's records after it
  atx::u64 budget{};         // the campaign's trial budget, fixed in advance
  std::string recipe_sha256; // the campaign's trial recipe (its identity)
  std::string confirm_begin, confirm_end; // YYYY-MM-DD, the confirm window [begin, end)
  std::string window_id;
};

// The line's compact JSON text (no newline). Err when the line would be refused.
[[nodiscard]] atx::core::Result<std::string> mine_ledger_line(const MineLedgerLine &line);

// Why backtest_integrity.campaign_line would not produce `text` (a ledger_line.json body, an
// optional trailing newline allowed); empty when it would, byte for byte.
[[nodiscard]] std::string mine_ledger_line_problem(std::string_view text);

} // namespace atx::impl::strategy
