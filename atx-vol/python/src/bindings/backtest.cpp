// Bindings for the backtest engine: the clock, the run configuration, the
// SoA result series, and `run_backtest` itself.
//
// The `BacktestResult` columns are handed to Python as NumPy arrays (copies —
// the result is owned by a temporary on the C++ side and the series are small
// relative to a run), so the whole track is directly plottable without a
// per-element conversion loop.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/vol/backtest.hpp"
#include "atx/vol/corpus.hpp"
#include "atx/vol/detail/counters.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/query_pricing.hpp"
#include "atx/vol/strategy.hpp"
#include "atx/vol/surface_db.hpp"
#include "backtest_util.hpp"
#include "result.hpp"

namespace py = pybind11;
using namespace atx::vol;

namespace {

py::array_t<double> to_numpy(const std::vector<double> &v) {
  py::array_t<double> out(static_cast<py::ssize_t>(v.size()));
  if (!v.empty()) {
    std::memcpy(out.mutable_data(), v.data(), v.size() * sizeof(double));
  }
  return out;
}

py::array_t<std::int64_t> to_numpy(const std::vector<std::int64_t> &v) {
  py::array_t<std::int64_t> out(static_cast<py::ssize_t>(v.size()));
  if (!v.empty()) {
    std::memcpy(out.mutable_data(), v.data(), v.size() * sizeof(std::int64_t));
  }
  return out;
}

std::vector<double> from_sequence(const py::object &value) {
  return py::cast<std::vector<double>>(value);
}

py::dict solve_ledger_dict(const counters::ledger::Counts &counts) {
  py::dict out;
  for (unsigned i = 0; i < counters::ledger::kCount; ++i) {
    out[py::str(counters::ledger::kNames[i])] = py::int_(counts.v[i]);
  }
  return out;
}

// Bind one `std::vector<double>` column as a NumPy property. The setter exists
// so a result written by `write_backtest_tsv` can be read back into a
// BacktestResult and re-folded by the library's own `tearsheet`, rather than
// reimplementing the fold in Python.
#define ATXVOL_SERIES(cls, name)                                                                   \
  cls.def_property(                                                                                \
      #name, [](const BacktestResult &r) { return to_numpy(r.name); },                             \
      [](BacktestResult &r, const py::object &v) { r.name = from_sequence(v); })

} // namespace

