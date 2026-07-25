#include "atx/vol/listed_dispersion.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

struct PairAtStrike {
  const ListedOptionQuote *call{nullptr};
  const ListedOptionQuote *put{nullptr};
  bool ambiguous{false};
};

struct CandidateResult {
  std::optional<ListedStraddle> straddle{};
  ListedDropReason reason{ListedDropReason::NoQuotesForExpiry};
  std::string detail{};
};

[[nodiscard]] bool finite_positive(double x) noexcept { return std::isfinite(x) && x > 0.0; }

[[nodiscard]] Result<std::vector<ListedOptionQuote>>
validate_and_sort_quotes(std::string_view trade_date, std::int64_t valuation_ts_ns,
                         std::span<const ListedOptionQuote> quotes) {
  std::map<std::uint32_t, std::string> id_to_raw;
  std::map<std::string, std::tuple<std::string, std::int64_t, double, Side, double>>
      raw_to_economics;
  std::vector<ListedOptionQuote> sorted{quotes.begin(), quotes.end()};

  for (const ListedOptionQuote &q : sorted) {
    if (q.trade_date != trade_date) {
      return Err(ErrorCode::InvalidArgument, "select_listed_dispersion: quote trade_date mismatch");
    }
    if (q.symbol.empty() || q.instrument_id == 0 || q.raw_symbol.empty() ||
        q.expiry_ts_ns <= valuation_ts_ns || q.quote_ts_ns > valuation_ts_ns ||
        q.quote_ts_ns <= 0 || !finite_positive(q.strike) || !finite_positive(q.multiplier)) {
      return Err(ErrorCode::InvalidArgument,
                 "select_listed_dispersion: malformed or look-ahead quote");
    }

    const auto [id_it, id_inserted] = id_to_raw.emplace(q.instrument_id, q.raw_symbol);
    if (!id_inserted && id_it->second != q.raw_symbol) {
      return Err(ErrorCode::InvalidArgument,
                 "select_listed_dispersion: instrument id maps to multiple raw symbols");
    }

    const auto economics = std::tuple{q.symbol, q.expiry_ts_ns, q.strike, q.side, q.multiplier};
    const auto [raw_it, raw_inserted] = raw_to_economics.emplace(q.raw_symbol, economics);
    if (!raw_inserted && raw_it->second != economics) {
      return Err(ErrorCode::InvalidArgument,
                 "select_listed_dispersion: raw symbol maps to multiple contracts");
    }
  }

  std::sort(
      sorted.begin(), sorted.end(), [](const ListedOptionQuote &a, const ListedOptionQuote &b) {
        return std::tie(a.symbol, a.expiry_ts_ns, a.strike, a.side, a.instrument_id, a.raw_symbol) <
               std::tie(b.symbol, b.expiry_ts_ns, b.strike, b.side, b.instrument_id, b.raw_symbol);
      });

  for (std::size_t i = 1; i < sorted.size(); ++i) {
    const ListedOptionQuote &a = sorted[i - 1];
    const ListedOptionQuote &b = sorted[i];
    if (a.symbol == b.symbol && a.expiry_ts_ns == b.expiry_ts_ns && a.strike == b.strike &&
        a.side == b.side && a.instrument_id == b.instrument_id && a.raw_symbol == b.raw_symbol) {
      return Err(ErrorCode::InvalidArgument,
                 "select_listed_dispersion: duplicate contract observation");
    }
  }

  return Ok(std::move(sorted));
}

[[nodiscard]] Result<void> validate_universe(const DispersionUniverse &universe,
                                             const ListedDispersionSelectionConfig &cfg) {
  if (universe.index.symbol.empty() || universe.index.uid == 0 || universe.names.empty() ||
      cfg.min_names == 0 || cfg.min_names > universe.names.size() ||
      !finite_positive(cfg.target_dte_days) || !finite_positive(cfg.min_dte_days) ||
      !finite_positive(cfg.max_dte_days) || cfg.min_dte_days > cfg.target_dte_days ||
      cfg.target_dte_days > cfg.max_dte_days || !finite_positive(cfg.required_multiplier)) {
    return Err(ErrorCode::InvalidArgument,
               "select_listed_dispersion: invalid universe or selection config");
  }

  std::set<std::string> symbols;
  std::set<std::uint32_t> uids;
  symbols.insert(universe.index.symbol);
  uids.insert(universe.index.uid);
  for (const DispersionMember &member : universe.names) {
    if (member.symbol.empty() || member.uid == 0 || !finite_positive(member.weight) ||
        !symbols.insert(member.symbol).second || !uids.insert(member.uid).second) {
      return Err(ErrorCode::InvalidArgument,
                 "select_listed_dispersion: invalid or duplicate universe member");
    }
  }
  return Ok();
}

