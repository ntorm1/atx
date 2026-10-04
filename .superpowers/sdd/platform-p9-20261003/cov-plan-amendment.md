# COV plan amendment (proposal; the PM rules it into `docs/plans/2026-10-03-p9-sprint-plan.md`)

Source: `cov-design.md` (same directory) and `briefs/brief-COV.md`. Rulings it builds on: COV-1, COV-2
(`progress.md:176-177`). Every block below is the exact text to paste at the named place; "after row X" means a new
table row directly below row X. Arithmetic is marked [arith]; nothing here was measured.

Summary of what the PM is asked to rule: (1) lane COV in wave 2 (one pool, L, slot 9); (2) contract K-P9-12;
(3) DEC-21; (4) gate G-P10; (5) the zero-trial diagnostic D-COV and its trial count (0 proposed); (6) cell P9-V
(N_c +1) and, only if COV-MV is funded, cell P9-MV (N_c +1), with the new ceiling; (7) whether to fund the optional
wave-3b lane COV-MV (default: no, P10).

---

## A. §0.4 Production gates: the platform (new row after G-P9)

```
| G-P10 risk model PIT + container | the covariance of record is point in time and its file is verified: `CovTruncation.*` and `RiskCovContainer.*` green on every build tag; root's container-read identity (brief-COV "Root verifies" 2-4) logged; D-COV printed before any cell that reads `atx-cov-v1` is registered; container bench medians in the integration log | COV gtests; integration-log rows |
```

## B. §0.5 Decisions (new row after DEC-20)

```
| DEC-21 | Covariance of record: recipe `atx-cov-v1` = atx-risk-v1.1 unchanged + Menchero-Wang-Orr eigenfactor adjustment (USE4's simulated form, a = 1, per-session seed) on the fully observed factor block, then VRA; written by the `risk` verb into an `atx.cov-container` file beside the legacy files (`--emit-container`); NAV rules read it through the unchanged `--risk-model` pin (a manifest `container` key switches `spo::RiskStore`); the default recipe and every existing store stay byte-identical; statistical factors, dense shrinkage and DCC-NL are P10 | the model of record has no eigen adjustment although the engine kernel exists (`strategy_risk_model.cpp`, `eigen_adjust.hpp:89-92`); no producer truncation test (`strategy_risk_model_test.cpp`); store read by seek-per-row and hashed whole per open (`strategy_spo.cpp:648-769`) | USE4 §4.2 (lowest-vol eigenfactors realise ~40% above forecast; shipped milder adjustment), MWO 2011; ELW 2019 GMV test and its forward-looking-universe caveat; Boyd et al. 2017 O(nk^2); cov-design §2 | COV, D-COV, P9-V |
```

## C. §2.1 Lane catalogue (new rows; COV after AL-SIG, COV-MV after AL-CLOCK)

```
| COV | INFRA-R | covariance of record without look-ahead: recipe `atx-cov-v1` (eigen adjustment), `atx.cov-container` mmap file + `RiskStore` read path, producer truncation suite, extended bias families, container bench | Sh, I | 2 | at dispatch (16 / 7 / 8 / fresh 22) | L | C1, T1 |
| COV-MV | INFRA-R | optional: `mv-aim-v1` NAV target rule (gross-normalised P V^-1 P alpha from the pinned store) for cell P9-MV | Sh | 3b | at dispatch | M | COV, C3; PM funds P9-MV |
```

## D. §2.2 Owned files (new rows)

```
| COV (wave 2) | new `atx-engine/include/atx/engine/risk/cov_container.hpp`, `atx-engine/src/risk/cov_container.cpp`, `atx-engine/tests/risk/risk_cov_container_test.cpp`, `atx-engine/bench/risk_cov_container_bench.cpp`, `atx-impl/tests/strategy_cov_{fixture.hpp,model_test.cpp,store_test.cpp}`; modified `atx-impl/src/strategy_risk_model.{hpp,cpp}`, `strategy_risk_verb.cpp`, `atx-impl/tools/equity_strategy_risk.cpp`, the RiskStore section of `strategy_spo.{hpp,cpp}` (`.hpp:221-266`, `.cpp:629-777`), `strategy_risk_model_test.cpp` (fixture extraction only), `atx-engine/include/atx/engine/risk/README.md`; cross-lane one line in `atx-engine/CMakeLists.txt` (risk source list) and two lines in `atx-impl/tests/CMakeLists.txt` (`atx-impl-strategy-target-tests`, shared with C2). In wave 3 C3 inherits `strategy_spo.*` (its core split moves it) |
| COV-MV (wave 3b, optional) | new `atx-engine/{include/atx/engine,src}/book/mv_target.*`, `atx-engine/tests/book/book_mv_target_test.cpp`, template `scripts/specs/p9/templates/mv-aim.json` and its rule file; one registry row and one parser entry in C3's files (cross-lane) |
```

