#include "atx/vol/sweep_driver.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"              // ATX_TRY, ATX_TRY_VOID
#include "atx/vol/detail/parallel_for.hpp" // parallel_for

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

[[nodiscard]] std::string hex_bytes(std::span<const std::uint8_t> bytes) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string out(bytes.size() * 2, '0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[i * 2] = kHexDigits[(bytes[i] >> 4U) & 0x0FU];
    out[i * 2 + 1] = kHexDigits[bytes[i] & 0x0FU];
  }
  return out;
}

// A minimal, deterministic human-queryable rendering of one variant's
// canonical economics. `TrackRegistration::config_json` is documented only
// as "canonical, human-queryable copy" (catalog.hpp) -- not a schema this
// driver or any other code parses back -- so a single hex field is
// sufficient: it is a faithful, order-preserving rendering of exactly the
// bytes `TrackKey` hashed, plus the template's own catalog id/name for a
// human skimming rows.
[[nodiscard]] std::string build_config_json(const BacktestStrategyTemplate &variant,
                                            std::span<const std::uint8_t> canonical_bytes) {
  std::string out = "{\"template_id\":\"";
  out += variant.id;
  out += "\",\"template_name\":\"";
  out += variant.name;
  out += "\",\"canonical_config_hex\":\"";
  out += hex_bytes(canonical_bytes);
  out += "\"}";
  return out;
}

} // namespace

