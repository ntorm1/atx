#include "atx/vol/listed_dispersion_pipeline.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp" // ATX_TRY, Err, Ok, ErrorCode
#include "atx/core/hash.hpp"  // atx::core::hash_bytes
#include "atx/vol/dispersion.hpp"         // DispersionUniverse, MissingNameSpec, resolve_universe_uids
#include "atx/vol/dispersion_workflow.hpp" // all_symbols, universe_at, UniverseRow, RunSpec
#include "atx/vol/opra_batch.hpp"        // OpraBatchResult, OpraBatchEntry, load_opra_daterange
#include "atx/vol/portfolio_pricer.hpp"  // kNsPerYear
#include "atx/vol/priced_surface.hpp"    // PricedSurface, FullGreekSeed

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Archive file fingerprint, byte-for-byte identical to the example's `hash_file`
// (spy_dispersion_backtest.cpp:82-102): read the whole archive as binary bytes and
// wyhash them, mapping a zero digest to 1 so the fingerprint is always nonzero.
// Preserved exactly so the lifted build path stamps the same `surface_fingerprint`
// the example did (pinned by the trade_schedule golden b640b3ab... at T10).
[[nodiscard]] Result<std::uint64_t> hash_archive_file(std::string_view path) {
  const std::string path_str(path);
  std::ifstream stream(path_str, std::ios::binary);
  if (!stream) {
    return Err(ErrorCode::NotFound, "cannot open " + path_str);
  }
  const std::string bytes((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>());
  if (!stream.good() && !stream.eof()) {
    return Err(ErrorCode::IoError, "cannot read " + path_str);
  }
  const std::uint64_t hash = atx::core::hash_bytes(bytes.data(), bytes.size());
  return Ok(hash == 0u ? 1u : hash);
}

} // namespace

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

Result<std::vector<ListedReconciliationSnapshot>>
assemble_reconciliation_snapshots(std::span<const ListedReconciliationSnapshot> full_timeline,
                                  const ListedDispersionSchedule &schedule) {
  // M1 fix. run-backtest hands the reconciler the FULL clock.refs() timeline; the
  // engine (and ListedDispersionStrategy::on_step) emits zero rows before the first
  // roll, so any leading warm-up / low-coverage session sits ahead of the first
  // roll date. The low-level reconcile_listed_dispersion hard-requires
  // snapshots.front().date == schedule.rolls.front().roll_date, so such a lead-in
  // aborts an otherwise-valid corpus. Trim those pre-roll sessions here so the
  // returned timeline starts exactly at the first roll date.
  if (schedule.rolls.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "assemble_reconciliation_snapshots: schedule has no rolls");
  }
  const std::string &first_roll_date = schedule.rolls.front().roll_date;
  std::size_t start = full_timeline.size();
  for (std::size_t index = 0; index < full_timeline.size(); ++index) {
    if (full_timeline[index].date == first_roll_date) {
      start = index;
      break;
    }
  }
  // The coupling is made explicit, not emergent: an absent first roll date is an
  // error, never a silent empty reconcile.
  if (start == full_timeline.size()) {
    return Err(ErrorCode::InvalidArgument,
               "assemble_reconciliation_snapshots: first roll date absent from timeline");
  }
  const std::span<const ListedReconciliationSnapshot> trimmed = full_timeline.subspan(start);
  return Ok(std::vector<ListedReconciliationSnapshot>(trimmed.begin(), trimmed.end()));
}

Result<ListedDispersionReconciliation>
reconcile_listed_schedule(const ListedDispersionSchedule &schedule,
                          std::span<const ListedReconciliationSnapshot> full_timeline,
                          const ListedReconciliationConfig &config) {
  ATX_TRY(std::vector<ListedReconciliationSnapshot> timeline,
          assemble_reconciliation_snapshots(full_timeline, schedule));
  return reconcile_listed_dispersion(schedule, timeline, config);
}

Status accept_listed_schedule(const ListedDispersionSchedule &schedule,
                              const ListedScheduleSpec &spec,
                              const ListedDispersionMethodology &method) {
  // Verbatim entry/three-roll acceptance gate (spy_dispersion_backtest.cpp:532-534),
  // with the loose literal `3u` replaced by the methodology's `core_min_rolls`
  // (value-identical, L9). NO other floor is applied here — `core_min_names_per_roll`
  // is deliberately NOT consulted (it is inert in the example's build path; enforcing
  // it here would be a new, behavior-changing gate).
  if (schedule.rolls.empty() ||
      (spec.core_mode && schedule.rolls.size() < method.core_min_rolls)) {
    return Err(ErrorCode::Unavailable,
               "schedule does not satisfy entry/three-roll acceptance gate");
  }
  return Ok();
}