// F6: tally one rejected quote and remember the reason so the drop detail can
// NAME it instead of the generic "no valid pair".
void tally(ListedQuoteRejectCounts &counts, ListedQuoteReject reject) noexcept {
  switch (reject) {
  case ListedQuoteReject::NotTwoSided:
    ++counts.not_two_sided;
    return;
  case ListedQuoteReject::ZeroBid:
    ++counts.zero_bid;
    return;
  case ListedQuoteReject::Stale:
    ++counts.stale;
    return;
  case ListedQuoteReject::Locked:
    ++counts.locked;
    return;
  case ListedQuoteReject::None:
    return;
  }
}

[[nodiscard]] std::string reject_detail(const ListedQuoteRejectCounts &counts) {
  if (counts.total_dropped() == 0u && counts.locked == 0u) {
    return {};
  }
  std::string out = " (quote rejects:";
  const auto add = [&out](const char *name, std::uint32_t n) {
    if (n != 0u) {
      out += ' ';
      out += name;
      out += '=';
      out += std::to_string(n);
    }
  };
  add("NotTwoSided", counts.not_two_sided);
  add("ZeroBid", counts.zero_bid);
  add("Stale", counts.stale);
  add("Locked", counts.locked);
  add("NonStandard", counts.non_standard);
  out += ')';
  return out;
}

[[nodiscard]] CandidateResult find_straddle(const DispersionMember &member,
                                            std::int64_t expiry_ts_ns,
                                            std::int64_t valuation_ts_ns,
                                            const std::vector<ListedOptionQuote> &quotes,
                                            const ListedForwardLookup &forward_lookup,
                                            double required_multiplier,
                                            const ListedQuoteQualityConfig &quality,
                                            ListedQuoteRejectCounts &counts) {
  std::map<double, PairAtStrike> pairs;
  bool saw_expiry = false;
  ListedQuoteRejectCounts local{}; // this member's own tally, for the detail text
  for (const ListedOptionQuote &q : quotes) {
    if (q.symbol != member.symbol || q.expiry_ts_ns != expiry_ts_ns) {
      continue;
    }
    saw_expiry = true;
    if (!q.standard_monthly || !q.standard_deliverable || q.multiplier != required_multiplier) {
      ++counts.non_standard;
      ++local.non_standard;
      continue;
    }
    // A locked market is FLAGGED unconditionally — the count is the signal even
    // when the policy admits it — then the policy decides admission.
    if (std::isfinite(q.bid) && q.bid > 0.0 && q.ask == q.bid) {
      ++counts.locked;
      ++local.locked;
    }
    // The staleness gate is only meaningful when the source carries an
    // observation time independent of the valuation instant. When it does not
    // (the OPRA panel is snapshot-stamped) every age is exactly 0 and a `stale`
    // count of 0 would describe the FEED, not the market. Count that case so the
    // report can tell the two apart.
    if (quality.max_quote_age_ns > 0 && q.quote_ts_ns == valuation_ts_ns) {
      ++counts.stale_unevaluable;
      ++local.stale_unevaluable;
    }
    const ListedQuoteReject reject = classify_listed_quote(q, valuation_ts_ns, quality);
    if (reject != ListedQuoteReject::None) {
      if (reject != ListedQuoteReject::Locked) { // already flagged above
        tally(counts, reject);
        tally(local, reject);
      }
      continue;
    }
    PairAtStrike &pair = pairs[q.strike];
    const ListedOptionQuote **slot = q.side == Side::Call ? &pair.call : &pair.put;
    if (*slot != nullptr) {
      pair.ambiguous = true;
    } else {
      *slot = &q;
    }
  }

  if (!saw_expiry) {
    return CandidateResult{std::nullopt, ListedDropReason::NoQuotesForExpiry,
                           "no quotes for common expiry"};
  }

  const Result<double> forward = forward_lookup(member, expiry_ts_ns);
  if (!forward || !finite_positive(forward.value())) {
    return CandidateResult{std::nullopt, ListedDropReason::ForwardUnavailable,
                           forward ? "nonpositive forward" : forward.error().to_string()};
  }

  const PairAtStrike *best_pair = nullptr;
  double best_strike = 0.0;
  double best_distance = std::numeric_limits<double>::infinity();
  for (const auto &[strike, pair] : pairs) {
    if (pair.ambiguous || pair.call == nullptr || pair.put == nullptr) {
      continue;
    }
    const double distance = std::fabs(strike - forward.value());
    if (distance < best_distance || (distance == best_distance && strike < best_strike)) {
      best_pair = &pair;
      best_strike = strike;
      best_distance = distance;
    }
  }

  if (best_pair == nullptr) {
    // F6: name WHY, so a name dropped for stale or zero-bid quotes is
    // distinguishable from one that simply has no listed pair at this expiry.
    return CandidateResult{std::nullopt, ListedDropReason::NoValidStraddle,
                           "no standard 100-share two-sided call/put pair" + reject_detail(local)};
  }

  ListedStraddle out;
  out.symbol = member.symbol;
  out.uid = member.uid;
  out.expiry_ts_ns = expiry_ts_ns;
  out.strike = best_strike;
  out.call = *best_pair->call;
  out.put = *best_pair->put;
  out.raw_weight = member.weight;
  return CandidateResult{std::move(out), ListedDropReason::NoValidStraddle, {}};
}

} // namespace

