#include "atx/vol/backtest_db_build.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/backtest.hpp"
#include "atx/vol/backtest_db.hpp"
#include "atx/vol/corpus.hpp"
#include "atx/vol/detail/archive_util.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/surface_db.hpp"

namespace atx::vol {
using atx::core::Ok;

namespace {

struct SourceSnapshot {
  SnapshotRef ref{};
  BacktestSourcePartition source{};
};

struct SymbolCoverage {
  std::string symbol{};
  std::optional<std::uint32_t> uid{};
  std::size_t source_begin{0};
  std::string error{};
};

struct SourceLoad {
  std::vector<SourceSnapshot> sources{};
  std::vector<SymbolCoverage> coverage{};
};

struct CellPlan {
  BacktestDbCellBuildMode mode{BacktestDbCellBuildMode::Full};
  std::string detail{};
};

[[nodiscard]] bool is_leap_year(int year) noexcept {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

[[nodiscard]] bool is_iso_date(std::string_view text) noexcept {
  if (text.size() != 10u || text[4] != '-' || text[7] != '-') {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i != 4u && i != 7u && !std::isdigit(static_cast<unsigned char>(text[i]))) {
      return false;
    }
  }
  const auto digit = [&text](std::size_t i) { return static_cast<int>(text[i] - '0'); };
  const int year = digit(0u) * 1000 + digit(1u) * 100 + digit(2u) * 10 + digit(3u);
  const int month = digit(5u) * 10 + digit(6u);
  const int day = digit(8u) * 10 + digit(9u);
  if (year == 0 || month < 1 || month > 12 || day < 1) {
    return false;
  }
  constexpr int kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int max_day = kMonthDays[month - 1] + (month == 2 && is_leap_year(year) ? 1 : 0);
  return day <= max_day;
}

[[nodiscard]] Status validate_spec(const BacktestDbBuildSpec &spec) {
  if (spec.surface_db_root.empty() || spec.backtest_db_root.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "build_backtest_db: surface and backtest database roots are required");
  }
  std::error_code source_path_error;
  std::error_code destination_path_error;
  const std::filesystem::path source_path =
      std::filesystem::absolute(spec.surface_db_root, source_path_error).lexically_normal();
  const std::filesystem::path destination_path =
      std::filesystem::absolute(spec.backtest_db_root, destination_path_error).lexically_normal();
  if (source_path_error || destination_path_error) {
    return Err(ErrorCode::IoError,
               "build_backtest_db: unable to resolve source or destination root");
  }
  if (source_path == destination_path) {
    return Err(ErrorCode::InvalidArgument,
               "build_backtest_db: source and destination roots must be different");
  }
  if (spec.templates.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "build_backtest_db: at least one strategy template is required");
  }
  if ((!spec.date_lo.empty() && !is_iso_date(spec.date_lo)) ||
      (!spec.date_hi.empty() && !is_iso_date(spec.date_hi))) {
    return Err(ErrorCode::InvalidArgument,
               "build_backtest_db: date bounds must be empty or strict YYYY-MM-DD dates");
  }
  if (!spec.date_lo.empty() && !spec.date_hi.empty() && spec.date_lo > spec.date_hi) {
    return Err(ErrorCode::InvalidArgument, "build_backtest_db: date_hi precedes date_lo");
  }
  return Ok();
}

[[nodiscard]] Result<BacktestDb> open_or_create_db(std::string_view root) {
  auto opened = BacktestDb::open(root);
  if (opened) {
    return Ok(std::move(*opened));
  }
  if (opened.error().code() != ErrorCode::NotFound) {
    return Err(std::move(opened).error());
  }
  return BacktestDb::create(root);
}

[[nodiscard]] Status require_source_generation(const SurfaceDb &source_db,
                                               std::uint64_t expected_generation,
                                               std::string_view phase) {
  auto observed = SurfaceDb::open(source_db.root());
  if (!observed) {
    return Err(std::move(observed).error());
  }
  const std::uint64_t observed_generation = observed->generation();
  if (observed_generation != expected_generation) {
    return Err(ErrorCode::Unavailable,
               "build_backtest_db: SurfaceDb generation changed during " + std::string(phase) +
                   " (expected " + std::to_string(expected_generation) + ", observed " +
                   std::to_string(observed_generation) + "); retry from a fresh snapshot");
  }
  return Ok();
}

