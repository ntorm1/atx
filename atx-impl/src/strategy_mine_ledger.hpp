#pragma once
// atx::impl::strategy — the mining campaign's cycle-ledger line (platform v8 H-3, Ruling E-33;
// review MINE-1). The C++ twin of atx-impl/tools/backtest_integrity.py `campaign_line`: the
// verb builds its OUTPUT/ledger_line.json here and refuses to write a line that campaign_line
// would refuse, so `research_cycle.py ledger-campaign` can append every line the verb writes.
//
// The line is one compact, sorted-key atx.trial-ledger/v1 JSON object:
//   {"campaign": ID, "count": 0, "kind": "mining-campaign", "origin": "mined",
//    "registry": {"bytes": B, "chain_head": SHA, "count": C, "path": P, "total": T},
//    "schema": "atx.trial-ledger/v1", "trial_id": TID, "window_id": W}
// chain_head is the SHA-256 (64 lowercase hex) of the campaign registry's first B bytes, the
// whole append-only log as the campaign left it; trial_id is the first 16 hex digits of
// SHA-256 of the compact ["mining-campaign", chain_head]. Ruling E-33a: C is the number of
// records this campaign added (>= 1), T the registry's cumulative size (>= C), so a registry
// shared by campaigns never counts an earlier campaign's trials twice.
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
  std::string window_id;
};

// The line's compact JSON text (no newline). Err when the line would be refused.
[[nodiscard]] atx::core::Result<std::string> mine_ledger_line(const MineLedgerLine &line);

// Why backtest_integrity.campaign_line would not produce `text` (a ledger_line.json body, an
// optional trailing newline allowed); empty when it would, byte for byte.
[[nodiscard]] std::string mine_ledger_line_problem(std::string_view text);

} // namespace atx::impl::strategy