const char *to_string(ListedDropReason reason) noexcept {
  switch (reason) {
  case ListedDropReason::NoQuotesForExpiry:
    return "NoQuotesForExpiry";
  case ListedDropReason::NoValidStraddle:
    return "NoValidStraddle";
  case ListedDropReason::ForwardUnavailable:
    return "ForwardUnavailable";
  }
  return "Unknown";
}

const char *to_string(ListedQuoteReject reject) noexcept {
  switch (reject) {
  case ListedQuoteReject::None:
    return "None";
  case ListedQuoteReject::NotTwoSided:
    return "NotTwoSided";
  case ListedQuoteReject::ZeroBid:
    return "ZeroBid";
  case ListedQuoteReject::Stale:
    return "Stale";
  case ListedQuoteReject::Locked:
    return "Locked";
  }
  return "Unknown";
}

bool is_valid_listed_quote(const ListedOptionQuote &quote) noexcept {
  // F6 (BT-P2-8): `quote.bid > 0.0`, not `>= 0.0`. A zero bid is not a market:
  // the mid collapses to ask/2 — a price nobody trades at — and there is no bid
  // to hit at all, so a straddle built on one cannot be exited.
  return std::isfinite(quote.bid) && quote.bid > 0.0 && std::isfinite(quote.ask) &&
         quote.ask > 0.0 && quote.ask >= quote.bid;
}

ListedQuoteReject classify_listed_quote(const ListedOptionQuote &quote,
                                        std::int64_t valuation_ts_ns,
                                        const ListedQuoteQualityConfig &quality) noexcept {
  if (!std::isfinite(quote.bid) || !std::isfinite(quote.ask) || !(quote.ask > 0.0) ||
      quote.ask < quote.bid) {
    return ListedQuoteReject::NotTwoSided;
  }
  // `min_bid` defaults to 0.0, so the default policy is exactly "a zero bid is
  // not a market"; a configured floor demands a real quoted bid on top.
  if (!(quote.bid > quality.min_bid) || !(quote.bid > 0.0)) {
    return ListedQuoteReject::ZeroBid;
  }
  if (quality.max_quote_age_ns > 0 && quote.quote_ts_ns > 0 &&
      valuation_ts_ns - quote.quote_ts_ns > quality.max_quote_age_ns) {
    return ListedQuoteReject::Stale;
  }
  if (quality.reject_locked && quote.ask == quote.bid) {
    return ListedQuoteReject::Locked;
  }
  return ListedQuoteReject::None;
}

