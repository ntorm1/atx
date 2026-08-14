// Export point-in-time OPRA instrument definitions for the strict listed
// dispersion workflow. MetadataGetCost is always called before egress; --dry-run
// performs no paid request.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <databento/constants.hpp>
#include <databento/datetime.hpp>
#include <databento/enums.hpp>
#include <databento/historical.hpp>
#include <databento/record.hpp>

#include "atx/core/datetime.hpp"
#include "atx/core/hash.hpp"
#include "marketdata/listed_opra.hpp"
#include "marketdata/occ_ess.hpp"
#include "atx/vol/api/marketdata/opra_panel.hpp"

namespace {

namespace time = atx::core::time;

struct Civil {
  int year{0};
  unsigned month{0};
  unsigned day{0};
};

struct Config {
  std::vector<std::string> symbols{};
  std::string start{};
  std::string end{};
  std::string out{"spy_dispersion_definitions.tsv"};
  std::string opra_root{};
  std::string occ_ess_root{};
  double cap_usd{5.0};
  bool dry_run{false};
};

[[nodiscard]] std::int64_t days_from_civil(int year, unsigned month, unsigned day) noexcept {
  year -= month <= 2u;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153u * (month > 2u ? month - 3u : month + 9u) + 2u) / 5u + day - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

[[nodiscard]] Civil civil_from_days(std::int64_t serial) noexcept {
  serial += 719468;
  const std::int64_t era = (serial >= 0 ? serial : serial - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(serial - era * 146097);
  const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  const int year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const unsigned mp = (5u * doy + 2u) / 153u;
  const unsigned day = doy - (153u * mp + 2u) / 5u + 1u;
  const unsigned month = mp < 10u ? mp + 3u : mp - 9u;
  return Civil{year + static_cast<int>(month <= 2u), month, day};
}

[[nodiscard]] bool parse_date(std::string_view text, Civil &out) {
  if (text.size() != 10u || text[4] != '-' || text[7] != '-') {
    return false;
  }
  const auto number = [](std::string_view field, int &value) {
    const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value);
    return error == std::errc{} && end == field.data() + field.size();
  };
  int year = 0;
  int month = 0;
  int day = 0;
  if (!number(text.substr(0, 4), year) || !number(text.substr(5, 2), month) ||
      !number(text.substr(8, 2), day) || month < 1 || month > 12 || day < 1 || day > 31) {
    return false;
  }
  const std::int64_t serial =
      days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  const Civil round_trip = civil_from_days(serial);
  if (round_trip.year != year || round_trip.month != static_cast<unsigned>(month) ||
      round_trip.day != static_cast<unsigned>(day)) {
    return false;
  }
  out = round_trip;
  return true;
}

[[nodiscard]] std::string format_date(const Civil &date) {
  char buffer[11];
  std::snprintf(buffer, sizeof buffer, "%04d-%02u-%02u", date.year, date.month, date.day);
  return buffer;
}

void split_csv(std::string_view text, std::vector<std::string> &output) {
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
    if (end > start) {
      output.emplace_back(text.substr(start, end - start));
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1u;
  }
}

[[nodiscard]] std::string parent_symbol(std::string_view symbol) {
  std::string parent;
  parent.reserve(symbol.size() + 4u);
  for (const char ch : symbol) {
    if (ch != '.') {
      parent.push_back(ch);
    }
  }
  parent.append(".OPT");
  return parent;
}

void usage() {
  std::fprintf(stderr, "usage: databento_spy_dispersion_definitions --symbols SPY,AAPL,... "
                       "--start YYYY-MM-DD --end YYYY-MM-DD [--opra-root DIR] "
                       "[--occ-ess-root DIR] [--out FILE] "
                       "[--cap USD] [--dry-run]\n");
}

[[nodiscard]] bool parse_args(int argc, char **argv, Config &config) {
  const auto value = [&](int &index) -> const char * {
    return index + 1 < argc ? argv[++index] : nullptr;
  };
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--dry-run") {
      config.dry_run = true;
    } else if (argument == "--symbols") {
      const char *next = value(i);
      if (next == nullptr)
        return false;
      split_csv(next, config.symbols);
    } else if (argument == "--start") {
      const char *next = value(i);
      if (next == nullptr)
        return false;
      config.start = next;
    } else if (argument == "--end") {
      const char *next = value(i);
      if (next == nullptr)
        return false;
      config.end = next;
    } else if (argument == "--out") {
      const char *next = value(i);
      if (next == nullptr)
        return false;
      config.out = next;
    } else if (argument == "--opra-root") {
      const char *next = value(i);
      if (next == nullptr)
        return false;
      config.opra_root = next;
    } else if (argument == "--occ-ess-root") {
      const char *next = value(i);
      if (next == nullptr)
        return false;
      config.occ_ess_root = next;
    } else if (argument == "--cap") {
      const char *next = value(i);
      if (next == nullptr)
        return false;
      char *end = nullptr;
      config.cap_usd = std::strtod(next, &end);
      if (end == next || *end != '\0' || !(config.cap_usd > 0.0))
        return false;
    } else {
      return false;
    }
  }
  Civil start;
  Civil end;
  return !config.symbols.empty() && parse_date(config.start, start) &&
         parse_date(config.end, end) &&
         days_from_civil(start.year, start.month, start.day) <=
             days_from_civil(end.year, end.month, end.day);
}

