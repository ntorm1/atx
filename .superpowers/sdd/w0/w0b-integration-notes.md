# W0b integration notes (from the W0a lane reports) — binding for R0, B0, I0a, I0b

Each lane picks up only the notes addressed to it. Details are in the named `lane-<id>-report.md`
(present in your tree once you have merged `feat/w0-integration`).

## I0b — `stage_equity_ic.cpp` (from E0a, `lane-e0a-report.md`)
- New eval defaults: execution_delay 1, `TwoHorizonV2`, `HansenHodrickV1`. The old behaviour is
  `HalfHorizonV1` with delay 0 — keep it reachable behind the versioned enums.
- `common_sample_dates` must subtract `eval::label_embargo(maxH, delay)`.
- Update the `kAlignment` (:112) and `block_len_rule` (:334) strings.
- **Grant:** the `atx-impl/src/trial_ledger.hpp:89-90` hunk (the same alignment/block-length
  strings) is granted to I0b (no other W0 owner).

## I0b — `stage_equity_mine.cpp` (from E0b, `lane-e0b-report.md`)
- Set `pnl_len` to the train+validation calendar (:1739).
- Record trials with `TrialMeta{window, fidelity, InSample, family, theme}` (:742).
- Take cluster-N DSR through the `TrialAccounting` overload.
- Export `chain_head()` to the report and manifest.

## I0b — config (from D0, `lane-d0-report.md`)
- `si_publication_lag = 7` NYSE sessions, with `FinraLagRule::NyseSessionsV2` and
  `after_close=true`. The int overload still means calendar days.

## Registry (from L0) — all stages that record learned-model trials
- A learned model's `trial_count` is now 1 per configuration (`TrialCountRule::PerConfigurationV2`).

## Not for W0b (recorded for later waves)
- Search driver (from A0, owner W2-A4): `search_driver.cpp` must call
  `CanonSet::insert/contains(h, form)` and key `fitness_cache` by (hash, form).
- CRLF hazard (from O1): `atx-engine/reviews/trial-ledger.jsonl` checks out CRLF under
  `core.autocrlf=true`; `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies` fails from the
  repo root. Fix planned at the W0 gate (`.gitattributes` `*.jsonl text eol=lf`). If that test
  fails in your whole-executable run for this reason only, record it as pre-existing, not a
  lane failure.
- E0a combiner-level V1 (per-combiner `IidV1`/`RawV1`, horizon-aware lag floor): W2-E3.

## R0
- Start by merging integration, which contains lane 6 (O1).
