#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/backtest.hpp"
#include "atx/vol/listed_dispersion.hpp"
#include "atx/vol/listed_dispersion_schedule.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

enum class ListedMarkRole : std::uint8_t { Entry = 0, Held = 1 };
enum class ListedMarkStatus : std::uint8_t {
  Ok = 0,
  NoRawQuote = 1,
  CrossedQuote = 2,
  NoSurface = 3,
  PricingError = 4,
};

[[nodiscard]] const char *to_string(ListedMarkRole role) noexcept;
[[nodiscard]] const char *to_string(ListedMarkStatus status) noexcept;

struct ListedContractMark {
  std::string date{};
  std::int64_t valuation_ts_ns{0};
  ListedMarkRole role{ListedMarkRole::Held};
  std::uint32_t cohort{0};
  std::string symbol{};
  std::uint32_t uid{0};
  std::uint32_t instrument_id{0};
  std::string raw_symbol{};
  std::int64_t expiry_ts_ns{0};
  double strike{0.0};
  Side side{Side::Call};
  double quantity{0.0};
  double multiplier{0.0};
  ListedMarkStatus status{ListedMarkStatus::Ok};
  double raw_bid{0.0};
  double raw_ask{0.0};
  double raw_mid{0.0};
  double model_mark{0.0};
  bool model_in_spread{false};

  [[nodiscard]] bool operator==(const ListedContractMark &) const = default;
};

struct ListedReconciliationRow {
  std::string date{};
  std::int64_t valuation_ts_ns{0};
  std::uint32_t held_cohort{0};
  double model_option_pnl{0.0};
  double quote_mid_pnl{0.0};
  double model_minus_quote_pnl{0.0};
  double model_nav{0.0};
  double quote_mid_nav{0.0};
  double quote_mid_coverage{1.0};
  std::uint32_t n_held_lots{0};
  std::uint32_t n_quote_mid_lots{0};

  [[nodiscard]] bool operator==(const ListedReconciliationRow &) const = default;
};

struct ListedDispersionReconciliation {
  std::vector<ListedContractMark> marks{};
  std::vector<ListedReconciliationRow> rows{};

  [[nodiscard]] bool operator==(const ListedDispersionReconciliation &) const = default;
};

// One independently loaded date. `surfaces` must originate from that date's
// persisted archive; quotes are the strict OPRA/definition join for the date.
struct ListedReconciliationSnapshot {
  std::string date{};
  std::int64_t valuation_ts_ns{0};
  const SurfaceSet *surfaces{nullptr};
  std::span<const ListedOptionQuote> quotes{};
};

struct ListedReconciliationConfig {
  bool strict_model{true};
  double entry_mark_tolerance{0.0};
};

// Reprice the exact scheduled contracts, independently join daily raw quotes,
// and compute endpoint-consistent model/quote P&L. On a roll date the old
// cohort is marked with role Held before the new cohort is recorded as Entry.
[[nodiscard]] Result<ListedDispersionReconciliation>
reconcile_listed_dispersion(const ListedDispersionSchedule &schedule,
                            std::span<const ListedReconciliationSnapshot> snapshots,
                            const ListedReconciliationConfig &config = {});

// Validate the independent exact-mark model P&L against the option portion of
// the canonical backtest result. Shares, financing, costs, and settlement are
// removed using the engine's own exported columns.
//
// The reconciliation must be a contiguous SUFFIX of the backtest, matched by
// date: it starts at some backtest row and runs to the last one. That is the M1
// shape — `assemble_reconciliation_snapshots` trims the warm-up sessions ahead
// of the first roll, so on a corpus with a lead-in the reconciliation is shorter
// than the backtest by exactly that lead-in. A reconciliation whose first date
// is absent from the backtest, or which stops before the backtest's last
// session, is rejected. With no lead-in the offset is zero and the comparison is
// the historical row-for-row one.
[[nodiscard]] Status
validate_listed_reconciliation_backtest(const ListedDispersionReconciliation &reconciliation,
                                        const BacktestResult &backtest,
                                        double absolute_tolerance = 1.0e-8);

[[nodiscard]] std::string
serialize_listed_contract_marks(const ListedDispersionReconciliation &reconciliation);
[[nodiscard]] std::string
serialize_listed_reconciliation(const ListedDispersionReconciliation &reconciliation);
[[nodiscard]] Status
write_listed_contract_marks_file(std::string_view path,
                                 const ListedDispersionReconciliation &reconciliation);
[[nodiscard]] Status
write_listed_reconciliation_file(std::string_view path,
                                 const ListedDispersionReconciliation &reconciliation);

} // namespace atx::vol
