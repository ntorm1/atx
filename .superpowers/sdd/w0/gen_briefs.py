"""Generate W0 lane briefs from the plan + findings docs (orchestrator tool, scratch only)."""
import pathlib
import re

ROOT = pathlib.Path(r"C:\atx-wt\pool-1")
PLAN = (ROOT / "docs/plans/2026-09-24-alpha-engine-production-swarm.md").read_text(encoding="utf-8").splitlines()
FIND = (ROOT / "docs/plans/2026-09-24-alpha-engine-review-findings.md").read_text(encoding="utf-8").splitlines()
OUT = ROOT / ".superpowers/sdd/w0"

E = "atx-engine/include/atx/engine"
S = "atx-engine/src"
I = "atx-impl/src"

LANES = [
    dict(id="o1", title="Integrate lane 6, equity-bench preset, red test", pool=7, branch="feat/w0-o1-l6",
         plan=(358, 369), ids=["R-14"], groups="risk", batch="W0a",
         targets=["atx-engine-risk-tests", "atx-impl-tests (+ atx-shm-worker)"],
         suites="the whole `atx-engine-risk-tests` (Nightly skipped) and the whole `atx-impl-tests`; "
                "new suites prefixed `ImplStageRunSmokeMalformed_*` / `RiskNightlyGate_*`",
         owned=["every file changed by `feat/qps-l6-optim` (merge it: `git -C C:\\atx-wt\\pool-7 merge --no-ff feat/qps-l6-optim`)",
                "`CMakePresets.json` (add `equity-bench`: inherits `equity-rel`, `ATX_BUILD_BENCH=ON`, `ATX_TEST_GROUPS=all`, own binaryDir `build-equity-bench`; keep the file's existing line endings — the CRLF/LF mismatch killed the qps Lane-0 attempt)",
                "the Nightly gate for `RiskQpAugment.MatchesDenseOracleAcrossBattery` (`atx-engine/tests/risk_qp_augment_test.cpp`; mechanism is your choice, e.g. `GTEST_SKIP()` unless env `ATX_NIGHTLY=1` — the test body must stay intact, not weakened; CMake edits allowed for this lane only)",
                "the minimal root-cause fix site for `StageRunSyntheticSmoke.SyntheticSmoke_OnFlagsProducesFiniteScorecard` (\"invalid stod argument\") — wherever it is (likely `atx-impl/src/config.cpp` or `stage_run.cpp`, owned by W0b lanes that start after you merge; keep the diff minimal and name the file in the report) + a regression test with the malformed input"],
         notes=["Use superpowers:systematic-debugging for the stod failure: reproduce, find root cause, then fix.",
                "Quiet-host bench baselines (L1 kernels, L2 WQ101 battery, L3 search, L6 modes 4/6/7) CANNOT be measured during W0a (6 lanes running). Your job: make `equity-bench` configure and build `atx-engine-bench` (or the lane-6 `optimizer_production_bench`) and prove each bench binary runs once with a tiny filter (smoke only, numbers not recorded as baselines). The orchestrator records quiet-host baselines at the W0 gate. Mark that acceptance item 'DEFERRED-to-gate (orchestrator)' in the report.",
                "Ledger line (R-14) is appended by the orchestrator only: put the exact proposed wording in 'Ledger candidates' — the lane-6 58 ms figure excludes costs and turnover.",
                "You merge FIRST into integration; R0 (risk) starts only after you merge. Do not edit `risk/factor_model.*` or `risk/exposures.hpp` (R0 owns them) beyond what the lane-6 merge itself brings."]),
    dict(id="a0", title="Alpha kernel correctness", pool=2, branch="feat/w0-a0",
         plan=(370, 391), ids=["A-01", "A-02", "A-03", "A-09", "A-13", "A-18"], groups="alpha;factory", batch="W0a",
         targets=["atx-engine-alpha-tests", "atx-engine-factory-tests"],
         suites="`AlphaCsRankTies_*`, `AlphaHumpWarmup_*`, `AlphaTypecheckScalarLiteral_*`, `AlphaFlatWindow_*`, "
                "`AlphaAuditExactParity_*`, `FactoryCanonCollision_*`; must stay green: whole alpha + factory "
                "targets including the VM↔oracle differential suites",
         owned=[f"`{E}/alpha/cs_ops.hpp`", f"`{E}/alpha/state_ops.hpp`",
                f"`{S}/alpha/typecheck.cpp` (+ `{E}/alpha/typecheck.hpp` if a declaration must change)",
                f"`{E}/alpha/oracle.hpp`, `{S}/alpha/oracle.cpp`",
                f"`{E}/factory/crossover.hpp`, `{S}/factory/crossover.cpp`",
                f"`{E}/factory/canonical.hpp`, `{S}/factory/canonical.cpp`",
                f"`{E}/alpha/ts_ops.hpp` — ONLY the flat-window guard and the AuditExact ts_sum/ts_mean routing",
                f"`{E}/alpha/vm.hpp` — ONLY the sites cited by A-02 (hump), A-03 (scalar operand reads) and A-13 (AuditExact ts_sum/ts_mean dispatch); W1-A1 owns the rest of vm.hpp",
                "existing tests that pin a cited defect (e.g. `atx-engine/tests/alpha/alpha_cs_test.cpp:256-272` pins A-01) and golden-digest tables that re-baseline"],
         notes=["Golden digests: produce the old→new table in the report, each row tied to A-01/A-02/A-09/A-13.",
                "`RankTies::OrdinalV1` must reproduce the old digests bit-exactly (prove it with a test)."]),
    dict(id="l0", title="Learn leakage", pool=3, branch="feat/w0-l0",
         plan=(452, 467), ids=["L-01", "L-02", "L-03", "L-07", "L-08"], groups="learn", batch="W0a",
         targets=["atx-engine-learn-tests"],
         suites="`LearnLabelMutationInvariance_*` (write it first as a probe — it fails on the base; that is expected, not TDD ceremony), "
                "`LearnLabelMaturity_*`, `LearnFoldLocalAug_*`, `LearnIcLossPerDate_*`; must stay green: whole learn target",
         owned=[f"`{E}/learn/tcn_alpha.hpp`, `{S}/learn/tcn_alpha.cpp`",
                f"`{E}/learn/nn/trainer.hpp`, `{S}/learn/nn/trainer.cpp`",
                f"`{E}/learn/nn/loss.hpp`, `{S}/learn/nn/loss.cpp`",
                f"`{E}/learn/latent.hpp`, `{S}/learn/latent.cpp`",
                f"`{S}/learn/linear_alpha.cpp` and `{S}/learn/gbt.cpp` — ONLY the fold-augmentation sites (L-03) and the trial-count sites (L-08); headers only if a declaration must change",
                f"`{E}/learn/feature_matrix.hpp` — label-horizon metadata only (W1-L1 owns the rest)"],
         notes=["L-08 is split: you own the learn-side trial counting (configurations, not folds × horizons) and IcLoss per-date batches; E0b owns registry-side trial accounting. The mislabelled autoencoder (L-08 tail) is W3-L4 — mark DEFERRED.",
                "Pool-3 was an alpha pool; reconfigure `-Groups \"learn\"` once."]),
    dict(id="d0", title="Data-layer PIT leak fixes", pool=4, branch="feat/w0-d0",
         plan=(392, 412), ids=["D-01", "D-02", "D-03", "D-04", "D-05", "D-06", "D-08", "D-09"], groups="data;alpha", batch="W0a",
         targets=["atx-engine-data-tests", "atx-engine-alpha-tests (augment only)"],
         suites="`DataLevelBasis_*`, `DataFinraLag_*`, `DataAdjustGap_*`, `DataAlignEvent_*`, `DataCorpActRebase_*`, "
                "`DataContextAsOf_*`, plus a `DataHistoryPanelFuturePerturb_*` suite for the future-perturbation "
                "invariance item; must stay green: whole data target + alpha augment tests",
         owned=[f"`{E}/data/history_panel.hpp`, `{S}/data/history_panel.cpp`",
                f"`{E}/alpha/augment.hpp` — ONLY the `dollar_volume`/`adv{{d}}`/`vwap` definition",
                f"`{E}/data/finra_short.hpp`, `{S}/data/finra_short.cpp`",
                f"`{E}/data/adjust.hpp`, `{S}/data/adjust.cpp`", f"`{E}/data/align.hpp`, `{S}/data/align.cpp`",
                f"`{E}/data/corporate_actions.hpp`, `{S}/data/corporate_actions.cpp`",
                f"`{E}/data/context.hpp`, `{S}/data/context.cpp`", f"`{E}/data/universe.hpp`, `{S}/data/universe.cpp`",
                f"`{E}/data/real_panel.hpp`, `{S}/data/real_panel.cpp`",
                "existing tests that pin a cited defect (e.g. `data_adjust_test.cpp:393` pins D-04)"],
         notes=["D-02 config default (`atx-impl/src/config.hpp` `si_publication_lag`) is delivered by I0b: write the exact new default + semantics in 'Integration notes'.",
                "D-06 is split: you own the share rebase + same-filed-date tie; the PIT shares source is W2-D3.",
                "D-01 consumer at `stage_equity_mine.cpp:1375` needs no edit if the engine definition is fixed; confirm in the report.",
                "Pool-4 was a factory pool; reconfigure `-Groups \"data;alpha\"` once."]),
    dict(id="e0a", title="Inference: HAC, block length, execution delay, caps", pool=5, branch="feat/w0-e0a",
         plan=(413, 431), ids=["E-02", "E-03", "E-08", "E-09", "E-15"], groups="eval;combine", batch="W0a",
         targets=["atx-engine-eval-tests", "atx-engine-combine-tests"],
         suites="`EvalHac_*`, `EvalIcCoverage_*`, `EvalIcDelay_*`, `EvalIcCaps_*` (+ `CombineHacTstat_*` for the combine sites); "
                "must stay green: whole eval + combine targets",
         owned=[f"new `{E}/eval/hac.hpp` (header-only)",
                f"`{E}/eval/cross_section_ic.hpp`, `{S}/eval/cross_section_ic.cpp`",
                f"`{S}/combine/signal_combiner.cpp` — t-stat sites only",
                f"`{S}/combine/orthogonalize.cpp` — the `marginal_ic` t-stat only",
                f"`{E}/combine/signal_store.hpp` — the winsor fix only (E-15)"],
         notes=["NW t must match a statsmodels fixture to 1e-8: check `python -c \"import statsmodels\"`; if present, generate the fixture with a committed script under `atx-engine/tests/eval/fixtures/` (no network installs). If absent, say so and use an independent reference implementation of the identical statsmodels formula (`cov_type='HAC'`, Bartlett kernel, `use_correction` as documented) — mark the item PARTIAL and flag it.",
                "E-09 consumer wiring at `atx-impl/src/stage_equity_ic.cpp:112-113` belongs to I0b (W0b): expose `execution_delay` (default 1) and write the integration note.",
                "Old streams must reproduce under `BlockLenRule::V1` (prove with a test)."]),
    dict(id="e0b", title="Trial accounting", pool=6, branch="feat/w0-e0b",
         plan=(432, 451), ids=["E-01", "E-16", "E-17", "L-08"], groups="eval", batch="W0a",
         targets=["atx-engine-eval-tests"],
         suites="`EvalTrialClusters_*`, `EvalRegistryWindows_*`, `EvalLockboxEmbargo_*`; must stay green: whole eval target",
         owned=[f"new `{E}/eval/trial_clusters.hpp` (header-only, or implemented inside `{S}/eval/trial_registry.cpp`; no new .cpp)",
                f"`{E}/eval/deflated_sharpe.hpp`", f"`{E}/eval/trial_registry.hpp`, `{S}/eval/trial_registry.cpp`",
                f"`{E}/eval/lockbox.hpp`",
                "the existing test `RegistryFedDsrIsLessOverDeflatedOnCorrelatedTrials` (replace it with a test asserting the correct behaviour — this is the cited E-01 pin, not a weakening)"],
         notes=["E-16 recording side (`stage_equity_mine.cpp:742`) is I0b (W0b): expose the registry API (window [start,end], fidelity, family/theme tags, variable pnl_len, OOS/IS flag) and write the integration note.",
                "L-08 here = registry-side trial accounting API (count configurations); the learn-side count sites are L0's.",
                "Registry format change: keep reading old logs (versioned record format) — cp14–cp22/L9/L10 sidecars are imported in W1-I1."]),
    dict(id="r0", title="Risk estimator blockers", pool=8, branch="feat/w0-r0",
         plan=(468, 483), ids=["R-03", "R-04", "R-05", "R-06"], groups="risk", batch="W0b",
         targets=["atx-engine-risk-tests"],
         suites="`RiskFactorModelPit_*`, `RiskSectorColumnsById_*`, `RiskThinNameFloor_*`; must stay green: whole risk target (Nightly skipped)",
         owned=[f"`{E}/risk/factor_model.hpp`, `{S}/risk/factor_model.cpp`", f"`{E}/risk/exposures.hpp`"],
         notes=["Start by merging `feat/w0-integration` (it contains O1/lane 6).",
                "No sanitizer build exists in this repo (`.agents/cpp/agent.md` §8), so 'run under UBSan/ASan' cannot be done literally. Prove no OOB with the Debug build's checked STL (`_ITERATOR_DEBUG_LEVEL=2`, `/RTC1`) plus explicit index assertions in the test, and mark the item 'MET-substitute (no sanitizer preset)' for the owner to accept or waive.",
                "Existing risk tests that encode contemporaneous exposures (R-03) may change expectation — list each with its defect ID."]),
    dict(id="b0", title="Replay correctness", pool=9, branch="feat/w0-b0",
         plan=(484, 498), ids=["B-02", "B-03", "B-04", "B-05"], groups="book", batch="W0b",
         targets=["atx-engine-book-tests", "atx-impl-tests (+ atx-shm-worker)"],
         suites="`BookReplayDelay_*`, `BookReplayDelist_*`, `BookBorrowSingleCount_*`, `BookLegacyReport_*`; must stay green: whole book target + atx-impl-tests",
         owned=[f"`{E}/book/replay.hpp`, `{S}/book/replay.cpp`", f"`{E}/book/borrow_schedule.hpp`", f"`{E}/book/report.hpp`",
                f"`{I}/stage_report.cpp` — EXCEPT the diagonal-risk call site near line 499 (I-04), which I0a owns in W0"],
         notes=["B-02 is split: engine rejection of `execution_delay=0` unless `allow_same_close` is yours; the CLI flag `--allow-same-close` and equity-mine sites are I0b's. Name the engine field `allow_same_close` so I0b can wire it.",
                "'PCS 2013-05-01 fixture': grep the tests for an existing PCS/MetroPCS fixture; if none exists, build a synthetic fixture reproducing that event (name leaves the panel mid-run without delisting evidence) — no real-data reads.",
                "Pool-9 is configured for book;risk;combine — reconfiguring is optional."]),
    dict(id="i0a", title="Pipeline look-ahead fixes: discover, combine, optimize, metabook", pool=10, branch="feat/w0-i0a",
         plan=(499, 518), ids=["I-01", "I-02", "I-03", "I-04", "I-06", "I-07", "I-08", "R-12"], groups="(n/a — atx-impl)", batch="W0b",
         targets=["atx-impl-tests (+ atx-shm-worker)"],
         suites="`ImplNestedSplits_*`, `ImplCombineNoHoldoutRead_*`, `ImplOptimizePit_*`, `ImplDeadAlpha_*`, `ImplMetabookUsesCombo_*`; must stay green: whole atx-impl-tests",
         owned=[f"`{I}/stage_discover.cpp`", f"`{I}/stage_run.cpp`", f"`{I}/stage_combine.hpp`, `{I}/stage_combine.cpp`",
                f"`{I}/stage_optimize.cpp`", f"`{I}/stage_metabook.hpp`, `{I}/stage_metabook.cpp`",
                f"`{I}/dead_alpha_wire.hpp`", f"`{I}/diag_risk.hpp`",
                f"`{I}/stage_report.cpp` — ONLY the diagonal-risk call site near line 499 (I-04); B0 owns the rest"],
         notes=["Do NOT edit `atx-impl/src/config.*` or `dispatch.*` (I0b owns them in W0). New knobs live in stage-private structs with safe defaults; W1-I1 makes stage-private configs reachable from the config file. Record any needed flag in 'Integration notes'.",
                "Combine memory (I-05) is W3-I6 — out of scope."]),
    dict(id="i0b", title="Equity stages and config hygiene", pool=11, branch="feat/w0-i0b",
         plan=(519, 540), ids=["D-12", "I-10", "I-11", "I-12", "I-15", "I-16", "I-17", "I-23", "E-18", "B-02", "D-02"],
         groups="(n/a — atx-impl)", batch="W0b",
         targets=["atx-impl-tests (+ atx-shm-worker)"],
         suites="`ImplConfigBool_*`, `ImplConfigFinite_*`, `ImplIcAsOfMembership_*`, `ImplMineRequiresMembership_*`, `ImplPendingOrder_*`, `ImplDelayGuard_*`; must stay green: whole atx-impl-tests",
         owned=[f"`{I}/config.hpp`, `{I}/config.cpp`", f"`{I}/dispatch.hpp`, `{I}/dispatch.cpp`",
                f"`{I}/stage_equity_ic.hpp`, `{I}/stage_equity_ic.cpp`",
                f"`{I}/equity_baseline_views.hpp`, `{I}/equity_baseline_views.cpp`",
                f"`{I}/stage_equity_baseline.cpp`, `{I}/stage_equity_book.cpp`",
                f"`{I}/stage_equity_mine.hpp`, `{I}/stage_equity_mine.cpp` — mask, delay, recording and pending-order edits only",
                f"`{I}/replay_report.cpp` — ONLY the I-11 default-cost site (no other W0 owner)"],
         notes=["Merge `feat/w0-integration` first: it carries E0a (`execution_delay`), E0b (registry windows / OOS flag), D0 (FINRA lag semantics) and O1. Wire them: E-09 delay in `stage_equity_ic` (default 1); E-16 registry fields in equity-mine recording; `si_publication_lag` default aligned with D0's integration note.",
                "B0 runs concurrently and adds the engine field `allow_same_close`; you own the CLI `--allow-same-close` and reject delay < 1 at config/stage level without it. If B0 has merged when you pre-merge, pass the flag through to the replay config.",
                "stage_run.cpp belongs to I0a — enforce 'report costs mandatory' (I-11) in config validation and `replay_report.cpp`, not in stage_run.cpp.",
                "Year-union → as-of membership (D-12): with constant membership the output must be identical to the year-union path (prove with a test)."]),
]