Note for the §2.2 table: `strategy_risk_model.*`, `strategy_risk_verb.cpp`, `tools/equity_strategy_risk.cpp` and
`strategy_spo.*` are owned by no lane in the plan today; this row assigns them.

## E. §2.3 Cross-lane contracts (new row after K-P9-11)

```
| K-P9-12 covariance container | COV | `spo::RiskStore` (vol-target-v1, risk-target-v1, spo-v1/2/3 through `--risk-model`), D-COV, PRE, AL-COMB's §8 Q5 diagnostic (may read either store), COV-MV / P10 | `<store dir>/model.atxcov`, format `atx.cov-container` 1.0 (cov-design §4): magic `89 43 4F 56 0D 0A 1A 0A`, little-endian, 4 KiB header {major 1, minor, endian tag, flags (complete, f32 exposures), D, K = 1 + G + S, first / last session, recipe / role / inputs SHA-256, root}, 4 KiB-aligned per-session blocks with 64-byte-aligned arrays {ids u64, role_index u32, group u32 (1..G, 0 none), exposures f32 or f64 N x S, specific f64 (priced rows first, finite > 0), factor_cov f64 K x K, optional factor_return, factor / row flags, diag, eigen gammas}, date index {as_of, data_cutoff, history_first, offset, bytes, rows, priced_rows, flags}, per-block SHA-256 under `root_sha256`, trailer; the store's `manifest.json` (`atx.risk-model/v1`) gains `container: {file, format, bytes, file_sha256, root_sha256}` only when written; invariant: block t reads data at sessions <= t only and is stamped as_of = cutoff = session t; a decision at d reads block d; readers refuse a cutoff after the request, a role / axis / recipe / root mismatch, an unfinished file and unknown required sections (cov-design §3.2) |
```

## F. §2.4 Lane blocks (new group after INFRA-T, before ALPHA)

```
**INFRA-R: the covariance of record**

- **COV covariance + container (wave 2).** Inputs: COV-1, COV-2, cov-design §1.4 (no eigen adjustment in the model
  of record, no producer truncation test, seek-per-row store hashed whole per open, no per-row stamp, no eigen / MVP /
  optimised bias families), USE4 §4-5, MWO 2011, ELW 2019 §6. Deliverables: `atx.cov-container` writer and mmap
  reader in `atx-engine/risk` (K-P9-12); recipe `atx-cov-v1` in the `risk` verb (`--recipe`, default
  `atx-risk-v1.1`); `--emit-container`; `RiskStore` reads a container-bearing store with bit-identical `RiskSlice`s,
  so vol-target, risk-target and spo read it through the unchanged `--risk-model` pin and no NAV file changes;
  producer truncation suite (delete-after, perturb-after, future instruments, planted leak); extended bias families
  (eigen, minvar, optimized, QLIKE, MRAD) behind `--bias-families extended`; `cov-info` / `cov-diff`; container
  bench. Root: builds the engine risk group, `atx-impl-strategy-target-tests`, the risk and targets exes; the R-8
  store rebuilt flag-absent (payload SHAs equal, `producer` substituted), with `--emit-container` (`cov-diff
  --legacy` exit 0), and Y-1's NAV on it byte-identical to its P9-B0 reference except the store pin; one
  `atx-cov-v1` build timed; bench on `rel`. Trials 0. Merge slot 9 of wave 2 (byte-neutral; may move earlier).
- **COV-MV max-Sharpe target (wave 3b, optional).** Inputs: cov-design §5.2, K-P9-7 `target` kind, C3's registry.
  Deliverables: `mv-aim-v1` (gross-normalised P V^-1 P alpha over priced names, alpha = sigma z, from the pinned
  store's row d; unpriced names pass through; gross matched to the desired target) as a NAV target rule; template.
  Root: NAV identity with the rule absent. Trials 0 (P9-MV is root's). Dispatched only if the PM registers P9-MV.
```