Result<ListedDispersionSelection> select_listed_dispersion(
    std::string_view trade_date, std::int64_t valuation_ts_ns, const DispersionUniverse &universe,
    std::span<const ListedOptionQuote> quotes, const ListedForwardLookup &forward_lookup,
    const ListedDispersionSelectionConfig &config) {
  if (trade_date.empty() || valuation_ts_ns <= 0 || !forward_lookup) {
    return Err(ErrorCode::InvalidArgument,
               "select_listed_dispersion: invalid date, timestamp, or forward lookup");
  }
  ATX_TRY_VOID(validate_universe(universe, config));
  ATX_TRY(auto sorted_quotes, validate_and_sort_quotes(trade_date, valuation_ts_ns, quotes));

  std::vector<std::int64_t> expiries;
  for (const ListedOptionQuote &q : sorted_quotes) {
    // F6: an expiry may only be NOMINATED by an admissible index quote — a
    // stale or zero-bid row must not steer the whole basket onto its tenor.
    if (q.symbol != universe.index.symbol || !q.standard_monthly || !q.standard_deliverable ||
        q.multiplier != config.required_multiplier ||
        classify_listed_quote(q, valuation_ts_ns, config.quality) != ListedQuoteReject::None) {
      continue;
    }
    const double dte = static_cast<double>(q.expiry_ts_ns - valuation_ts_ns) / kListedNsPerDay;
    if (dte < config.min_dte_days || dte > config.max_dte_days) {
      continue;
    }
    expiries.push_back(q.expiry_ts_ns);
  }
  std::sort(expiries.begin(), expiries.end());
  expiries.erase(std::unique(expiries.begin(), expiries.end()), expiries.end());
  std::sort(expiries.begin(), expiries.end(), [&](std::int64_t a, std::int64_t b) {
    const double da = static_cast<double>(a - valuation_ts_ns) / kListedNsPerDay;
    const double db = static_cast<double>(b - valuation_ts_ns) / kListedNsPerDay;
    const double aa = std::fabs(da - config.target_dte_days);
    const double ab = std::fabs(db - config.target_dte_days);
    return aa < ab || (aa == ab && a < b);
  });

  for (const std::int64_t expiry : expiries) {
    // F6: the tally is per CANDIDATE expiry — it describes the expiry actually
    // chosen, not the union of every expiry tried and discarded.
    ListedQuoteRejectCounts rejects{};
    CandidateResult index =
        find_straddle(universe.index, expiry, valuation_ts_ns, sorted_quotes, forward_lookup,
                      config.required_multiplier, config.quality, rejects);
    if (!index.straddle.has_value()) {
      continue;
    }

    std::vector<ListedStraddle> names;
    std::vector<ListedDroppedName> dropped;
    names.reserve(universe.names.size());
    dropped.reserve(universe.names.size());
    double survivor_weight = 0.0;
    for (const DispersionMember &member : universe.names) {
      CandidateResult found =
          find_straddle(member, expiry, valuation_ts_ns, sorted_quotes, forward_lookup,
                        config.required_multiplier, config.quality, rejects);
      if (found.straddle.has_value()) {
        survivor_weight += member.weight;
        names.push_back(std::move(*found.straddle));
      } else {
        dropped.push_back(ListedDroppedName{member.symbol, found.reason, std::move(found.detail)});
      }
    }

    if (names.size() < config.min_names || !finite_positive(survivor_weight)) {
      continue;
    }
    for (ListedStraddle &name : names) {
      name.normalized_weight = name.raw_weight / survivor_weight;
    }

    ListedDispersionSelection out;
    out.trade_date = std::string{trade_date};
    out.valuation_ts_ns = valuation_ts_ns;
    out.expiry_ts_ns = expiry;
    out.dte_days = static_cast<double>(expiry - valuation_ts_ns) / kListedNsPerDay;
    out.index = std::move(*index.straddle);
    out.index.raw_weight = 0.0;
    out.index.normalized_weight = 0.0;
    out.names = std::move(names);
    out.dropped = std::move(dropped);
    out.quote_rejects = rejects;
    return Ok(std::move(out));
  }

  return Err(ErrorCode::Unavailable,
             "select_listed_dispersion: no common expiry has a valid index and minimum basket");
}

} // namespace atx::vol
