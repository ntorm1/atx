#pragma once

// Strict point-in-time join from an OPRA quote panel to listed-option
// observations. Databento instrument ids are scoped by trade date; contract
// definitions supply exact expiration timestamps and deliverable semantics.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/listed_dispersion.hpp"
#include "atx/vol/listed_quote_key.hpp"
#include "atx/vol/opra_panel.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

struct ListedContractDefinition {
  std::string trade_date{};
  std::uint32_t instrument_id{0};
  std::string raw_symbol{};
  std::int64_t definition_ts_ns{0};
  std::int64_t expiry_ts_ns{0};
  double multiplier{0.0};
  bool standard_monthly{false};
  bool standard_deliverable{false};
  std::uint64_t source_fingerprint{0};

  [[nodiscard]] bool operator==(const ListedContractDefinition &) const = default;
};

class ListedDefinitionTable {
public:
  ListedDefinitionTable() = default;

  // Definitions are canonicalized by (trade_date, instrument_id, raw_symbol).
  // Duplicate keys, contradictory economics, future definitions, and invalid
  // source fingerprints are rejected.
  [[nodiscard]] static Result<ListedDefinitionTable>
  create(std::vector<ListedContractDefinition> definitions);

  [[nodiscard]] const ListedContractDefinition *find(std::string_view trade_date,
                                                     std::uint32_t instrument_id,
                                                     std::string_view raw_symbol) const noexcept;
  [[nodiscard]] std::span<const ListedContractDefinition> definitions() const noexcept {
    return definitions_;
  }
  // Content hash of the table's canonical serialization, computed LAZILY on the
  // first call and memoized thereafter. `create` deliberately does NOT compute
  // it: on the production definitions table (~700 MB of TSV, ~8.7M rows) the
  // eager form built a throwaway ~700 MB serialization that was live at the same
  // time as both the file bytes and the row vector, for a value the backtest
  // read path never reads. Its two consumers — a write-path stdout diagnostic
  // and a round-trip equality in the tests — both pay for it only if they ask.
  //
  // NOT `noexcept`: the first call serializes and can throw `bad_alloc`. That
  // throw used to escape `create` (also not noexcept), so it is merely
  // relocated; a `noexcept` here would turn it into `std::terminate`.
  //
  // NOT thread-safe on the first call, and deliberately a plain `mutable` memo
  // rather than a `std::once_flag`: `std::once_flag` is neither copyable nor
  // movable, so a member of that type would delete this class's copy AND move
  // constructors — and every read path moves the table out of a `Result`. No
  // caller demands the fingerprint concurrently; `find()`, the only method the
  // per-date path touches, does not read it.
  [[nodiscard]] std::uint64_t fingerprint() const;

private:
  std::vector<ListedContractDefinition> definitions_{};
  // 0 means "not yet computed". Unambiguous as a sentinel because the hash is
  // folded away from 0 (`fingerprint_text` maps 0 -> 1), so no real fingerprint
  // is ever 0.
  mutable std::uint64_t fingerprint_{0};
};

// Versioned deterministic definition exchange format. It is intentionally a
// primitive TSV so a definition export can be produced independently of the
// fitting process and audited before any schedule is built.
[[nodiscard]] std::string serialize_listed_definitions(const ListedDefinitionTable &table);
[[nodiscard]] Result<ListedDefinitionTable> parse_listed_definitions(std::string_view tsv);
[[nodiscard]] Status write_listed_definitions_file(std::string_view path,
                                                   const ListedDefinitionTable &table);
[[nodiscard]] Result<ListedDefinitionTable> read_listed_definitions_file(std::string_view path);

// Calendar-free classification of US listed-equity standard-monthly expiries.
//
// Standard monthlies settle the third Friday of the month, but shift to the
// immediately preceding Thursday when that Friday is an exchange holiday (e.g.
// Juneteenth on the third Friday of June 2026). Rather than carry a holiday
// calendar, these functions use the market's own listing evidence for ONE trade
// date: no contract of any root expires on an exchange holiday, so a month's
// standard-monthly session is
//   - the third Friday, if any observed contract expires on it; else
//   - the Thursday immediately before it, if any observed contract expires then;
//   - else the month has no standard-monthly session in this universe.
//
// standard_monthly_sessions derives the session dates from the full expiry set
// observed on one trade date; is_standard_monthly_expiry tests membership. Both
// operate on UTC dates (expiry instants are canonicalized to their UTC date, so
// the exact time-of-day of an expiry stamp is irrelevant). Session dates are
// returned/consumed as day serials (days since 1970-01-01 UTC), sorted
// ascending. This keeps normal months exact — a weekly Thursday is NOT flagged
// when the third Friday exists in the set — while flagging holiday-shifted
// monthlies without an external calendar.
[[nodiscard]] std::vector<std::int64_t>
standard_monthly_sessions(std::span<const std::int64_t> expiry_ts_ns);
[[nodiscard]] bool is_standard_monthly_expiry(std::span<const std::int64_t> sessions,
                                              std::int64_t expiry_ts_ns);

