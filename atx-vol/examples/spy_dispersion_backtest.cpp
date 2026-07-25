// CLI over the listed SPY-dispersion library seam (atx/vol/dispersion_run.hpp).
//
// Each subcommand is a process boundary; no fitter/session object crosses it.
// Most library workflow — corpus build, schedule build, listed + surface-only
// backtests, projected VaR, and the verify/reference-reconcile — lives in the
// library so it is unit-testable off the filesystem. The two projected-definition
// subcommands (project-schedule, run-projected-backtest) are still implemented in
// this translation unit pending their library-seam extraction (WS-F). The pinned
// admission thresholds that make the dispersion golden
// (final_nav = 24740.624124981368) reproduce byte-for-byte are DispersionCorpusPolicy
// library constants, not literals here.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"
#include "atx/vol/backtest.hpp"
#include "atx/vol/corpus.hpp"
#include "atx/vol/counters.hpp"
#include "atx/vol/dispersion.hpp"
#include "atx/vol/dispersion_backtest.hpp"
#include "atx/vol/dispersion_run.hpp"
#include "atx/vol/dispersion_workflow.hpp"
#include "atx/vol/historical_projection.hpp"
#include "atx/vol/listed_dispersion.hpp"
#include "atx/vol/listed_dispersion_reconciliation.hpp"
#include "atx/vol/listed_dispersion_schedule.hpp"
#include "atx/vol/listed_dispersion_strategy.hpp"
#include "atx/vol/listed_opra.hpp"
#include "atx/vol/occ_ess.hpp"
#include "atx/vol/opra_batch.hpp"
#include "atx/vol/phase_profile.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/strategy.hpp"
#include "atx/vol/tearsheet.hpp"
#include "atx/vol/types.hpp"

using namespace atx::vol;