## G. §3.1 Graph (additions to the mermaid block)

Inside `subgraph W2`: `COV[COV covariance + container]`. Inside `subgraph W3`: `CMV[COV-MV mv-aim, optional]`.
Edges:

```
  M1 --> COV
  COV --> M2
  M2 --> DCOV[D-COV diagnostic, 0 trials]
  DCOV --> PRE
  C3 --> CMV
  COV --> CMV
  CMV --> M3
  PX --> PV[P9-V vol-target on atx-cov-v1]
  PV --> AP9
```

and replace `PX[P9-X leverage] --> AP9[P9 adoption print]` by the two `PV` edges above. If P9-MV is registered:
replace `PC --> PW` by `PC --> PMV[P9-MV mv-aim] --> PW`.

## H. §3.2 Dependency table (new rows; amend the wave-2 slot cell)

```
| COV | C1, T1 merged (wave-1 merges + P9-B0 = the dispatch base) | C2 (shares the `atx-impl-strategy-target-tests` list, textual) | wave 2: ... AL-SIG 8, COV 9 |
| D-COV | COV merged; the store-of-record build of R-8's role under `atx-cov-v1` | - | root, between wave 2 and the P9 cells (§3.4 step 6) |
| COV-MV | COV, C3 merged; PM funds P9-MV | - | wave 3b, after AL-CLOCK |
| P9-V | COV merged; D-COV printed; P9-X is the Y-1 (vol-target) form; PRE ruled | - | root, serial, after P9-X |
| P9-MV | COV-MV merged; PRE ruled | - | root, serial, after P9-C |
```

The wave-2 cell of the existing row "A3 | A2 merged | - | wave 2: E2 1, A3 2, S2 3, B2 4, D2 5, C2 6, AL-COMB 7,
AL-SIG 8" becomes "... AL-SIG 8, COV 9". In the PRE row add "D-COV printed; COV merged" to the soft column (PRE
records D-COV's numbers as P9-V's predictions).

**What COV hard-depends on:** C1 (the per-book `BookScaler` that calls `RiskStore::read`, and the vol-target replay
fixtures COV's identity tests reuse) and T1 (CTest labels; the canary root re-checks). **What depends on COV in wave
3:** PRE (registers P9-V / P9-MV with D-COV's printed numbers), C3 (moves `strategy_spo.*` with COV's read path in
its `atx-impl-core` split; NavSpec's `--risk-model` field must accept a container-bearing store, which needs no code
because the flag is unchanged), COV-MV (optional), and the cells P9-V and P9-MV.

## I. §3.3 Waves (amend rows)

- Wave 2 lanes cell: append "COV (pool at dispatch: 16 if released, else 7 / 8 after recovery, else a fresh 22 with
  `-MaxPool 22`)".
