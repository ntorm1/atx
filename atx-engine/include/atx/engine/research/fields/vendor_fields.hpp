#pragma once

// atx::engine::research::fields -- the fields on the shared vendor panel (P9 lane A3, migration
// slice 4), ported from research_fields_price.py (ret_overnight, ret_intraday, ceq_iss_5y) and
// research_fields_ohlc.py (open_adj, high_adj, low_adj). The spec text of each is the registry's
// (field_registry.json spec_text; the formula fingerprint test pins every copy), the arithmetic is
// the Python's operation for operation (each product and each log in its own statement), and the
// payload, its coverage and its formula fingerprint must equal the Python builder's (gtest
// ResearchFieldsVendorFixture.*).
//
// Clocks:
//   price-close-lag1-v1 (ret_overnight, ret_intraday, ceq_iss_5y): role row t reads axis rows
//     <= prefix + t - 1 (the panel's extended axis: NYSE rule sessions before the role, the role's
//     sessions inside it); ceq_iss_5y also reads the row 1260 sessions earlier and the shares
//     observations the 90-day lag (A8) allows;
//   ohlc-same-session-v1 (open_adj, high_adj, low_adj): role row t reads the vendor bar of
//     session t and the role's close.f64, raw_close.f64, present.u8 of row t.
// coskew_60m (price module) is not ported: its equal-weight market is a DuckDB fsum whose row
// order is DuckDB's own, so no engine sum can claim its bits; xrd0_ttm reads other fields of the
// same run, not the panel. Both stay Python fields.
//
// The row kernels below are the builders' arithmetic for one row, given the axis row a clock
// reads; the builders own the clock. Tests drive the kernels with a shifted clock to plant a leak
// that the look-ahead probe must catch.

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/field_writer.hpp"
#include "atx/engine/research/fields/role_axes.hpp"
#include "atx/engine/research/fields/sources/vendor_panel.hpp"

namespace atx::engine::research::fields {

inline constexpr usize kPriceLagSessions = 1; // research_fields_price.py LAG_SESSIONS
inline constexpr usize kOhlcLagSessions = 0;  // research_fields_ohlc.py LAG_SESSIONS
inline constexpr usize kCeqSessions = 1260;   // five years of sessions (Daniel-Titman 2006)
inline constexpr usize kPriceHistorySessions = kCeqSessions + 1;
inline constexpr usize kOpenReturnSessions = 2;  // ret_overnight row 0 reads two sessions back
inline constexpr i64 kSharesLagDays = 90;        // A8
inline constexpr i64 kSharesMaxAgeDays = 400;    // A8
inline constexpr f64 kGuardLog = 1.5;            // the house rough-return guard
inline constexpr f64 kGuardExcess = 0.10;
// CEQ_DOMAIN = (-ln 100, ln 100): Python math.log(100.0), repr 4.605170185988092.
inline constexpr f64 kCeqDomainHigh = 4.605170185988092;

// The vendor-panel fields, in registration order.
[[nodiscard]] std::span<const std::string_view> vendor_field_names() noexcept;

// The declared spec of a vendor-panel field; nullptr for another name.
[[nodiscard]] const FieldSpec *vendor_field_spec(std::string_view name);

// What the field needs from the panel (research_fields_price.compute's history rule: 1,261 rule
// sessions and the 490-day share lookback with ceq_iss_5y, two sessions for the open returns; the
// role's sessions only for the bars); nullopt for another name.
[[nodiscard]] std::optional<VendorPanelRequest> vendor_request(std::string_view name);

// True for the ohlc fields, which also read the role's close.f64, raw_close.f64 and present.u8.
[[nodiscard]] bool reads_role_close(std::string_view name) noexcept;

struct VendorField {
  WrittenField field;
  std::vector<SourceRecord> sources; // the vendor file; then (ohlc) close, raw_close, present
  nlohmann::json extra;              // the entry's own keys (Python field_extras)
  nlohmann::json source_checks;      // the panel's read statistics and the field's clock
};

// Writes `output_dir`/<name>.f64 from the panel (loaded for this role with the field's request).
// Err(InvalidArgument) for another name, a panel of another role or without the field's matrices.
[[nodiscard]] core::Result<VendorField> build_vendor_field(std::string_view name,
                                                           const VendorPanel &panel,
                                                           const RoleAxes &role,
                                                           const std::filesystem::path &output_dir);

// ---- row kernels ----------------------------------------------------------------------------

enum class OpenReturn : u8 { Overnight, Intraday };

// ret_overnight / ret_intraday reading axis row a (and a - 1): `out` gets the row's values; the
// return value counts the member cells the guard set to NaN. Preconditions (asserted): 1 <= a <
// rows(); member and out hold lines() values; the panel has price and price_open.
[[nodiscard]] u64 open_return_row(OpenReturn which, const VendorPanel &panel, usize a,
                                  std::span<const u8> member, std::span<f64> out);

// Each line's shares observations (axis rows with a finite shares value), for the A8 lag rule.
class ShareIndex {
public:
  explicit ShareIndex(const VendorPanel &panel);
  // The share observation the lag rule picks for axis row s: the line's last one dated <=
  // day(s) - 90 and >= day(s) - 490; nullopt when none.
  [[nodiscard]] std::optional<usize> lagged(usize line, usize s) const noexcept;

private:
  const VendorPanel *panel_; // non-owning, non-null; the panel outlives the index
  std::vector<std::vector<u32>> rows_;
};

// ceq_iss_5y reading axis row a and a - 1260 (all NaN while a < 1260): `out` gets the row's
// values; the return value counts the member cells outside the declared domain.
[[nodiscard]] u64 ceq_issuance_row(const VendorPanel &panel, const ShareIndex &shares, usize a,
                                   std::span<const u8> member, std::span<f64> out);

enum class Bar : u8 { Open, High, Low };

struct BarCounts {
  u64 order_violation_member_cells{};
  u64 missing_bar_member_cells{};
};

// open_adj / high_adj / low_adj of one role row from the vendor bar at axis row `bar_row` (nullopt:
// no bar) and the role's close, raw close and presence of the same row.
void ohlc_bar_row(Bar which, const VendorPanel &panel, std::optional<usize> bar_row,
                  std::span<const f64> close, std::span<const f64> raw_close,
                  std::span<const u8> present, std::span<const u8> member, BarCounts &counts,
                  std::span<f64> out);

} // namespace atx::engine::research::fields