namespace {

namespace fs = std::filesystem;
using atx::core::ErrorCode;
using atx::core::Err;
using atx::core::Ok;
using atx::vol::Status;

// Projected-definition schedule (route P canonical): take each frozen listed roll and
// reprice its members at the surface ATM-forward strike (the exact interpolated strike
// instead of the nearest listed grid strike) with COLD certified greeks, keeping
// roll_date / valuation_ts_ns / cohort / expiry_ts_ns / n_names / weights / side
// identical to the listed build. The projected portfolio differs from the listed one
// ONLY by contract idealization — not by tenor and not by solver tier. The listed
// sizing rule (build_listed_dispersion_roll) is reused verbatim, so projected_schedule
// .tsv is the exact ATX_LISTED_DISPERSION_SCHEDULE format and passes the shared
// validator (net vega ~ 0, gross = 2x target); only per-member strike and its cold
// greeks differ from trade_schedule.tsv.
Status project_schedule_command(const fs::path &run_dir) {
  ATX_TRY(CorpusManifest manifest, read_manifest_file((run_dir / "surface_manifest.tsv").string()));
  ATX_TRY(Clock clock, Clock::from_manifest(manifest));
  ATX_TRY(ListedDispersionSchedule listed,
          read_listed_dispersion_schedule_file((run_dir / "trade_schedule.tsv").string()));

  std::map<std::string, std::string> archive_of;
  for (const SnapshotRef &ref : clock.refs()) {
    archive_of.emplace(ref.date, ref.archive_path);
  }

  // Cold certified economics on both sides, matching the run-projected-backtest
  // --execution cold replay route (RunConfig default analytic AL greeks +
  // ColdReference), so the persisted schedule marks equal the live cold seed marks
  // that replay recomputes.
  const bool analytic = true;
  const QueryExecution execution = QueryExecution::ColdReference;

  ListedDispersionSchedule projected;
  projected.rolls.reserve(listed.rolls.size());
  for (const ListedScheduleRoll &roll : listed.rolls) {
    const auto archive = archive_of.find(roll.roll_date);
    if (archive == archive_of.end()) {
      return Err(ErrorCode::NotFound,
                 "project-schedule: no qualified archive for roll date " + roll.roll_date);
    }
    ATX_TRY(MarketSnapshot snapshot, MarketSnapshot::load(archive->second));
    if (snapshot.ts_ns() != roll.valuation_ts_ns) {
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
    const ListedRiskLookup cold_lookup =
        [&](std::uint32_t uid, const ListedOptionQuote &quote) -> Result<ListedOptionRisk> {
      const SurfaceRef surface = snapshot.find(uid);
      if (surface == nullptr) {
        return Err(ErrorCode::NotFound, "project-schedule: projected surface unavailable");
      }
      ATX_TRY(FullGreekSeed seed,
              surface->full_greek_seed(quote.strike, residual_T, quote.side, analytic, execution));
      return Ok(ListedOptionRisk{seed.greeks().price, seed.greeks().delta, seed.greeks().vega});
    };

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
      const SurfaceRef surface = snapshot.find(call_leg.uid);
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
    std::printf("  roll %u %s: net_vega=%.10g gross_vega=%.10g index_K=%.6f (listed %.6f)\n",
                projected_roll.cohort, projected_roll.roll_date.c_str(),
                projected_roll.net_vega_per_vol_point, projected_roll.gross_vega_per_vol_point,
                projected_roll.legs.front().strike, roll.legs.front().strike);
    projected.rolls.push_back(std::move(projected_roll));
  }

  ATX_TRY_VOID(validate_listed_dispersion_schedule(projected));
  ATX_TRY_VOID(write_listed_dispersion_schedule_file(
      (run_dir / "projected_schedule.tsv").string(), projected));
  std::printf("projected schedule built: rolls=%zu\n", projected.rolls.size());
  return Ok();
}

// Projected replay of a listed-format schedule. `--execution configured` (default) is
// the Task 2 diagnostic: reprice through the fast cached-surrogate tier under
// QueryExecution::Configured (genuine interpolation) — its fast-tier accuracy gap is
// under separate investigation. `--execution cold` is route P canonical: no fast tier,
// QueryExecution::ColdReference with ScheduleMarkPolicy::Record (Configured-required
// economics permitted with a cold price execution while no fast tier is prepared).
// Records per-roll mark divergence between the frozen schedule marks and the live seed
// marks. `--schedule` selects the input schedule (default trade_schedule.tsv);
// `--out` the backtest output (default projected_backtest.tsv).
Status run_projected_backtest_command(const fs::path &run_dir, const fs::path &schedule_file,
                                      const std::string &execution, const fs::path &out_file) {
  if (execution != "configured" && execution != "cold") {
    return Err(ErrorCode::InvalidArgument,
               "run-projected-backtest: --execution must be 'configured' or 'cold'");
  }
  const bool cold = execution == "cold";
  ATX_TRY(RunSpec spec, read_run_spec(run_dir / "run_spec.tsv"));
  ATX_TRY(CorpusManifest manifest, read_manifest_file((run_dir / "surface_manifest.tsv").string()));
  ATX_TRY(Clock clock, Clock::from_manifest(manifest));
  ATX_TRY(ListedDispersionSchedule schedule,
          read_listed_dispersion_schedule_file((run_dir / schedule_file).string()));

  // Shared query route for the divergence replay and the priced run.
  RunConfig config;
  config.unpriced = UnpricedLotPolicy::Error;
  config.snapshot_cache = std::make_shared<SnapshotCache>();
  if (cold) {
    // Route P canonical: cold certified economics both sides, no fast tier attached.
    // Record policy reprices the projected definitions through the ColdReference route;
    // the engine gate permits the strategy's Configured-required economics with a cold
    // price execution precisely because no fast query tier is prepared.
    config.price.query_execution = QueryExecution::ColdReference;
  } else {
    // Attaching the prepared fast tier (with_query_pricing, propagated by
    // MarketSnapshot::load with no silent cold fallback) makes the Configured queries
    // genuinely interpolate the cached surrogate instead of reproducing the cold
    // archive marks.
    config.query_pricing_tier = QueryPricingTier::RepresentativeFast;
    config.price.query_execution = QueryExecution::Configured;
  }

  // mark_divergence.tsv: last_mark_divergences() is cleared every step and the
  // engine's run_backtest loop hides per-step strategy state, so drive a separate
  // Record replay here and snapshot the record after each roll step.
  ATX_TRY(ListedDispersionStrategy divergence_strategy,
          ListedDispersionStrategy::create(schedule, spec.delta_band, ScheduleMarkPolicy::Record));
  PortfolioState divergence_book;
  std::uint64_t divergence_next_id = 1;
  std::ofstream div_out(run_dir / "mark_divergence.tsv", std::ios::binary | std::ios::trunc);
  if (!div_out) {
    return Err(ErrorCode::IoError, "cannot write mark divergence");
  }
  div_out << std::setprecision(17)
          << "date\tsymbol\traw_symbol\tstrike\texpiry_ts_ns\tside\tschedule_mark\tlive_mark\t"
             "diff\tabs_diff_bps_of_mark\n";
  for (std::size_t i = 0; i < clock.size(); ++i) {
    const SnapshotRef &ref = clock.refs()[i];
    ATX_TRY(MarketSnapshot snapshot,
            MarketSnapshot::load(ref.archive_path, config.query_pricing_tier));
    ATX_TRY_VOID(divergence_strategy.on_step(snapshot, i, divergence_book, divergence_next_id,
                                             config.price));
    const std::vector<MarkDivergence> &divergences = divergence_strategy.last_mark_divergences();
    if (divergences.empty()) {
      continue;
    }
    // Divergences are populated only on a roll step; the roll that just fired owns
    // the legs carrying each contract's symbol/raw_symbol.
    const ListedScheduleRoll &roll = schedule.rolls[divergence_strategy.next_roll_index() - 1u];
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
      const double diff = divergence.live_mark - divergence.schedule_mark;
      const double denom = std::abs(divergence.schedule_mark);
      const double abs_diff_bps_of_mark = denom > 0.0 ? std::abs(diff) / denom * 1.0e4 : 0.0;
      div_out << ref.date << '\t' << matched->symbol << '\t' << matched->raw_symbol << '\t'
              << divergence.strike << '\t' << divergence.expiry_ts_ns << '\t'
              << (divergence.side == Side::Call ? "Call" : "Put") << '\t' << divergence.schedule_mark
              << '\t' << divergence.live_mark << '\t' << diff << '\t' << abs_diff_bps_of_mark
              << '\n';
    }
  }
  if (!div_out) {
    return Err(ErrorCode::IoError, "cannot flush mark divergence");
  }

  // Primary priced run: the same strategy-aware engine as run-backtest, under the
  // Record policy so the interpolated live seed marks (not the frozen schedule
  // marks) seed each entry, and Configured economics end to end.
  ATX_TRY(ListedDispersionStrategy strategy,
          ListedDispersionStrategy::create(schedule, spec.delta_band, ScheduleMarkPolicy::Record));
  ATX_TRY(BacktestResult backtest, run_backtest(clock, strategy, config));
  if (!strategy.all_rolls_consumed()) {
    return Err(ErrorCode::Unavailable, "projected backtest did not consume every scheduled roll");
  }
  ATX_TRY_VOID(write_backtest_tsv(backtest, (run_dir / out_file).string()));
  std::printf("projected backtest complete [%s]: dates=%zu rolls=%zu final_nav=%.10g\n",
              execution.c_str(), backtest.size(), schedule.rolls.size(), backtest.nav.back());
  return Ok();
}


void usage() {
  std::fprintf(stderr, "usage:\n"
                       "  atxvol_spy_dispersion_backtest build-corpus --spec FILE --out DIR\n"
                       "  atxvol_spy_dispersion_backtest build-schedule --run DIR\n"
                       "  atxvol_spy_dispersion_backtest run-backtest --run DIR\n"
                       "  atxvol_spy_dispersion_backtest project-schedule --run DIR\n"
                       "  atxvol_spy_dispersion_backtest run-projected-backtest --run DIR "
                       "[--schedule FILE] [--execution cold|configured] [--out FILE]\n"
                       "  atxvol_spy_dispersion_backtest run-surface-backtest --run DIR\n"
                       "  atxvol_spy_dispersion_backtest run-projected-var --run DIR\n"
                       "  atxvol_spy_dispersion_backtest verify --run DIR\n");
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string command = argv[1];
  fs::path spec;
  fs::path run;
  fs::path out;
  fs::path schedule;
  std::string execution;
  for (int i = 2; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (i + 1 >= argc) {
      usage();
      return 2;
    }
    if (argument == "--spec") {
      spec = argv[++i];
    } else if (argument == "--run") {
      run = argv[++i];
    } else if (argument == "--out") {
      out = argv[++i];
    } else if (argument == "--schedule") {
      schedule = argv[++i];
    } else if (argument == "--execution") {
      execution = argv[++i];
    } else {
      usage();
      return 2;
    }
  }
  Status status = atx::core::Err(ErrorCode::InvalidArgument, "unknown command");
  if (command == "build-corpus" && !spec.empty() && !run.empty()) {
    status = atx::vol::dispersion_build_corpus(spec, run);
  } else if (command == "build-schedule" && !run.empty()) {
    status = atx::vol::dispersion_build_schedule(run);
  } else if (command == "run-backtest" && !run.empty()) {
    status = atx::vol::dispersion_run_backtest(run);
  } else if (command == "project-schedule" && !run.empty()) {
    status = project_schedule_command(run);
  } else if (command == "run-projected-backtest" && !run.empty()) {
    status = run_projected_backtest_command(
        run, schedule.empty() ? fs::path("trade_schedule.tsv") : schedule,
        execution.empty() ? std::string("configured") : execution,
        out.empty() ? fs::path("projected_backtest.tsv") : out);
  } else if (command == "run-surface-backtest" && !run.empty()) {
    status = atx::vol::dispersion_run_surface_backtest(run);
  } else if (command == "run-projected-var" && !run.empty()) {
    status = atx::vol::dispersion_run_projected_var(run);
  } else if (command == "verify" && !run.empty()) {
    auto report = atx::vol::dispersion_verify(run);
    status = report ? atx::core::Ok() : atx::core::Err(report.error());
  } else {
    usage();
    return 2;
  }
  if (!status) {
    std::fprintf(stderr, "%s\n", status.error().to_string().c_str());
    return 1;
  }
  return 0;
}
