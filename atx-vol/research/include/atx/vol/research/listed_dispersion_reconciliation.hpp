#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/backtest/backtest.hpp"
#include "atx/vol/api/backtest/listed_dispersion.hpp"
#include "atx/vol/api/backtest/listed_dispersion_schedule.hpp"
#include "atx/vol/api/backtest/portfolio_pricer.hpp"
#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

enum class ListedMarkRole : std::uint8_t { Entry = 0, Held = 1 };
enum class ListedMarkStatus : std::uint8_t {
  Ok = 0,
  NoRawQuote = 1,
  CrossedQuote = 2, // ask < bid, or a non-finite / non-positive ask
  NoSurface = 3,
  PricingError = 4,
  // FIX-F M3. F6 tightened `is_valid_listed_quote` from `bid >= 0` to `bid > 0`,
  // which silently re-labelled every zero-bid quote here as `CrossedQuote` — the
  // right DROP with the wrong REASON, in a persisted artifact. A zero bid is not
  // a crossed book; it is an absent bid. Dropping behaviour is unchanged
  // (`has_raw_mid` still keys on `Ok` alone), so no NAV or mark moves — only the
  // reason an operator reads.
  ZeroBidQuote = 5,
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
  // M3: RELATIVE tolerance for the entry-mark cross-check. On a roll date the
  // reconcile route reprices the entry with `PricedSurface::fair_value`, while the
  // schedule stored `leg.model_mark` from the build route `PricedSurface::evaluate`
  // — two American-pricing entry points that can differ by a few ULPs on a real
  // board. The check now allows |fair_value - evaluate| <= tol * max(|a|, |b|, 1),
  // so a benign 1-ULP route divergence no longer hard-aborts a valid run, while a
  // genuine economic mismatch (schedule vs archive disagree) is still caught. The
  // previous float-exact `0.0` was an absolute tolerance; set this to 0.0 to
  // restore that strict bit-for-bit check. Shared with the strategy's identical
  // guard via kListedEntryMarkTolerance / listed_entry_mark_agrees — the two must
  // never diverge (listed_dispersion_schedule.hpp).
  double entry_mark_tolerance{kListedEntryMarkTolerance};
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

} // namespace atx::vol
