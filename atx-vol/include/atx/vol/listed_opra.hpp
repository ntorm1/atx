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
  [[nodiscard]] std::uint64_t fingerprint() const noexcept { return fingerprint_; }

private:
  std::vector<ListedContractDefinition> definitions_{};
  std::uint64_t fingerprint_{0};
};

// Versioned deterministic definition exchange format. It is intentionally a
// primitive TSV so a definition export can be produced independently of the
// fitting process and audited before any schedule is built.
[[nodiscard]] std::string serialize_listed_definitions(const ListedDefinitionTable &table);
[[nodiscard]] Result<ListedDefinitionTable> parse_listed_definitions(std::string_view tsv);
[[nodiscard]] Status write_listed_definitions_file(std::string_view path,
                                                   const ListedDefinitionTable &table);
[[nodiscard]] Result<ListedDefinitionTable> read_listed_definitions_file(std::string_view path);

// Join one single-symbol OPRA panel to the point-in-time definition table.
// Exact OSI economics must agree with the definition, every aligned instrument
// id must resolve on the same trade date, and neither quote nor definition may
// be later than valuation_ts_ns. No default multiplier or synthetic expiry is
// introduced on this path.
[[nodiscard]] Result<std::vector<ListedOptionQuote>>
listed_quotes_from_opra(std::string_view trade_date, std::int64_t valuation_ts_ns,
                        const OpraPanel &panel, const ListedDefinitionTable &definitions);

} // namespace atx::vol