void bind_backtest(py::module_ &m) {
  // ── Corpus manifest (the windowed-clock construction path) ──
  py::enum_<CorpusFitStatus>(m, "CorpusFitStatus")
      .value("OK", CorpusFitStatus::Ok)
      .value("FAILED", CorpusFitStatus::Failed)
      .value("SKIPPED", CorpusFitStatus::Skipped);

  py::class_<CorpusEntry>(m, "CorpusEntry")
      .def(py::init<>())
      .def_readwrite("date", &CorpusEntry::date)
      .def_readwrite("symbol", &CorpusEntry::symbol)
      .def_readwrite("status", &CorpusEntry::status)
      .def_readwrite("n_slices", &CorpusEntry::n_slices)
      .def_readwrite("oos_in_band", &CorpusEntry::oos_in_band)
      .def_readwrite("archive_path", &CorpusEntry::archive_path);

  py::class_<CorpusManifest>(m, "CorpusManifest")
      .def(py::init<>())
      .def_readwrite("dates", &CorpusManifest::dates)
      .def_readwrite("entries", &CorpusManifest::entries)
      .def_readwrite("n_boards", &CorpusManifest::n_boards)
      .def_readwrite("n_ok", &CorpusManifest::n_ok)
      .def_readwrite("n_failed", &CorpusManifest::n_failed)
      .def_readwrite("n_skipped", &CorpusManifest::n_skipped);

  // A corpus archive directory is indexed by a `manifest.tsv`, which is a
  // different on-disk format from a `SurfaceDb`'s `manifest.atxdb` — opening an
  // archive tree as a SurfaceDb fails with NotFound. This is the archive path.
  m.def(
      "read_corpus_manifest",
      [](const std::string &path) {
        py::gil_scoped_release release;
        return atxvol::python::unwrap(read_manifest_file(path));
      },
      py::arg("path"), "Read a corpus archive's manifest.tsv (feeds Clock.from_manifest).");

  m.def(
      "write_corpus_manifest",
      [](const std::string &path, const CorpusManifest &manifest) {
        py::gil_scoped_release release;
        atxvol::python::unwrap(write_manifest_file(path, manifest));
      },
      py::arg("path"), py::arg("manifest"));

  // ── Clock ──
  py::class_<SnapshotRef>(m, "SnapshotRef")
      .def(py::init<>())
      .def_readwrite("date", &SnapshotRef::date)
      .def_readwrite("archive_path", &SnapshotRef::archive_path)
      .def("__repr__", [](const SnapshotRef &r) {
        return "SnapshotRef(date='" + r.date + "', archive_path='" + r.archive_path + "')";
      });

  py::class_<Clock>(m, "Clock")
      .def_static(
          "from_surface_db",
          [](const SurfaceDb &db) { return atxvol::python::unwrap(Clock::from_surface_db(db)); },
          py::arg("db"), "Build a clock over every partition in the db, in date order.")
      .def_static(
          "from_manifest",
          [](const CorpusManifest &manifest) {
            return atxvol::python::unwrap(Clock::from_manifest(manifest));
          },
          py::arg("manifest"))
      // The date-window subset every SurfaceDb-driven run carves its window with.
      // Unwrapped like the factories above, so the empty/inverted-window case
      // reaches Python as `AtxError` (code INVALID_ARGUMENT) carrying the
      // engine's message — which NAMES the available range, so a caller who
      // asked for a window the corpus does not cover can correct it without
      // dumping the manifest.
      .def(
          "between",
          [](const Clock &self, const std::string &date_lo, const std::string &date_hi) {
            return atxvol::python::unwrap(self.between(date_lo, date_hi));
          },
          py::arg("date_lo"), py::arg("date_hi"),
          "Subset to the refs whose date lies in [date_lo, date_hi], INCLUSIVE on "
          "both ends. Bounds outside the corpus clamp; a window selecting no ref "
          "raises AtxError(INVALID_ARGUMENT) naming the available range. `self` is "
          "unchanged.")
      .def_property_readonly("refs",
                             [](const Clock &self) {
                               const auto refs = self.refs();
                               return std::vector<SnapshotRef>{refs.begin(), refs.end()};
                             })
      .def("__len__", &Clock::size)
      .def_property_readonly("size", &Clock::size);

  // ── Pricing / execution policy enums ──
  py::enum_<QueryPricingTier>(m, "QueryPricingTier")
      .value("LEGACY_COMPATIBLE", QueryPricingTier::LegacyCompatible)
      .value("COLD_REFERENCE", QueryPricingTier::ColdReference)
      .value("REPRESENTATIVE_FAST", QueryPricingTier::RepresentativeFast)
      .value("CARRY_BANK", QueryPricingTier::CarryBank);
  py::enum_<QueryCacheBuildPolicy>(m, "QueryCacheBuildPolicy")
      .value("EAGER", QueryCacheBuildPolicy::Eager)
      .value("REUSE_ONLY", QueryCacheBuildPolicy::ReuseOnly);
  py::enum_<QueryExecution>(m, "QueryExecution")
      .value("CONFIGURED", QueryExecution::Configured)
      .value("COLD_REFERENCE", QueryExecution::ColdReference);
  py::enum_<UnpricedLotPolicy>(m, "UnpricedLotPolicy")
      .value("EXCLUDE_AND_REPORT", UnpricedLotPolicy::ExcludeAndReport)
      .value("ERROR", UnpricedLotPolicy::Error);
  py::enum_<SurfaceProvenancePolicy>(m, "SurfaceProvenancePolicy")
      .value("COMPATIBILITY", SurfaceProvenancePolicy::Compatibility)
      .value("REQUIRE_ADMITTED_RISK", SurfaceProvenancePolicy::RequireAdmittedRisk);

  py::class_<GreekNeeds>(m, "GreekNeeds")
      .def(py::init<>())
      .def_readwrite("vega", &GreekNeeds::vega)
      .def_readwrite("rho", &GreekNeeds::rho)
      .def_readwrite("charm", &GreekNeeds::charm)
      .def_property_readonly("full", &GreekNeeds::full);

  py::class_<PriceOptions>(m, "PriceOptions")
      .def(py::init<>())
      .def_readwrite("n_threads", &PriceOptions::n_threads)
      .def_readwrite("analytic_greeks", &PriceOptions::analytic_greeks)
      .def_readwrite("adjoint_greeks", &PriceOptions::adjoint_greeks)
      .def_readwrite("prices_only", &PriceOptions::prices_only)
      .def_readwrite("skew_adjusted_delta", &PriceOptions::skew_adjusted_delta)
      .def_readwrite("query_execution", &PriceOptions::query_execution)
      .def_readwrite("greek_needs", &PriceOptions::greek_needs);

  // ── Frictions / financing ──
  py::class_<FrictionModel> frictions(m, "FrictionModel");
  py::enum_<FrictionModel::SpreadKind>(frictions, "SpreadKind")
      .value("NONE", FrictionModel::SpreadKind::None)
      .value("PRICE_BPS", FrictionModel::SpreadKind::PriceBps)
      .value("VOL_TICKS", FrictionModel::SpreadKind::VolTicks);
  frictions.def(py::init<>())
      .def_readwrite("spread_kind", &FrictionModel::spread_kind)
      .def_readwrite("half_spread_bps", &FrictionModel::half_spread_bps)
      .def_readwrite("vol_tick", &FrictionModel::vol_tick)
      // C-4: the additive market-impact lane — a fraction of the mark, charged on
      // TOP of whichever `spread_kind` lane is selected (including NONE).
      .def_readwrite("impact_fraction", &FrictionModel::impact_fraction)
      .def_readwrite("per_contract_cost", &FrictionModel::per_contract_cost)
      .def_readwrite("hedge_slippage_bps", &FrictionModel::hedge_slippage_bps);

  py::class_<ShareDividend>(m, "ShareDividend")
      .def(py::init<>())
      .def(py::init([](std::uint32_t uid, std::int64_t ex_ts_ns,
                       double amount) {
             return ShareDividend{uid, ex_ts_ns, amount};
           }),
           py::arg("uid"), py::arg("ex_ts_ns"), py::arg("amount"))
      .def_readwrite("uid", &ShareDividend::uid)
      .def_readwrite("ex_ts_ns", &ShareDividend::ex_ts_ns)
      .def_readwrite("amount", &ShareDividend::amount);

  py::class_<FinancingConfig>(m, "FinancingConfig")
      .def(py::init<>())
      .def_readwrite("borrow_rate", &FinancingConfig::borrow_rate)
      .def_readwrite("finance_premium", &FinancingConfig::finance_premium)
      .def_readwrite("shares_carry", &FinancingConfig::shares_carry)
      .def_readwrite("initial_cash", &FinancingConfig::initial_cash)
      .def_readwrite("share_dividends", &FinancingConfig::share_dividends,
                     "Exact per-uid ex-date cash dividends for hedge shares.");

  // ── Snapshot cache ──
  py::class_<SnapshotCacheStats>(m, "SnapshotCacheStats")
      .def_readonly("loads", &SnapshotCacheStats::loads)
      .def_readonly("hits", &SnapshotCacheStats::hits)
      .def_readonly("prefetches", &SnapshotCacheStats::prefetches)
      .def_readonly("retained_entries", &SnapshotCacheStats::retained_entries)
      .def_readonly("evictions", &SnapshotCacheStats::evictions)
      .def_readonly("fast_build_loads", &SnapshotCacheStats::fast_build_loads)
      .def_readonly("reuse_only_fast_hits", &SnapshotCacheStats::reuse_only_fast_hits)
      .def_readonly("reuse_only_cold_resolutions", &SnapshotCacheStats::reuse_only_cold_resolutions);

  py::class_<SnapshotCache, std::shared_ptr<SnapshotCache>>(m, "SnapshotCache")
      .def(py::init<>())
      .def(py::init<std::size_t>(), py::arg("max_retained_entries"))
      .def("stats", &SnapshotCache::stats)
      .def("clear", &SnapshotCache::clear);

  // ── Run configuration ──
  py::class_<RunConfig>(m, "RunConfig")
      .def(py::init<>())
      .def_readwrite("price", &RunConfig::price)
      .def_readwrite("query_pricing_tier", &RunConfig::query_pricing_tier)
      .def_readwrite("frictions", &RunConfig::frictions)
      .def_readwrite("financing", &RunConfig::financing)
      .def_readwrite("record_every_n", &RunConfig::record_every_n)
      .def_readwrite("unpriced", &RunConfig::unpriced)
      .def_readwrite("snapshot_cache", &RunConfig::snapshot_cache)
      .def_readwrite("prefetch_snapshots", &RunConfig::prefetch_snapshots)
      .def_readwrite("prefetch_depth", &RunConfig::prefetch_depth)
      .def_readwrite("query_cache_build_policy", &RunConfig::query_cache_build_policy)
      .def_readwrite("surface_provenance_policy", &RunConfig::surface_provenance_policy)
      .def_readwrite("settlement_mark_memo", &RunConfig::settlement_mark_memo)
      .def_readwrite("reconcile_nav", &RunConfig::reconcile_nav)
      .def_readwrite("book_entry_fill_slippage",
                     &RunConfig::book_entry_fill_slippage)
      .def_readwrite("reconcile_nav_tol", &RunConfig::reconcile_nav_tol);

  // ── The result series ──
  py::class_<BacktestResult> result(m, "BacktestResult");
  result.def(py::init<>())
      .def("__len__", &BacktestResult::size)
      .def_property_readonly("size", &BacktestResult::size)
      .def(
          "resize",
          [](BacktestResult &r, std::size_t n) {
            r.date.resize(n);
            r.ts_ns.resize(n);
#define ATXVOL_RESIZE(name) r.name.resize(n)
            ATXVOL_RESIZE(pnl_total);
            ATXVOL_RESIZE(pnl_delta);
            ATXVOL_RESIZE(pnl_gamma);
            ATXVOL_RESIZE(pnl_vega);
            ATXVOL_RESIZE(pnl_vanna);
            ATXVOL_RESIZE(pnl_volga);
            ATXVOL_RESIZE(pnl_theta);
            ATXVOL_RESIZE(pnl_rho);
            ATXVOL_RESIZE(pnl_charm);
            ATXVOL_RESIZE(pnl_unexplained);
            ATXVOL_RESIZE(pnl_settlement);
            ATXVOL_RESIZE(pnl_shares);
            ATXVOL_RESIZE(financing);
            ATXVOL_RESIZE(cost);
            ATXVOL_RESIZE(nav);
            ATXVOL_RESIZE(cash);
            ATXVOL_RESIZE(gross_delta);
            ATXVOL_RESIZE(gross_gamma);
            ATXVOL_RESIZE(gross_vega);
            ATXVOL_RESIZE(gross_theta);
            ATXVOL_RESIZE(turnover_notional);
            ATXVOL_RESIZE(turnover_vega);
            ATXVOL_RESIZE(n_open_lots);
            ATXVOL_RESIZE(n_unpriced_lots);
            ATXVOL_RESIZE(n_unpriced_greeks);
#undef ATXVOL_RESIZE
          },
          py::arg("n"),
          "Size every row-parallel column to n (zero-filled). Call this before "
          "assigning columns on a hand-built result: the library's consumers "
          "index all columns by the row count, so a ragged result is invalid.")
      .def(
          "validate",
          [](const BacktestResult &r) { atxvol::python::require_consistent(r, "validate"); },
          "Raise ValueError if any column length disagrees with the row count.")
      .def_readwrite("date", &BacktestResult::date)
      .def_property(
          "ts_ns", [](const BacktestResult &r) { return to_numpy(r.ts_ns); },
          [](BacktestResult &r, const py::object &v) {
            r.ts_ns = py::cast<std::vector<std::int64_t>>(v);
          })
      .def_property_readonly("signals",
                             [](const BacktestResult &r) {
                               py::dict out;
                               for (const auto &[name, series] : r.signals) {
                                 out[py::str(name)] = to_numpy(series);
                               }
                               return out;
                             })
      .def(
          "to_dict",
          [](const BacktestResult &r) {
            py::dict out;
            out["date"] = py::cast(r.date);
            out["ts_ns"] = to_numpy(r.ts_ns);
#define ATXVOL_COL(name) out[#name] = to_numpy(r.name)
            ATXVOL_COL(pnl_total);
            ATXVOL_COL(pnl_delta);
            ATXVOL_COL(pnl_gamma);
            ATXVOL_COL(pnl_vega);
            ATXVOL_COL(pnl_vanna);
            ATXVOL_COL(pnl_volga);
            ATXVOL_COL(pnl_theta);
            ATXVOL_COL(pnl_rho);
            ATXVOL_COL(pnl_charm);
            ATXVOL_COL(pnl_unexplained);
            ATXVOL_COL(pnl_settlement);
            ATXVOL_COL(pnl_shares);
            ATXVOL_COL(financing);
            ATXVOL_COL(cost);
            ATXVOL_COL(nav);
            ATXVOL_COL(cash);
            ATXVOL_COL(gross_delta);
            ATXVOL_COL(gross_gamma);
            ATXVOL_COL(gross_vega);
            ATXVOL_COL(gross_vega_abs);
            ATXVOL_COL(gross_theta);
            ATXVOL_COL(turnover_notional);
            ATXVOL_COL(turnover_vega);
            ATXVOL_COL(n_open_lots);
            ATXVOL_COL(n_unpriced_lots);
            ATXVOL_COL(n_unpriced_greeks);
            ATXVOL_COL(step_pnl_total);
            ATXVOL_COL(nav_liquidation);
#undef ATXVOL_COL
            for (const auto &[name, series] : r.signals) {
              out[py::str(name)] = to_numpy(series);
            }
            return out;
          },
          // The roster above is hand-listed, so this text states what it does
          // NOT reach as well as what it does: shrinking the claim to fit would
          // hide the swap lane rather than route a reader to the one entry
          // point that emits it. Whoever adds an ATXVOL_COL owes this sentence
          // a read.
          "The recorded series plus the run's signals, as a dict of NumPy arrays "
          "(signals folded in by name).\n\n"
          "This is the binding's hand-listed roster, not every column of "
          "BacktestResult. The swap lane -- swap_pv, swap_pnl and the eight "
          "swap_explain_* P&L-attribution columns -- is not exposed here and is "
          "not reachable from Python. examples/varswap_compare_example.cpp is "
          "the C++ entry point that produces those columns, attaching them as "
          "signals before it writes its TSV.");

  ATXVOL_SERIES(result, pnl_total);
  ATXVOL_SERIES(result, pnl_delta);
  ATXVOL_SERIES(result, pnl_gamma);
  ATXVOL_SERIES(result, pnl_vega);
  ATXVOL_SERIES(result, pnl_vanna);
  ATXVOL_SERIES(result, pnl_volga);
  ATXVOL_SERIES(result, pnl_theta);
  ATXVOL_SERIES(result, pnl_rho);
  ATXVOL_SERIES(result, pnl_charm);
  ATXVOL_SERIES(result, pnl_unexplained);
  ATXVOL_SERIES(result, pnl_settlement);
  ATXVOL_SERIES(result, pnl_shares);
  ATXVOL_SERIES(result, financing);
  ATXVOL_SERIES(result, cost);
  ATXVOL_SERIES(result, nav);
  ATXVOL_SERIES(result, cash);
  ATXVOL_SERIES(result, gross_delta);
  ATXVOL_SERIES(result, gross_gamma);
  ATXVOL_SERIES(result, gross_vega);
  ATXVOL_SERIES(result, gross_vega_abs);
  ATXVOL_SERIES(result, gross_theta);
  ATXVOL_SERIES(result, turnover_notional);
  ATXVOL_SERIES(result, turnover_vega);
  ATXVOL_SERIES(result, n_open_lots);
  ATXVOL_SERIES(result, n_unpriced_lots);
  ATXVOL_SERIES(result, n_unpriced_greeks);
  ATXVOL_SERIES(result, step_pnl_total);
  ATXVOL_SERIES(result, nav_liquidation);

  m.def(
      "run_backtest",
      [](const Clock &clock, IStrategy &strat, const RunConfig &cfg) {
        py::gil_scoped_release release;
        return atxvol::python::unwrap(run_backtest(clock, strat, cfg));
      },
      py::arg("clock"), py::arg("strategy"), py::arg("config") = RunConfig{},
      "Run `strategy` over `clock`, returning the per-step BacktestResult series.");

  m.def("solve_ledger", [] { return solve_ledger_dict(counters::ledger::snapshot()); },
        "Return a merged point-in-time snapshot of the always-on solve counters.");
  m.def("reset_solve_ledger", &counters::ledger::reset,
        "Reset every solve counter; no pricing producer may run concurrently.");
}
