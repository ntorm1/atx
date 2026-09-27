# W1-I1 preparation: existing evidence and integration gaps

Docs-only inventory after G0 completion, 2026-09-25. No production edits, new
measurements or registry writes. Source inspected: pool3 `08cb4ae3` (production
`c32df951`); root may have subsequent integration changes. MCP graph tools were
unavailable, so code discovery used targeted `rg`.

G0 is complete at `08cb4ae37381e294d6c5129b599f2e13c45881a7`. Its immutable evidence is
`C:/atx-wt/g0-data/bc5cc646_20260925`, final manifest SHA256
`3e2fd328a15c6671d81aff9aa2012388aad924e995b6b8185591259f117da679`.
The report is `.superpowers/sdd/w0/lane-g0-codex-report.md`: 35 measurements,
54 comparison tables / 2,027,028 metric+metadata cells, no alpha promoted. This
preparation note is deliberately outside that published artifact bundle.

## Prior sidecars and declared counts

All paths below are relative to `atx-engine/reviews/`; each listed JSONL has its
matching `.manifest.json` sidecar. Counts are directly inspected, not inferred from
filenames. A run pair means one pre-registration and one completed line.

| JSONL basename | Lines / run pairs | Stored checkpoint | Declared N per pre-line | Historical increment |
|---|---:|---|---:|---:|
| trial-ledger | 4 / 2 | 14 and 15 | 30 and 0 | 30 |
| trial-ledger-cp16-restrictions | 26 / 13 | **14** | 30 | **0**, repeated restrictions |
| trial-ledger-cp17-families | 26 / 13 | 17 | 140 | 140 |
| trial-ledger-cp18-batch2 | 26 / 13 | 18 | 140 | 140 |
| trial-ledger-cp19-floor | 26 / 13 | 19 | 20 | 20, unfloored measured attempt |
| trial-ledger-cp19-floor-r2 | 26 / 13 | 19 | 20 | 20, corrected floored universe |
| trial-ledger-cp20-batch3a | 2 / 1 | 20 | 140 | 0, NaN-only attempt |
| trial-ledger-cp20-batch3a-r2 | 26 / 13 | 20 | 140 | 140 |
| trial-ledger-cp21-batch3c | 26 / 13 | 21 | 80 | 80 |

Nine ledgers contain 188 lines / 94 run pairs but only 79 distinct textual trial IDs.
Neither number is a trial N. IDs collide across retry/sidecar histories; import keys
must bind source ledger/hash and distinguish attempt identity from configuration
identity. Preserve failed and superseded attempts, including cp19's separately
measured universe. The cp20 failed-looking attempt is actually `completed` in the
ledger; its exclusion from new N comes from the recorded NaN-only experiment ruling,
not its terminal status.

The historical cumulative reconciliation is
`30 + 140 + 140 + (20 + 20) + 140 + 80 = 570` through cp21.
Sources: iteration16 design R16-2/R16-13b; cp17/18/19/20/21 scorecards in this same
reviews directory, especially their N notes (cp19 R19-4; cp20 R20-3/R20-4; cp21 R21-2).
Cp16's 13 repeated declarations sum to 390 mechanically but its design expressly says
that is not N. Do not sum per-cell declarations or count year/cut restrictions again.

Completeness gaps: no cp22 ledger was found in this tracked catalog. Production
currently has 29 families, retained26, checkpoint22: the three extra FINRA `si_shares`
families cannot run on the frozen cp21 contexts. Treat cp22 as unresolved declared/run
provenance until its source evidence is located; do not invent three successful trials
or mark the cp14-cp22 import complete. Cp15 is in the canonical ledger; a distinct
cp16 checkpoint value is intentionally absent because it used the frozen cp14 binary.

Byte-chain observation is **pool3-specific**: these nine working copies at08cb4ae3
contain CRLF and fail direct byte-chain checks; the exact git blobs contain LF and
all nine verify previous-line hashes, sidecar head and line count. Root independently
reports all nine `i/lf w/lf attr/text eol=lf` under existing `.gitattributes`.
Importer may consume a validated LF checkout or an exact bound git blob. It must
never silently normalize hash-bound history or regenerate a sidecar to make a check
pass. No repository-wide export/rewrite is needed.

## L9, L10 and G0 inputs for E2

- Historical L9: `C:/atx/data/equity_mine_l9_guard_20260923/{manifest.json,gate_report.json,
  candidates.csv,trial_registry.bin}`. 2,243 candidates, 2,065 scored records; binary
  magic `ATXTRG01`, calendar1008, registry hash `43add7981a12592d`, descriptive
  N_eff5.714134683. Candidate CSV includes origin, sign, DSL, scored flag and train
  metrics. Use the guarded run, not the superseded unguarded headline.
