#include "atx/vol/listed_opra.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"
#include "atx/vol/data.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

constexpr std::string_view kMagic = "ATX_LISTED_DEFINITIONS\t1";
constexpr std::string_view kHeader =
    "trade_date\tinstrument_id\traw_symbol\tdefinition_ts_ns\texpiry_ts_ns\t"
    "multiplier\tstandard_monthly\tstandard_deliverable\tsource_fingerprint";

[[nodiscard]] auto definition_key(const ListedContractDefinition &definition) {
  return std::tie(definition.trade_date, definition.instrument_id, definition.raw_symbol);
}

void append_u64(std::string &out, std::uint64_t value) {
  char buffer[32];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, value);
  (void)error;
  out.append(buffer, end);
}

void append_i64(std::string &out, std::int64_t value) {
  char buffer[32];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, value);
  (void)error;
  out.append(buffer, end);
}

void append_double(std::string &out, double value) {
  char buffer[64];
  const auto [end, error] =
      std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::general,
                    std::numeric_limits<double>::max_digits10);
  (void)error;
  out.append(buffer, end);
}

[[nodiscard]] std::uint64_t fingerprint_text(std::string_view text) {
  const std::uint64_t hash = atx::core::hash_bytes(text.data(), text.size());
  return hash == 0u ? 1u : hash;
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char delimiter) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find(delimiter, start);
    fields.push_back(
        text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return fields;
}

template <typename T> [[nodiscard]] bool parse_integer(std::string_view text, T &value) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] bool parse_double(std::string_view text, double &value) {
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
  return error == std::errc{} && end == text.data() + text.size() && std::isfinite(value);
}

[[nodiscard]] std::uint64_t joined_source_fingerprint(std::uint64_t quote,
                                                      std::uint64_t definition) {
  std::string material;
  material.reserve(48);
  append_u64(material, quote);
  material.push_back('|');
  append_u64(material, definition);
  return fingerprint_text(material);
}

[[nodiscard]] Result<void> write_atomic(std::string_view path, std::string_view contents) {
  const std::filesystem::path target{path};
  if (target.empty()) {
    return Err(ErrorCode::InvalidArgument, "listed definitions: empty output path");
  }
  std::error_code error;
  if (!target.parent_path().empty()) {
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) {
      return Err(ErrorCode::IoError, "listed definitions: cannot create output directory");
    }
  }
  std::filesystem::path pending = target;
  pending += ".pending";
  {
    std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
    if (!stream || !stream.write(contents.data(), static_cast<std::streamsize>(contents.size()))) {
      return Err(ErrorCode::IoError, "listed definitions: cannot write pending file");
    }
  }
  std::filesystem::remove(target, error);
  error.clear();
  std::filesystem::rename(pending, target, error);
  if (error) {
    return Err(ErrorCode::IoError, "listed definitions: cannot publish file");
  }
  return Ok();
}

} // namespace

Result<ListedDefinitionTable>
ListedDefinitionTable::create(std::vector<ListedContractDefinition> definitions) {
  std::sort(definitions.begin(), definitions.end(),
            [](const auto &a, const auto &b) { return definition_key(a) < definition_key(b); });

  for (std::size_t i = 0; i < definitions.size(); ++i) {
    const ListedContractDefinition &definition = definitions[i];
    if (definition.trade_date.empty() || definition.instrument_id == 0 ||
        definition.raw_symbol.empty() || definition.definition_ts_ns <= 0 ||
        definition.expiry_ts_ns <= definition.definition_ts_ns ||
        !std::isfinite(definition.multiplier) || !(definition.multiplier > 0.0) ||
        definition.source_fingerprint == 0u) {
      return Err(ErrorCode::InvalidArgument, "listed definitions: malformed definition");
    }
    const std::int64_t trade_end = iso_to_ns(definition.trade_date + "T23:59:59.999999999Z");
    if (trade_end <= 0 || definition.definition_ts_ns > trade_end) {
      return Err(ErrorCode::InvalidArgument,
                 "listed definitions: future or invalid date-scoped definition");
    }
    if (i > 0 && definition_key(definitions[i - 1]) == definition_key(definition)) {
      return Err(ErrorCode::AlreadyExists, "listed definitions: duplicate definition key");
    }
  }

  ListedDefinitionTable table;
  table.definitions_ = std::move(definitions);
  const std::string serialized = serialize_listed_definitions(table);
  table.fingerprint_ = fingerprint_text(serialized);
  return Ok(std::move(table));
}

