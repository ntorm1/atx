// Ingest of the trial ledger (`atx.trial-ledger/v1`, JSON lines; writer
// atx-impl/tools/backtest_integrity.py ledger_append: json.dumps(rec, sort_keys=True,
// separators=(",", ":")) + "\n", LF) as an index (ruling SQL-9: the hash-chained JSONL stays
// the authority through P9): one append-only trial_line row per non-blank line (its exact bytes
// without the line end and their SHA-256, as backtest_integrity.line_sha256 hashes them) and a
// ledger_state row. The chain rule is never re-implemented here (G-P5): the head comes only
// from the LedgerHeadFn seam below.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/catalog/ingest.hpp"
#include "atx/engine/research/store/catalog/ops_records.hpp"
#include "research/store/catalog/catalog_detail.hpp"

namespace atx::engine::research::store::catalog {

// THE seam to lane B2 (ingest.hpp): bind B2's research/ledger chain-head function here, e.g.
//   return [](std::span<const std::string> lines) -> core::Result<LedgerHead> {
//     ATX_TRY(auto head, ledger::chain_head(lines));
//     return LedgerHead{head.sha256, std::string{head.rule}};
//   };
// At this lane's base B2 is not merged, so the function is empty and ledger_state's head stays
// NULL (SQL4 fills it). Nothing else in the catalog computes or compares a chain head.
LedgerHeadFn default_ledger_head() { return {}; }

namespace detail {
namespace {

[[nodiscard]] bool blank(std::string_view line) noexcept {
  return line.find_first_not_of(" \t\r\f\v") == std::string_view::npos;
}

[[nodiscard]] std::optional<std::string> text_at(const OJson &obj, std::string_view key) {
  const OJson *v = member(obj, key);
  return v != nullptr ? fit_text(*v) : std::nullopt;
}

// trial_line is append-only: every stored line of this ledger must still be there with the same
// bytes; only lines after them are inserted.
[[nodiscard]] core::Status append_lines(core::db::Database &db, const std::string &ledger,
                                        const std::vector<TrialLineRow> &rows) {
  ATX_TRY(core::db::Statement stmt,
          db.prepare("SELECT seq, line_sha256 FROM trial_line WHERE ledger_path = ?1 "
                     "ORDER BY seq;"));
  ATX_TRY_VOID(stmt.bind(1, std::string_view{ledger}));
  std::vector<std::pair<i64, std::string>> stored;
  // Bounded by the ledger's stored row count.
  for (;;) {
    ATX_TRY(const core::db::Statement::Step step, stmt.step());
    if (step == core::db::Statement::Step::Done) {
      break;
    }
    ATX_TRY(const i64 seq, stmt.checked_int(0));
    ATX_TRY(std::string sha, stmt.checked_text(1));
    stored.emplace_back(seq, std::move(sha));
  }
  usize next = 0;
  for (const auto &[seq, sha] : stored) {
    if (next >= rows.size() || rows[next].seq != seq || rows[next].line_sha256 != sha) {
      return core::Err(core::ErrorCode::PermissionDenied,
                       "trial_line is append-only: " + ledger + " line " + std::to_string(seq) +
                           " changed or was removed since it was catalogued; the JSONL ledger "
                           "is the authority (ruling SQL-9): investigate, then catalog --rebuild");
    }
    ++next;
  }
  for (; next < rows.size(); ++next) {
    ATX_TRY_VOID(insert(db, rows[next]));
  }
  return core::Ok();
}

} // namespace

core::Status ingest_trial_ledger(const IngestContext &ctx, const FileView &file,
                                 IngestResult &out) {
  std::vector<TrialLineRow> rows;
  std::vector<std::string> lines;
  std::string_view rest = file.bytes;
  i64 number = 0;
  // Bounded by the file size: each pass consumes one line.
  while (!rest.empty()) {
    const usize end = rest.find('\n');
    std::string_view line = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
    ++number;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (blank(line)) {
      continue;
    }
    const std::optional<OJson> rec = parse_strict(line);
    if (!rec || !rec->is_object()) {
      out.unparsed = true;
      return core::Ok();
    }
    TrialLineRow row;
    row.ledger_path = file.path;
    row.seq = number;
    row.line = std::string{line};
    ATX_TRY(row.line_sha256, core::sha256_hex(line));
    row.schema = text_at(*rec, "schema");
    row.kind = text_at(*rec, "kind");
    row.cell = text_at(*rec, "cell");
    row.trial_id = text_at(*rec, "trial_id");
    if (const OJson *count = member(*rec, "count"); count != nullptr) {
      row.count = fit_int(*count);
    }
    if (const OJson *prev = member(*rec, "prev_sha256"); prev != nullptr) {
      row.prev_sha256 = fit_sha(*prev);
    }
    if (!out.json_schema && row.schema) {
      out.json_schema = row.schema;
    }
    lines.push_back(row.line);
    rows.push_back(std::move(row));
  }
  LedgerStateRow state;
  state.ledger_path = file.path;
  state.lines = static_cast<i64>(rows.size());
  state.file_sha256 = file.sha256;
  if (ctx.ledger_head) {
    ATX_TRY(LedgerHead head, ctx.ledger_head(std::span<const std::string>{lines}));
    state.head_sha256 = std::move(head.head_sha256);
    state.head_rule = std::move(head.rule);
  }
  out.writes.emplace_back([state = std::move(state), rows = std::move(rows)](
                              core::db::Database &db) -> core::Status {
    ATX_TRY_VOID(append_lines(db, state.ledger_path, rows));
    return upsert(db, state);
  });
  return core::Ok();
}

} // namespace detail
} // namespace atx::engine::research::store::catalog