- Wave 3 lanes cell: append "COV-MV (3b, optional, after C3, pool at dispatch)".
- Critical path: unchanged (COV is off root's chain; D-COV adds one zero-trial run of the risk exe, ~1 root step).

## J. §3.4 Root between waves (amend step 6)

Append to step 6: "and D-COV: the `risk` verb on R-8's role twice, `--recipe atx-risk-v1.1` and `--recipe
atx-cov-v1`, both `--bias-families extended --emit-container` (0 trials, <= 600 s / <= 8,192 MiB each; Release only
after its v1.1 payload identity if the Debug wall exceeds the cap, DEC-11 form); print the bias families random,
factor, eigen, minvar, optimized, QLIKE and MRAD side by side on identical observations into the integration log; no
book family, no NAV, no statistic of any book is read; the `atx-cov-v1` store's manifest SHA becomes the pin P9-V
registers."

## K. §4.2 Per wave (amend the suites row)

Append to the suites cell: "`atx-engine-risk-tests` (`RiskCovContainer.*` and `Risk*` minus the 31-minute oracle),
and in `atx-impl-strategy-target-tests` the `Cov*` suites; `atx-equity-strategy-risk cov-diff --legacy` on the
current store of record exits 0".

## L. §5.2 P9 budget (new rows; COV-2 asks for them)

```
| P9-V | risk model (COV-2) | vol-target-v1 with its registered constants (cap = the parent's L, floor 1, cadence 21, annualisation 252) on the P9 parent, reading the `atx-cov-v1` store pinned by D-COV instead of the atx-risk-v1.1 store; registered only if P9-X is the Y-1 form, else it lapses | paired dSR > 0 vs P9-X AND mechanics at P9-X's scaled limits | predicted (from D-COV, registered before the run): the book's sigma_hat bias closer to 1; mean L_t and its changes; 4x; D-COV's minvar / optimized bias rows beside the result | N_c +1 |
| P9-MV | construction (COV-2; only if COV-MV is funded) | `mv-aim-v1` target on the P9 parent, gross-matched (PM6-6 via C2) | dSR > 0 AND mechanics | predicted: vol ratio < 1 at gross alpha ratio ~1; turnover per unit gross; capacity curve; 4x | N_c +1 |
```

Amend the ceiling row: "N_c <= 62 + 7 + 1 = 70 with P9-V (71 with P9-MV); K_a <= 10; M + 0; N_tot <= 251 (252)".
Amend DEC-18's cap: "<= 7 hypothesis cells (8 with P9-MV) + <= 1 protocol cell".

Order (amend the sentence after the table): "P9-L -> P9-S -> P9-H -> P9-M -> P9-C -> [P9-MV] -> P9-W -> P9-X -> P9-V
-> P9 adoption print -> OD-3 -> freeze". P9-V's parent and paired reference is P9-X; a rejected P9-X still serves as
P9-V's reference (P9-V tests the store, not the leverage rule).

Deflation (amend the paragraph) [arith; v8y §4's E[max], gamma .5772]: E[max] factor 2.397 at N_c 69 -> 2.4025 at 70
(+0.22%) -> 2.4077 at 71 (+0.44%); 2.838 at N_tot 250 -> 2.839 at 251 (+0.04%) -> 2.840 at 252 (+0.09%); both remain
inside lit §5.2 (c)'s "under about 2%".

Power note (append): P9-V's two books differ only by the store's forecasts, so their paired difference has low
variance and the test is sharper than a combination cell's; its expected effect is small [est] because the eigen
adjustment mainly moves the small eigen-directions that a diversified book loads on little. Registering it costs
+0.22% of E[max]; skipping it leaves the store question to D-COV alone.

D-COV (append below the table, not a row): "D-COV is a zero-trial diagnostic (no book statistic; like AL-COMB's
lit §8 Q5 diagnostic) run by root between wave 2 and the P9 cells; it gates nothing by itself; its printed numbers are
P9-V's registered predictions."

## M. §5.3 The OD-3 history read (amend the "Not readable" bullet)

Append: "P9-V reads `--risk-model` and is not read on history either. (P10 note: a COV build over the history block,
registered and pinned before the read, would make a vol-targeted book readable.)"

## N. Sprint-dir carry (proposal for `wave2-carry.md`, a PM file)

```
## COV (pool at dispatch)
- COV-1 / COV-2: 0 trials; flag-absent byte-identical (default recipe, no container, `--bias-families v1`).
- Owns `strategy_risk_model.*`, `strategy_risk_verb.cpp`, `tools/equity_strategy_risk.cpp` and the RiskStore section
  of `strategy_spo.*` for wave 2; C3 inherits `strategy_spo.*`.
- Cross-lane: one source line in `atx-engine/CMakeLists.txt`; two lines in `atx-impl/tests/CMakeLists.txt` (C2 shares
  the list).
- C1 merge state: `BookScaler::estimate` reads `RiskStore::read(d)`; reuse C1's vol-target replay fixtures for the
  identity test; never edit C1 / C2 files.
- Carried C1 minor in scope: "config check skips a null risk store" stays C2's; COV only guarantees `RiskStore::open`
  refuses a container whose root is not the manifest's.
```
