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

[[nodiscard]] CandidateResult find_straddle(const DispersionMember &member,
                                            std::int64_t expiry_ts_ns,
                                            const std::vector<ListedOptionQuote> &quotes,
                                            const ListedForwardLookup &forward_lookup,
                                            double required_multiplier) {
  std::map<double, PairAtStrike> pairs;
  bool saw_expiry = false;
  for (const ListedOptionQuote &q : quotes) {
    if (q.symbol != member.symbol || q.expiry_ts_ns != expiry_ts_ns) {
      continue;
    }
    saw_expiry = true;
    if (!q.standard_monthly || !q.standard_deliverable || q.multiplier != required_multiplier ||
        !is_valid_listed_quote(q)) {
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
    return CandidateResult{std::nullopt, ListedDropReason::NoValidStraddle,
                           "no standard 100-share two-sided call/put pair"};
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

bool is_valid_listed_quote(const ListedOptionQuote &quote) noexcept {
  return std::isfinite(quote.bid) && quote.bid >= 0.0 && std::isfinite(quote.ask) &&
         quote.ask > 0.0 && quote.ask >= quote.bid;
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
    if (q.symbol != universe.index.symbol || !q.standard_monthly || !q.standard_deliverable ||
        q.multiplier != config.required_multiplier || !is_valid_listed_quote(q)) {
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
    CandidateResult index = find_straddle(universe.index, expiry, sorted_quotes, forward_lookup,
                                          config.required_multiplier);
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
          find_straddle(member, expiry, sorted_quotes, forward_lookup, config.required_multiplier);
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
    return Ok(std::move(out));
  }

  return Err(ErrorCode::Unavailable,
             "select_listed_dispersion: no common expiry has a valid index and minimum basket");
}

} // namespace atx::vol
