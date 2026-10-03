#pragma once

// atx::engine::research::admission -- the screen's decision table as fit_composition_weights.py
// writes it (admission_csv over V4_CSV_COLUMNS, fit:1751-1774): the same columns, the same cells
// and the same float spelling (Python repr: shortest round-trip digits, exponent form below 1e-4
// and from 1e16), so a C++ admission.csv compares with the fitter's cell by cell. The traded
// horizon (F-3, report only) is a separate file, so admission.csv keeps the fitter's columns.

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/admission/screen.hpp"
#include "atx/engine/research/admission/traded_horizon.hpp"

namespace atx::engine::research::admission {

// TIER_GRADES (fit:292), strongest first: a grade's index is its rank.
inline constexpr std::array<std::string_view, 10> kTierGrades{"A+", "A",  "A-", "B+", "B",
                                                              "B-", "C+", "C",  "C-", "D"};

// A candidate's tier: the CSV cell and the order key of the redundancy pass.
struct Tier {
  std::string text; // a grade, or a decimal integer
  usize rank{};     // the grade's index in kTierGrades, or the integer
};
// Err(InvalidArgument) for a string outside kTierGrades.
[[nodiscard]] core::Result<Tier> tier_from_grade(std::string_view grade);
[[nodiscard]] Tier tier_from_integer(u64 value);

// The per-candidate metadata the table carries beside the screen's row (the fitter's library,
// prior metadata, runner orientations and cache entries).
struct CandidateMeta {
  std::string id;
  std::string family;
  std::string theme;
  Tier tier;
  i32 prior_sign{};
  i32 runner_sign{};
  std::string cache_entry;
  std::string cache_payload_sha256;
};

// Python's repr(float) for a double: "0.0", "-0.0", "1e-05", "0.0001", "1e+16", "nan", "inf".
[[nodiscard]] std::string python_float_repr(f64 x);

// V4_CSV_COLUMNS (fit:1751-1754).
inline constexpr std::array<std::string_view, 24> kV4CsvColumns{
    "id",           "family",           "theme",              "tier",
    "prior_sign",   "status",           "failed_checks",      "redundant_with",
    "redundant_rho", "admission_rank",  "s_k",                "runner_sign",
    "sign_agrees",  "tau",              "train_days",         "train_mean",
    "train_sharpe", "hac_t",            "max_abs_rho",        "max_abs_rho_with",
    "low_overlap_with", "undefined_rho_with", "cache_entry",  "cache_payload_sha256"};

// admission.csv: the header, one line per candidate in roster order, "\n" after every line.
// Err(InvalidArgument) when meta and rows differ in length, a row names a candidate outside the
// roster, or a cell holds ',' or a newline (the fitter's "unsafe cell" refusal).
[[nodiscard]] core::Result<std::string> admission_csv(std::span<const CandidateMeta> meta,
                                                      std::span<const ScreenRow> rows);

// traded_horizon.csv (report only): id, status, s_k, h21_days, ic_h21, ic_h21_hac_t.
[[nodiscard]] core::Result<std::string>
traded_horizon_csv(std::span<const CandidateMeta> meta, std::span<const ScreenRow> rows,
                   std::span<const TradedHorizonRow> horizon);

} // namespace atx::engine::research::admission
