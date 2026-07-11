#pragma once

// Deterministic selection of actual listed ATM straddles for a traditional
// index-versus-components dispersion book. This module owns contract selection
// only: callers provide date-scoped OPRA observations and a forward resolver
// (normally backed by reloaded PricedSurfaces).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/dispersion.hpp" // DispersionUniverse
#include "atx/vol/types.hpp"      // Result, Side

namespace atx::vol {

inline constexpr double kListedNsPerDay = 86400.0 * 1.0e9;

// One point-in-time listed option observation after the source/definition join.
// `trade_date` scopes Databento's daily instrument id. Exact expiry timestamps,
// deliverable state, and multipliers come from point-in-time definitions or a
// sourced series rule, never from a synthetic target tenor.
struct ListedOptionQuote {
  std::string trade_date{};
  std::string symbol{};
  std::uint32_t instrument_id{0};
  std::string raw_symbol{};
  std::int64_t expiry_ts_ns{0};
  double strike{0.0};
  Side side{Side::Call};
  double bid{0.0};
  double ask{0.0};
  std::int64_t quote_ts_ns{0};
  double multiplier{100.0};
  bool standard_monthly{false};
  bool standard_deliverable{false};
  std::uint64_t source_fingerprint{0};

  [[nodiscard]] bool operator==(const ListedOptionQuote &) const = default;
};

enum class ListedDropReason : std::uint8_t {
  NoQuotesForExpiry = 0,
  NoValidStraddle = 1,
  ForwardUnavailable = 2,
};

[[nodiscard]] const char *to_string(ListedDropReason reason) noexcept;

struct ListedDroppedName {
  std::string symbol{};
  ListedDropReason reason{ListedDropReason::NoQuotesForExpiry};
  std::string detail{};

  [[nodiscard]] bool operator==(const ListedDroppedName &) const = default;
};

struct ListedStraddle {
  std::string symbol{};
  std::uint32_t uid{0};
  std::int64_t expiry_ts_ns{0};
  double strike{0.0};
  ListedOptionQuote call{};
  ListedOptionQuote put{};
  double raw_weight{0.0};
  double normalized_weight{0.0};

  [[nodiscard]] bool operator==(const ListedStraddle &) const = default;
};

struct ListedDispersionSelection {
  std::string trade_date{};
  std::int64_t valuation_ts_ns{0};
  std::int64_t expiry_ts_ns{0};
  double dte_days{0.0};
  ListedStraddle index{};
  std::vector<ListedStraddle> names{};
  std::vector<ListedDroppedName> dropped{};

  [[nodiscard]] bool operator==(const ListedDispersionSelection &) const = default;
};

struct ListedDispersionSelectionConfig {
  double target_dte_days{30.0};
  double min_dte_days{21.0};
  double max_dte_days{60.0};
  std::size_t min_names{10};
  double required_multiplier{100.0};
};

// Resolve the forward for one universe member at an exact listed expiry.
using ListedForwardLookup =
    std::function<Result<double>(const DispersionMember &, std::int64_t expiry_ts_ns)>;

// True only for the locked run's executable quote contract: finite nonnegative
// bid, finite positive ask, and a noncrossed market.
[[nodiscard]] bool is_valid_listed_quote(const ListedOptionQuote &quote) noexcept;

// Select one common standard-monthly expiry and one actual listed ATM-forward
// call/put pair for the index and every surviving name.
//
// Expiries are sourced from valid index pairs, restricted to [min_dte,max_dte],
// ordered by |DTE-target| then earlier expiry, and tried until one carries at
// least `min_names` valid component straddles. For each member the strike
// minimizes |K-F| with the lower strike winning a tie. Expected component market
// unavailability is recorded in `dropped`; malformed identities, look-ahead,
// invalid config/universe, and any index-leg failure are hard errors.
[[nodiscard]] Result<ListedDispersionSelection> select_listed_dispersion(
    std::string_view trade_date, std::int64_t valuation_ts_ns, const DispersionUniverse &universe,
    std::span<const ListedOptionQuote> quotes, const ListedForwardLookup &forward_lookup,
    const ListedDispersionSelectionConfig &config = {});

} // namespace atx::vol
