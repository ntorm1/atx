# Lane PY: the atxvol Python bindings on HEAD (2026-10-23 mini-sprint)

Branch `feat/vol-PY-bindings-20261023` off `main @ a53c48c4`; report
`.superpowers/sdd/2026-10-23-vol-bindings-storage/lane-PY-bindings-report.md`.

## What and why

The Python suite had gated nothing for weeks. The `atx-vol-python` ctest exited SKIP 77 in
every tree because the monorepo never built `_core` (LEDGER L199, L560). The only compiled
extension on the box was a stale `0.1.0` editable install. The r5/r6 sprints had added
about 3,500 lines of public API (serving records, the serve book, A1 served analytics, the
exercise-region classifier, D1's discrete-dividend batch) and the ATXVFA fit-archive
reader, and none of it was reachable from Python.

1. **In-tree build, the durable gate.** `-DATX_VOL_PYTHON=ON` (default OFF) builds `_core`
   as `atx-vol-python-core` from one TU list shared with the wheel
   (`python/cmake/atxvol_core.cmake`). It stages the module and its vcpkg DLL closure beside
   `python/src/atxvol`, and the ctest runs with `--expect-core`: a missing or stale module
   is red, never Skipped. The driver runs the suite in two processes, because vcpkg Arrow
   24.0.0 and pyarrow 18.0.0 cannot share one in either import order.
2. **Compile and enum currency.** Every bound enum now lists every enumerator its header
   declares. The drift found: `ErrorCode` `Cancelled` / `SwapFixingScheduleViolation`,
   `FrictionModel::SpreadKind::QuoteSide`, `LifecycleSpec::Holding::FixedExpiryRestrike`.
   This is pinned two ways. A static source gate (`test_binding_enum_parity.py`, 98
   qualified enums, including those without a C++ `to_string`) compares each binding
   against its header. A runtime reflection gate (`_core._enum_names`, every enum with a
   `to_string`) compares each Python enum against the C++ names.
3. **New bindings** (4 TUs + the quote-slice deltas, which include the repair /
   box-certificate config and lane C4's in-box projection record). `fit_archive.cpp`: the ATXVFA reader,
   writer and series, with zero-copy read-only numpy views anchored to the archive and a
   typed `FitArchiveError(AtxError)`. `serve.cpp`: serving records, the book, and
   `serve_chain` columns carrying every `ServedChainStatement` field.
   `served_analytics.cpp`: hedge Greeks, scenarios, attribution and dated-cash scenarios,
   each with its declaration, plus `ServedSession` over a `PricedSurface`. `region.cpp`:
   the classifier, classified Andersen-Lake and `DiscreteDivBatch`.
4. **Coverage matrix** `python/docs/binding-coverage.md`: 109 headers = 36 bound + 9 newly
   bound + 9 partial + 29 missing-product + 25 missing-internal + 1 n/a.

## Numbers

- `atx-vol-python` ctest at 62db7d01: PASSED in 126 s. In-process partition (23 modules):
  310 passed, 8 skipped, 1 xfailed (a recorded sp100 driver/engine defect). Child partition
  (21 modules): 477 passed, 217 skipped. All 225 skips are environmental: 198 quote-bench
  CLI not built,
  18 Markov-audit CLI not built, 7 dispersion e2e fixture absent, 1 QuantLib, and 1
  in-tree metadata check (that one runs and passes under the wheel).
- New tests: 32 across `test_fit_archive.py` (7), `test_region.py` (5), `test_serve.py`
  (6), `test_served_analytics.py` (6), `test_served_session.py` (3) and
  `test_quote_slice_repair.py` (5), plus 2 static enum gates and 6 reflection gates.
- Wheel: `pip wheel` 501 s at `-j 4` (cold wheel tree; 74 s incremental after the last
  binding commit). `atxvol-1.1.0-cp312-cp312-win_amd64.whl` imported with
  `__version__ == 1.1.0 ==` metadata.
- No library file changed, so the quote-bench parity hash is N/A.

## Not done (missing-product, in priority order)

The `VolaSession` overload of `ServedSession::bind` and the common-model European outputs
(`analytics/common_model_analytics.hpp`): both need an upstream object Python cannot build
(`VolaSession`, `AmericanMarkovModel`). The Markov fit and pricer come first. The other
missing-product rows are listed in the coverage doc.
