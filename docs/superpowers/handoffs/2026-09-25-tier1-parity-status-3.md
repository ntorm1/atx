# atx-db tier-1 parity — controller status (session 3), 2026-09-26 ~01:00Z

Branch `feat/tier1-parity`, repo `C:\atx`, package `C:\atx\atx-db`. The ledger with every ruling is
`.superpowers/sdd/tier1-parity/claude-ctl/progress.md` ("Session 3" section). Reports, reviews and review packages
live in `.superpowers/sdd/tier1-parity/claude-ctl/` (and its `review-pkg/`). Common agent rules:
`claude-ctl/restart-s3-common.md`, `global-rules.md`, `reviewer-contract.md`.

## 1. Headline

- 50 commits since `493cf3bc` (this context), 13 code tasks closed after review, and a further 15 committed or in fix rounds.
- The research lane now has, as code:
  - price/liquidity natives (P2);
  - earnings-event features (P3);
  - factor returns and spanning (P4);
  - build-forward composites (P5);
  - ownership / short-interest features (P9);
  - a qualification ledger (R4 v3) with honest EWC inference, which also covers the legacy evaluators (FQ2 v3).
- Survivorship is repaired in code: reconstructed delisted-line owners cut operating-company attrition from 33.5% to 12.0% (RI1 + P1). Two dependencies remain:
  - RI2, the writer that puts RI1 evidence in the warehouse, is in progress;
  - the P1 preferred-line fix is queued behind it.
- **Nothing is materialized yet.** The heavy warehouse chain (archive21 → … → B0 → B1–B4 → RR1–RR5) has not run: see §5. Every feature above is proven on fixtures and retained files only.
- **Dispatch stopped at the user's request.** 13 agents were still finishing their current tasks (§3). No new work was given out after the stop.

## 2. Tasks closed this session (reviewed and approved, or accepted by ruling)

| Task | Commits | Note |
|---|---|---|
| A9 (0327 units) | 493cf3bc, 396a552c | row ceiling 1e8; run the read-only B0 inventory well before B0 |
| C4 (release eligibility) | fcc69fd5, 6303e89f | minors parked to AF3 |
| R2e (NULL-revival fix, one-formation API) | 95176f25 | |
| R3c (+ follow-ups) | 2e20d88d, ad89d812, 0d49d195 | R3b refuses store_schema < 3 |
| R4 (qualification ledger v3) | 64901702, 64dc1c98, a79bded5, bae1b08b | superseded / spec-less policies refused |
| RI1 (reconstructed owner links) | 1c0e0d47, d1883d9b | tier history is point-in-time (`tier_at(cutoff)`) |
| P7 (PIT screen) | 4b1b89eb, 60f4e713 | |
| P11 (IFRS / foreign filers) | 9f7b1fb0, d28a6b6c | 808 IFRS-only, 54 switchers, 1,647 no-XBRL windows |
| P3 (earnings events) | 12ddeec5, 2a1a174f, 35fd4f9f | |
| P4 (factors / exposures / span test) | 464732ee, 742161ca, dfc6b5c4 | exact h-month factor rows |
| R1d (split epochs) | 2b98ca32, 68170a44, 4649571b | 3,148 splits / 4,119 distributions on retained bars |
| Catalog fix R1a + R1b | 5755e232, 47502a6a, 468eb2f5 | no imputed flow zeros; aliases inert until re-standardize |
| P6 (FF12 / FF49 classification) | af9b6644, 8f87429f | French Siccodes verified exactly; 750 issuers re-classed |
| A8 (market cap / share basis) | a2e5d929, b347e6c5 (+ follow-up b4345421) | |
| CB1 (catalog batch) | 4cadff6c, fe7113fd | `panel_native` source kind; `liquidity` class |
| P2 (price / liquidity natives) | a9c5ca7f, 3c636d10, d94db27b | session-calendar windows; literature minimums |
| FQ2 v3 | f0f60138, 2fd6ab98 | legacy runs labeled `inference_overconfident_legacy` |
| W1 (label / reason wiring in screens) | f9a5be06 | |
| FP-rdq | 8447882f | rdq stays NULL when there is no 8-K |
| P12 (FSDS benchmark) + fact_disagreement fixes | 7a10bcb3, 3bf2dca2 | 84.5% of cells mapped; rule gaps → S1 |
| C9 pilot (measurement) | 725814d7 | RX5 forms rarely state symbols; full acquisition parked (RX13) |
| Security: user-agent scrub | f5fe7c76 | 14 defaults now `atx-db/0.1 atx-research@example.com` |

## 3. In flight at stop (agent → task → uncommitted files it owns)

**UPDATE ~01:10Z: at the user's second "stop", all 12 remaining agents were killed (TaskStop) mid-task.** Their partial,
uncommitted edits are in the working tree in the files listed below — nothing was discarded. A resumed agent must
start with `git diff -- <its files>`, keep what is correct, finish, and commit. Do not commit these files blind.

Resume any agent with SendMessage to its id: "continue; check git status/diff of your owned files first; same rules".

