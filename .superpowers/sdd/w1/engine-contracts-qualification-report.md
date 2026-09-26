# W1 engine contracts: bounded integrated qualification

Date: 2026-09-26. Root pool-2, `feat/aes-codex-integration-20260925`.

## Result and source identity

283 distinct C++ checks passed with zero unresolved failures or skips. An earlier
synthetic D1 Python run passed another 22 checks. These qualify the implemented
contracts below; they do not complete W1 or its production-scale acceptance gates.

Production and clean build identity: `d9031039601a9ed12ff4291f0e5b9708cb2246ad`.
The only subsequent code change is fixture-only `0f3ace263d000c8bb052815902de693dfc4d34b1`.
The original DAG records each lane's source/import/review commits. No actual market
payload, warehouse, 2020+ input, long comparison or mining rerun was used.

| Target / artifact prefix | Result | Native test seconds |
|---|---:|---:|
| Cost / `w1-cost-qualified` | 35/35 | 0.670 |
| Library / `w1-library-qualified` | initially 36/37 | 3.022 |
| Corrected library fixture / `w1-library-fixture-qualified` | 1/1 | 0.122 |
| Dated identity / `w1-data-qualified` | 19/19 | 0.002 |
| Evaluation / `w1-eval-qualified` | 72/72 | 10.905 |
| IC engine / `w1-ic-qualified` | 64/64 | 18.935 |
| Selected IC/discovery/provenance consumers / `w1-impl-qualified` | 56/56 | 47.394 |

All artifacts are under `C:/atx-wt/pool-2/build-equity/`. Each prefix has `.log`,
`.xml`, and `-receipt.json`, with native exit, source identity, exact filter,
binary SHA256 and wall time. XML classname/name union, replacing the repaired
fixture result, has 283 entries, no failures and no skips. The initial library
exit was 1; every other runtime exit was 0. This is not a whole-impl suite claim.
The compiled impl identity remains d9031039; committing the fixture did not
rebuild its provenance object or executable.

D1 Python source `dce7312c`: 15/15 in 0.025s and 7/7 in 0.154s,
`w1-d1-integrated-core-tests.log` and `w1-d1-integrated-export-tests.log`, native0.

## Initial failures and incremental compilation

The preliminary stable-object batch stopped after 88.563s on eleven incomplete
Provenance aggregates in a new fixture under warnings-as-errors. Test-only
`9c4a2fd8` supplied the missing fields; completed production/test objects were retained.

The integrated six-target build then passed in 646.203s at Jobs1. Its dry graph
contained 70 object actions and eight links. Existing PCH and dependencies were
reused; no PCH, dependency or worker rebuild occurred. Physical headroom at launch
was 2.597GiB and the lowest sampled during the build was 1.237GiB; commit headroom
at launch was 3.054GiB and the lowest sampled was 2.118GiB. Samples are not a
continuous peak measurement. No other compiler was launched by this team.

The library append fixture attempted to select signed-V2 after staging records in
a metadata-less store. The explicit-migration guard correctly rejected it. The
fixture now binds the recipe before the first admission, as the production facade
does. No production guard was weakened. Its one-object-plus-link rebuild passed
in 16.989s, then the affected append/reopen check passed. The other 36 library
checks retain their original evidence because their source and production were unchanged.

Build receipts/logs: `w1-stable-objects-*`, `w1-integrated-build-*`,
`w1-integrated-plan.log`, `w1-integrated-configure.log`, and
`w1-library-fixture-build-*`. These timings are shared-host receipts, not controlled
speedup measurements. Previous measured no-op build evidence remains applicable;
another no-op was not run solely to repeat it.

## Qualified behavior and limits

- B1 core/calibration: consistent one-way costs across fitness, optimizer and replay
  adapters; clamped/fixed-exponent refits and numerical-rank cases. New spread/FIM/
  borrow source in pool-5 is outside these binaries. Full consumers remain later lanes.
- A5: compact record round trips, immutable append slabs and reopen, saved recipes,
  explicit migration, bound resolver context, signed correlation and lifecycle.
  10k/T5000 recall, p99 and production compact-format adoption remain open.
- D1: prospective corroboration, issuer expiry, separately dated successor conflict
  retirement, strict availability and schema-version behavior. Historical coverage,
  acquisition and actual file-loader adoption remain open.
- E6: cached PBO/reference fallbacks and caller/recipe identities, PSD trace breadth,
  standardized fixed-gross source, causal expanding regimes and finite-score guards.
  CPCV date embargo/path work and the 50x PBO speed gate remain open.
- IC: existing vectorized four-horizon screen, conservative V3 decisions, durable
  rejected trials without fabricated PnL and pipeline bindings. Practical V3 noisy-null
  pruning, broad alpha recall and measured search throughput remain unqualified.

No alpha was promoted, no performance gate was waived, and the owner-deferred
81-case W0 comparison remains deferred, not passed.

## Binary bindings

| Artifact | SHA256 |
|---|---|
| cost | `c15d20cb6bf05b568404bf5aa836542254b1786ae06437050edcfd3a7f68bf6e` |
| library initial | `1beda29eed86e4304b8dbc6d323a47932b1e4e05de6d5f44a3e2bd690d0abaf7` |
| library corrected | `6140a4d2e615c964fdbee1255409c42a04f5cb9084472f00eeab0c7bd726595c` |
| data | `fce02f5017f48f0ac70588d2bb4028dedb7962153447654dce0bdc8c8f523912` |
| eval | `b59c638a8aec7cf0fa166b48b36551029fa87d3172c02b61480fe3366984848c` |
| IC engine | `e3f014f33d22f0520c5798c5dd0bb2cd95d41eec25cc238d98c2f129ce28b449` |
| impl | `7fa46990dbe3f82c0ff9cad5f5cc8a1720f454221332bbd9bb6c2968e9483416` |

## XML bindings

| Prefix | SHA256 |
|---|---|
| cost | `2c2e6622f65b6cede9e28f57b9ad385c4e4eaddfbb20d11f5b9163c53bf94bc5` |
| library initial | `f17e8b8bff1eb081e99388f062347922c97d9632c63e76b40fc5d960ca8147a0` |
| library corrected | `14e7d2366b5b350ec39bfefaba76c78a58991442b8299c21cf33f0bfea604e3d` |
| data | `699165f79ea3a4e240737c8704c8c2f99bc1ab7640e6ce9dc679fdc28e73645a` |
| eval | `723db25721cedea9913d8ad2f80d2e16883bb6e933ff8d1ab8b6d69a5596ad00` |
| IC engine | `c90cf01c54d50aa69f809e6ea0dadff8ef48eeb6ca2d6f754512d483ff31c29a` |
| impl | `a255cb5892a373538a81be7845c55224812b8b72ffd6a6013f4b71c43631f9b5` |
