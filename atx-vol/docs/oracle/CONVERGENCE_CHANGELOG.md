# SpiderRock oracle convergence changelog

This is the durable chronological record of changes that move the SpiderRock
oracle loop toward price, fair-vol, greek, and speed parity. `NORTHSTAR.md`
remains the mutable current dashboard, while `../LEDGER.md` remains the separate
append-only research log.

## Record invariants

- Append only. Never delete, reorder, or silently rewrite a published entry.
  Append a dated correction that names the entry it supersedes.
- Record only claims supported by cited command output, committed artifacts, or
  sealed broker receipts. Missing evidence means no claim.
- Label every entry as a bootstrap transition, an iteration verdict, or a code/
  harness change. A bootstrap PASS is not an accuracy ACCEPT.
- Label every metric by cohort: `smoke`, `tune`, or `holdout`. Never imply a
  smoke/tune result is a holdout result or a ratchet baseline.
- Record holdout results only after the frozen iteration-0 membership gate and
  only from Ratchet evidence. The analyst never tunes against holdout.
- Record vetted oracle-suspect exclusions explicitly. Cells where market
  evidence supports atx-vol against SpiderRock are excluded from the ratchet,
  not optimized away.
- Distinguish diagnostic timing from a pinned speed baseline. Diagnostic wall
  time or throughput is not speed-gate evidence.

## Entry template

```markdown
## YYYY-MM-DD — <iteration/stage and outcome> (`<SHA>`)

- Type / cohort / verdict: <bootstrap | iteration | code>; <smoke | tune |
  holdout | none>; <BOOTSTRAP | ACCEPT | REJECT | FAILED | N/A>
- Capability: <before> -> <after>, or unchanged
- Code/artifacts: <hard-cutover summary and exact paths/SHAs>
- Metrics: <cohort-qualified deltas or "none measured">
- Speed: <pinned result, diagnostic-only result, or not measured>
- Gates: <exact IDs and pasted-output-backed results>
- Hypotheses: confirmed <...>; refuted <...>; unchanged <...>
- Oracle-suspect exclusions: <cells and evidence, or none>
- Evidence: <committed/local artifact paths, receipt IDs, and SHAs>
- Next: <one concrete convergence target>
```

## 2026-08-15 — iter-000 bootstrap smoke reference (`6cfaccac`)

- Type / cohort / verdict: bootstrap; `smoke` only; `BOOTSTRAP`.
- Capability: charter stages 1 and 2 were complete on
  `integ/oracle-bootstrap-2026-08-15`; convention resolution remained next.
- Code/artifacts: the data store and Mode A bench bootstrap were qualified. The
  reference scorecard is
  `C:\atx-cache\oracle\scorecard_smoke_ratchet_iter000_2026-08-15.json`.
- Metrics (`smoke`, Mode A, n-weighted): 13,926 priced, 662 sentinel-null, and
  0 bad-input rows out of 14,588. Price results were:

  | DTE band | Cells / n | MAE | Within tolerance |
  |---|---:|---:|---:|
  | all | 37 / 13,926 | $1.262936 | 0.4560 |
  | 0-7 | 8 / 1,384 | $0.000183 | 1.0000 |
  | 8-30 | 9 / 3,421 | $0.000288 | 1.0000 |
  | 31-90 | 10 / 2,872 | $0.534888 | 0.2093 |
  | 90+ | 10 / 6,249 | $2.568444 | 0.1511 |

- Speed: no pinned `rel-avx2` baseline existed. The recorded dev timing was
  diagnostic only and is not performance evidence.
- Gates: 30/30 `OracleBench*` tests passed; the gate and Ratchet payloads were
  equal in 370/370 cells.
- Hypotheses: none confirmed or refuted. The corrected band aggregation proves
  material residuals in both `31-90` and `90+`; it does not identify their
  cause.
- Oracle-suspect exclusions: none vetted.
- Holdout: untouched by design; no holdout metric or delta was measured.
- Evidence: [`NORTHSTAR.md`](NORTHSTAR.md), the 2026-08-15 `oracle` and
  `CORRECTION` entries in [`../LEDGER.md`](../LEDGER.md), and the scorecard path
  above (SHA-256
  `c0bf7d3be75747c5a7b38b4735e2d71ce9150e2a6bdb9444b10af76d13d70e6a`).
  This entry uses the corrected aggregation and supersedes no history.
- Next: resolve Mode A conventions on smoke/tune and establish the residual
  floor before any holdout ratchet.

## 2026-08-16 — Stage 1 canonical receipt promotion (`d2d4c0c6b77f8851e9d240f47545566a9a43c942`)

- Type / cohort / verdict: bootstrap capability transition; no pricing cohort;
  `BOOTSTRAP`.
- Capability: `missing_data` -> `missing_mode_a`; `refs/heads/oracle/canonical`
  advanced to `d2d4c0c6b77f8851e9d240f47545566a9a43c942`.
- Code/artifacts: no pricing/model code changed. The exact two-path receipt
  commit added `atx-vol/bench/oracle/bootstrap/data.json` and
  `atx-vol/bench/oracle/cohorts/holdout.sha256`, replayed byte-for-byte from
  validated source commit `58a94584baabae8263d16421f633540b420de10b`.
- Data: the adopted 2026-08-14 aggregate store contained 31,771,788 rows in 19
  `bucket_et` partitions and occupied 3.10 GB zstd. Its ingest manifest SHA-256
  was `67e5ac9d130e1ee3b0a668612ecf3bc9a3e724c28987d54ac565c7f1ceadcab5`.
  The frozen holdout-membership SHA-256 was
  `44a7b6641616161ede494a3e0353cb7ae5fb83db65b358b6c803ee915aa9f1c0`.
