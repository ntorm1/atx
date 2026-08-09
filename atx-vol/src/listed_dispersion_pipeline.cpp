#include "atx/vol/research/listed_dispersion_pipeline.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp" // ATX_TRY, Err, Ok, ErrorCode
#include "atx/core/hash.hpp"  // atx::core::hash_bytes
#include "atx/vol/detail/log_emit.hpp"
#include "atx/vol/contract_projection.hpp" // OptionProjectionSpec, ProjectedStrikeSpec, ProjectedOption
#include "atx/vol/dispersion.hpp"         // DispersionUniverse, DispersionBook, MissingNameSpec, resolve_universe_uids
#include "atx/vol/research/dispersion_workflow.hpp" // all_symbols, universe_at, UniverseRow, RunSpec
#include "atx/vol/historical_projection.hpp" // RelativeOptionPosition, PreparedHistoricalProjection, projected_historical_var
#include "atx/vol/opra_batch.hpp"        // OpraBatchResult, OpraBatchEntry, load_opra_daterange
#include "atx/vol/portfolio_pricer.hpp"  // kNsPerYear, Position
#include "atx/vol/priced_surface.hpp"    // PricedSurface, FullGreekSeed
#include "atx/vol/research/run_diagnostics.hpp"   // PhaseTimer (optional build-schedule phase timing, T9/O4)

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
  // v2: the four fields no consumer ever read (admission rule,
  // core_min_names_per_roll, query_route, occ_ess_authority) were removed from the
  // policy, so the key version is bumped — a v1 and a v2 fingerprint are not
  // comparable. The fingerprint is not persisted anywhere (the corpus
  // `policy_fingerprint` the CLI stamps is an unrelated literal hash), so the bump
  // moves no stored bytes.
  key.append("listed-dispersion-methodology-v2|");
  append_u64(min_names_entry);
  append_u64(core_min_dates);
  append_u64(core_min_rolls);

  const std::uint64_t hash = atx::core::hash_bytes(key.data(), key.size());
  return hash == 0u ? 1u : hash;
}

Result<std::vector<ListedOptionQuote>>
listed_quotes_for_date(const RunSpec &spec, const ListedDefinitionTable &definitions,
                       std::span<const std::string> symbols, std::string_view date,
                       std::span<const ListedQuoteKey> wanted) {
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
                                    definitions, MissingDefinitionPolicy::SkipUnlisted, wanted));
    quotes.insert(quotes.end(), std::make_move_iterator(joined.begin()),
                  std::make_move_iterator(joined.end()));
  }
  return Ok(std::move(quotes));
}

