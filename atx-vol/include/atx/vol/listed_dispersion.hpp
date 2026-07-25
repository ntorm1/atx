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

// ── WS-F F6 (BT-P2-8): quote-quality admission ──────────────────────────────
//
// The loader drops crossed NBBO, but selection accepted a ZERO BID and had no
// staleness check at all beyond "not in the future". A zero-bid leg yields
// mid = ask/2 — a price nobody trades at, on a side nobody can sell to — and it
// could enter straddle selection and the recorded `raw_mid` PnL series. A
// 30-minute-old NBBO row passed as current.
struct ListedQuoteQualityConfig {
  // A bid AT OR BELOW this floor is not an executable market. The default 0.0
  // therefore rejects the zero bid (the defect) while accepting any real one;
  // raise it to demand a minimum quoted bid.
  double min_bid{0.0};
  // Maximum age of `quote_ts_ns` behind the valuation instant. 0 DISABLES the
  // check (the pre-F6 contract). Default 10 minutes.
  std::int64_t max_quote_age_ns{600LL * 1'000'000'000LL};
  // A LOCKED market (ask == bid) is counted but NOT dropped by default: the mid
  // is still the true price, so dropping it would discard a good quote. Set
  // true to exclude locked markets from selection as well.
  bool reject_locked{false};
};

// Why one quote was not admitted. `None` means admitted.
enum class ListedQuoteReject : std::uint8_t {
  None = 0,
  NotTwoSided = 1, // non-finite, ask <= 0, or crossed (ask < bid)
  ZeroBid = 2,     // bid <= min_bid: the mid would be the fiction ask/2
  Stale = 3,       // quote_ts_ns older than max_quote_age_ns
  Locked = 4,      // ask == bid, and reject_locked is set
};

[[nodiscard]] const char *to_string(ListedQuoteReject reject) noexcept;

// Per-date admission tally for the join report. `locked` counts EVERY locked
// market seen, whether or not the policy dropped it, so the flag survives the
// default no-drop policy.
struct ListedQuoteRejectCounts {
  std::uint32_t not_two_sided{0};
  std::uint32_t zero_bid{0};
  std::uint32_t stale{0};
  std::uint32_t locked{0};
  std::uint32_t non_standard{0}; // wrong multiplier / not standard monthly or deliverable

  [[nodiscard]] std::uint32_t total_dropped() const noexcept {
    return not_two_sided + zero_bid + stale + non_standard;
  }
  [[nodiscard]] bool operator==(const ListedQuoteRejectCounts &) const = default;
};

// Classify one quote against the quality policy. `valuation_ts_ns` anchors the
// staleness window; pass the selection's valuation instant.
[[nodiscard]] ListedQuoteReject
classify_listed_quote(const ListedOptionQuote &quote, std::int64_t valuation_ts_ns,
                      const ListedQuoteQualityConfig &quality) noexcept;

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
  // F6: per-date quote-admission tally over every quote inspected while
  // selecting the CHOSEN expiry (index leg + every universe member).
  ListedQuoteRejectCounts quote_rejects{};

  [[nodiscard]] bool operator==(const ListedDispersionSelection &) const = default;
};

struct ListedDispersionSelectionConfig {
  double target_dte_days{30.0};
  double min_dte_days{21.0};
  double max_dte_days{60.0};
  std::size_t min_names{10};
  double required_multiplier{100.0};
  ListedQuoteQualityConfig quality{}; // F6
};

// Resolve the forward for one universe member at an exact listed expiry.
using ListedForwardLookup =
    std::function<Result<double>(const DispersionMember &, std::int64_t expiry_ts_ns)>;

// True only for the locked run's executable quote contract: finite nonnegative
// bid, finite positive ask, and a noncrossed market.
// F6 (BT-P2-8): a ZERO bid is no longer executable. It used to pass — yielding
// mid = ask/2, a fictional price on a side nobody can sell to.
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
