#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/dispersion.hpp"        // DispersionSide
#include "atx/vol/listed_dispersion.hpp" // ListedDispersionSelection
#include "atx/vol/portfolio_pricer.hpp"  // SurfaceSet
#include "atx/vol/types.hpp"             // Result, Side

namespace atx::vol {

struct ListedOptionRisk {
  double model_mark{0.0};
  double delta_per_share{0.0};
  double vega_per_unit_vol{0.0};
};

using ListedRiskLookup =
    std::function<Result<ListedOptionRisk>(std::uint32_t uid, const ListedOptionQuote &quote)>;

struct ListedScheduleLeg {
  std::string roll_date{};
  std::uint32_t cohort{0};
  bool is_index{false};
  std::string symbol{};
  std::uint32_t uid{0};
  std::uint32_t instrument_id{0};
  std::string raw_symbol{};
  std::int64_t expiry_ts_ns{0};
  double strike{0.0};
  Side side{Side::Call};
  double quantity{0.0};
  double multiplier{100.0};
  double raw_bid{0.0};
  double raw_ask{0.0};
  double raw_mid{0.0};
  double model_mark{0.0};
  double delta_per_share{0.0};
  double vega_per_unit_vol{0.0};
  double vega_per_contract_per_vol_point{0.0};
  double normalized_weight{0.0};
  double target_straddle_vega_per_vol_point{0.0};
  double achieved_leg_vega_per_vol_point{0.0};
  std::uint64_t source_fingerprint{0};
  std::uint64_t surface_fingerprint{0};

  [[nodiscard]] bool operator==(const ListedScheduleLeg &) const = default;
};

struct ListedScheduleRoll {
  std::string roll_date{};
  std::int64_t valuation_ts_ns{0};
  std::uint32_t cohort{0};
  std::int64_t expiry_ts_ns{0};
  double gross_index_vega_target_per_vol_point{0.0};
  double net_vega_per_vol_point{0.0};
  double gross_vega_per_vol_point{0.0};
  std::uint32_t n_names{0};
  std::vector<ListedScheduleLeg> legs{};

  [[nodiscard]] bool operator==(const ListedScheduleRoll &) const = default;
};

struct ListedDispersionSchedule {
  std::vector<ListedScheduleRoll> rolls{};

  [[nodiscard]] bool operator==(const ListedDispersionSchedule &) const = default;
};

struct ListedScheduleBuildConfig {
  double gross_index_vega_target_per_vol_point{10000.0};
  DispersionSide side{DispersionSide::ShortIndexLongNames};
  double max_relative_vega_residual{1.0e-10};
  std::uint32_t cohort{0};
  std::uint64_t surface_fingerprint{0};
};

[[nodiscard]] Status
validate_listed_dispersion_schedule(const ListedDispersionSchedule &schedule,
                                    double max_relative_vega_residual = 1.0e-10);

// Build one roll from already-selected listed straddles. The risk lookup returns
// per-share American marks/Greeks for each exact contract. Quantities are
// continuous strategy notionals and identical for the call/put of a straddle.
[[nodiscard]] Result<ListedScheduleRoll>
build_listed_dispersion_roll(const ListedDispersionSelection &selection,
                             const ListedRiskLookup &risk_lookup,
                             const ListedScheduleBuildConfig &config = {});

// Production adapter: resolve exact residual T from the selection valuation and
// query the corresponding PricedSurface in `surfaces`.
[[nodiscard]] Result<ListedScheduleRoll>
build_listed_dispersion_roll(const ListedDispersionSelection &selection, const SurfaceSet &surfaces,
                             const ListedScheduleBuildConfig &config = {});

// Versioned deterministic TSV. Floating values use shortest round-trip decimal;
// parse validates row order, unique contract keys, roll aggregates, and vega
// arithmetic rather than trusting the persisted totals.
[[nodiscard]] Result<std::string>
serialize_listed_dispersion_schedule(const ListedDispersionSchedule &schedule);
[[nodiscard]] Result<ListedDispersionSchedule>
parse_listed_dispersion_schedule(std::string_view tsv);

[[nodiscard]] Status
write_listed_dispersion_schedule_file(std::string_view path,
                                      const ListedDispersionSchedule &schedule);
[[nodiscard]] Result<ListedDispersionSchedule>
read_listed_dispersion_schedule_file(std::string_view path);

} // namespace atx::vol
