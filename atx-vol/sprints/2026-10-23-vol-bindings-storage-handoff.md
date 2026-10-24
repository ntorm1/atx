# Handoff — vol bindings + storage mini-sprint (2026-10-23 → 2026-10-24)

PM: Fable 5.1 (run_id `vol-bs-20261023`). Base `main @ a53c48c4`. Two file-disjoint lanes, Opus 5.5 implementers in pool-leased worktrees, light Sonnet reviews (diff read + owning suites re-run + one independent revert spot-check each). Integration branch `feat/vol-bindings-storage-20261023`; merge commit `d07775dd`. Sprint workspace (briefs, reports, review packages, reviews, PM ledger): `.superpowers/sdd/2026-10-23-vol-bindings-storage/`.

## What landed

### Lane SER @ 59087cd2 — surface archive / surface db (DONE_WITH_CONCERNS → APPROVE_WITH_MINORS)
Sprint note `2026-10-23-SER-sprint.md`; spec `docs/atxvsa2-format.md`; CHANGELOG "BREAKING — ATXVSA21 + ATXVDB02".
- Format break, no migration: partitions `ATXVSA21` (major 5), manifest `ATXVDB02` (major 2); `ATXVSA20` / `ATXVSA03` / `ATXVDB01` refused with a `ParseError` naming the magic. Rebuild surface dbs from hives (none existed outside rebuildable caches).
- `point_read_archive_surface` / `point_read_archive_reconstruct`: three positional reads (header, slot run, record), every touched byte CRC-checked, record bound to the query by the symbol it now carries. `SurfaceDb::map_surface` / `load_surface` serve through it; LRU partition cache deleted.
- Removed: prior-salt accept-list, zero-marker legacy provenance (explicit `prov_status`), ATXVSA03 declarations + `run_archive.cpp` recogniser (every run dir's identity hash changes; a run dir holding an older archive is refused), reserved-field reuse in the manifest.
- Fixed with failing-then-passing tests: republish under concurrent point readers on Windows (POSIX-semantics replace first); manifest lookups aliasing a >32-char query onto a 32-char record.
- Measured (6,000 symbols, 11.3 MB partition, vol-rel, unqualified, host 18 % CPU): cold `map_surface` 11,806 → 24.2 µs p50, 14,602 → 41.2 µs p99; bytes per read 11.58 MB / 2,826 calls → 2,214 B / 3 calls; whole board 12.3 → 7.6 ms; write 27.7 → 27.9 ms; warm repeated map 18.7 → 23.6 µs (cache-deletion cost, Ruling R6).
- Gates: `atx-vol-surface-archive` (94), `atx-vol-surface-db` (136), `atx-vol-fit-archive` (57) pass; hygiene over 10 TUs exit 0; every deliverable-5 call site `check`s clean.

### Lane PY @ 3964e733 — Python bindings (DONE_WITH_CONCERNS → APPROVE_WITH_MINORS)
Sprint note `2026-10-23-PY-bindings-sprint.md`; coverage `python/docs/binding-coverage.md`; CHANGELOG "NEW — Python bindings current with r5/r6".
- `-DATX_VOL_PYTHON=ON` (default OFF) builds `_core` in-tree (`atx-vol-python-core`) from the TU list shared with the wheel; `atx-vol-python` ctest then PASSES instead of skipping 77. Default-OFF trees still skip — configure the gating tree with the option on.
- New TUs `fit_archive.cpp` (ATXVFA reader / writer / series, zero-copy numpy views), `serve.cpp`, `served_analytics.cpp` (A1 Greeks / scenarios / attribution, `ServedSession` over `PricedSurface`), `region.cpp` (region classifier, D1 `DiscreteDivBatch`), quote-slice r5/r6 deltas incl. the C4 in-box rung record. 98 bound enums checked complete.
- Coverage of 109 public headers: 36 bound + 9 newly bound + 9 partial + 29 missing-product + 25 missing-internal + 1 n/a. Unbound by design / remainder: `common_model_analytics.hpp` and the `VolaSession` `ServedSession` overload (need `AmericanMarkovModel` / `VolaSession` bindings), `ServeBook` async rebuild callback, `AloRegionPricer`, `JointConflictKind` (integer).
- Gates: ctest `atx-vol-python` PASSED (787 passed / 225 environmental skips / 1 strict xfail); wheel built and imported, `__version__` 1.1.0 == metadata (501 s cold, 74 s incremental).

## Integration verification (pool-35, merged tree, `-DATX_VOL_PYTHON=ON`)
`check python/src/bindings/surface_db.cpp` against the new `SurfaceDb` API: exit 0. Build `atx-vol-python-core`, `atx-vol-surface-archive-tests`, `atx-vol-surface-db-tests`, `atx-vol-fit-archive-tests`: exit 0. `ctest -R "^atx-vol-(surface-archive|surface-db|fit-archive|python)$"`: 4/4 Passed (2.7 s / 3.4 s / 8.6 s / 114 s). No library file on the quote-bench fit path changed; parity hash not re-run (ruling).

## Deferred review findings (no fix round; USER RULING 2026-10-23 16:05 light gates)
SER (`review-SER-59087cd.md`): F-1 adversarial corpus not extended for the new regions (placement, S≤N, reserved bytes, `prov_status`, empty-slot form, hash≠symbol); F-2 report G5(b) lacks command/exit; F-3 `write_surface_archive_v2_file` (`surface_archive.cpp:1093-1106`) never checks the `ofstream` close/flush — a failed final flush publishes a short file (pre-existing; header/metadata CRC turns it into a `ParseError` at open, not a wrong surface); F-4 python edit uncompiled in-lane (closed at integration).
PY (`review-PY-3964e73.md`): M-1 stale CRLF golden TSVs in pre-existing worktrees (see trap below; fixed in `C:\atx` at integration); m-2 `test_binding_enum_parity.py` compares enumerator names only, not the Python label/value; m-3 strict xfail in `test_run_sp100_strangle_backtest.py` records an out-of-lane driver defect (ExcludeAndReport + default-on `reconcile_nav` aborts the gap corpus) that needs an owner; m-4 `served_analytics.cpp:3857` throws a bare `std::runtime_error` on a ragged scenario grid (unreachable today); m-5 `test_report_from_archive.py` sidecar writes a `friction_regime` the fixture docstring calls unrecoverable (nothing asserts it).

## Follow-ups (named, unowned)
1. ATXVFA per-section lazy-verify level (design paragraph in `docs/storage/ARCHIVE_FORMAT.md`); line audit of `fit_archive_records.cpp` / `fit_archive_series.cpp`.
2. `snapshot_cache.cpp` / `snapshot_pool.cpp` still probe the archive header through `std::ifstream` (can push a republish onto the retry path).
3. SER F-1 and F-3 above; stale comment `src/backtest/priced_surface_view.cpp:285` names the deleted `ArchivePricingRecord`.
4. Bind `AmericanMarkovModel` / `VolaSession` so `common_model_analytics.hpp` and the `VolaSession` `ServedSession` overload can follow; PY m-2..m-5.
5. Decide whether `vol-rel` presets used for gating should carry `ATX_VOL_PYTHON=ON` by default (today the Python suite gates only where the option is set).

## Traps recorded this sprint (also in `docs/LEDGER.md`)
- `tests/data/spx_wilmott_2019/.gitattributes` `-text` normalizes only future checkouts; a worktree checked out before it keeps CRLF copies of `figure1_fit.tsv` / `figure1_market.tsv` and `test_digitize_spx_wilmott` fails there. Refresh: delete the two files, `git checkout -- atx-vol/tests/data/spx_wilmott_2019/`.
- `scripts/atx-build.ps1` refuses with `WRONG-TREE GUARD` unless the shell's cwd is inside the tree whose script you call.
- Windows: a classic rename over a file readers hold open is refused even with delete sharing; POSIX-semantics replace succeeds (SER).
- vcpkg Arrow 24 and pyarrow 18 cannot share a process in either import order; the Python driver runs two processes (PY).