| Agent (id) | Task | State / owned WIP |
|---|---|---|
| impl-p12 `aea6cba381aac5def` | S1: standardization gaps from FSDS | Owns `_standardization_set_based.py`, `standardization.py`, the concept_map / statement_map / fundamental_items / standardization_rules seeds, and `fsds_baseline.py`. Work: capex alias + sign fix, derived-SE common_equity, NCI-identity liabilities, utility revenue. |
| impl-r1d `a4430f334b8ef46a2` | P8 fix 1 | Owns `corporate_actions.py`, `adjustment_factors.py`, `daily_adjustments.py`, `corporate_action_metrics.py`, the build script, `_split_epochs.py` (docstring) and the test. Work: revisioned per-step events, tie-out raises. R1e is committed (f3830159) but not yet reviewed. |
| impl-r4 `a73ae4c5e5c73f8d7` | P5 store-path smoke | Composites; fix 1 is in re-review. |
| impl-p3 `ae8dc861b402f558a` | P9 fix 1 | `ownership_identity.py`, `ownership_features.py`. VA1 is committed (5272dfae) but not yet reviewed. |
| impl-ri1 `ae014f941f4fc59ac` | RI2, then P1 fix 1 | SERIALIZED `activation.py`, `identity_reconstruction.py`, stage pins, runbook. P1 fix next: preferred lines linked as common. |
| impl-r2e `aee40a62ea09b13bd` | E1 + R2d fix 1 | `research/features.py` (event / factor_exposure / ownership adapters, public derived-version writer); `research/panel.py` (R2d I1 verdicts, bar-count chunking, Feb assertion). |
| impl-c4 `a87ed7f01c227ea1d` | P13 fix 1 | `publication.py`, `api/catalog.py`, tests. CA events withheld; listing_status capped at candidate; market_daily key includes source; 0328 eligibility columns. |
| impl-r3b `ac0d6582c67eb4469` | R3d | DONE after stop: commit da28fb60 (composite versions evaluated only in their window's one recorded P5 run). Unreviewed. |
| impl-a8f `afbcc22508fd80989` | currency_status writer | `market_daily.py` |
| impl-p4 `aadfc1429b2d35d4d` | W2: 0328 column writers | `fundamental_statements.py`, `reference_classifications.py` |
| impl-a9f `a98c4dce993ac3ebf` | 0328 registration + schema_contract pin-hazard fix | SERIALIZED migrations / registry; `schema.py` |
| rv-p5 `adf7e82f043b7e551` | P5 re-review 1 | — |
| rv-a8 `a9481e7fb000fe0d6` | P14 + A8 follow-up review | — |

The following are committed but still need their review, or their fix round then review:

| Task | Commits | Needs |
|---|---|---|
| P5 | 64e4ed10, 753e4cf8 | re-review in flight |
| P8 | a84111db | fix 1 in flight |
| P9 | e27c8fb8 | fix 1 in flight |
| P1 | 275f0000, d1865b4b, fb60f644 | fix 1 queued |
| P13 | 2629f15f | fix 1 in flight |
| R2d | c2bbea26 | fix 1 in flight |
| P14 | 8aef02ae, 251f1188 | review in flight |
| R1e | f3830159 | first review not dispatched |
| VA1 | 5272dfae | first review not dispatched. Note: this commit also carries A8's follow-up change to `market_daily.py`. |
| MIG0328 | 17ecfe0e, 1258b8db | registration + pin fix in flight, then review |

## 4. Queued (not dispatched)

- **CB2 catalog rows** for P3 (event), P4 (beta_252d, ivol_252d) and P9 (ownership). Needs E1 first; the exact rows are in P3-, P4-, P9- and E1-report.md.
- **AF3 minors bundle**, parked in the ledger:
  - C4: N1, N2, M3, M5;
  - P7: N1–N3 (done in W1);
  - P11: R1;
  - P6: R1;
  - A8: NEW-2, NEW-3;
  - labels vendor_shares_zero / bar_price_invalid in the data dictionary.
- **A10 / B0 preflight:**
  - jobs.py maps corporate_actions to `tbltickerhistory_daily` (~line 1525);
  - count-query cost (A8 N5);
  - DATA_DICTIONARY regeneration;
  - collect-only check of the bridge tests from the pinned export;
  - read-only `ticker_history_unit_inventory` on prod.
- **public_api_snapshot refresh.** Run it once every lane is quiet. It picks up new modules: governed_read, foreign_filers, factor_returns, events, composites, ownership_*, identity_reconstruction, ticker_history_incremental, plus new asof names.
- **Exposing the high-only RI1 tier sensitivity** in MarketDailyOptions / the research panel.
- **R5 / RR runners** must call `stage_foreign_filer_reasons`, build P4 runs and register the P5 policy v2 before RR4 (RX7).

## 5. Blockers needing the user

1. **The heavy chain is blocked by host memory and a permission denial.**
   - The watcher is `autolaunch-archive21.ps1`, pid 8072, started 3:15pm local with a 12 h limit. It requires 3.6 GiB physical and 6 GiB commit free, sustained. It has returned rc=78 on every window (window86 at 00:58Z), even while zero agents were running. The idle host baseline is about 5.6–5.9 GiB free commit: the VS Code host is about 2.3 GB, plus other Claude sessions.
   - I parametrized the script (`-PhysGib`, `-CommitGib`; defaults unchanged). The auto-mode classifier denied both restarting it at `-CommitGib 5.4` and stopping pid 8072.
   - The guard's own floors are unchanged: preflight min(phys, commit) ≥ 3.5, and in-run stop at phys < 1.5 or commit < 3.
   - The agent gate was raised to phys ≥ 1.8 / commit ≥ 3.6 so the swarm can't push a heavy job under its stop floor.
   - To proceed, allow restarting the watcher with a reachable commit bar (≈ 5.4), or free host memory.
   - B0 export requirements: at or after the 0328 registration commit, plus RI2 and the P1 fix.
2. **RX13 (proposed): symbol-bearing SEC filings for 2012–2018 delisted-line identity** (8-K 2.01 / 3.01, 10-K Item 5). This means tens of thousands of fetches with the approved user agent. C9 showed that RX5 forms (8-A / 25) almost never state a symbol.
3. **Security housekeeping.** The personal email is gone from src/ and scripts/ (f5fe7c76). Git-ignored stale copies in `atx-db/build/lib/atx_db/*.py` (6 files) still contain it. Delete `atx-db/build/`; I did not touch it.

## 6. Key rulings this session (full text in the ledger)

**Statistics**
- P5 composites select constituents on non-holdout R4 evidence only. In a null simulation, holdout-selected composites passed the holdout 100% of the time; the v2 selection passes 6.1% against a nominal 7.4%.
- P5 composites are graded within the full family and stamped `composite_post_selection`.
- P4 span_test compounds factors over identical h windows (exact label-window rows for h ≥ 3).
- FQ2 v3 uses EWC robust inference; the legacy results can't pass the desk-Q5 gate.

**Returns and prices**
- VA1: the vendor's 2021-01-04 factor-decrease artifact biased forward returns for ~3,415 dividend payers by −1.5% to −3.4%, correlated with dividend yield. It is now neutralized in every return builder that uses adjusted close.
- P2 windows run on the XNYS session calendar with literature minimums: MAX and Amihud need ≥ 15 of 21 returns; the 52-week ratio needs 252 sessions of history and ≥ 200 observed prices.
- P14 records factor rebases as a compact multiplicative ledger. Bar clocks are never re-stamped.

**Identity and events (point-in-time)**
- RI1 tier is point-in-time. The P1 default is high+medium via `tier_at(cutoff)`; high-only is a labeled sensitivity.
- P8 events will be revisioned per step, with a pending row followed by resolutions at their own decision clocks.
- P9 fix 1:
  - the IO numerator counts common equity only;
  - a CUSIP is "current" only with a minimum filer count plus name agreement;
  - ticker hops must be dated.

**Fundamentals and market data**
- R2-I1: no imputed payout / issuance zeros until an any-duration presence item exists.
- The A8 split-pending window opens only on real splits (R1d classifier). Split-derived share runs wait for the prior run's availability.
- rdq stays NULL when there is no 8-K; rdq_basis moves to 0328.
- 0328 is registered now, so B0 applies {0327, 0328} while market_daily_metrics is still empty.

## 7. Measurements worth keeping

- **RI1 + P1 identity:** 3,362 of 18,416 unlinked lines are now linked (high 1,218 / medium 2,145). Operating-company attrition fell from 33.5% to 12.0%; all lines from 57.8% to 44.8%.
- **P6 FF12 fix:** 750 of 6,944 issuers moved (10.8%).
- **P8 events on the retained file:** 370,973 cash dividends, 3,100 splits (1,933 confirmed late) and 1,149 unclassified distributions. The tie-out is ≤ 1e-6 on all 25,760 lines.
- **P12 FSDS benchmark:** 2,112 of 2,500 cells mapped (84.5%). The gaps (capex, equity, liabilities, utility revenue) are being fixed in S1.
- **P11 foreign filers:** 862 CIKs flagged IFRS (591 with a current ticker); 1,118 US-GAAP foreign filers kept.
- **Memory:**
  - `import numpy` commits about 536 MB with default threading versus about 53 MB with OPENBLAS_NUM_THREADS=1, so that setting is mandatory;
  - the P2 panel natives need about 395 MB per formation;
  - the P5 streaming composites need about 420 MB for 40 signals.
- **MIG0328 cost:** a 1M-row swap takes 13.5 s. At 32M rows that extrapolates to about 7 min and 9 GB, which is why 0328 is applied at B0.

## 8. How to resume

1. Read `progress.md` from the "Session 3" header. `git log --oneline 493cf3bc..HEAD` shows what landed.
2. For each agent in §3, check `git status` on its files. Then resume it by id, or let its report arrive.
3. Package and review the unreviewed commits: R1e, VA1, MIG0328, RI2, E1, S1, and the fix rounds.
4. Then run CB2, AF3 and A10. Refresh the public API snapshot and the DATA_DICTIONARY last.
5. Heavy chain: once the user allows the watcher restart, run archive21 → submissions → CVX → A10 → B0 (0327 + 0328) → B1–B4 → RR1–RR5.