[[nodiscard]] Result<std::vector<std::string>>
resolve_symbols(const SurfaceDb &source_db, std::span<const std::string> requested) {
  std::vector<std::string> symbols =
      requested.empty() ? source_db.symbols()
                        : std::vector<std::string>(requested.begin(), requested.end());
  for (std::string &symbol : symbols) {
    if (symbol.empty() || symbol.size() > kArchiveSymbolMax) {
      return Err(ErrorCode::InvalidArgument, "build_backtest_db: symbols must contain 1.." +
                                                 std::to_string(kArchiveSymbolMax) + " bytes");
    }
    symbol = detail::canonicalize_symbol(symbol, kArchiveSymbolMax);
  }
  std::sort(symbols.begin(), symbols.end());
  symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());
  if (symbols.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "build_backtest_db: no symbols requested or registered in SurfaceDb");
  }
  return Ok(std::move(symbols));
}

[[nodiscard]] Result<SourceLoad> load_sources(const SurfaceDb &source_db, std::string_view date_lo,
                                              std::string_view date_hi,
                                              std::span<const std::string> symbols) {
  ATX_TRY(Clock all, Clock::from_surface_db(source_db));
  SourceLoad loaded;
  loaded.sources.reserve(all.size());
  loaded.coverage.reserve(symbols.size());
  for (const std::string &symbol : symbols) {
    SymbolCoverage coverage;
    coverage.symbol = symbol;
    coverage.source_begin = all.size();
    loaded.coverage.push_back(std::move(coverage));
  }
  for (const SnapshotRef &ref : all.refs()) {
    if (!is_iso_date(ref.date)) {
      return Err(ErrorCode::InvalidArgument,
                 "build_backtest_db: non-ISO SurfaceDb partition key '" + ref.date + "'");
    }
    if ((!date_lo.empty() && ref.date < date_lo) || (!date_hi.empty() && ref.date > date_hi)) {
      continue;
    }
    ATX_TRY(SurfaceArchiveV2 archive, SurfaceArchiveV2::open_mapped(ref.archive_path));
    const std::size_t source_index = loaded.sources.size();
    loaded.sources.push_back(
        SourceSnapshot{ref, BacktestSourcePartition{ref.date, archive.identity()}});
    for (SymbolCoverage &coverage : loaded.coverage) {
      if (!coverage.error.empty()) {
        continue;
      }
      auto mapped = archive.map_symbol(coverage.symbol);
      if (!mapped) {
        if (!coverage.uid.has_value() && mapped.error().code() == ErrorCode::NotFound) {
          continue;
        }
        coverage.error = "surface unavailable on " + ref.date + ": " + mapped.error().to_string();
        coverage.uid.reset();
        continue;
      }
      const std::uint32_t observed_uid = mapped->uid();
      if (observed_uid == 0u) {
        coverage.error = "surface uid is zero on " + ref.date;
        coverage.uid.reset();
      } else if (!coverage.uid.has_value()) {
        coverage.uid = observed_uid;
        coverage.source_begin = source_index;
      } else if (*coverage.uid != observed_uid) {
        coverage.error = "surface uid changed on " + ref.date + " from " +
                         std::to_string(*coverage.uid) + " to " + std::to_string(observed_uid);
        coverage.uid.reset();
      }
    }
  }
  if (loaded.sources.empty()) {
    return Err(ErrorCode::NotFound,
               "build_backtest_db: no SurfaceDb partitions fall inside the requested range");
  }
  for (SymbolCoverage &coverage : loaded.coverage) {
    if (!coverage.uid.has_value() && coverage.error.empty()) {
      coverage.error = "surface coverage has no available partition";
    }
  }
  return Ok(std::move(loaded));
}

[[nodiscard]] Result<Clock> make_clock(std::span<const SourceSnapshot> sources) {
  CorpusManifest manifest;
  manifest.dates.reserve(sources.size());
  manifest.entries.reserve(sources.size());
  for (const SourceSnapshot &source : sources) {
    manifest.dates.push_back(source.ref.date);
    CorpusEntry entry;
    entry.date = source.ref.date;
    entry.status = CorpusFitStatus::Ok;
    entry.archive_path = source.ref.archive_path;
    manifest.entries.push_back(std::move(entry));
  }
  return Clock::from_manifest(manifest);
}

[[nodiscard]] std::vector<BacktestSourcePartition>
source_identities(std::span<const SourceSnapshot> sources) {
  std::vector<BacktestSourcePartition> out;
  out.reserve(sources.size());
  for (const SourceSnapshot &source : sources) {
    out.push_back(source.source);
  }
  return out;
}

[[nodiscard]] bool same_dates(std::span<const BacktestSourcePartition> lhs,
                              std::span<const BacktestSourcePartition> rhs) {
  return lhs.size() == rhs.size() &&
         std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                    [](const auto &a, const auto &b) { return a.date == b.date; });
}

