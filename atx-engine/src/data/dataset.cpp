// atx::engine::data — Dataset::create validation body + column_by_name.
//
// Construction is COLD-PATH; validation is explicit and returns Result<Dataset>
// on any contract violation. Hot-path accessors are all inline in dataset.hpp.

#include "atx/engine/data/dataset.hpp"

#include <chrono>
#include <limits>
#include <string>
#include <unordered_set>

#include "atx/core/error.hpp"

namespace atx::engine::data {

namespace {

[[nodiscard]] atx::core::Result<DateKey>
availability_date(DateKey date, DateKeyEncoding encoding, atx::u16 delay) {
  using atx::core::Err;
  using atx::core::ErrorCode;
  using atx::core::Ok;
  DateKey increment = static_cast<DateKey>(delay);
  switch (encoding) {
  case DateKeyEncoding::Opaque:
    if (delay != 0U) {
      return Err(ErrorCode::InvalidArgument,
                 "Dataset: positive pit_delay requires an explicit date_encoding");
    }
    return Ok(date);
  case DateKeyEncoding::EpochDays:
    break;
  case DateKeyEncoding::UnixNanoseconds:
    // u16's maximum * nanoseconds/day fits in i64; addition is checked below.
    increment *= DateKey{86'400'000'000'000};
    break;
  case DateKeyEncoding::YYYYMMDD: {
    if (date < 10101 || date > 99991231) {
      return Err(ErrorCode::InvalidArgument, "Dataset: YYYYMMDD year must be in [1, 9999]");
    }
    const std::chrono::year_month_day calendar{
        std::chrono::year{static_cast<int>(date / 10000)},
        std::chrono::month{static_cast<unsigned>((date / 100) % 100)},
        std::chrono::day{static_cast<unsigned>(date % 100)}};
    if (!calendar.ok()) {
      return Err(ErrorCode::InvalidArgument, "Dataset: invalid YYYYMMDD date");
    }
    const std::chrono::year_month_day available{
        std::chrono::sys_days{calendar} + std::chrono::days{delay}};
    const int year = static_cast<int>(available.year());
    if (year > 9999) {
      return Err(ErrorCode::InvalidArgument, "Dataset: availability date exceeds year 9999");
    }
    return Ok(static_cast<DateKey>(year) * 10000 +
              static_cast<DateKey>(static_cast<unsigned>(available.month())) * 100 +
              static_cast<DateKey>(static_cast<unsigned>(available.day())));
  }
  default:
    return Err(ErrorCode::InvalidArgument, "Dataset: unknown date_encoding");
  }
  if (date > std::numeric_limits<DateKey>::max() - increment) {
    return Err(ErrorCode::InvalidArgument, "Dataset: availability date overflows DateKey");
  }
  return Ok(date + increment);
}

} // namespace

// static
atx::core::Result<Dataset> Dataset::create(DatasetSchema schema, std::vector<DateKey> dates,
                                           std::vector<InstKey> instruments,
                                           std::vector<std::vector<atx::f64>> columns,
                                           std::vector<std::uint8_t> mask,
                                           DatasetProvenance provenance) {
  // 1. Schema dtype/role coherence.
  if (!schema_is_coherent(schema)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "Dataset::create: schema is not coherent (columns empty or "
                          "columns.size() != dtypes.size())");
  }

  // Validate encoding even for an empty dataset (there may be no date to shift).
  const DateKey probe = schema.date_encoding == DateKeyEncoding::YYYYMMDD ? 20000101 : 0;
  ATX_TRY(const DateKey validated_probe,
          availability_date(probe, schema.date_encoding, schema.pit_delay));
  (void)validated_probe;
  std::vector<DateKey> available_dates;
  available_dates.reserve(dates.size());
  for (const DateKey date : dates) {
    ATX_TRY(const DateKey available, availability_date(date, schema.date_encoding, schema.pit_delay));
    available_dates.push_back(available);
  }

  // 2. Caller-supplied columns count must match schema column count.
  if (columns.size() != schema.columns.size()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "Dataset::create: columns.size() (" + std::to_string(columns.size()) +
                              ") != schema.columns.size() (" +
                              std::to_string(schema.columns.size()) + ")");
  }

  // Identity must be unambiguous before any adapter constructs an InstKey map.
  std::unordered_set<InstKey> unique_instruments;
  unique_instruments.reserve(instruments.size());
  for (const InstKey instrument : instruments) {
    if (!unique_instruments.insert(instrument).second) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Dataset::create: duplicate instrument key");
    }
  }

  // 3. Every column must have exactly dates*instruments cells (no ragged).
  if (!instruments.empty() && dates.size() >
      std::numeric_limits<atx::usize>::max() / instruments.size()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "Dataset::create: dates*instruments overflows");
  }
  const atx::usize expected_cells = dates.size() * instruments.size();
  for (atx::usize c = 0; c < columns.size(); ++c) {
    if (columns[c].size() != expected_cells) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Dataset::create: column[" + std::to_string(c) + "] has " +
                                std::to_string(columns[c].size()) + " cells; expected " +
                                std::to_string(expected_cells) +
                                " (dates=" + std::to_string(dates.size()) +
                                " * instruments=" + std::to_string(instruments.size()) + ")");
    }
  }

  // 4. Mask, when non-empty, must match dates*instruments.
  if (!mask.empty() && mask.size() != expected_cells) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "Dataset::create: mask.size() (" + std::to_string(mask.size()) +
                              ") != dates*instruments (" + std::to_string(expected_cells) + ")");
  }

  for (const std::uint8_t membership : mask) {
    if (membership > 1U) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Dataset::create: mask must contain only 0 or 1");
    }
  }

  Dataset ds;
  ds.schema_ = std::move(schema);
  ds.dates_ascending_ = is_strictly_ascending(dates);
  ds.dates_ = std::move(dates);
  ds.available_dates_ = std::move(available_dates);
  ds.instruments_ = std::move(instruments);
  ds.columns_ = std::move(columns);
  ds.mask_ = std::move(mask);
  ds.provenance_ = std::move(provenance);
  return atx::core::Ok(std::move(ds));
}

atx::core::Result<std::optional<atx::usize>>
Dataset::available_as_of_index(DateKey decision_date) const {
  if (!dates_ascending_) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "Dataset: availability lookup requires strictly ascending dates");
  }
  ATX_TRY(const DateKey validated, availability_date(decision_date, schema_.date_encoding, 0));
  return atx::core::Ok(as_of_index(available_dates_, validated));
}

atx::core::Result<std::span<const atx::f64>> Dataset::column_by_name(std::string_view name) const {
  const std::vector<std::string> &names = schema_.columns;
  for (atx::usize i = 0; i < names.size(); ++i) {
    if (names[i] == name) {
      return atx::core::Ok(std::span<const atx::f64>{columns_[i]});
    }
  }
  return atx::core::Err(atx::core::ErrorCode::NotFound,
                        std::string{"Dataset::column_by_name: unknown column '"} +
                            std::string{name} + "'");
}

} // namespace atx::engine::data