const ListedContractDefinition *
ListedDefinitionTable::find(std::string_view trade_date, std::uint32_t instrument_id,
                            std::string_view raw_symbol) const noexcept {
  const auto key = std::tuple{trade_date, instrument_id, raw_symbol};
  const auto found = std::lower_bound(
      definitions_.begin(), definitions_.end(), key,
      [](const auto &definition, const auto &rhs) { return definition_key(definition) < rhs; });
  return found != definitions_.end() && definition_key(*found) == key ? &*found : nullptr;
}

std::string serialize_listed_definitions(const ListedDefinitionTable &table) {
  std::string out;
  out.append(kMagic).push_back('\n');
  out.append(kHeader).push_back('\n');
  for (const ListedContractDefinition &definition : table.definitions()) {
    out.append(definition.trade_date).push_back('\t');
    append_u64(out, definition.instrument_id);
    out.push_back('\t');
    out.append(definition.raw_symbol).push_back('\t');
    append_i64(out, definition.definition_ts_ns);
    out.push_back('\t');
    append_i64(out, definition.expiry_ts_ns);
    out.push_back('\t');
    append_double(out, definition.multiplier);
    out.push_back('\t');
    out.push_back(definition.standard_monthly ? '1' : '0');
    out.push_back('\t');
    out.push_back(definition.standard_deliverable ? '1' : '0');
    out.push_back('\t');
    append_u64(out, definition.source_fingerprint);
    out.push_back('\n');
  }
  return out;
}

Result<ListedDefinitionTable> parse_listed_definitions(std::string_view tsv) {
  const std::vector<std::string_view> lines = split(tsv, '\n');
  if (lines.size() < 2 || lines[0] != kMagic || lines[1] != kHeader) {
    return Err(ErrorCode::ParseError, "listed definitions: bad header");
  }
  std::vector<ListedContractDefinition> definitions;
  definitions.reserve(lines.size() - 2);
  for (std::size_t i = 2; i < lines.size(); ++i) {
    if (lines[i].empty()) {
      continue;
    }
    const std::vector<std::string_view> fields = split(lines[i], '\t');
    ListedContractDefinition definition;
    std::uint64_t instrument_id = 0;
    unsigned monthly = 0;
    unsigned deliverable = 0;
    if (fields.size() != 9 || !parse_integer(fields[1], instrument_id) ||
        instrument_id > std::numeric_limits<std::uint32_t>::max() ||
        !parse_integer(fields[3], definition.definition_ts_ns) ||
        !parse_integer(fields[4], definition.expiry_ts_ns) ||
        !parse_double(fields[5], definition.multiplier) || !parse_integer(fields[6], monthly) ||
        monthly > 1 || !parse_integer(fields[7], deliverable) || deliverable > 1 ||
        !parse_integer(fields[8], definition.source_fingerprint)) {
      return Err(ErrorCode::ParseError, "listed definitions: malformed row");
    }
    definition.trade_date = fields[0];
    definition.instrument_id = static_cast<std::uint32_t>(instrument_id);
    definition.raw_symbol = fields[2];
    definition.standard_monthly = monthly != 0;
    definition.standard_deliverable = deliverable != 0;
    definitions.push_back(std::move(definition));
  }
  auto table = ListedDefinitionTable::create(std::move(definitions));
  if (!table) {
    return Err(ErrorCode::ParseError, table.error().to_string());
  }
  return table;
}

Status write_listed_definitions_file(std::string_view path, const ListedDefinitionTable &table) {
  return write_atomic(path, serialize_listed_definitions(table));
}

Result<ListedDefinitionTable> read_listed_definitions_file(std::string_view path) {
  std::ifstream stream(std::filesystem::path{path}, std::ios::binary);
  if (!stream) {
    return Err(ErrorCode::NotFound, "listed definitions: file not found");
  }
  std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  if (!stream.good() && !stream.eof()) {
    return Err(ErrorCode::IoError, "listed definitions: read failed");
  }
  return parse_listed_definitions(contents);
}

