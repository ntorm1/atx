#pragma once

// atx::engine::research::ledger -- the hash chain of the research trial ledger
// (atx.trial-ledger/v1, build-equity/trials.jsonl; review C-6), the one C++ copy of the rule
// backtest_integrity.py writes and reads (chain_head, _walk). Platform P9 B2; carry SQL-7: the
// SQLite catalog (SQL2) and SQL4's ingest_ledger.cpp call chain_head / prefix_heads and never
// re-implement the rule (G-P5). The ledger's bytes and append format are unchanged (SQL-9).
//
// THE RULE (kChainRule). The head starts at 64 zeros. For each non-blank line, in file order:
//   * a CHAINED line (its top-level JSON object has the key "prev_sha256"): head =
//     line_sha256(line), the SHA-256 of the line's UTF-8 bytes without its line end;
//   * an UNCHAINED (legacy, pre-v8) line: head = fold_head(head, line) = SHA-256 of the ASCII
//     text head + line_sha256(line).
// So the first chained line pins every legacy line before it and each chained line pins the
// whole prefix. The rule applies to ANY prefix of the file: chain_head(lines[0..k)) is the
// head a verdict recorded when the ledger held those k lines.
//
// VERIFICATION (verify_chain, the reader's _walk): every line's "schema" is
// atx.trial-ledger/v1; a chained line's prev_sha256 equals the head of the lines before it;
// no unchained line (prev_sha256 absent or null) follows a chained one.
//
// Lines are passed as texts; a trailing CR / LF is stripped as Python's rstrip("\r\n") does
// and a whitespace-only line is not a ledger line (skipped, as the readers skip it). A JSON
// line may carry Python's NaN / Infinity tokens; they read as null (no rule reads them).

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::ledger {

inline constexpr std::string_view kLedgerSchema = "atx.trial-ledger/v1";
inline constexpr std::string_view kChainGenesis =
    "0000000000000000000000000000000000000000000000000000000000000000";
// The rule's name, stored beside a computed head (SQL4 ledger_state.head_rule). A change of
// the rule is a new name, never a silent edit.
inline constexpr std::string_view kChainRule = "atx-ledger-chain-fold/v1";

struct ChainHead {
  std::string head_sha256{std::string(kChainGenesis)};
  usize lines{}; // non-blank lines folded
  std::string rule{std::string(kChainRule)};
};

// The line without its trailing CR / LF characters (rstrip("\r\n")).
[[nodiscard]] std::string_view strip_line_end(std::string_view line) noexcept;
// Whether the line holds only ASCII whitespace (a non-line).
[[nodiscard]] bool is_blank_line(std::string_view line) noexcept;

// SHA-256 hex of the line's bytes without its line end.
[[nodiscard]] core::Result<std::string> line_sha256(std::string_view line);
// The head after an unchained line: SHA-256 of head + line_sha256(line).
[[nodiscard]] core::Result<std::string> fold_head(std::string_view head, std::string_view line);
// Whether the line's top-level JSON object has the key prev_sha256. ParseError when the line
// is not a JSON object.
[[nodiscard]] core::Result<bool> is_chained_line(std::string_view line);

// THE CHAIN-HEAD FUNCTION (carry SQL-7): the head after the given line texts, in file order,
// under kChainRule. Any prefix of a ledger is valid input. Computes, never verifies links
// (backtest_integrity.chain_head). ParseError when a non-blank line is not a JSON object.
[[nodiscard]] core::Result<ChainHead> chain_head(std::span<const std::string> lines);

// The head after each line: out[k] = chain_head(lines[0..k]).head_sha256 (a blank line
// repeats the head before it). One pass; SQL4's verify --ledger matches recorded heads here.
[[nodiscard]] core::Result<std::vector<std::string>>
prefix_heads(std::span<const std::string> lines);

// The reader's walk (backtest_integrity._walk): verifies schema and links, returns the head.
// ParseError on a non-object line; Internal on a wrong schema, a broken link or an unchained
// line after a chained one. Messages name the 1-based physical line number.
[[nodiscard]] core::Result<ChainHead> verify_chain(std::span<const std::string> lines);

} // namespace atx::engine::research::ledger
