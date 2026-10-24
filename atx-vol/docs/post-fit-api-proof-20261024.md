# Same-surface post-fit proof: full SPY correctness receipt

One fresh process from reviewed source
`3b3fd81e038ba6ca90d0e946ce1b0d0fbfb1cbc1` completed the full SPY fit and
then audited that same retained surface. All 13 financial checks and all eight
post-fit ownership/provenance checks pass under the independently reviewed
`2c08202a76435cc7d4932e0eaeb44230ed4dae72` verifier. There was one actual
surface-fit invocation before and after proof, and zero refits.

The result serves all 12,866 eligible rows across all 33 valid slices, with
12,851 inside their original raw quote boxes (99.8834136484%). It publishes
12,850 identified IVs and 16 truthful PriceOnly results. The actual API has zero
shape/parity/calendar, row-price, metadata, target-floor or economic cold-price
failures. The maximum independent cold Reference price residual is
`0.0005012966094284366` USD/share, within each row's
`min(0.001, 0.01 * original raw spread)` budget. The two reviewed floor
transitions pass the explicit economic-v2 contract; the strict baseline
identification diagnostic remains false. No strict-IV or Greek accuracy claim
is made.

## Clock and ownership evidence

The new `retained_surface_publication_v1` clock includes ownership setup and
publication. Proof runs after the cold clock and fit/CPU snapshots. It queries
one retained snapshot containing all 33 members, then releases its owner.
All attempted/retained/serialized member IDs agree. The recorded durations are
diagnostics from this correctness run:

| Region | Seconds |
| --- | ---: |
| Cold publication, including retention setup | 1.5679207 |
| Post-fit actual-API proof | 14.536691 |
| Subsequent retained-owner destruction | 0.0001511 |
| Historical process-wall boundary, through CSV write | 1.9022354 |
| Launcher wall, including proof and report output | 17.5780843 |

No duration was subtracted from a measured interval. The historical
process-wall boundary excludes the later proof and report output, as stated in
the native report. These are not qualified single-core latency measurements:
the binary is Release AVX2 without IPO, the process was unpinned, and the host
was active. This launch did not set `ATX_VOL_FIT_WORKERS`; the native report
records automatic resolution to 16. The initial launcher metadata incorrectly
asserted one worker. Original `launch.json`, `receipt.json`, and `native.json`
are preserved unchanged; `receipt-derived-worker-erratum.json` records the
correction separately. No retry was run.

## Reproduction and immutable evidence

The exact native command is in `receipt.json`; it uses the original frozen SPY
CSV, the unchanged joint-feasible-mid policy, funding-PV dividends,
`--post-fit-acceptance-audit`, and `--timing-detail`. The full verifier command is:

```text
python spy_acceptance.py verify --input quotes-SPY.csv --audit-dir post-fit-full-spy-v1 --out verification.json --accuracy-contract raw-spread-price-v1 --identification-contract economic-floor-identification-v2
```

The separate marker verification invokes `post_fit_proof_checks` on that same
native report, its own CSV hash and its launch receipt. The exact verifier and
both pinned identification fixtures are archived alongside the output. The
archived executable equals the executable used for this run; the archive alone
is not a dependency-complete distribution.

Evidence directory:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/calendar/post-fit-full-spy-v1`.
Its `evidence-manifest.json` pins 18 artifacts and has SHA-256
`4dd1785aef4488b4767a2fa4d9b13b7f514797dbfeb26e58cdd1ae87841fafda`.

| Artifact | SHA-256 |
| --- | --- |
| Executable | `6f76eee6f6e4b2873aac815c7a76c1debfa487c605ab390d0c704f5bd807de2d` |
| Native report | `e7c04805ec5dd63ed94d71fe1f2585752eccc3a4b4d950a7f013f7f0f8d638c3` |
| Published CSV | `e086fd069a81c2c9eb84bb99e25e8a59e5d78fca8b02b0d51556668eff354d5f` |
| Original input | `b3fafd80f09326ce039ebdcfd7c26914ecbeb39817fdd00b35c6d49dcb2bf33e` |
| Policy | `781ddc09f7f26cfbfeb6c32edae1e3591b725a8f8d8d5b85d18eeda00b23f34a` |

CSV equality with an earlier qualified solution is only a diagnostic. This run
passed its own complete financial proof; numerical identity was not required.
No timing campaign, sub-second result, serving-tier build, discrete-dividend
accuracy or live-tick qualification follows from this receipt.