[[nodiscard]] std::uint64_t source_fingerprint(const databento::InstrumentDefMsg &definition) {
  const std::uint64_t hash = atx::core::hash_bytes(&definition, sizeof definition);
  return hash == 0u ? 1u : hash;
}

[[nodiscard]] std::string utc_date(std::int64_t timestamp_ns) {
  const time::CivilTime civil = time::to_civil_utc(time::Timestamp::from_unix_nanos(timestamp_ns));
  return format_date(Civil{civil.date.year, civil.date.month, civil.date.day});
}

} // namespace

int main(int argc, char **argv) {
  Config config;
  if (!parse_args(argc, argv, config)) {
    usage();
    return 2;
  }
  const char *key = std::getenv("DATABENTO_API_KEY");
  if (key == nullptr || *key == '\0') {
    std::fprintf(stderr, "DATABENTO_API_KEY is required for the free Metadata preflight\n");
    return 4;
  }

  Civil end_date;
  Civil start_date;
  (void)parse_date(config.start, start_date);
  (void)parse_date(config.end, end_date);
  const std::int64_t start_serial =
      days_from_civil(start_date.year, start_date.month, start_date.day);
  const std::int64_t end_serial = days_from_civil(end_date.year, end_date.month, end_date.day);
  std::vector<std::pair<std::string, std::string>> ranges;
  constexpr std::int64_t days_per_request = 7;
  for (std::int64_t first = start_serial; first <= end_serial; first += days_per_request) {
    const std::int64_t last_exclusive = std::min(end_serial + 1, first + days_per_request);
    ranges.emplace_back(format_date(civil_from_days(first)) + "T00:00:00Z",
                        format_date(civil_from_days(last_exclusive)) + "T00:00:00Z");
  }
  const databento::DateTimeRange<std::string> full_range{
      config.start + "T00:00:00Z", format_date(civil_from_days(end_serial + 1)) + "T00:00:00Z"};
  std::vector<std::string> parents;
  parents.reserve(config.symbols.size());
  for (const std::string &symbol : config.symbols) {
    parents.push_back(parent_symbol(symbol));
  }

  std::map<std::string, atx::vol::OccEssReport> occ_reports;
  if (!config.occ_ess_root.empty()) {
    for (std::int64_t serial = start_serial; serial <= end_serial; ++serial) {
      const std::string date = format_date(civil_from_days(serial));
      const std::filesystem::path path =
          std::filesystem::path(config.occ_ess_root) / (date + ".txt");
      std::error_code file_error;
      if (!std::filesystem::is_regular_file(path, file_error)) {
        continue;
      }
      auto report = atx::vol::read_occ_ess_report_file(path.string());
      if (!report) {
        std::fprintf(stderr, "cannot load OCC ESS authority from %s: %s\n",
                     path.string().c_str(), report.error().to_string().c_str());
        return 1;
      }
      if (report->trade_date() != date) {
        std::fprintf(stderr, "OCC ESS report date mismatch for %s\n", path.string().c_str());
        return 1;
      }
      occ_reports.emplace(date, std::move(*report));
    }
  }

  std::map<std::string, std::vector<std::uint32_t>> ids_by_date;
  if (!config.opra_root.empty()) {
    for (std::int64_t serial = start_serial; serial <= end_serial; ++serial) {
      const std::string date = format_date(civil_from_days(serial));
      std::vector<std::uint32_t> &date_ids = ids_by_date[date];
      for (const std::string &symbol : config.symbols) {
        const std::filesystem::path path =
            std::filesystem::path(config.opra_root) / symbol / (date + ".parquet");
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
          continue;
        }
        atx::vol::OpraLoadSpec load;
        load.path = path.string();
        load.underlying = symbol;
        load.snapshot_iso = date + "T19:55:00Z";
        load.r = 0.043;
        load.provenance_mode = atx::vol::OpraProvenanceMode::Strict;
        auto panel = atx::vol::load_opra_cbbo_parquet(load);
        if (!panel) {
          std::fprintf(stderr, "cannot load quote identities from %s: %s\n", path.string().c_str(),
                       panel.error().to_string().c_str());
          return 1;
        }
        date_ids.insert(date_ids.end(), panel->source_instrument_ids.begin(),
                        panel->source_instrument_ids.end());
      }
      std::sort(date_ids.begin(), date_ids.end());
      date_ids.erase(std::unique(date_ids.begin(), date_ids.end()), date_ids.end());
      if (date_ids.empty()) {
        ids_by_date.erase(date);
      }
    }
    std::size_t total_ids = 0u;
    for (const auto &[date, ids] : ids_by_date) {
      (void)date;
      total_ids += ids.size();
    }
    if (total_ids == 0u) {
      std::fprintf(stderr, "no strict quote instrument IDs found under --opra-root\n");
      return 1;
    }
    std::printf("loaded %zu exact quote instrument IDs across %zu dates\n", total_ids,
                ids_by_date.size());
  }

  try {
    auto client = databento::Historical::Builder().SetKey(key).Build();
    constexpr std::size_t max_parents = 5u;
    const double estimated_cost =
        client.MetadataGetCost(databento::dataset::kOpraPillar, full_range, parents,
                               databento::Schema::Definition, databento::SType::Parent, 0);
    std::printf("OPRA definition preflight: symbols=%zu window=%s..%s estimated=$%.6f cap=$%.2f\n",
                parents.size(), config.start.c_str(), config.end.c_str(), estimated_cost,
                config.cap_usd);
    if (estimated_cost > config.cap_usd) {
      std::fprintf(stderr, "REFUSED: estimated definition cost exceeds cap; no data pulled\n");
      return 3;
    }
    if (config.dry_run) {
      std::printf("dry run complete; no definition data pulled\n");
      return 0;
    }

    using Key = std::tuple<std::string, std::uint32_t, std::string>;
    std::map<Key, atx::vol::ListedContractDefinition> latest;
    std::size_t rejected = 0u;
    // FIX-E (E2-a). `rejected` alone hid two unrelated outcomes behind one
    // number: a record that is malformed or out of window, and a record whose
    // root is not in the requested universe at all. That is what let the BRK.B
    // defect below stay invisible — every BRK.B definition this tool PAID for was
    // discarded, and the only trace was a larger `rejected`.
    std::size_t unknown_root_rejected = 0u;
    std::size_t malformed_rejected = 0u;
    std::size_t sourced_standard_fallbacks = 0u;
    std::size_t occ_special_rejected = 0u;
    std::size_t missing_occ_authority = 0u;
    const auto consume_definition = [&](const databento::InstrumentDefMsg &source,
                                        std::string_view trade_date) {
      if (source.security_update_action == databento::SecurityUpdateAction::Delete ||
          (source.instrument_class != databento::InstrumentClass::Call &&
           source.instrument_class != databento::InstrumentClass::Put)) {
        return;
      }
      const std::int64_t definition_ts = source.ts_recv.time_since_epoch().count();
      const std::int64_t expiry_ts = source.expiration.time_since_epoch().count();
      const std::string raw_symbol = source.RawSymbol();
      const auto osi = atx::vol::parse_osi_symbol(raw_symbol);
      // FIX-E (E2-a). This was a byte-exact `std::find` of `osi->root` in
      // `config.symbols`, and it made this tool self-inconsistent: `parent_symbol`
      // above STRIPS DOTS to build the Databento request, so a `BRK.B` universe
      // entry is requested as `BRKB.OPT` and every returned record carries the
      // root `BRKB` — which then failed an exact compare against `BRK.B` and was
      // thrown away. One file asked for the data correctly and discarded all of
      // it, silently, into an undifferentiated `rejected` tally; the downstream
      // effect is that BRK.B is absent from every dispersion basket, because
      // `listed_quotes_from_opra` can only emit a contract it has a definition
      // for.
      //
      // The shared predicate is the right relaxation and the ONLY one: it accepts
      // a difference of punctuation and nothing else, so an adjusted-deliverable
      // root (`AAPL1`) is still rejected here. That matters because this tool is
      // the sole producer of the definitions table — accepting more definitions
      // must not smuggle a non-comparable contract into the join. (Belt and
      // braces: `listed_opra.cpp` also skips digit-bearing roots downstream.)
      if (definition_ts <= 0 || expiry_ts <= definition_ts || raw_symbol.empty() || !osi) {
        ++malformed_rejected;
        ++rejected;
        return;
      }
      // The exact compare is KEPT as the first disjunct so this change can only
      // ever accept MORE, never less: the shared predicate skips dots in the
      // TICKER only, so it would answer false for the (unobserved, but not
      // structurally impossible) case of a root that itself carries a dot and
      // equals a universe symbol byte-for-byte -- which the old `std::find`
      // accepted. This tool's output is a paid artifact; a relaxation that
      // silently dropped something is exactly the failure being repaired.
      const bool matches_universe_root =
          std::any_of(config.symbols.begin(), config.symbols.end(),
                      [&](const std::string &symbol) {
                        return symbol == osi->root ||
                               atx::vol::osi_root_matches_ticker(osi->root, symbol);
                      });
      if (!matches_universe_root) {
        ++unknown_root_rejected;
        ++rejected;
        return;
      }
      constexpr std::int32_t undefined_i32 = std::numeric_limits<std::int32_t>::max();
      const bool source_has_multiplier =
          source.contract_multiplier > 0 && source.contract_multiplier != undefined_i32;
      const bool source_has_original_size =
          source.original_contract_size > 0 && source.original_contract_size != undefined_i32;
      const atx::vol::OccEssReport *occ_report = nullptr;
      if (!source_has_multiplier || !source_has_original_size) {
        const auto found = occ_reports.find(std::string(trade_date));
        if (found == occ_reports.end()) {
          ++missing_occ_authority;
          ++rejected;
          return;
        }
        occ_report = &found->second;
        if (occ_report->is_special(osi->root)) {
          ++occ_special_rejected;
          ++rejected;
          return;
        }
      }
      const double multiplier =
          source_has_multiplier ? static_cast<double>(source.contract_multiplier) : 100.0;
      const bool standard_deliverable =
          source.leg_count == 0u &&
          source.user_defined_instrument == databento::UserDefinedInstrument::No &&
          ((!source_has_multiplier && !source_has_original_size) ||
           (source.contract_multiplier == 100 && source.original_contract_size == 100));
      if (!source_has_multiplier && !source_has_original_size && standard_deliverable) {
        ++sourced_standard_fallbacks;
      }
      atx::vol::ListedContractDefinition definition;
      definition.trade_date = trade_date;
      definition.instrument_id = source.hd.instrument_id;
      definition.raw_symbol = raw_symbol;
      definition.definition_ts_ns = definition_ts;
      definition.expiry_ts_ns = expiry_ts;
      definition.multiplier = multiplier;
      // standard_monthly is left at its default (false) here and assigned in a
      // per-trade-date pass below, once the full expiry set for each date is
      // known (holiday-aware classification needs the whole set, not one row).
      definition.standard_deliverable = standard_deliverable;
      // Bind the Databento definition and the daily OCC authority into the
      // contract provenance. The rule tag prevents collision with native
      // populated multiplier/deliverable fields.
      definition.source_fingerprint = static_cast<std::uint64_t>(atx::core::hash_combine(
          source_fingerprint(source),
          occ_report == nullptr ? 0u : occ_report->source_fingerprint(),
          source_has_multiplier ? 0u : 0x6f63632d31303075ULL));
      if (definition.source_fingerprint == 0u) {
        definition.source_fingerprint = 1u;
      }
      const Key definition_key{definition.trade_date, definition.instrument_id,
                               definition.raw_symbol};
      auto found = latest.find(definition_key);
      if (found == latest.end() || found->second.definition_ts_ns < definition.definition_ts_ns) {
        latest[definition_key] = std::move(definition);
      }
    };

    if (!ids_by_date.empty()) {
      constexpr std::size_t ids_per_request = 500u;
      std::size_t request_count = 0u;
      for (const auto &[date, ids] : ids_by_date) {
        (void)date;
        request_count += (ids.size() + ids_per_request - 1u) / ids_per_request;
      }
      std::size_t request_index = 0u;
      for (const auto &[date, ids] : ids_by_date) {
        Civil date_civil;
        (void)parse_date(date, date_civil);
        const std::string next_date = format_date(civil_from_days(
            days_from_civil(date_civil.year, date_civil.month, date_civil.day) + 1));
        const databento::DateTimeRange<std::string> range{date + "T00:00:00Z",
                                                          next_date + "T00:00:00Z"};
        for (std::size_t offset = 0; offset < ids.size(); offset += ids_per_request) {
          const std::size_t count = std::min(ids_per_request, ids.size() - offset);
          std::vector<std::string> batch;
          batch.reserve(count);
          for (std::size_t i = 0; i < count; ++i) {
            batch.push_back(std::to_string(ids[offset + i]));
          }
          std::printf("definition ID chunk %zu/%zu: date=%s ids=%zu\n", ++request_index,
                      request_count, date.c_str(), batch.size());
          std::fflush(stdout);
          client.TimeseriesGetRange(
              databento::dataset::kOpraPillar, range, batch, databento::Schema::Definition,
              databento::SType::InstrumentId, databento::SType::InstrumentId, 0,
              [](databento::Metadata &&) {},
              [&](const databento::Record &record) {
                if (record.Holds<databento::InstrumentDefMsg>()) {
                  consume_definition(record.Get<databento::InstrumentDefMsg>(), date);
                }
                return databento::KeepGoing::Continue;
              });
        }
      }
    } else {
      const std::size_t parent_batches = (parents.size() + max_parents - 1u) / max_parents;
      const std::size_t request_count = ranges.size() * parent_batches;
      std::size_t request_index = 0u;
      for (const auto &[range_start, range_end] : ranges) {
        const databento::DateTimeRange<std::string> range{range_start, range_end};
        for (std::size_t offset = 0; offset < parents.size(); offset += max_parents) {
          const std::size_t count = std::min(max_parents, parents.size() - offset);
          const std::vector<std::string> batch(
              parents.begin() + static_cast<std::ptrdiff_t>(offset),
              parents.begin() + static_cast<std::ptrdiff_t>(offset + count));
          std::printf("definition chunk %zu/%zu: %s..%s parents=%zu\n", ++request_index,
                      request_count, range_start.c_str(), range_end.c_str(), batch.size());
          std::fflush(stdout);
          client.TimeseriesGetRange(
              databento::dataset::kOpraPillar, range, batch, databento::Schema::Definition,
              databento::SType::Parent, databento::SType::InstrumentId, 0,
              [](databento::Metadata &&) {},
              [&](const databento::Record &record) {
                if (!record.Holds<databento::InstrumentDefMsg>()) {
                  return databento::KeepGoing::Continue;
                }
                const auto &source = record.Get<databento::InstrumentDefMsg>();
                consume_definition(source, utc_date(source.ts_recv.time_since_epoch().count()));
                return databento::KeepGoing::Continue;
              });
        }
      }
    }

    if (missing_occ_authority > 0u) {
      std::fprintf(stderr,
                   "REFUSED: %zu OPRA definitions require missing OCC ESS authority; "
                   "no table written\n",
                   missing_occ_authority);
      return 1;
    }

    // Holiday-aware standard_monthly classification. US listed-equity monthlies
    // settle the third Friday, or the Thursday before when that Friday is an
    // exchange holiday (e.g. Juneteenth). The classifier reads each trade date's
    // full observed expiry set — available only now, after all rows are buffered
    // — and never a hardcoded calendar. `latest` is keyed with trade_date first,
    // so entries are already grouped by date.
    {
      std::map<std::string, std::vector<std::int64_t>> expiries_by_date;
      for (const auto &[key, definition] : latest) {
        (void)key;
        expiries_by_date[definition.trade_date].push_back(definition.expiry_ts_ns);
      }
      std::map<std::string, std::vector<std::int64_t>> sessions_by_date;
      for (const auto &[date, expiries] : expiries_by_date) {
        sessions_by_date.emplace(date, atx::vol::standard_monthly_sessions(expiries));
      }
      for (auto &[key, definition] : latest) {
        (void)key;
        definition.standard_monthly = atx::vol::is_standard_monthly_expiry(
            sessions_by_date.at(definition.trade_date), definition.expiry_ts_ns);
      }
    }

    std::vector<atx::vol::ListedContractDefinition> definitions;
    definitions.reserve(latest.size());
    for (auto &[unused, definition] : latest) {
      (void)unused;
      definitions.push_back(std::move(definition));
    }
    auto table = atx::vol::ListedDefinitionTable::create(std::move(definitions));
    if (!table) {
      std::fprintf(stderr, "definition validation failed: %s\n", table.error().to_string().c_str());
      return 1;
    }
    const auto write = atx::vol::write_listed_definitions_file(config.out, *table);
    if (!write) {
      std::fprintf(stderr, "definition write failed: %s\n", write.error().to_string().c_str());
      return 1;
    }
    // `rejected` is the TOTAL and stays first for continuity; the two FIX-E
    // sub-counts break out the halves that used to be indistinguishable. A large
    // `unknown_root_rejected` against a small universe is the signature of a
    // symbol whose spelling does not reach the matcher — the shape of the BRK.B
    // defect this counter exists to make visible.
    std::printf("wrote %zu point-in-time definitions to %s "
                "(rejected=%zu unknown_root_rejected=%zu malformed_rejected=%zu "
                "occ_special_rejected=%zu sourced_standard_fallbacks=%zu "
                "fingerprint=%llu)\n",
                table->definitions().size(), config.out.c_str(), rejected, unknown_root_rejected,
                malformed_rejected, occ_special_rejected, sourced_standard_fallbacks,
                static_cast<unsigned long long>(table->fingerprint()));
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Databento definition request failed: %s\n", error.what());
    return 1;
  }
}
