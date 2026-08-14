#pragma once

// OCC Equity Special Settlements (ESS) report ingestion. The daily report is
// the authoritative list of non-standard equity-option product symbols and
// their deliverables. A symbol absent from a validated report may use the
// standard 100-share equity/ETF contract rule; a symbol present in the report
// requires its explicit OCC deliverable and is excluded by the listed
// dispersion workflow.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

class OccEssReport {
public:
  OccEssReport() = default;

  // `trade_date()` and `special_symbols()` BORROW storage this report owns; the
  // report is the owner, the view is not. A report is immutable once
  // `parse_occ_ess_report` has returned it (the parser is its only friend and no
  // member function writes), so both views stay valid for the report's lifetime
  // and concurrent const readers are safe. Destroying the report, or assigning
  // over it (the type is copyable AND movable), invalidates them — and a view
  // taken from one report never names a copy's storage. Copy out
  // (`std::vector<std::string>{sp.begin(), sp.end()}`) to outlive the report.
  [[nodiscard]] std::string_view trade_date() const noexcept { return trade_date_; }
  [[nodiscard]] std::span<const std::string> special_symbols() const noexcept {
    return special_symbols_;
  }
  [[nodiscard]] bool is_special(std::string_view option_product_symbol) const noexcept;
  [[nodiscard]] std::uint64_t source_fingerprint() const noexcept { return source_fingerprint_; }

private:
  friend Result<OccEssReport> parse_occ_ess_report(std::string_view text);

  std::string trade_date_{};
  std::vector<std::string> special_symbols_{};
  std::uint64_t source_fingerprint_{0};
};

// Parse the OCC fixed-width ESS text report. The parser validates the report
// title, activity date, record id/product kind, process date, and every data
// row before returning a canonical sorted symbol set.
[[nodiscard]] Result<OccEssReport> parse_occ_ess_report(std::string_view text);
[[nodiscard]] Result<OccEssReport> read_occ_ess_report_file(std::string_view path);

} // namespace atx::vol