ListedForwardLookup make_listed_forward_lookup(const MarketSnapshot &snapshot) {
  return [&snapshot](const DispersionMember &member, std::int64_t expiry) -> Result<double> {
    // WS-ZC1: SurfaceSet::find resolves to a `SurfaceRef` handle (owned OR mapped-view
    // backed), not a `const PricedSurface *`. Only the DECLARED TYPE changes — the
    // self-proxy `operator->` and the nullptr comparison keep their exact syntax.
    const SurfaceRef surface = snapshot.find(member.uid);
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
    const SurfaceRef surface = snapshot.find(uid); // WS-ZC1 handle, see above
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
  // (value-identical, L9). NO other floor is applied here — in particular no
  // names-per-roll floor, which the example's build path never applied either;
  // enforcing one here would be a new, behavior-changing gate. The 40-name floor
  // that does exist belongs to RunVerifyOptions, at archive-verify time.
  if (schedule.rolls.empty() ||
      (spec.core_mode && schedule.rolls.size() < method.core_min_rolls)) {
    return Err(ErrorCode::Unavailable,
               "schedule does not satisfy entry/three-roll acceptance gate");
  }
  return Ok();
}

ListedDispersionSelectionConfig listed_selection_config_from(const ListedScheduleSpec &spec) {
  // REV-FIXTAIL I-A. The first four are the verbatim assignments the selection
  // loop made inline; `quality` is the fix. Everything not assigned keeps
  // `ListedDispersionSelectionConfig`'s own default (`required_multiplier`), so
  // a default spec reproduces the pre-fix config field for field.
  ListedDispersionSelectionConfig selection_config;
  selection_config.target_dte_days = spec.target_dte_days;
  selection_config.min_dte_days = spec.min_dte_days;
  selection_config.max_dte_days = spec.max_dte_days;
  selection_config.min_names = spec.min_names;
  selection_config.quality = spec.quality;
  return selection_config;
}

Result<ListedDispersionSchedule>
build_listed_dispersion_schedule_audited(
    const Clock &clock, const ListedScheduleSpec &spec,
    const ListedDispersionMethodology &method, std::span<const UniverseRow> universe_rows,
    const ListedDefinitionTable &definitions, const RunSpec &quote_source, PhaseTimer *timer,
    const ListedQuoteRejectSink &quote_reject_sink) {
  // Verbatim lift of build_schedule_command's selection loop
  // (spy_dispersion_backtest.cpp:446-535). The trade_schedule / section writes stay
  // in the CLI (T9); this function owns the economics that produce the
  // ListedDispersionSchedule. `timer` (optional, T9/O4) charges the `selection` /
  // `quote_join` phases exactly as the example measured them inline, so the CLI's
  // build-schedule diagnostics keep their pre-lift per-phase granularity; a null
  // timer skips the charges and is economically identical.
  const std::vector<std::string> symbols = all_symbols(universe_rows, quote_source.index_symbol);
  ListedDispersionSchedule schedule;
  std::int64_t active_expiry = 0;
  for (const SnapshotRef &ref : clock.refs()) {
    // selection: snapshot load + universe resolve. count=1 is charged once per
    // evaluated roll date (deferrals included); DTE-skip dates below charge only
    // their load time, with no evaluation count.
    const auto sel_start = PhaseTimer::now();
    ATX_TRY(MarketSnapshot snapshot, MarketSnapshot::load(ref.archive_path));
    const double active_dte =
        active_expiry == 0
            ? 0.0
            : static_cast<double>(active_expiry - snapshot.ts_ns()) / kListedNsPerDay;
    if (active_expiry != 0 && active_dte > spec.roll_dte_days) {
      if (timer) {
        timer->add("selection", sel_start);
      }
      continue;
    }
    ATX_TRY(DispersionUniverse authored,
            universe_at(universe_rows, ref.date, quote_source.index_symbol));
    MissingNameSpec missing{MissingNamePolicy::DropRenormalize, spec.min_names};
    ATX_TRY(
        ResolvedUniverse resolved,
        resolve_universe_uids(
            authored, [&](std::string_view symbol) { return snapshot.uid_of(symbol); }, missing));
    if (timer) {
      timer->add("selection", sel_start, 1u);
    }

    // quote_join: the OPRA parquet join re-marking the roll-date universe.
    const auto join_start = PhaseTimer::now();
    ATX_TRY(std::vector<ListedOptionQuote> quotes,
            listed_quotes_for_date(quote_source, definitions, symbols, ref.date));
    if (timer) {
      timer->add("quote_join", join_start, 1u);
    }

    const auto eval_start = PhaseTimer::now();
    const ListedDispersionSelectionConfig selection_config = listed_selection_config_from(spec);
    const ListedForwardLookup forward = make_listed_forward_lookup(snapshot);
    ListedQuoteRejectCounts attempted_rejects{};
    const auto selected =
        select_listed_dispersion(ref.date, snapshot.ts_ns(), resolved.universe, quotes, forward,
                                 selection_config, &attempted_rejects);
    if (timer) {
      timer->add("selection", eval_start);
    }
    if (quote_reject_sink) {
      quote_reject_sink(ref.date, selected.has_value(),
                        selected ? selected->quote_rejects : attempted_rejects);
    }
    if (!selected) {
      if (active_expiry == 0) {
        continue;
      }
      detail::log_emitf(LogLevel::Warn, LogStream::Stderr, "roll deferred on %s: %s",
                        ref.date.c_str(), selected.error().to_string().c_str());
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
      detail::log_emitf(LogLevel::Warn, LogStream::Stderr,
                        "roll deferred on %s: weight coverage %.6f", ref.date.c_str(), coverage);
      continue;
    }
    const auto build_start = PhaseTimer::now();
    ListedScheduleBuildConfig build;
    build.gross_index_vega_target_per_vol_point = spec.gross_index_vega;
    build.cohort = static_cast<std::uint32_t>(schedule.rolls.size() + 1u);
    ATX_TRY(const std::uint64_t archive_fingerprint, hash_archive_file(ref.archive_path));
    build.surface_fingerprint = archive_fingerprint;
    ATX_TRY(ListedScheduleRoll roll,
            build_listed_dispersion_roll(*selected, snapshot.set(), build));
    active_expiry = roll.expiry_ts_ns;
    schedule.rolls.push_back(std::move(roll));
    if (timer) {
      timer->add("selection", build_start);
    }
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

Result<ListedDispersionSchedule>
build_listed_dispersion_schedule(const Clock &clock, const ListedScheduleSpec &spec,
                                 const ListedDispersionMethodology &method,
                                 std::span<const UniverseRow> universe_rows,
                                 const ListedDefinitionTable &definitions,
                                 const RunSpec &quote_source, PhaseTimer *timer) {
  return build_listed_dispersion_schedule_audited(clock, spec, method, universe_rows, definitions,
                                                  quote_source, timer, {});
}

Result<ListedDispersionSchedule> project_listed_schedule(const ListedDispersionSchedule &listed,
                                                         const ListedArchiveLookup &archives,
                                                         const ProjectionConfig &cfg) {
  // Verbatim lift of project_schedule_command's cold reprice
  // (spy_dispersion_backtest.cpp:700-825). Cold certified economics on both sides,
  // matching the run-projected-backtest --execution cold replay route (RunConfig
  // default analytic AL greeks + ColdReference), so the persisted schedule marks equal
  // the live cold seed marks that replay recomputes. The knobs live in
  // ProjectionConfig — the SINGLE asserted parity constant BOTH cold routes read (I1),
  // not two hand-maintained copies that could drift.
  const bool analytic = cfg.analytic;
  const QueryExecution execution = cfg.execution;

  ListedDispersionSchedule projected;
  projected.rolls.reserve(listed.rolls.size());
  for (const ListedScheduleRoll &roll : listed.rolls) {
    // Resolve (load) the per-roll snapshot via the archive lookup, replacing the
    // example's archive_of map + inline MarketSnapshot::load. A missing archive is
    // the lookup's error (propagated); a null Ok is guarded explicitly to preserve
    // the example's "no qualified archive for roll date" NotFound semantics.
    ATX_TRY(const MarketSnapshot *snapshot, archives(roll.roll_date));
    if (snapshot == nullptr) {
      return Err(ErrorCode::NotFound,
                 "project-schedule: no qualified archive for roll date " + roll.roll_date);
    }
    if (snapshot->ts_ns() != roll.valuation_ts_ns) {
      return Err(ErrorCode::InvalidArgument,
                 "project-schedule: archive valuation timestamp differs from roll");
    }
    const double residual_T =
        static_cast<double>(roll.expiry_ts_ns - roll.valuation_ts_ns) / kNsPerYear;
    if (!(residual_T > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "project-schedule: nonpositive residual tenor");
    }
    if (roll.legs.size() != 2u * (1u + roll.n_names) || roll.legs.size() < 2u) {
      return Err(ErrorCode::InvalidArgument, "project-schedule: malformed frozen roll");
    }

    // Cold per-share greeks at (uid, projected strike, residual T, side) for sizing.
    // This is exactly make_listed_risk_lookup (T1) — the SAME seam the projected-
    // backtest replay recomputes marks through, so the two routes share one cold code
    // path keyed on ProjectionConfig (I1), not two hand-copied lambdas.
    const ListedRiskLookup cold_lookup =
        make_listed_risk_lookup(*snapshot, residual_T, analytic, execution);

    // Rebuild one member straddle from its frozen call/put legs, replacing the listed
    // strike with the surface ATM forward at residual T. forward_at is the same accessor
    // the synthetic dispersion route (resolve_leg / resolve_atm_iv) uses for its
    // ATM-forward strike. The synthetic raw quote is priced at the cold model value
    // (zero synthetic spread — there is no listed market at the interpolated strike);
    // raw_symbol / instrument_id / source_fingerprint retain the listed contract each
    // projected straddle idealizes (provenance + a unique per-leg source key).
    const auto make_straddle =
        [&](const ListedScheduleLeg &call_leg,
            const ListedScheduleLeg &put_leg) -> Result<ListedStraddle> {
      const SurfaceRef surface = snapshot->find(call_leg.uid); // WS-ZC1 handle, see above
      if (surface == nullptr) {
        return Err(ErrorCode::NotFound, "project-schedule: projected surface unavailable");
      }
      const double K = surface->forward_at(residual_T);
      if (!(K > 0.0)) {
        return Err(ErrorCode::Unavailable, "project-schedule: no ATM forward at residual tenor");
      }
      const auto make_quote = [&](const ListedScheduleLeg &leg,
                                  Side side) -> Result<ListedOptionQuote> {
        ATX_TRY(FullGreekSeed seed,
                surface->full_greek_seed(K, residual_T, side, analytic, execution));
        ListedOptionQuote quote;
        quote.trade_date = roll.roll_date;
        quote.symbol = leg.symbol;
        quote.instrument_id = leg.instrument_id;
        quote.raw_symbol = leg.raw_symbol;
        quote.expiry_ts_ns = leg.expiry_ts_ns;
        quote.strike = K;
        quote.side = side;
        quote.bid = seed.greeks().price;
        quote.ask = seed.greeks().price;
        quote.quote_ts_ns = roll.valuation_ts_ns;
        quote.multiplier = leg.multiplier;
        quote.standard_monthly = true;
        quote.standard_deliverable = true;
        quote.source_fingerprint = leg.source_fingerprint;
        return Ok(std::move(quote));
      };
      ListedStraddle straddle;
      straddle.symbol = call_leg.symbol;
      straddle.uid = call_leg.uid;
      straddle.expiry_ts_ns = call_leg.expiry_ts_ns;
      straddle.strike = K;
      ATX_TRY(straddle.call, make_quote(call_leg, Side::Call));
      ATX_TRY(straddle.put, make_quote(put_leg, Side::Put));
      straddle.raw_weight = call_leg.normalized_weight;
      straddle.normalized_weight = call_leg.normalized_weight;
      return Ok(std::move(straddle));
    };

    // Frozen roll legs are call/put pairs, index pair first.
    ListedDispersionSelection selection;
    selection.trade_date = roll.roll_date;
    selection.valuation_ts_ns = roll.valuation_ts_ns;
    selection.expiry_ts_ns = roll.expiry_ts_ns;
    selection.dte_days =
        static_cast<double>(roll.expiry_ts_ns - roll.valuation_ts_ns) / kListedNsPerDay;
    ATX_TRY(selection.index, make_straddle(roll.legs[0], roll.legs[1]));
    selection.names.reserve(roll.n_names);
    for (std::size_t i = 2u; i + 1u < roll.legs.size(); i += 2u) {
      ATX_TRY(ListedStraddle name, make_straddle(roll.legs[i], roll.legs[i + 1u]));
      selection.names.push_back(std::move(name));
    }

    ListedScheduleBuildConfig build_cfg;
    build_cfg.gross_index_vega_target_per_vol_point = roll.gross_index_vega_target_per_vol_point;
    build_cfg.side = DispersionSide::ShortIndexLongNames;
    build_cfg.cohort = roll.cohort;
    build_cfg.surface_fingerprint = roll.legs.front().surface_fingerprint;
    ATX_TRY(ListedScheduleRoll projected_roll,
            build_listed_dispersion_roll(selection, cold_lookup, build_cfg));
    detail::log_emitf(LogLevel::Info, LogStream::Stdout,
                      "  roll %u %s: net_vega=%.10g gross_vega=%.10g index_K=%.6f (listed %.6f)",
                      projected_roll.cohort, projected_roll.roll_date.c_str(),
                      projected_roll.net_vega_per_vol_point,
                      projected_roll.gross_vega_per_vol_point,
                      projected_roll.legs.front().strike, roll.legs.front().strike);
    projected.rolls.push_back(std::move(projected_roll));
  }

  // The CLI keeps projected_schedule.tsv / section / diagnostics writes (T9); the
  // pipeline owns the economics, including the shared validator (net vega ~ 0,
  // gross = 2x target) the persisted schedule must pass.
  ATX_TRY_VOID(validate_listed_dispersion_schedule(projected));
  return Ok(std::move(projected));
}

double listed_mark_divergence_bps(const double schedule_mark, const double live_mark) noexcept {
  // Verbatim arithmetic from the example's `collect_mark_divergence_replay`: the
  // ratio is taken against |schedule_mark|, and a zero frozen mark collapses the
  // metric to 0.0 rather than reporting an infinite relative error (finding L2).
  const double diff = live_mark - schedule_mark;
  const double denom = std::abs(schedule_mark);
  return denom > 0.0 ? std::abs(diff) / denom * 1.0e4 : 0.0;
}

StepObserver make_mark_divergence_observer(const ListedDispersionSchedule &schedule,
                                           std::vector<ListedMarkDivergenceRow> &out) {
  // Both captures are borrows, documented at the declaration: the returned observer
  // is handed to one run_backtest call that both must outlive.
  return [&schedule, &out](const StepEvent &event) -> Status {
    // The rows need the listed strategy's per-step divergence record, which is not on
    // IStrategy. Downcast rather than widen the (documented, ABI-fragile) IStrategy
    // vtable for a single consumer; a foreign strategy is an InvalidArgument, never a
    // silent no-op, because a run whose observer quietly observed nothing is exactly
    // the "dropped observation the caller believes it made" failure class.
    const auto *strategy = dynamic_cast<const ListedDispersionStrategy *>(&event.strategy);
    if (strategy == nullptr) {
      return Err(ErrorCode::InvalidArgument,
                 "mark divergence observer: strategy is not a ListedDispersionStrategy");
    }
    const std::vector<MarkDivergence> &divergences = strategy->last_mark_divergences();
    if (divergences.empty()) {
      return Ok();
    }
    // Divergences are populated only on a roll step; the roll that just fired owns
    // the legs carrying each contract's symbol/raw_symbol. A non-empty record implies
    // on_step returned Ok, so the cursor has already advanced past that roll and is
    // at least 1 — but `schedule` here is an INDEPENDENT input and is never the
    // strategy's own object (create() stores a copy), so the bound is checked against
    // the observed schedule rather than assumed from the cursor. Indexing a schedule
    // that is not value-equal to the strategy's would be undefined behavior; the shadow
    // replay this observer replaces could not reach that state, because its strategy
    // was constructed from the very schedule its loop indexed.
    const std::size_t roll_index = strategy->next_roll_index();
    if (roll_index == 0u || roll_index > schedule.rolls.size()) {
      return Err(ErrorCode::InvalidArgument,
                 "mark divergence observer: roll cursor outside the observed schedule");
    }
    const ListedScheduleRoll &roll = schedule.rolls[roll_index - 1u];
    // Fail-closed cross-check in the codebase's existing belt-and-braces style:
    // ListedDispersionStrategy::on_step already errors unless the stepped snapshot's
    // valuation timestamp equals the roll's, so this can only fire when the observer
    // was handed a schedule that does not belong to the observed run. It is also what
    // makes StepEvent::snapshot load-bearing here rather than decorative.
    if (event.snapshot.ts_ns() != roll.valuation_ts_ns) {
      return Err(ErrorCode::InvalidArgument,
                 "mark divergence observer: step snapshot is not the roll valuation date");
    }
    for (const MarkDivergence &divergence : divergences) {
      const ListedScheduleLeg *matched = nullptr;
      for (const ListedScheduleLeg &leg : roll.legs) {
        if (leg.uid == divergence.uid && leg.strike == divergence.strike &&
            leg.expiry_ts_ns == divergence.expiry_ts_ns && leg.side == divergence.side) {
          matched = &leg;
          break;
        }
      }
      if (matched == nullptr) {
        return Err(ErrorCode::NotFound, "mark divergence leg not found in roll");
      }
      ListedMarkDivergenceRow row;
      row.date = event.ref.date;
      row.symbol = matched->symbol;
      row.raw_symbol = matched->raw_symbol;
      row.strike = divergence.strike;
      row.expiry_ts_ns = divergence.expiry_ts_ns;
      row.side = divergence.side;
      row.schedule_mark = divergence.schedule_mark;
      row.live_mark = divergence.live_mark;
      row.diff = divergence.live_mark - divergence.schedule_mark;
      row.abs_diff_bps_of_mark =
          listed_mark_divergence_bps(divergence.schedule_mark, divergence.live_mark);
      out.push_back(std::move(row));
    }
    return Ok();
  };
}

Result<DispersionBookVar>
dispersion_book_var(const DispersionBook &book, const ProjectedMaturitySpec &maturity,
                    std::size_t n_scenarios, std::span<const double> confidences,
                    const DispersionProjectionEvaluator &evaluate,
                    const HistoricalProjectionConfig &cfg) {
  // Verbatim lift of run_projected_var_command's book -> OptionProjectionSpec
  // synthesis + PreparedHistoricalProjection::evaluate_into + projected_historical_var
  // per confidence (spy_dispersion_backtest.cpp:1119-1194), minus the loose-TSV
  // emission the CLI keeps (T9). Each book position becomes a relative template with
  // the same uid / side / multiplier / qty, an ATM-forward strike, and the caller's
  // relative `maturity`; the templates re-project onto every scenario, then the loss
  // quantile splits per requested confidence. No vega x100 here: the book handed in
  // is already sized. (E1 abolished the upstream per-vol-point -> per-unit-vol
  // boundary outright; its constant was deleted as dead in the C-2 follow-up.)
  std::vector<RelativeOptionPosition> relative_positions;
  relative_positions.reserve(book.positions.size());
  for (const Position &position : book.positions) {
    OptionProjectionSpec option;
    option.uid = position.contract.uid;
    option.maturity = maturity;
    option.strike = ProjectedStrikeSpec::atm_forward();
    option.side = position.contract.side;
    option.multiplier = position.multiplier;
    relative_positions.push_back({option, position.qty});
  }

  ATX_TRY(PreparedHistoricalProjection prepared,
          PreparedHistoricalProjection::create(relative_positions));

  DispersionBookVar result;
  result.n_positions = relative_positions.size();
  if (result.n_positions != 0u &&
      n_scenarios > std::numeric_limits<std::size_t>::max() / result.n_positions) {
    return Err(ErrorCode::InvalidArgument, "projected VaR: scenario output size overflows");
  }
  // Surfaced for the CLI's projected_var.tsv provenance column (prepared_fingerprint),
  // so the caller need not rebuild the projection just to echo its identity hash.
  result.prepared_fingerprint = prepared.fingerprint();
  result.frames.assign(n_scenarios, HistoricalProjectionFrame{});
  result.legs.assign(n_scenarios * relative_positions.size(), ProjectedOption{});
  if (!evaluate) {
    return Err(ErrorCode::InvalidArgument, "projected VaR: missing scenario evaluator");
  }
  ATX_TRY_VOID(evaluate(prepared, result.frames, result.legs, cfg));

  // Post-projection gate (spy_dispersion_backtest.cpp:1175-1178): any incomplete
  // scenario aborts — a VaR over a partially-failed frame set is not meaningful. On
  // this path no risk summary is produced (the CLI errors before writing its summary).
  for (const HistoricalProjectionFrame &frame : result.frames) {
    if (frame.n_failed != 0u) {
      return Err(ErrorCode::Unavailable, "projected VaR: incomplete scenario projection");
    }
  }

  // One risk summary per confidence, reference = the last scenario's value
  // (spy_dispersion_backtest.cpp:1186-1188). frames is non-empty on any successful
  // path (an empty scenario span yields no frames, so projected_historical_var below
  // returns Err — the reference guard only avoids UB, it never feeds a real result).
  result.risks.reserve(confidences.size());
  const double reference_value = result.frames.empty() ? 0.0 : result.frames.back().value;
  for (const double confidence : confidences) {
    ATX_TRY(ProjectedHistoricalVar risk,
            projected_historical_var(result.frames, reference_value, confidence));
    result.risks.push_back(risk);
  }
  return Ok(std::move(result));
}

Result<DispersionBookVar>
dispersion_book_var(const DispersionBook &book, const ProjectedMaturitySpec &maturity,
                    std::span<const HistoricalProjectionScenario> scenarios,
                    std::span<const double> confidences, const HistoricalProjectionConfig &cfg) {
  return dispersion_book_var(
      book, maturity, scenarios.size(), confidences,
      [scenarios](const PreparedHistoricalProjection &prepared,
                  std::span<HistoricalProjectionFrame> frames, std::span<ProjectedOption> legs,
                  const HistoricalProjectionConfig &config) {
        // I2: this overload feeds projected_historical_var below, so force
        // the same cold-confirmed marks VarEvaluationConfig defaults to.
        return prepared.evaluate_into(scenarios, frames, legs, config, QueryExecution::ColdReference);
      },
      cfg);
}

} // namespace atx::vol
