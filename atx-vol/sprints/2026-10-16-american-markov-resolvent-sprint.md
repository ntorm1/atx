# Opt-in positive variance resolvent

Lane `feat/vol-markov-resolvent-20261016`, frozen base
`5564fc51d6b27255979b1a79fe30e4252dbf5294`. Mathematical/API design was approved
by the parent and independent reviewer before implementation. Tests were added
after implementation. No vendor price or volatility labels were read. The
[design note](2026-10-16-american-markov-resolvent-design.md) derives the operator
and explains why this deterministic-calendar, randomized-latent-variance finite
Bermudan model is distinct from Carr's randomized-maturity contract.

## Implementation and invariants

`AmericanMarkovDiffusionMethod::VarianceResolvent` selects positive
`(I-hQ)^-1` diffusion on the existing common grid and zero-drift birth-death
generator. `ExplicitEuler` remains the default. Calendar and variance step caps,
local volatility sampling, deterministic carry, discount, cash transitions, and
exercise layers remain explicit. The opt-in method omits explicit CFL
subdivision, without increasing any existing model or fitting cap.

Immutable factor blocks own two normalized Thomas weights per source state and
share only exact represented variance-step bit patterns within an interval.
The factors implement two positive convex sweeps, with an O(N) transpose.
Raw continuation is retained separately from discounted/exercised output during
the second sweep; both prepared scalar and batch paths reuse their two existing
buffers. The public checked raw action rejects bad dimensions, overlap, invalid
actions, and nonfinite inputs. Successful calls allocate no storage.

Logical `stored_rows` remains N per transition. `transition_work` and
`max_transition_work` charge N visits for explicit/cash and 2N for resolvents.
The 8,000,000 default work limit admits every former explicit row-budget
configuration; the 16,000,000 hard work cap is distinct from the unchanged
8,000,000 hard row cap. Default rows remain 1,000,000. Fitting charges this
weighted count for each quoted expiry prefix. Physical factor counts do not
discount logical work. Factor construction, compile-time constant/stock probes,
and forward mass propagation are additional bounded compile work, rather than
option rollback work. The two sweeps include discount/exercise in their second
pass, without an uncounted third rollback sweep.

Compilation checks action on constants and original stock levels, with
per-state relative mean residual `abs(RX-X)/max(X,spot)` and fixed 1e-12
ceiling. Direct original-state probing fixes the independently discovered
subnormal normalization counterexample in the design note. Forward mass is
checked for nonnegativity and unit mass; there is no clipping or normalization.
These are observed numerical residuals, not certified pricing bounds. PWL
construction explicitly refuses resolvent models before rollback.

## Owning validation

- Release focused pricing tests: **60/60**, including 11 resolvent cases.
  Independent dense inverse/basis tests cover positivity, absorbing boundaries,
  constants, means, transpose duality and forward mass over variance steps from
  zero through 1e5. Independent dense Snell rollback separately computes P,
  discount and exercise for active interior call and put exercise, cash, and
  unequal clocks. Other cases cover exact work caps, extreme range refusal,
  paused variance, local volatility, shape relations, and copy/move lifetimes.
- Release focused fit tests: **16/16**. A two-quote/three-state resolvent fit
  requires exactly 12 rollback visits; 11 and the old six-row count refuse.
  A synthetic fit recovers its generating volatility using the chosen method.
- Owning PCH-off source/test checks passed for core, batch, PWL, fitting,
  resolvent tests, fit tests and standalone probe. The corrected stock-probe
  core/test objects were checked again after the review finding. No broad
  full-library hygiene claim is made.
- Default explicit mode: all **143** existing audit cases retain **69,426**
  non-timing binary64 values exactly against the pre-storage baseline. Seven
  expanded-operator fingerprints and all **616** scalar price bits also match.
  The preexisting 321 scalar/batch bit differences (maximum 8.53e-14) remain
  unchanged; cross-kernel bit equality was not a former default guarantee.
- Six resolvent probe configurations cover 42 models and 3,696 scalar prices.
  Prepared scalar and batch calls record zero ordinary/aligned C++ allocations.
  Their bits agree on this panel. Maximum original-state mean residual is
  2.2178312014901162e-16; cumulative forward mass residual is
  1.1546319456101628e-14. After fixing the stock probe, all 42 operator
  fingerprints and all 3,696 scalar bits remain unchanged.

The owning Release toolchain is clang-cl 18.1.8, AVX2, ThinLTO, precise default
floating point, Windows 11 10.0.26200 on Intel i7-1260P (12 cores/16 logical).
Timing samples were unpinned with concurrent work. They are diagnostics, not
speed claims. Parent should measure quietly before comparing latency.

## Independent reference and refinement