// Caller policy for an OPRA quote whose contract has NO row in the point-in-time
// definition authority (i.e. ListedDefinitionTable::find returns nullptr).
//
//   Error (default): a missing definition is a fatal NotFound. This is the
//     strict, fail-closed default — every existing caller keeps the exact prior
//     behavior without opting in.
//   SkipUnlisted: drop the quote and continue past it. This is consumer-scoped
//     for workflows whose universe is standard-monthly, 21-60 DTE contracts,
//     which are defined well in advance. An OPRA row with no definition on the
//     valuation date is a contract listed intraday — the point-in-time authority
//     correctly does not yet know it — and is un-tradeable by such a workflow on
//     its listing day. It is panel noise outside the consumer's universe, not a
//     data defect, so it must not be fatal for these consumers.
//
// SkipUnlisted narrows ONLY the definition==nullptr fall-through. It does NOT
// weaken any authority guarantee:
//   - the OSI parse of raw_symbol at the head of the nullptr branch stays FATAL
//     under BOTH policies — a symbol that is not a well-formed OSI symbol is a
//     malformed panel, never an unlisted contract, so no policy softens it;
//   - the structural numeric-root skip and the same-session (0DTE) skip in the
//     nullptr branch already fire unconditionally under BOTH policies;
//   - once a definition IS found, the look-ahead/expiry guard, the OSI parse
//     of raw_symbol re-checked on this path, the quote/OSI/definition
//     economics-agreement check, and the future-quote guard all remain fatal
//     under BOTH policies. The first three signal a definition that exists but
//     contradicts the quote (corrupted authority); the future-quote guard
//     signals a quote stamped after valuation. None of the four signals an
//     absent definition, and no policy softens any of them.
enum class MissingDefinitionPolicy : std::uint8_t { Error = 0, SkipUnlisted = 1 };

// Join one single-symbol OPRA panel to the point-in-time definition table.
// Exact OSI economics must agree with the definition, every aligned instrument
// id must resolve on the same trade date, all source liquidity counts must be
// nonnegative, and neither quote nor definition may be later than
// valuation_ts_ns. No default multiplier or synthetic expiry is introduced on
// this path. `policy` governs only the missing-definition fall-through (see
// MissingDefinitionPolicy); it defaults to the strict Error behavior so
// existing callers are unaffected.
//
// ── `wanted`: the leg-key filter, and the validation it NARROWS ──────────────
//
// EMPTY `wanted` (the default) is today's behavior, bit for bit: every panel row
// is joined and every check below fires for every row.
//
// A NON-EMPTY `wanted` is a consumer declaring the exact contract set it will
// read. It MUST be SORTED and DEDUPED in `ListedQuoteKey` order, and that is
// ENFORCED, not merely documented: a `wanted` that is not strictly increasing is
// rejected with InvalidArgument before any row is joined. The filter binary-
// searches `wanted`, so an unsorted span would locate a wrong (usually empty)
// run and silently drop legs — no gate would fire, and the miss would surface
// only as degraded marks downstream. The result is then exactly the unfiltered
// result INTERSECTED with `wanted`, element for element and in the same panel
// order — a row whose `quote_key_of` is not in `wanted` is never emitted.
//
// The filter is applied in two stages. A cheap `raw_symbol` membership test runs
// AFTER the liquidity and aligned-source-identity fatal gates, and BEFORE
// `definitions.find`, the OSI parse and quote construction — that is where the
// cost is, and skipping it is the entire point. The exact key (which needs the
// definition's `expiry_ts_ns`, and so cannot be known before the lookup) is
// re-checked immediately before the quote is emitted.
//
// THIS NARROWS VALIDATION, DELIBERATELY. The loop body has EIGHT fatal exits for
// a panel row under an empty `wanted`, listed here in the order they are reached.
// Exactly TWO stay panel-wide; the other six are narrowed to rows whose
// raw_symbol appears in `wanted`:
//
//   PANEL-WIDE — precedes the raw_symbol stage, so it is still fatal for a row
//   no consumer wants:
//     1. negative source liquidity count              InvalidArgument
//     2. aligned source identity missing              NotFound
//
//   NARROWED to the wanted raw_symbol set — all six sit after that stage:
//     3. OSI parse of raw_symbol, definition absent   parse_osi_symbol's error
//     4. contract definition missing                  NotFound
//     5. definition look-ahead / expiry               InvalidArgument
//     6. OSI parse of raw_symbol, definition found    parse_osi_symbol's error
//     7. quote/OSI/definition economics disagree      InvalidArgument
//     8. future quote                                 InvalidArgument
//
// On the narrowed six: (3) and (6) are the SAME condition — raw_symbol is not a
// well-formed OSI symbol — reached on the two mutually exclusive branches of the
// definition lookup, and (3) is the one exit on the missing-definition path that
// `MissingDefinitionPolicy::SkipUnlisted` does not disarm. (4) is inert for a
// caller passing SkipUnlisted (as the listed-dispersion workflow does) and live
// under the default Error policy. (5)-(8) are the definition-exists gates.
//
// Checks 3-8 therefore stop being a panel-wide audit of the definition table and
// become an audit of the contracts the caller consumes. That is the accepted
// trade: validating definitions for the ~100k contracts a reconciliation never
// reads is not the reconciliation's job, and the exporter that produced the
// definitions owns that audit. What must NOT happen is a fail-closed gate going
// quiet for a contract the caller DOES read, so the raw_symbol stage is
// deliberately coarser than the full key: a row whose economics DISAGREE (check
// 7) still carries the wanted raw_symbol and still trips the gate, rather than
// being filtered out on the strike/side it is lying about. Each of the six has a
// test asserting it still fires for a wanted key, and five of them assert on the
// SAME input that it is tolerated when the contract is NOT wanted.
[[nodiscard]] Result<std::vector<ListedOptionQuote>>
listed_quotes_from_opra(std::string_view trade_date, std::int64_t valuation_ts_ns,
                        const OpraPanel &panel, const ListedDefinitionTable &definitions,
                        MissingDefinitionPolicy policy = MissingDefinitionPolicy::Error,
                        std::span<const ListedQuoteKey> wanted = {});

} // namespace atx::vol
