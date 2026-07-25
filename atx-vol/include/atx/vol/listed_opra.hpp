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
//   - the structural numeric-root skip and the same-session (0DTE) skip in the
//     nullptr branch already fire unconditionally under BOTH policies;
//   - once a definition IS found, the look-ahead/expiry guard and the
//     quote/OSI/definition economics-agreement check remain fatal under BOTH
//     policies. Those signal a definition that exists but contradicts the quote
//     (corrupted authority), never an absent one, and no policy softens them.
enum class MissingDefinitionPolicy : std::uint8_t { Error = 0, SkipUnlisted = 1 };

// Join one single-symbol OPRA panel to the point-in-time definition table.
// Exact OSI economics must agree with the definition, every aligned instrument
// id must resolve on the same trade date, and neither quote nor definition may
// be later than valuation_ts_ns. No default multiplier or synthetic expiry is
// introduced on this path. `policy` governs only the missing-definition
// fall-through (see MissingDefinitionPolicy); it defaults to the strict Error
// behavior so existing callers are unaffected.
[[nodiscard]] Result<std::vector<ListedOptionQuote>>
listed_quotes_from_opra(std::string_view trade_date, std::int64_t valuation_ts_ns,
                        const OpraPanel &panel, const ListedDefinitionTable &definitions,
                        MissingDefinitionPolicy policy = MissingDefinitionPolicy::Error);

} // namespace atx::vol