[[nodiscard]] bool same_sources(std::span<const BacktestSourcePartition> lhs,
                                std::span<const BacktestSourcePartition> rhs) {
  return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

[[nodiscard]] bool exact_prefix(std::span<const BacktestSourcePartition> prefix,
                                std::span<const BacktestSourcePartition> all) {
  return prefix.size() <= all.size() && std::equal(prefix.begin(), prefix.end(), all.begin());
}

[[nodiscard]] bool old_dates_are_covered(std::span<const BacktestSourcePartition> old_sources,
                                         std::span<const BacktestSourcePartition> current) {
  return std::all_of(old_sources.begin(), old_sources.end(), [&](const auto &old_source) {
    return std::any_of(current.begin(), current.end(),
                       [&](const auto &new_source) { return old_source.date == new_source.date; });
  });
}

[[nodiscard]] CellPlan choose_plan(const BacktestSeriesData *previous,
                                   std::span<const BacktestSourcePartition> current) {
  if (previous == nullptr) {
    return CellPlan{BacktestDbCellBuildMode::Full, "new series"};
  }
  if (!old_dates_are_covered(previous->sources, current)) {
    return CellPlan{BacktestDbCellBuildMode::Failed,
                    "requested/source coverage would drop a stored historical date"};
  }
  if (same_sources(previous->sources, current)) {
    return CellPlan{BacktestDbCellBuildMode::Unchanged, "source identities unchanged"};
  }
  if (exact_prefix(previous->sources, current)) {
    return CellPlan{BacktestDbCellBuildMode::Extended, "new source dates append stored history"};
  }
  if (same_dates(previous->sources, current)) {
    return CellPlan{BacktestDbCellBuildMode::Rebuilt, "historical source identity changed"};
  }
  return CellPlan{BacktestDbCellBuildMode::Rebuilt,
                  "historical source dates were inserted or range was expanded"};
}

[[nodiscard]] RunConfig run_config(const BacktestStrategyTemplate &strategy_template,
                                   unsigned price_threads) {
  RunConfig config;
  config.price.n_threads = price_threads;
  config.price.analytic_greeks = strategy_template.projection.analytic_greeks;
  config.price.query_execution = strategy_template.projection.query_execution;
  config.frictions = strategy_template.frictions;
  config.record_every_n = 1u;
  config.unpriced = UnpricedLotPolicy::Error;
  // Controller follow-up (2026-08-08, post D1): C1's per-step settlement-mark memo
  // (`StepMarkMemo`, commit 06e87d7) is walk-continuity state -- it is populated by
  // the PRIOR step's book-greeks pass at that step's exact base date and consulted
  // on the VERY NEXT step (backtest.cpp `execute()`/`compute_step`) -- and, like the
  // pending stride-block state that makes `run_backtest_incremental` itself refuse
  // to resume a non-1 `record_every_n` (backtest.cpp, `run_backtest_incremental`'s
  // own guard -- this file just always supplies 1u, above), it is intentionally NOT
  // part of `BacktestCheckpoint`. A resumed leg (`run_extension`, below) therefore
  // always starts with a cold memo, while a continuous one-shot leg (`run_full`) over
  // the SAME combined range may be warm at that exact step (e.g. any Daily DeltaToZero
  // hedge keeps it warm on every step) -- so whether an expiring lot's settlement
  // mark is SERVED from the memo (a FullGreeks whole-book AVX2 batch) or FRESHLY
  // SOLVED (a Marks-only AVX2 batch over just the expiring lots) becomes a function
  // of how the walk happened to be CHUNKED, not of the data. The two are only an
  // economic-parity match, not a bit-identity one -- C1's own
  // `BacktestExec.L2StrategyCohortSettlementMemoBitIdentical` tolerates up to 1e-9
  // absolute between memo on/off for exactly this reason -- which silently broke
  // `docs/backtest-db.md`'s "Incremental equality is an engine invariant" guarantee
  // (`BacktestDbBuild.ExtensionAcrossExactProjectedExpiryMatchesOneShotSettlement`)
  // the first time an appended range's checkpoint boundary landed one step before an
  // exact expiry that a continuous run would have kept the memo warm for. BacktestDb
  // needs strict bit-for-bit reproducibility, a stronger contract than
  // `run_backtest_incremental` itself promises, so this cell disables the memo
  // rather than loosening that invariant: both `run_full` and `run_extension` share
  // this same config, so every settlement always takes the fresh-solve path
  // regardless of chunking, and the two legs stay bit-identical by construction. The
  // cost is a handful of extra `fair_value` solves per expiry event, not a per-step
  // hot-path cost -- negligible for a batch build.
  config.settlement_mark_memo = false;
  return config;
}

[[nodiscard]] Result<BacktestSeriesData> run_full(std::span<const SourceSnapshot> sources,
                                                  const BacktestStrategyTemplate &strategy_template,
                                                  std::uint32_t uid, unsigned price_threads) {
  ATX_TRY(Clock clock, make_clock(sources));
  ATX_TRY(ProjectedTemplateStrategy strategy,
          ProjectedTemplateStrategy::create(strategy_template, uid));
  ATX_TRY(BacktestContinuation continuation,
          run_backtest_incremental(clock, strategy, run_config(strategy_template, price_threads)));
  BacktestSeriesData data;
  data.backtest = std::move(continuation.rows);
  data.checkpoint = std::move(continuation.checkpoint);
  data.next_cohort = strategy.next_cohort_counter();
  data.sources = source_identities(sources);
  return Ok(std::move(data));
}

[[nodiscard]] Result<BacktestSeriesData>
run_extension(std::span<const SourceSnapshot> sources,
              const BacktestStrategyTemplate &strategy_template, std::uint32_t uid,
              unsigned price_threads, BacktestSeriesData previous) {
  const std::size_t old_size = previous.sources.size();
  if (old_size == 0u || old_size >= sources.size()) {
    return Err(ErrorCode::Internal, "build_backtest_db: invalid extension source boundary");
  }
  ATX_TRY(Clock clock, make_clock(sources.subspan(old_size - 1u)));
  ATX_TRY(ProjectedTemplateStrategy strategy,
          ProjectedTemplateStrategy::create(strategy_template, uid, previous.next_cohort));
  ATX_TRY(BacktestContinuation continuation,
          run_backtest_incremental(clock, strategy, run_config(strategy_template, price_threads),
                                   &previous.checkpoint));
  ATX_TRY_VOID(append_backtest_results(previous.backtest, continuation.rows));
  previous.checkpoint = std::move(continuation.checkpoint);
  previous.next_cohort = strategy.next_cohort_counter();
  previous.sources = source_identities(sources);
  return Ok(std::move(previous));
}

void add_cell(BacktestDbBuildReport &report, BacktestDbCellBuildReport cell) {
  report.rows_computed += cell.rows_computed;
  report.rows_added += cell.rows_added;
  switch (cell.mode) {
  case BacktestDbCellBuildMode::Full:
    ++report.n_full;
    break;
  case BacktestDbCellBuildMode::Extended:
    ++report.n_extended;
    break;
  case BacktestDbCellBuildMode::Rebuilt:
    ++report.n_rebuilt;
    break;
  case BacktestDbCellBuildMode::Unchanged:
    ++report.n_unchanged;
    break;
  case BacktestDbCellBuildMode::Failed:
    ++report.n_failed;
    break;
  }
  report.cells.push_back(std::move(cell));
}

void add_failed_cell(BacktestDbBuildReport &report,
                     const BacktestStrategyTemplate &strategy_template,
                     const SymbolCoverage &coverage, std::size_t source_dates, std::string detail) {
  add_cell(report, BacktestDbCellBuildReport{strategy_template.id, coverage.symbol,
                                             BacktestDbCellBuildMode::Failed, source_dates, 0u, 0u,
                                             0u, 0u, std::move(detail)});
}

[[nodiscard]] Status build_cell(const SurfaceDb &source_db, std::uint64_t source_generation,
                                BacktestDb &db, std::span<const SourceSnapshot> sources,
                                const BacktestStrategyTemplate &strategy_template,
                                const SymbolCoverage &coverage, unsigned price_threads,
                                BacktestDbBuildReport &report) {
  if (!coverage.uid.has_value()) {
    add_failed_cell(report, strategy_template, coverage, 0u, coverage.error);
    return Ok();
  }
  if (coverage.source_begin >= sources.size()) {
    return Err(ErrorCode::Internal, "build_backtest_db: invalid symbol source boundary");
  }
  sources = sources.subspan(coverage.source_begin);
  const std::vector<BacktestSourcePartition> identities = source_identities(sources);
  const std::uint64_t current_source_fingerprint = backtest_source_fingerprint(identities);

  std::optional<BacktestSeriesData> previous;
  auto existing = db.find_series(strategy_template.id, coverage.symbol);
  if (existing) {
    if (existing->uid == *coverage.uid && existing->row_count == identities.size() &&
        existing->source_fingerprint == current_source_fingerprint) {
      BacktestDbCellBuildReport cell{strategy_template.id,
                                     coverage.symbol,
                                     BacktestDbCellBuildMode::Unchanged,
                                     sources.size(),
                                     static_cast<std::size_t>(existing->row_count),
                                     static_cast<std::size_t>(existing->row_count),
                                     0u,
                                     0u,
                                     "source fingerprint unchanged"};
      add_cell(report, std::move(cell));
      return Ok();
    }
    ATX_TRY(BacktestSeriesData loaded, db.load_series(strategy_template.id, coverage.symbol));
    previous = std::move(loaded);
  } else if (existing.error().code() != ErrorCode::NotFound) {
    return Err(std::move(existing).error());
  }

  const CellPlan plan = choose_plan(previous ? &*previous : nullptr, identities);
  const std::size_t rows_before = previous ? previous->backtest.size() : 0u;
  if (plan.mode == BacktestDbCellBuildMode::Failed) {
    BacktestDbCellBuildReport cell{strategy_template.id, coverage.symbol, plan.mode, sources.size(),
                                   rows_before,          rows_before,     0u,        0u,
                                   plan.detail};
    add_cell(report, std::move(cell));
    return Ok();
  }
  if (plan.mode == BacktestDbCellBuildMode::Unchanged) {
    BacktestDbCellBuildReport cell{strategy_template.id, coverage.symbol, plan.mode, sources.size(),
                                   rows_before,          rows_before,     0u,        0u,
                                   plan.detail};
    add_cell(report, std::move(cell));
    return Ok();
  }

  Result<BacktestSeriesData> built =
      plan.mode == BacktestDbCellBuildMode::Extended
          ? run_extension(sources, strategy_template, *coverage.uid, price_threads,
                          std::move(*previous))
          : run_full(sources, strategy_template, *coverage.uid, price_threads);
  if (!built) {
    add_failed_cell(report, strategy_template, coverage, sources.size(),
                    "backtest failed: " + built.error().to_string());
    report.cells.back().rows_before = rows_before;
    report.cells.back().rows_after = rows_before;
    return Ok();
  }

  const std::size_t rows_after = built->backtest.size();
  if (plan.mode == BacktestDbCellBuildMode::Extended && rows_after < rows_before) {
    return Err(ErrorCode::Internal,
               "build_backtest_db: extension unexpectedly reduced stored row count");
  }
  const std::size_t rows_computed =
      plan.mode == BacktestDbCellBuildMode::Extended ? rows_after - rows_before : rows_after;
  const std::size_t rows_added = rows_after > rows_before ? rows_after - rows_before : 0u;
  ATX_TRY_VOID(require_source_generation(source_db, source_generation, "series publication"));
  ATX_TRY_VOID(db.write_series(strategy_template.id, coverage.symbol, *coverage.uid, *built));
  BacktestDbCellBuildReport cell{strategy_template.id, coverage.symbol, plan.mode,
                                 sources.size(),       rows_before,     rows_after,
                                 rows_computed,        rows_added,      plan.detail};
  add_cell(report, std::move(cell));
  return Ok();
}

} // namespace

