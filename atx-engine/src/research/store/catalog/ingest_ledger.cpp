// Ingest of the trial ledger (`atx.trial-ledger/v1`, JSON lines; writer
// atx-impl/tools/backtest_integrity.py ledger_append: json.dumps(rec, sort_keys=True,
// separators=(",", ":")) + "\n", LF) as an index (ruling SQL-9: the hash-chained JSONL stays
// the authority through P9): one append-only trial_line row per non-blank line (its exact bytes
// without the line end and their SHA-256, as backtest_integrity.line_sha256 hashes them) and a
// ledger_state row. The chain rule is never re-implemented here (G-P5): the head comes only
// from the LedgerHeadFn seam below, and `verify --ledger` (verify_ledger_heads, P9 lane SQL4)
// compares recorded heads with B2's prefix heads only.

#include <algorithm>
#include <array>
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
#include "atx/engine/research/ledger/chain.hpp"
#include "atx/engine/research/store/catalog/ingest.hpp"
#include "atx/engine/research/store/catalog/ops_records.hpp"
#include "research/store/catalog/catalog_detail.hpp"

namespace atx::engine::research::store::catalog {
namespace lg = atx::engine::research::ledger;

// THE seam to lane B2 (ingest.hpp), bound by P9 lane SQL4: B2's research/ledger chain_head over
// the ledger's line texts (the one copy of the rule). Nothing else in the catalog computes a
// chain head; verify_ledger_heads below reads B2's prefix_heads.
LedgerHeadFn default_ledger_head() {
  return [](std::span<const std::string> lines) -> core::Result<LedgerHead> {
    ATX_TRY(lg::ChainHead head, lg::chain_head(lines));
    return core::Ok(LedgerHead{std::move(head.head_sha256), std::move(head.rule)});
  };
}

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

// ---- verify --ledger (P9 lane SQL4; rulings SQL-7 / SQL-9; ingest.hpp) ----------------------
namespace {

using core::db::Statement;

// A column that may be NULL: nullopt for NULL, else its text.
[[nodiscard]] core::Result<std::optional<std::string>> nullable_text(const Statement &stmt,
                                                                     i32 col) {
  if (stmt.column_is_null(col)) {
    return core::Ok(std::optional<std::string>{});
  }
  ATX_TRY(std::string text, stmt.checked_text(col));
  return core::Ok(std::optional<std::string>{std::move(text)});
}

// The catalogued line texts of `ledger`, in file order.
[[nodiscard]] core::Result<std::vector<std::string>> catalogued_lines(core::db::Database &db,
                                                                      const std::string &ledger) {
  ATX_TRY(Statement stmt,
          db.prepare("SELECT line FROM trial_line WHERE ledger_path = ?1 ORDER BY seq;"));
  ATX_TRY_VOID(stmt.bind(1, std::string_view{ledger}));
  std::vector<std::string> lines;
  // Bounded by the ledger's stored row count.
  for (;;) {
    ATX_TRY(const Statement::Step step, stmt.step());
    if (step == Statement::Step::Done) {
      return core::Ok(std::move(lines));
    }
    ATX_TRY(std::string line, stmt.checked_text(0));
    lines.push_back(std::move(line));
  }
}

// Why `ledger`'s ledger_state row disagrees with its `lines` rows of head `head`; nullopt: it
// agrees.
[[nodiscard]] core::Result<std::optional<std::string>>
state_problem(core::db::Database &db, const std::string &ledger, i64 lines,
              const std::string &head) {
  ATX_TRY(Statement stmt, db.prepare("SELECT lines, head_sha256, head_rule FROM ledger_state "
                                     "WHERE ledger_path = ?1;"));
  ATX_TRY_VOID(stmt.bind(1, std::string_view{ledger}));
  ATX_TRY(const Statement::Step step, stmt.step());
  if (step == Statement::Step::Done) {
    return core::Ok(std::optional<std::string>{lines == 0 ? "not catalogued"
                                                          : "no ledger_state row"});
  }
  ATX_TRY(const i64 stored_lines, stmt.checked_int(0));
  ATX_TRY(const std::optional<std::string> stored_head, nullable_text(stmt, 1));
  ATX_TRY(const std::optional<std::string> stored_rule, nullable_text(stmt, 2));
  if (stored_lines != lines) {
    return core::Ok(std::optional<std::string>{"ledger_state.lines " +
                                               std::to_string(stored_lines) + " != " +
                                               std::to_string(lines) + " trial_line rows"});
  }
  if (stored_head && (*stored_head != head || stored_rule != std::string{lg::kChainRule})) {
    return core::Ok(std::optional<std::string>{"ledger_state head " + *stored_head + " (" +
                                               stored_rule.value_or("no rule") +
                                               ") is not the chain head of its lines"});
  }
  return core::Ok(std::optional<std::string>{});
}

// Every non-null recorded head: verdicts, then wave results, each in path order. A wave result
// names its ledger in doc.ledger.path (no typed column), a verdict in ledger_path.
[[nodiscard]] core::Result<std::vector<RecordedLedgerHead>>
recorded_heads(core::db::Database &db) {
  constexpr std::array<std::pair<std::string_view, std::string_view>, 2> kQueries{{
      {"cycle_verdict", "SELECT path, ledger_path, ledger_head FROM cycle_verdict "
                        "WHERE ledger_head IS NOT NULL ORDER BY path;"},
      {"wave_result", "SELECT path, CASE json_type(doc, '$.ledger.path') WHEN 'text' THEN "
                      "json_extract(doc, '$.ledger.path') END, ledger_head FROM wave_result "
                      "WHERE ledger_head IS NOT NULL ORDER BY path;"},
  }};
  std::vector<RecordedLedgerHead> out;
  for (const auto &[table, sql] : kQueries) {
    ATX_TRY(Statement stmt, db.prepare(sql));
    // Bounded by the table's row count.
    for (;;) {
      ATX_TRY(const Statement::Step step, stmt.step());
      if (step == Statement::Step::Done) {
        break;
      }
      RecordedLedgerHead head;
      head.table = std::string{table};
      ATX_TRY(head.path, stmt.checked_text(0));
      ATX_TRY(head.ledger_path, nullable_text(stmt, 1));
      ATX_TRY(head.head_sha256, stmt.checked_text(2));
      out.push_back(std::move(head));
    }
  }
  return core::Ok(std::move(out));
}

} // namespace

bool LedgerVerifyReport::ok() const noexcept {
  return std::all_of(ledgers.begin(), ledgers.end(),
                     [](const LedgerIndexCheck &c) { return !c.problem.has_value(); }) &&
         std::all_of(recorded.begin(), recorded.end(),
                     [](const RecordedLedgerHead &r) { return r.prefix_lines.has_value(); });
}

core::Result<LedgerVerifyReport> verify_ledger_heads(core::db::Database &db,
                                                     std::span<const std::string> ledger_paths) {
  LedgerVerifyReport report;
  // prefixes[i][k]: the head of the first k lines of ledger i (k = 0: the genesis head).
  std::vector<std::vector<std::string>> prefixes;
  for (const std::string &given : ledger_paths) {
    LedgerIndexCheck check;
    check.ledger_path = given;
    std::replace(check.ledger_path.begin(), check.ledger_path.end(), '\\', '/');
    check.rule = std::string{lg::kChainRule};
    ATX_TRY(const std::vector<std::string> lines, catalogued_lines(db, check.ledger_path));
    ATX_TRY(std::vector<std::string> heads, lg::prefix_heads(lines));
    heads.insert(heads.begin(), std::string{lg::kChainGenesis});
    check.lines = static_cast<i64>(lines.size());
    check.head_sha256 = heads.back();
    ATX_TRY(check.problem, state_problem(db, check.ledger_path, check.lines, check.head_sha256));
    report.ledgers.push_back(std::move(check));
    prefixes.push_back(std::move(heads));
  }
  ATX_TRY(std::vector<RecordedLedgerHead> recorded, recorded_heads(db));
  for (RecordedLedgerHead &r : recorded) {
    bool checked = false;
    for (usize i = 0; i < report.ledgers.size() && !r.prefix_lines; ++i) {
      if (r.ledger_path && *r.ledger_path != report.ledgers[i].ledger_path) {
        continue;
      }
      checked = true;
      const auto at = std::find(prefixes[i].begin(), prefixes[i].end(), r.head_sha256);
      if (at != prefixes[i].end()) {
        r.prefix_lines = static_cast<i64>(at - prefixes[i].begin());
      }
    }
    if (!checked) {
      ++report.other;
      continue;
    }
    report.recorded.push_back(std::move(r));
  }
  return core::Ok(std::move(report));
}

} // namespace atx::engine::research::store::catalog