- G0 L9: evidence-root `data/equity_mine_l9_guard_g0_bc5cc646/` contains2,504 candidates,
  2,306 scored V2 records. Calendar1510/train-window1008, all2,306 InSample; chain
  `14faf8293000bf69`; cluster-N56, descriptive N_eff6.750301476. Changed search digest
  means the old and new candidate populations are not identical. Import both histories
  and compute configuration overlap; do not blindly add counts or overwrite old trials.
- Historical L10: `C:/atx/data/equity_fund_zoo_ic_l10v2_20260923/` has manifest,
  alignment, annual IC, pooled and split CSVs. Manifest declares120 trials from60 zoo
  lines, horizons5/21; `ref_momentum_12_1` is explicitly not a trial. This directory
  has **no binary trial registry or per-trial PnL series**, and no producer hash. The
  G0 rerun at evidence-root `data/equity_fund_zoo_ic_l10_g0_bc5cc646/` preserves the
  same120 declarations; it does not manufacture missing legacy cluster inputs.
- G0 cp21: evidence-root `ledger/trial-ledger-g0-cp21.jsonl` plus sidecar, 13 measured
  cells. Exact old/new signals and cost recipes match; overall frozen N stays570.
  Numeric checkpoint21/N80 is authoritative here; the diagnostic pin retained the
  production `iteration22-cross-section-ic-` ID prefix/prose. Bind the saved pin patch,
  producer SHA and manifests instead of treating those strings as a cp22 search.

R-2 requires cumulative N for DSR and epoch-only N beside it. Report imported unique
configuration count, new E2 unique count, their union, scored/failed/incomplete counts,
and cluster input coverage separately. The simple declared sum570+2065+120=2755 is a
reconciliation starting point, **not a proven deduplicated or cluster N**. G0 novel
configurations and incomplete cp22 provenance still need explicit accounting.

`TrialRegistry` in `eval/trial_registry.hpp` already supplies V1/V2 readers, durable
chain anchors, configuration dedup, family/theme tags, windows, fidelity and IS/OOS.
Dedup key is `(TrialKind, config_hash)`, not the metadata. PnL calendars must be
compatible; old V1 calendar1008 and G0 V2 calendar1510 sketches cannot simply be
concatenated. Recover aligned return series or a justified compatible sketch mapping;
never fabricate zero PnL or silently drop legacy declarations because clustering data
is missing. Cluster-N uses `accounting()` and E0b's cluster/MC rule; N_eff remains a
diagnostic and cannot replace clusters or be combined with cross-trial variance again.

## Runtime pre-registration and stage boundaries

The runtime file needs version/epoch, stable family ID/name, DSL+hash, explicit sign,
theme, horizon list, variant/restriction choices, declared-count rule and retained/
remeasured lineage. Canonicalize/hash once, bind it to the run manifest, and validate
duplicates, signs, horizons and declared totals before registering or computing.
Signs/horizons and genuinely selected parameter alternatives must participate in
configuration identity. A measurement-only repeat may point to an existing trial;
it must retain its own attempt receipt. Expected acceptance: add a family without
rebuilding; reproducible declared totals; a single no-override guard.

Current integration inventory (source paths relative to `atx-impl/src/`):

| Stage / layer | Existing artifact or accounting | Gap for W1-I1 |
|---|---|---|
| equity-mine | Per-output V2 `trial_registry.bin`, chain in manifest; cluster DSR | Opens its own registry at `out/trial_registry.bin`; no shared E2 handle |
| equity-ic / equity-universe | Hash-chained JSONL pre-registration and manifests | Separate ledger path; no TrialRegistry handle; IC families/counts compile-time |
| equity-baseline / equity-book | Bound manifests and nested replay report | No shared registry/prereg identity; duplicated guards |
| discover / sweep | `_manifest.txt`, store `_manifest.bin` cumulative counter | No shared TrialRegistry; legacy snapshot failure can silently lose count |
| combine | Artifact outputs; DSR call at stage_combine.cpp:209 uses pool size `na` | No shared registry/cluster-N; sweep alternatives need registration |
| optimize / metabook / regime | Stage/artifact outputs | No shared registry handle or common run envelope located |
| load / panel / report | Ingestion/context/replay artifact manifests | Artifact identity is not the complete config/git/exe/seed/input run envelope |
| run | Folds six stage digests and returns KVs (`stage_run.cpp` tail) | No complete run manifest or shared registry propagated through stages |

`RunConfig` currently exposes `equity_trial_ledger` only; no shared epoch/prereg handle.
The dispatcher routes stages but owns no common manifest/registration transaction.
Keep existing specialized artifact manifests; add a uniform envelope with config hash,
source/executable hash, seeds, input hashes, prereg hash, registry epoch/chain anchor,
status and evidence eligibility. Parent must bind child receipts before publishing the
completed manifest. Preserve G0's failed/assumed-liquidation qualifications in that
envelope. Wire the planned stage-private MetaBook, Combiner, RiskModel,
EquityAllocation and Mine configs through config-file loading.

This is preparation, not an import or acceptance claim. No registry was opened with
a potentially mutating repair API, no history was rewritten, and no sealed data read.
