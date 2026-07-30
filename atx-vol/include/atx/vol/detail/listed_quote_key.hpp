#pragma once

// The one identity a listed-option leg and a joined OPRA quote agree on.
//
// This type was `LegKey`/`key_of` in the anonymous namespace of
// `src/listed_dispersion_reconciliation.cpp`. It is promoted here unchanged so
// that the OPRA join's leg-key FILTER and the reconciliation's leg-key LOOKUP
// are provably the same key: there is exactly one definition of "same contract"
// on this path, and a filter that used a different one could silently drop a
// leg the reconciliation then fails to mark. The reconciliation now consumes
// this type and no longer carries a private duplicate.
//
// (raw_symbol, expiry_ts_ns, strike, side) is deliberately NOT instrument-id
// scoped: Databento instrument ids are day-scoped, and a schedule leg written on
// its roll date must still match the same contract's quote on a later session.

#include <compare>
#include <cstdint>
#include <string>

#include "atx/vol/listed_dispersion.hpp"          // ListedOptionQuote
#include "atx/vol/listed_dispersion_schedule.hpp" // ListedScheduleLeg
#include "atx/vol/types.hpp"                      // Side

namespace atx::vol {

// Ordered by (raw_symbol, expiry_ts_ns, strike, side), which is the order the
// reconciliation's `std::map` has always used (it was a `std::tie` comparison
// over exactly these four members, in this order) and the order the OPRA join's
// filter binary-searches. Member order IS the comparison order — do not reorder.
//
// The defaulted `<=>` is `std::partial_ordering` because of the `double`. That
// is only observable for a non-finite strike, which cannot occur on this path
// (a strike is parsed out of the OSI symbol and re-checked against the quote
// row), and where it would occur it collapses two keys into one, which is
// fail-closed: the reconciliation's `quote_index` rejects a duplicate key.
struct ListedQuoteKey {
  std::string raw_symbol{};
  std::int64_t expiry_ts_ns{0};
  double strike{0.0};
  Side side{Side::Call};

  [[nodiscard]] auto operator<=>(const ListedQuoteKey &) const = default;
};

[[nodiscard]] inline ListedQuoteKey quote_key_of(const ListedScheduleLeg &leg) {
  return ListedQuoteKey{leg.raw_symbol, leg.expiry_ts_ns, leg.strike, leg.side};
}

[[nodiscard]] inline ListedQuoteKey quote_key_of(const ListedOptionQuote &quote) {
  return ListedQuoteKey{quote.raw_symbol, quote.expiry_ts_ns, quote.strike, quote.side};
}

} // namespace atx::vol