def findings_rows(ids):
    rows = []
    for fid in ids:
        pat = re.compile(r"^\|\s*" + re.escape(fid) + r"\s*\|")
        hit = [ln for ln in FIND if pat.match(ln)]
        rows.append(hit[0] if hit else f"| {fid} | (row not found) |")
    return rows


HEADER_ROW = "| ID | Sev | Location | Problem | Lane |\n|---|---|---|---|---|"

for ln in LANES:
    lid = ln["id"]
    a, b = ln["plan"]
    section = "\n".join(PLAN[a - 1:b - 1]).rstrip()
    body = f"""# Lane W0-{lid.upper()}: {ln['title']}

Host tag: **RAM:light** · Batch: **{ln['batch']}** · Pool: **`C:\\atx-wt\\pool-{ln['pool']}`** ·
Branch: **`{ln['branch']}`** (run id `aes-w0-{lid}`) · Base: the W0 base commit (head of
`feat/w0-integration` at lease time; see `progress.md`).

Read first: `.superpowers/sdd/w0/RULES.md` (binding), then this brief, then the plan and findings
docs under `docs/plans/` (all inside your pool).

## Goal (plan §7, verbatim)

{section}

## Cited findings rows (verbatim; every ID must end CLOSED or explicitly DEFERRED in your report)

{HEADER_ROW}
""" + "\n".join(findings_rows(ln["ids"])) + f"""

## Scope

- Files in scope (the ONLY files you may modify; new test files per RULES §2 are always allowed):
""" + "\n".join(f"  - {o}" for o in ln["owned"]) + f"""
- Files forbidden: everything else — in particular files owned by other W0 lanes (see
  `progress.md` ownership table). Needs elsewhere → report "Integration notes".

## Gate closure

- Test groups for `build-equity`: `{ln['groups']}` (reconfigure only if the tree differs).
- Owning targets: {', '.join('`' + t + '`' for t in ln['targets'])}.
- Suites: {ln['suites']}.
- Anchored runs: `-Ctest -Preset equity-dev -R '^<Suite>'` per suite; whole owning executable once
  before review (`--gtest_brief=1`).

## Done criteria

Every plan **Accept** item above MET with a named test and pasted evidence; every cited ID CLOSED
or DEFERRED with reason; owning targets green; tree clean; report committed at
`.superpowers/sdd/w0/lane-{lid}-report.md`.

## Lane notes (orchestrator)

""" + "\n".join(f"- {n}" for n in ln["notes"]) + """

## Out of scope

Anything the plan assigns to W1+ lanes; any real-data run; any CMake/preset edit (except O1);
refactors not required by a cited defect.
"""
    (OUT / f"lane-{lid}-brief.md").write_text(body, encoding="utf-8", newline="\n")
    print(lid, len(body.splitlines()), "lines")