Result<ListedDispersionSchedule>
build_listed_dispersion_schedule(const Clock &clock, const ListedScheduleSpec &spec,
                                 const ListedDispersionMethodology &method,
                                 std::span<const UniverseRow> universe_rows,
                                 const ListedDefinitionTable &definitions,
                                 const RunSpec &quote_source) {
  // Verbatim lift of build_schedule_command's selection loop
  // (spy_dispersion_backtest.cpp:446-535). Phase timing and the trade_schedule /
  // section writes stay in the CLI (T9); this function owns only the economics that
  // produce the ListedDispersionSchedule.
  const std::vector<std::string> symbols = all_symbols(universe_rows);
  ListedDispersionSchedule schedule;
  std::int64_t active_expiry = 0;
  for (const SnapshotRef &ref : clock.refs()) {
    ATX_TRY(MarketSnapshot snapshot, MarketSnapshot::load(ref.archive_path));
    const double active_dte =
        active_expiry == 0
            ? 0.0
            : static_cast<double>(active_expiry - snapshot.ts_ns()) / kListedNsPerDay;
    if (active_expiry != 0 && active_dte > spec.roll_dte_days) {
      continue;
    }
    ATX_TRY(DispersionUniverse authored, universe_at(universe_rows, ref.date));
    MissingNameSpec missing{MissingNamePolicy::DropRenormalize, spec.min_names};
    ATX_TRY(
        ResolvedUniverse resolved,
        resolve_universe_uids(
            authored, [&](std::string_view symbol) { return snapshot.uid_of(symbol); }, missing));

    ATX_TRY(std::vector<ListedOptionQuote> quotes,
            listed_quotes_for_date(quote_source, definitions, symbols, ref.date));

    ListedDispersionSelectionConfig selection_config;
    selection_config.target_dte_days = spec.target_dte_days;
    selection_config.min_dte_days = spec.min_dte_days;
    selection_config.max_dte_days = spec.max_dte_days;
    selection_config.min_names = spec.min_names;
    const ListedForwardLookup forward = make_listed_forward_lookup(snapshot);
    const auto selected = select_listed_dispersion(ref.date, snapshot.ts_ns(), resolved.universe,
                                                   quotes, forward, selection_config);
    if (!selected) {
      if (active_expiry == 0) {
        continue;
      }
      std::fprintf(stderr, "roll deferred on %s: %s\n", ref.date.c_str(),
                   selected.error().to_string().c_str());
      continue;
    }
    double requested_weight = 0.0;
    for (const DispersionMember &name : authored.names) {
      requested_weight += name.weight;
    }
    double traded_weight = 0.0;
    for (const ListedStraddle &name : selected->names) {
      traded_weight += name.raw_weight;
    }
    const double coverage = traded_weight / requested_weight;
    if (coverage < spec.min_weight_coverage) {
      if (active_expiry == 0) {
        continue;
      }
      std::fprintf(stderr, "roll deferred on %s: weight coverage %.6f\n", ref.date.c_str(),
                   coverage);
      continue;
    }
    ListedScheduleBuildConfig build;
    build.gross_index_vega_target_per_vol_point = spec.gross_index_vega;
    build.cohort = static_cast<std::uint32_t>(schedule.rolls.size() + 1u);
    ATX_TRY(const std::uint64_t archive_fingerprint, hash_archive_file(ref.archive_path));
    build.surface_fingerprint = archive_fingerprint;
    ATX_TRY(ListedScheduleRoll roll,
            build_listed_dispersion_roll(*selected, snapshot.set(), build));
    active_expiry = roll.expiry_ts_ns;
    schedule.rolls.push_back(std::move(roll));
  }

  // Entry/three-roll acceptance gate (extracted so it is unit-testable).
  ATX_TRY_VOID(accept_listed_schedule(schedule, spec, method));

  // M1 (design §3): make the clock/first-roll coupling explicit rather than
  // emergent. Every roll_date is a clock ref.date by construction (the loop
  // iterates clock.refs()), so this holds trivially today. Enforcing it here pins
  // the invariant run-backtest relies on: reconcile_listed_schedule, fed the full
  // clock.refs() timeline, trims the warm-up lead-in down to exactly
  // rolls.front().roll_date. `accept_listed_schedule` already rejected an empty
  // roll set, so rolls.front() is safe here.
  const std::string &first_roll_date = schedule.rolls.front().roll_date;
  bool clock_carries_first_roll = false;
  for (const SnapshotRef &ref : clock.refs()) {
    if (ref.date == first_roll_date) {
      clock_carries_first_roll = true;
      break;
    }
  }
  if (!clock_carries_first_roll) {
    return Err(ErrorCode::InvalidArgument,
               "build_listed_dispersion_schedule: clock does not contain first roll date");
  }
  return Ok(std::move(schedule));
}

} // namespace atx::vol
