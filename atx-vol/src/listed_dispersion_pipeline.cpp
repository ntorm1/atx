#include "atx/vol/listed_dispersion_pipeline.hpp"

#include <cmath>
#include <cstdio>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp" // ATX_TRY, Err, Ok, ErrorCode
#include "atx/core/hash.hpp"  // atx::core::hash_bytes
#include "atx/vol/opra_batch.hpp"        // OpraBatchResult, OpraBatchEntry, load_opra_daterange
#include "atx/vol/portfolio_pricer.hpp"  // kNsPerYear
#include "atx/vol/priced_surface.hpp"    // PricedSurface, FullGreekSeed

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

std::uint64_t ListedDispersionMethodology::policy_fingerprint() const {
  // Compose a deterministic byte key over every policy field, then hash it. A
  // string key keeps the fingerprint padding-free (no struct-layout dependence)
  // and makes every field independently contribute — a single-field change moves
  // the key, hence the hash.
  std::string key;
  key.reserve(256);
  const auto append_u64 = [&key](std::uint64_t value) {
    key.append(std::to_string(value));
    key.push_back('|');
  };
  const auto append_dbl = [&key](double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    key.append(buffer);
    key.push_back('|');
  };
  const auto append_opt = [&](const std::optional<double> &value) {
    if (value) {
      append_dbl(*value);
    } else {
      key.append("na|");
    }
  };

  key.append("listed-dispersion-methodology-v1|");
  append_u64(admission.min_quotes);
  append_u64(admission.min_slices);
  append_u64(admission.min_holdout);
  append_opt(admission.min_fit_in_band);
  append_opt(admission.min_oos_in_band);
  append_opt(admission.min_oos_vega_weighted);
  append_opt(admission.max_mean_vol_rmse);
  append_opt(admission.max_mean_reduced_chi2);
  append_u64(admission.require_calendar_arb_free ? 1u : 0u);
  append_dbl(admission.calendar_abs_k);
  append_u64(admission.require_source_provenance ? 1u : 0u);
  append_u64(min_names_entry);
  append_u64(core_min_dates);
  append_u64(core_min_rolls);
  append_u64(core_min_names_per_roll);
  append_u64(static_cast<std::uint64_t>(query_route));
  append_u64(occ_ess_authority ? 1u : 0u);

  const std::uint64_t hash = atx::core::hash_bytes(key.data(), key.size());
  return hash == 0u ? 1u : hash;
}

Result<std::vector<ListedOptionQuote>>
listed_quotes_for_date(const RunSpec &spec, const ListedDefinitionTable &definitions,
                       std::span<const std::string> symbols, std::string_view date) {
  ATX_TRY(OpraBatchResult batch, load_opra_daterange(batch_spec(spec, symbols, date, date)));
  std::vector<ListedOptionQuote> quotes;
  for (const OpraBatchEntry &entry : batch.entries) {
    if (!entry.panel) {
      continue;
    }
    // SkipUnlisted: both consumers of this helper (build-schedule roll-date
    // selection and run-backtest reconciliation) only ever act on defined,
    // standard-monthly 21-60 DTE contracts. A quote with no point-in-time
    // definition is an intraday-listed contract outside that universe on its
    // listing day; dropping it is a no-op on every date where the join already
    // succeeds (the skip can only fire where the strict Error policy would have
    // hard-failed), so currently-passing runs stay bit-for-bit unchanged.
    ATX_TRY(std::vector<ListedOptionQuote> joined,
            listed_quotes_from_opra(date, entry.panel->frame.snapshot_ts_ns, *entry.panel,
                                    definitions, MissingDefinitionPolicy::SkipUnlisted));
    quotes.insert(quotes.end(), std::make_move_iterator(joined.begin()),
                  std::make_move_iterator(joined.end()));
  }
  return Ok(std::move(quotes));
}

ListedForwardLookup make_listed_forward_lookup(const MarketSnapshot &snapshot) {
  return [&snapshot](const DispersionMember &member, std::int64_t expiry) -> Result<double> {
    const PricedSurface *surface = snapshot.find(member.uid);
    if (surface == nullptr) {
      return Err(ErrorCode::NotFound, "surface missing");
    }
    const double term = static_cast<double>(expiry - snapshot.ts_ns()) / kNsPerYear;
    const double value = surface->forward_at(term);
    return std::isfinite(value) && value > 0.0 ? Ok(value)
                                               : Err(ErrorCode::Unavailable, "forward unavailable");
  };
}

ListedRiskLookup make_listed_risk_lookup(const MarketSnapshot &snapshot, double residual_T,
                                         bool analytic, QueryExecution execution) {
  return [&snapshot, residual_T, analytic, execution](
             std::uint32_t uid, const ListedOptionQuote &quote) -> Result<ListedOptionRisk> {
    const PricedSurface *surface = snapshot.find(uid);
    if (surface == nullptr) {
      return Err(ErrorCode::NotFound, "project-schedule: projected surface unavailable");
    }
    ATX_TRY(FullGreekSeed seed,
            surface->full_greek_seed(quote.strike, residual_T, quote.side, analytic, execution));
    return Ok(ListedOptionRisk{seed.greeks().price, seed.greeks().delta, seed.greeks().vega});
  };
}

} // namespace atx::vol