Result<std::vector<ListedOptionQuote>>
listed_quotes_from_opra(std::string_view trade_date, std::int64_t valuation_ts_ns,
                        const OpraPanel &panel, const ListedDefinitionTable &definitions) {
  if (trade_date.empty() || valuation_ts_ns <= 0 || panel.frame.uid.empty() ||
      panel.frame.snapshot_ts_ns != valuation_ts_ns || panel.source_schema_version < 2 ||
      !panel.provenance_complete || panel.source_fingerprint == 0u ||
      panel.source_instrument_ids.size() != panel.frame.rows.size()) {
    return Err(ErrorCode::InvalidArgument,
               "listed OPRA join: panel lacks strict aligned source provenance");
  }
  if (ns_to_iso_date(valuation_ts_ns) != trade_date) {
    return Err(ErrorCode::InvalidArgument, "listed OPRA join: valuation date mismatch");
  }

  std::vector<ListedOptionQuote> quotes;
  quotes.reserve(panel.frame.rows.size());
  for (std::size_t i = 0; i < panel.frame.rows.size(); ++i) {
    const QuoteRow &row = panel.frame.rows[i];
    const std::uint32_t instrument_id = panel.source_instrument_ids[i];
    const auto identity = std::lower_bound(
        panel.source_identities.begin(), panel.source_identities.end(), instrument_id,
        [](const OpraInstrumentIdentity &candidate, std::uint32_t id) {
          return candidate.instrument_id < id;
        });
    if (instrument_id == 0 || identity == panel.source_identities.end() ||
        identity->instrument_id != instrument_id) {
      return Err(ErrorCode::NotFound, "listed OPRA join: aligned source identity missing");
    }
    const ListedContractDefinition *definition =
        definitions.find(trade_date, instrument_id, identity->raw_symbol);
    if (definition == nullptr) {
      // OCC numeric root suffixes identify adjusted/non-standard contracts.
      // The strict exporter intentionally omits them because OPRA does not
      // populate deliverable fields; they are ineligible for this workflow.
      ATX_TRY(const OsiSymbol missing_osi, parse_osi_symbol(identity->raw_symbol));
      if (std::any_of(missing_osi.root.begin(), missing_osi.root.end(),
                      [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        continue;
      }
      // Same-session (0DTE) contracts — OSI expiry date equal to the trade date
      // — are structurally outside this consumer's universe and are skipped, not
      // treated as a missing authority. The OPRA panel keeps them because their
      // PM-settled (16:00 ET) expiry instant is after the intraday snapshot, but
      // the point-in-time definition authority predates same-day rows and stamps
      // expiration at midnight-UTC of the expiry date, so a 0DTE contract can
      // never satisfy this join even when a definition row exists (it would trip
      // the look-ahead/expiry guard below). The listed-dispersion workflow only
      // ever selects 21-60 DTE, so these are pure noise here. Invariant: on this
      // path every kept quote has expiry strictly after the trade date.
      if (missing_osi.expiry_iso == trade_date) {
        continue;
      }
      return Err(ErrorCode::NotFound, "listed OPRA join: contract definition missing");
    }
    if (definition->definition_ts_ns > valuation_ts_ns ||
        definition->expiry_ts_ns <= valuation_ts_ns) {
      return Err(ErrorCode::InvalidArgument, "listed OPRA join: definition look-ahead or expiry");
    }
    ATX_TRY(const OsiSymbol osi, parse_osi_symbol(identity->raw_symbol));
    const std::string symbol = row.uid.empty() ? panel.frame.uid : row.uid;
    if (osi.root != symbol || osi.expiry_iso != row.expiry_iso || osi.side != row.side ||
        osi.strike != row.strike || ns_to_iso_date(definition->expiry_ts_ns) != osi.expiry_iso) {
      return Err(ErrorCode::InvalidArgument,
                 "listed OPRA join: quote, OSI, and definition economics disagree");
    }

    ListedOptionQuote quote;
    quote.trade_date = trade_date;
    quote.symbol = symbol;
    quote.instrument_id = instrument_id;
    quote.raw_symbol = identity->raw_symbol;
    quote.expiry_ts_ns = definition->expiry_ts_ns;
    quote.strike = row.strike;
    quote.side = row.side;
    quote.bid = row.bid;
    quote.ask = row.ask;
    quote.quote_ts_ns = row.ts_ns == 0 ? valuation_ts_ns : row.ts_ns;
    quote.multiplier = definition->multiplier;
    quote.standard_monthly = definition->standard_monthly;
    quote.standard_deliverable = definition->standard_deliverable;
    quote.source_fingerprint =
        joined_source_fingerprint(panel.source_fingerprint, definition->source_fingerprint);
    if (quote.quote_ts_ns > valuation_ts_ns) {
      return Err(ErrorCode::InvalidArgument, "listed OPRA join: future quote");
    }
    quotes.push_back(std::move(quote));
  }
  return Ok(std::move(quotes));
}

} // namespace atx::vol