- Metrics: none measured; no price, vol, greek, or holdout result changed.
- Speed: not measured.
- Gates: the committed recovery SHA/tree passed all four fixed gates with exit
  code 0:

  | Gate ID | Status | Receipt ID |
  |---|---|---|
  | `aggregate_store` | PASS | `26e31e78cf728901b965a4bdec22de84df2a1f890901819b228e9ae9cc4df84a` |
  | `ingest_manifest` | PASS | `c19dc23f19ae01de6458e3035971933e0c9ab402f9ff9bef1db0bd8d35e75f56` |
  | `cohort_manifests` | PASS | `7964fe2a23c59c7a291b894edf899cea719e93f601c921943446a91bdbe0213f` |
  | `holdout_digest` | PASS | `5fca01866a60251c894cb48fa42531b795b62ceeebf101fb0607d26fd53c79d9` |

  Canonical promotion consumed this sealed receipt set; it did not rerun the
  four gates or re-ingest licensed data.
- Hypotheses: unchanged; none confirmed or refuted.
- Oracle-suspect exclusions: unchanged; none vetted.
- Evidence: sealed recovery journal
  `C:\atx\.git\oracle-lane-broker-v3\recoveries\d4a5b890e8531cd380cae69035a37f14a605765a9a1b29044840cc45a8378201.json`,
  file SHA-256
  `2403ad38e53380b589558f94fcad67b719e006e1294c88a9425d62dd0f929bcd`,
  seal `e9321e64680297bef1dad6482bf9bef45a30970ba7c9a85db96d3b212f4316ba`,
  committed receipt artifacts at the two paths above, commit/tree
  `d2d4c0c6b77f8851e9d240f47545566a9a43c942` /
  `1651c257b2751382e81d058c5eea7c3881c7988d`, and the data counts in
  [`../LEDGER.md`](../LEDGER.md).
- Next: qualify and commit the Mode A production receipt without exposing or
  tuning against holdout.

## 2026-08-16 — Mode A production gate hard cutover (`db20531f81e9cb8b0cd47d5f9f85422d76162766`)

- Type / cohort / verdict: code/harness hard cutover; `smoke` only; N/A (gate
  qualification, not a Ratchet verdict).
- Capability: unchanged at `missing_mode_a`; this entry records production gate
  readiness, not completion of the Stage 2 canonical transition.
- Code/artifacts: commits `10662c4ed0eabdfc77409fabdf037ff692e07dee`,
  `df8c17388dd521830e87ca3752a44b3649fcc3fa`, and
  `db20531f81e9cb8b0cd47d5f9f85422d76162766` hard-cut the Stage 2 adapter to
  real worktree-local `OracleBench*` tests and the real
  `atx-vol-oracle-bench` smoke CLI; expanded the closed Mode A/Mode B target
  registries from six to eleven metrics; and bound broker receipts to exact
  SHA/tree, command output, raw-output digest, and independently recomputed
  receipt ID. Legacy six-target or synthesized receipts fail closed with no
  compatibility flag. Principal paths were
  `scripts/oracle-targeted-gate.ps1`, `scripts/oracle-lane-broker.mjs`,
  `.claude/workflows/vol-oracle-iter.js`, the oracle bench/scorecard sources and
  tests under `atx-vol/`, and the targeted contract tests under `scripts/tests/`.
- Metrics: the production `mode_a_smoke` gate processed 13,926 priced rows out
  of 14,588, with 662 sentinel-null, 0 bad-input, and 0 engine-error rows. Its
  exact required metric IDs were:

  1. `mode_a_price_mae`
  2. `mode_a_vol_mae`
  3. `mode_a_delta_rel`
  4. `mode_a_gamma_rel`
  5. `mode_a_theta_rel`
  6. `mode_a_vega_rel`
  7. `mode_a_rho_rel`
  8. `mode_a_phi_rel`
  9. `mode_a_volga_rel`
  10. `mode_a_vanna_rel`
  11. `mode_a_delta_decay_rel`

  No convergence delta was claimed: this run qualified metric coverage and
  receipt integrity, not a new convention or pricing implementation.
- Speed: 7.9457249 seconds and 1,752.6405929306713 rows/second were recorded by
  the dev smoke scorecard. Both are diagnostic only; neither pins nor satisfies
  the required `rel-avx2` speed baseline.
- Gates: `mode_a_targeted_tests` passed the closed 31/31 fully-qualified
  `OracleBench` IDs; `mode_a_smoke` passed with all eleven required metric IDs
  and the row accounting above.
- Hypotheses: unchanged; none confirmed or refuted.
- Oracle-suspect exclusions: unchanged; none vetted.
- Holdout/tune: neither cohort was evaluated or exposed.
- Evidence: production smoke artifact
  `C:\atx-wt\pool-13\build\oracle-gates\mode-a-smoke-df8c17388dd521830e87ca3752a44b3649fcc3fa.json`
  has SHA-256
  `c4b859498c455b1de7058a6491c23d43753a9d7c32598639ffc0fc6c816a6d0d`.
  It is the measured gate SHA; `db20531f` is its receipt-digest-validation
  descendant. Additional evidence is the three commits above and the BREAKING entry in
  [`../../CHANGELOG.md`](../../CHANGELOG.md).
- Next: land the receipt-only Stage 2 canonical transition, then move directly
  to smoke/tune convention resolution and its first evidence-backed residual
  floor.