Result<SweepResult> run_sweep(const SweepSpec &spec, const SweepConfig &config) {
  if (spec.variants.empty()) {
    return Err(ErrorCode::InvalidArgument, "run_sweep: spec.variants is empty");
  }
  if (config.lake_root.empty()) {
    return Err(ErrorCode::InvalidArgument, "run_sweep: config.lake_root is empty");
  }
  if (config.sweep_id.empty()) {
    return Err(ErrorCode::InvalidArgument, "run_sweep: config.sweep_id is empty");
  }

  const std::string engine_id = make_engine_id();
  const std::size_t n = spec.variants.size();

  // â”€â”€ Phase 1: TrackKey per ORIGINAL variant, dedupe by first occurrence â”€â”€â”€â”€
  //
  // `unique_order[u]` is the index (into `spec.variants`) of the u-th DISTINCT
  // key, in the order that key was FIRST seen scanning `spec.variants` left to
  // right -- deterministic because it is the scan order, never the iteration
  // order of `seen` (an unordered_map used strictly for O(1) membership;
  // its own iteration order never reaches `unique_order` or any output).
  std::vector<TrackKey> keys;
  keys.reserve(n);
  std::vector<std::size_t> unique_order;
  std::unordered_map<std::string, std::size_t> seen;

  for (std::size_t i = 0; i < n; ++i) {
    const BacktestStrategyTemplate &variant = spec.variants[i];
    const Status valid = validate_backtest_template(variant);
    if (!valid) {
      return Err(ErrorCode::InvalidArgument, "run_sweep: variants[" + std::to_string(i) +
                                                 "] failed validate_backtest_template: " +
                                                 valid.error().to_string());
    }
    const std::vector<std::uint8_t> canonical = canonical_config_bytes(variant, spec.base_config);
    const TrackKey key = make_track_key(canonical, engine_id, spec.data_snapshot_id);
    const std::string hex = key.hex();
    keys.push_back(key);
    if (!seen.contains(hex)) {
      seen.emplace(hex, unique_order.size());
      unique_order.push_back(i);
    }
  }

  // â”€â”€ Phase 2: open the lakehouse â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
  ATX_TRY(Catalog catalog, Catalog::open(config.lake_root));
  TrackStore store(config.lake_root);

  // â”€â”€ Phase 3: probe every unique key, cache-first â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
  std::vector<SweepVariantOutcome> outcomes(unique_order.size());
  std::vector<std::size_t> misses; // indices into unique_order/outcomes
  for (std::size_t u = 0; u < unique_order.size(); ++u) {
    const std::size_t variant_idx = unique_order[u];
    outcomes[u].key = keys[variant_idx];
    outcomes[u].first_variant_index = variant_idx;
    ATX_TRY(std::optional<TrackRow> probed, catalog.probe(keys[variant_idx]));
    if (probed.has_value()) {
      outcomes[u].cache_hit = true;
    } else {
      misses.push_back(u);
    }
  }

  // â”€â”€ Phase 4: run every miss, variant-parallel, RunConfig::price.n_threads
  // forced to 1 per variant (the outer fan-out is where the concurrency
  // lives -- see the header's "Threading" doc). Each worker writes only its
  // own disjoint `run_results[i]` slot, mirroring the proven-race-free
  // `out[t] = run_backtest(...)` pattern in
  // BacktestExec.SnapshotPoolConcurrentRunsMatchSerial.
  std::vector<Result<BacktestResult>> run_results(
      misses.size(), Err(ErrorCode::Internal, "run_sweep: variant slot never assigned"));
  parallel_for(misses.size(), config.n_threads, [&](std::size_t i) {
    const std::size_t u = misses[i];
    const std::size_t variant_idx = unique_order[u];
    RunConfig cfg = spec.base_config;
    cfg.price.n_threads = 1;
    cfg.snapshot_pool = config.snapshot_pool;
    Result<ProjectedTemplateStrategy> strat =
        ProjectedTemplateStrategy::create(spec.variants[variant_idx], spec.uid);
    if (!strat.has_value()) {
      run_results[i] = Err(strat.error());
      return;
    }
    run_results[i] = run_backtest(spec.clock, *strat, cfg);
  });

  // â”€â”€ Phase 5: publish, serially, on the calling thread â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
  //
  // `Catalog` wraps exactly one `atx::core::db::Database`, which "must NOT be
  // shared across threads" (atx/core/db/sqlite.hpp) -- so registration happens
  // strictly AFTER the fan-out's join barrier above, never inside it. Order is
  // `unique_order`'s (deterministic), not completion order.
  for (std::size_t i = 0; i < misses.size(); ++i) {
    const std::size_t u = misses[i];
    const std::size_t variant_idx = unique_order[u];
    if (!run_results[i].has_value()) {
      return Err(run_results[i].error());
    }
    BacktestResult &result = *run_results[i];
    if (result.date.empty()) {
      return Err(ErrorCode::Internal, "run_sweep: variants[" + std::to_string(variant_idx) +
                                          "] produced an empty result");
    }
    ATX_TRY_VOID(store.write_staging(keys[variant_idx], result, spec.meta));

    const std::vector<std::uint8_t> canonical =
        canonical_config_bytes(spec.variants[variant_idx], spec.base_config);
    TrackRegistration registration;
    registration.config_json = build_config_json(spec.variants[variant_idx], canonical);
    registration.engine_id = engine_id;
    registration.economics_rev = kBacktestEconomicsRev;
    registration.data_snapshot_id = hex_bytes(spec.data_snapshot_id);
    registration.date_min = result.date.front();
    registration.date_max = result.date.back();
    ATX_TRY_VOID(catalog.register_staging(keys[variant_idx], spec.meta, registration));

    outcomes[u].ran = true;
    outcomes[u].result = std::move(result);
  }

  // â”€â”€ Phase 6: one trial per ORIGINAL variant -- attempts, not unique configs â”€
  for (std::size_t i = 0; i < n; ++i) {
    ATX_TRY_VOID(catalog.record_trial(keys[i], config.sweep_id, std::nullopt));
  }

  SweepResult out;
  out.variants = std::move(outcomes);
  out.n_variants_submitted = n;
  out.engine_runs = misses.size();
  out.cache_hits = unique_order.size() - misses.size();
  return Ok(std::move(out));
}

} // namespace atx::vol