`american_reference_audit.py` ran in the existing isolated Python 3.12.2 /
QuantLib 1.44 environment. Initial system Python lacked QuantLib; using
`C:/atx-cache/american-reference-venv/Scripts/python.exe` corrected that without
installation. Initial 128-time/256-space, three-level refinement left the
high-volatility American put unresolved. Repeating at 256-time/512-space with
three levels resolved all **120 supported ATM rows** across the six probes.
The remaining **48 rows are explicitly unsupported**: the reference does not
infer local volatility, and its automatic mesher refuses the limited-liability
cash-collapse fixture. These exclusions are not accuracy successes. Local-vol
and cash semantics have internal independent invariants/dense Snell checks.

The following are maximum absolute ATM American errors over call/put against
the refined FD result, in price units. `--steps s` means both maximum clocks
are 1/s; it is not the number of transitions for every fixture.

| Fixture | 129 states, upper 4, s=4 | 129, upper 4, s=1024 | 257, upper 8, s=1024 | FD refinement uncertainty |
| --- | ---: | ---: | ---: | ---: |
| Martingale | 0.457821 | 0.004246 | 0.002671 | 0.000011 |
| Short, low volatility | 0.008907 | 0.000562 | 0.000554 | 0.000006 |
| High volatility | 1.187871 | 0.012660 | 0.005749 | 0.000555 |
| Dated cash | 0.229334 | 0.003228 | 0.002194 | 0.000055 |
| Paused variance clock | 0.271332 | 0.006638 | 0.005742 | 0.000055 |

Intermediate s=16,64,256 data are retained. Coarse-step error is substantial;
time refinement improves this panel, while state spacing, tails and exercise
discretization still matter. The combined 257-state/upper-8 check changes both
grid density and domain; it does not identify their individual contributions.
The reference's observed stability is not a rigorous accuracy certificate.
This first-order operator and these results justify no automatic method switch,
production-accuracy guarantee, higher-order claim or universal default width.

## Receipts and integration

Artifacts use prefix `C:/atx-cache/quote-benchmark/markov-resolvent-20261016-`:

| Suffix | SHA256 |
| --- | --- |
| `tests.xml` | `AD5A90B70F4AD53B30A1DADB7B76558DA283BE2B9A0E25866A58DDF287454EC2` |
| `fit-tests.xml` | `BBD87EF08A895A46ECB21C7D296FF029CFE9B07B6F317DADECD6BFCE2FD80B42` |
| `explicit.json` | `C74AAEFAC286DEEC925B83385FCE5D4EB8639EDF69CEC2AB1D361CE2D05765C3` |
| `explicit-audit.json` | `3E37987DA1DF3659762DE4A8E36590295E41868DD04549551DB31B5C1688376A` |
| `explicit-comparison.json` | `2FBAC20CBB36A9ABF88F0D81943147B577B87051C7E5359869C558C35D0E77D5` |
| `explicit-audit-comparison.json` | `A8A79871B47622A44657A0CE5051748D25F5539E0385B73E5AE496F9388BA78C` |
| `probe-summary.json` | `C22BB79D21C1F4353E2F10DEE5324260D7E67EA15F8ED879D3375332D16A6AC7` |
| `reference-input.json` | `F6D9F21B6A08CD8A8B7B6C6C151F403DA9B37F4467CCA0EF1069339062A8BC4F` |
| `reference-refined.json` | `C974936165CFE808FB097F204675CF8979B4D05FF29DDA9511FA07A8E60CDCF1` |
| `reference-summary.json` | `FE5F12FB812410FB0605E0C9B146084BCC29CCBE6B56D4728E924127C178B775` |

Raw probe suffixes are `n129-u4-s{4,16,64,256,1024}.json` and
`n257-u8-s1024.json`. Reference checkpoints are per-contract durable SQLite.
The unrefined reference receipt is preserved separately. Source inputs, method,
work counts, full numeric operator/price fingerprints and physical storage
diagnostics accompany each probe. No parquet-specific logic was added.

Temporary CMake changes are restored before commit. Parent owns permanent
registration of `american_markov_resolvent_test.cpp` in the focused pricing
target, the existing standalone probe target, API manifests and native/audit
integration. No new public header is introduced. New internal helper is
`src/pricing/american_markov_transition.hpp`.

Parent policy validation and JSON provenance must include `diffusion_method`
and `max_transition_work`; diagnostics include `transition_work`,
`resolvent_steps`, `physical_resolvent_factors`, `resolvent_factor_blocks` and
`max_forward_mass_error`. Existing explicit `rows()` is empty on a resolvent
transition: consumers must dispatch on kind or use the checked raw action.
The parent owns policy validation negative cases, because that policy layer is
not part of this lane's frozen base. Exact-SHA independent review precedes
integration.