Result<BacktestDbBuildReport> build_backtest_db(const BacktestDbBuildSpec &spec) {
  ATX_TRY_VOID(validate_spec(spec));
  ATX_TRY(SurfaceDb source_db, SurfaceDb::open(spec.surface_db_root));
  const std::uint64_t source_generation = source_db.generation();
  if (source_generation == 0u) {
    return Err(ErrorCode::ParseError,
               "build_backtest_db: SurfaceDb has an invalid zero generation");
  }
  ATX_TRY(BacktestDb db, open_or_create_db(spec.backtest_db_root));
  ATX_TRY(std::vector<std::string> symbols, resolve_symbols(source_db, spec.symbols));
  ATX_TRY(SourceLoad loaded, load_sources(source_db, spec.date_lo, spec.date_hi, symbols));
  ATX_TRY_VOID(require_source_generation(source_db, source_generation, "source loading"));

  for (const BacktestStrategyTemplate &strategy_template : spec.templates) {
    ATX_TRY_VOID(db.register_template(strategy_template));
  }

  BacktestDbBuildReport report;
  report.cells.reserve(spec.templates.size() * loaded.coverage.size());
  for (const BacktestStrategyTemplate &strategy_template : spec.templates) {
    for (const SymbolCoverage &symbol : loaded.coverage) {
      ATX_TRY_VOID(build_cell(source_db, source_generation, db, loaded.sources, strategy_template,
                              symbol, spec.price_threads, report));
    }
  }
  ATX_TRY_VOID(require_source_generation(source_db, source_generation, "final consistency check"));
  return Ok(std::move(report));
}

} // namespace atx::vol
