#pragma once

// atx::engine::research::ledger -- the research trial ledger file (atx.trial-ledger/v1) as the
// statistics read it: every non-blank line, its JSON object and the verified chain head
// (backtest_integrity.ledger_read / ledger_head; dsr_total.read_ledger). Read-only: the
// ledger's writer stays Python through P9 (ruling SQL-9), so nothing here appends.

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/ledger/chain.hpp"

namespace atx::engine::research::ledger {

// What an absent file means: an empty ledger (ledger_read: N reads 0) or a refusal
// (dsr_total.read_ledger: an absent ledger would print a DSR at N 1).
enum class MissingLedger : u8 { Empty = 0, Refuse = 1 };

struct LedgerFile {
  std::string path;
  std::vector<std::string> lines;        // non-blank line texts, file order, no line ends
  std::vector<nlohmann::json> records;   // their JSON objects (NaN tokens read as null)
  ChainHead head;                        // verified (verify_chain)
};

// The lines of a ledger text: split on LF, a trailing CR dropped, blank lines kept (so a
// line's index + 1 is its physical line number).
[[nodiscard]] std::vector<std::string> split_lines(std::string_view text);

// Parse the verified, non-blank lines (`physical` from split_lines) into a LedgerFile.
[[nodiscard]] core::Result<LedgerFile> ledger_from_lines(std::string path,
                                                         const std::vector<std::string> &physical);

// Read, verify and parse a ledger file. NotFound for an absent file under Refuse; IoError
// when it cannot be read; the verify_chain errors otherwise.
[[nodiscard]] core::Result<LedgerFile> read_ledger(const std::filesystem::path &path,
                                                   MissingLedger missing);

} // namespace atx::engine::research::ledger
