# atx-vol CI gates (Task E3)

No `.github/workflows`, `ci/` directory, or other CI runner configuration
existed anywhere in this repo when this task landed (checked: no `.github/`,
no top-level `ci/`, nothing CI-shaped under `scripts/`). These four gates —
the sprint's Step 2 list — are therefore plain, locally-invocable PowerShell
scripts, runnable by hand today and wireable as build steps into whatever CI
runner shows up later.

```
powershell.exe atx-vol/ci/run_all_gates.ps1
```

runs all four and prints a summary; each can also be run standalone. All four
default to the `dev` (Debug) preset — these are correctness gates (bit-exact
NAV, bit-exact thread-invariance, a clean link), not perf claims, so `dev`/
`rel` is the right tier per the sprint's own constraint ("Correctness gates
run on dev/rel presets; perf claims only from rel-avx2 on a quiet host").

| Gate | Script | What it checks | Fails closed on |
|---|---|---|---|
| Determinism | `determinism_gate.ps1` | `n_threads=1` vs `n_threads=N` bit-identical results (`bits_equal`, memcpy-into-uint64 comparison — the "memcmp" the sprint plan names), over three existing `BacktestExec.*_Deterministic`/`*ComposeAtScale` gtest cases (B1-B5 composed paths) | build failure, gtest failure, or the filter matching anything other than exactly 3 tests (a rotted filter) |
| Golden replay + economics tripwire | `golden_replay_gate.ps1` | Replays the pinned 82-session SPY corpus through `atxvol_spy_dispersion_backtest run-surface-backtest` and compares `final_nav` bit-for-bit against `golden_pin.hpp`'s `kGolden82SessionFinalNav` | corpus not found (real market data, never checked into git — see below), build failure, or a NAV mismatch |
| `ATX_VOL_LAKEHOUSE=OFF` link | `lakehouse_off_link_gate.ps1` | The minimal-install configuration (no Parquet track store / `track_compact` CLI) still configures, links, and runs — via a dedicated `dev-lakehouse-off` CMake preset (own `build-lakehouse-off/` dir, never touches the warm `dev` `build/`) | configure/build/link failure, `track_compact` reappearing in the OFF ninja graph, or the resulting binary failing to run |
| 8-thread pool soak | `pool_soak_gate.ps1` | `BacktestExec.SnapshotPoolConcurrentRunsMatchSerial` (C2): 8 concurrent `run_backtest` calls sharing one `SnapshotPool`, 20 repeats, bit-identical to the serial baseline every repeat | build failure, gtest failure, or the filter not matching exactly 1 test |

## The golden-replay gate's fail-closed contract

The real 82-session SPY corpus is real market data. It is **not** checked
into any git worktree — confirmed absent from every candidate path this gate
(and `tests/track_key_test.cpp`'s `find_golden_82_session_corpus_root`, which
this gate's search mirrors) checks. Running `golden_replay_gate.ps1` with no
further setup on a fresh checkout will therefore **fail loudly** ("corpus not
found... refusing to pass vacuously") rather than silently pass — this is
deliberate, per the sprint controller's explicit instruction that this gate
must never vacuously pass.

To actually exercise the tripwire (e.g. on a dev host that happens to have
the corpus cached locally), point the gate at a `run-surface-backtest`-shaped
directory (`run_spec.tsv` + `definitions.tsv` + `universe_schedule.tsv`,
see `atx-vol/tools/spy_dispersion_backtest.cpp`):

```
$env:ATX_VOL_GOLDEN_82_SESSION_CORPUS = "C:/atx-data/spy-dispersion/runs/bt-sota-baseline"
powershell.exe atx-vol/ci/golden_replay_gate.ps1
```

This was verified end-to-end during Task E3 on a host with that corpus
present: a clean run PASSES bit-for-bit; a deliberately mismatched pin FAILS
with the tripwire message; a missing corpus FAILS closed. See
`task-E3-report.md` for the transcripts of all three.
