#pragma once

// atx::engine::research::fields -- typed decoders of one decoded stage table's columns into plain
// vectors, with the null mapping of research_fields_holdings.py's column helpers (_f64, _ids,
// _days, _instants, _text). The typed parquet bridges of atx::core::io drop validity, so every
// decoder here reads the column's null mask. Err(InvalidArgument) for an absent column or a type
// the decoder does not take; Err(ParseError) for an id text that is not a decimal integer.

#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/io/parquet.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::fields {

// The day a null date reads as (the Python _days fill, -(1 << 30)).
inline constexpr i64 kNullDay = -(1LL << 30);

// Any integer or floating column as f64; null -> NaN.
[[nodiscard]] core::Result<std::vector<f64>> real_column(const core::io::ParquetTable &table,
                                                         std::string_view name);

// An integer column, or a text column of decimal integers, as i64; null -> -1.
[[nodiscard]] core::Result<std::vector<i64>> id_column(const core::io::ParquetTable &table,
                                                       std::string_view name);

// A DATE32 column as calendar days since 1970-01-01; null -> kNullDay.
[[nodiscard]] core::Result<std::vector<i64>> day_column(const core::io::ParquetTable &table,
                                                        std::string_view name);

// A TIMESTAMP column (any unit) as ns since 1970-01-01 UTC; null -> kNeverNs (stage.hpp).
[[nodiscard]] core::Result<std::vector<i64>> instant_column(const core::io::ParquetTable &table,
                                                            std::string_view name);

// A UTF-8 column as owned strings; null -> "".
[[nodiscard]] core::Result<std::vector<std::string>>
text_column(const core::io::ParquetTable &table, std::string_view name);

} // namespace atx::engine::research::fields
