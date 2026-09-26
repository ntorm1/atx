# Dated exposures and residual IC bounded qualification

2026-09-26. Clean compiled and tested source `4816bb85`; configured provenance
`73ace5dd`. **All 12 new focused checks pass**, with no runtime failure or skip.
Source reviews are `1eee7c95` (I2) and `4816bb85` (A4).

| Cohort | Result | Native seconds |
|---|---:|---:|
| ExposurePanel | 6/6 | 0.010 |
| FactoryResidualObjective | 6/6 | 5.746 |

I2 checks cover independent cap-centering arithmetic, original security slots,
strict evidence and closure clocks, missing prerequisites, partial warmup,
unknown membership, honest normalization bounds and resource refusal.
A4 checks cover sqrt-cap WLS, exact factor-proxy removal, a small genuine
residual, inverse orientation, near-collinear cancellation, rank deficiency,
future mutation, explicit price presence, independently summed calendar HAC,
consecutive-date persistence and source/clock/budget binding.

The source review led to `e04b1a49`/`99111bbc`: weighted QR orthogonal-complement
projection and a cancellation-aware roundoff guard. The near-collinear fixture
reaches full QR rank, refuses a pure proxy and retains a distinguishable residual.
This is a numerical guard, not an economic minimum-alpha amplitude.

## Compilation and attribution

Registration configured once in **21.252283 s**, preserving the existing PCH,
compiler flags, dependency tree and cache. The first Jobs2 build stopped after
**13.3670732 s** because the new digest helper lacked its closing brace. Root
`f4c6ff38` adds that single brace; no numerical algorithm changed. Two successful
objects were retained. One resume launch was refused before compilation when
physical headroom was815 MiB; it produced no compiler actions.

The Jobs2 resume at `4816bb85` passed in **21.8520023 s**, compiling the remaining
production and fixture files and performing three links. Across both actual
builds: four distinct new CPPs, five compiler attempts including the failed one,
and three links. No existing caller, PCH or dependency was recompiled. Build
time was35.2190755 s excluding configuration,56.4713585 s including it.

An exact unchanged repeat took **3.750396 s**, with Ninja reporting no work and
no compiler/cache calls. These are shared-host measurements, not controlled
cold-build or throughput comparisons. The low-memory launch used two workers;
no unrelated process was stopped.

The companion receipt binds both executable hashes, all XML/log/receipt files,
configuration and build attempts. Full case index:
`build-equity/w2-exposure-ic-qualification-index.json`, SHA256
`ef523c3cc985a7731d993de905b69a80773cbcd73dcbdcea2889448756dc5f93`.
The previous E2/L2 engine-IC binary is now historical; its old receipt remains
valid for that earlier execution and is not attributed to the new executable.

## Remaining scope

These checks qualify an in-memory exposure contract and its residual objective
kernel. The actual six-price-descriptor producer and residual Fitness/Search
consumer are still in development. Real PIT cap/classification, persisted I2
stage/artifact, coverage, combined delayed-net objective, practical screen
pruning/recall, scale gates and empirical alpha admission remain open. No actual
market payload, long benchmark, mining run or warehouse write occurred.
