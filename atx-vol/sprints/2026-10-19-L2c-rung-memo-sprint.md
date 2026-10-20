# L2c-rung-memo sprint: one inversion pass per expiry, shared by every rung (vol-sota W3, 2026-10-19)

Lane branch `feat/vol-L2c-rung-memo-20261019` (base: PM prebase `feat/vol-w3-prebase-20261019 @
53ba6a3f` = root c8acdd90 + L2d + L1 + L1c + L2a + L2b). Opt-in only: key `graded.rung_memo`
(`QuoteSurfaceConfig::rung_memo`, default off); every default path is byte-identical (section 4).
Evidence: `C:\atx-cache\quote-benchmark\L2c-rung-memo-20261019-evidence\` (`sha256.txt`).

## 1. The measured problem (prebase binary, SPY cohort5 slice, `--timing-detail`)

With L2a's accurate cache ON (`fit_space vol`, `deam_cache`), the cache is THREAD-LOCAL, and the
per-expiry fan-out (L2d) spawns fresh worker threads for EVERY stage (`parallel_for_dynamic`
creates its jthreads per call; the calling thread does not take tasks). So above one worker
pass A, B1, B2 and the serial calendar/repair rungs each rebuild an expiry's inversions:

| prebase, L2a keys ON | workers | de-Am AL solves (deam + other) | price AL | wall s |
| --- | --- | --- | --- | --- |
| SPY graded-v1 | 1 | 11,425 + 79,787 = 91,212 | 34,694 | 29.7 |
| SPY graded-v1 | 2 | 32,536 + 158,975 = 191,511 | 34,694 | 32.6 |
| SPY ladder point | 1 | 11,425 + 87,270 = 98,695 | 58,817 | 43.0 |
| SPY ladder point | 2 | 37,718 + 292,820 = 330,538 | 58,817 | 81.6 |

(At 2 workers graded-v1 is SLOWER than serial.) The vol-space band fills were also charged to
the `other` timing bucket (no Deam scope around `vol_space_channel_observations`).

## 2. What was built (all opt-in)

- `src/fitting/rung_memo.hpp` (new, internal): `detail::ExpiryRungMemo` (one per expiry slot),
  `ScopedRungMemo` (installs a slot's memo on the fitting thread). `fit_quote_surface` installs
  slot i's memo around every fit of expiry i: the fan-out tasks (pass A / knot pass, B1, B2),
  `channel_fit` (B1/B2, calendar retry, ladder retry, dominance refit) and both repair rungs.
  A slot is touched by one thread at a time (fan-out writes slot i from task i; stages join
  before the next), so no lock.
  1. De-Am IV cache: under `deam_cache`, `VolSpaceCache` takes the expiry's cache from the slot
     instead of the thread-local memo, with L2a's EXACT key (`deam_iv_key`, compared word for
     word). Invalidation = the key. No rung changes a keyed input: an expiry's rungs differ only
     in the repair settings (`QuoteSliceConfig::repair`, off at pass A), the channel prior and its
     strength, none of which an inversion reads (`DeAmIvCache.MemoKeyCoversEveryInputThatChanges
     AnInversion` pins key coverage); a rung that changed a keyed input would simply miss.
  2. Candidate (certification-grid) memo: a candidate is UNCHANGED when the fit-level American
     model words (`deam_iv_key` of the prepared fit with no quotes: market, derived carry and
     clocks, dividend model and dated dividends, AlOpts, lattice controls) and, at every
     certification strike, (strike, fitted sigma, contract yield) are bit-identical; then its 2N
     American grid prices are bit-identical and are reused. The certificate (AL-free) and every
     repair still run on them, so outcome and message come from the same code. Hits: a repair
     rung that refits the pass-A candidate with the repair on; B2/retry refits that land on the
     B1 curve.
- `QuoteSlicePreScreen {Pass, EmptyBand, TooFewChannels}` in `QuoteSliceChannelDiagnostics::
  prescreen`: the typed reason of the pre-solve refusal ("too few usable bid/ask channels",
  InsufficientQuotes, unchanged), read from the usable bands (the cache under VolChannel).
- Timing (item 6): the channel observation build (both spaces) runs under the Deam bucket, so
  `al_by_bucket.deam` is the whole inversion work and `other` prices nothing.
- review-L2d F2: `fit_quote_surface` returns a typed error (bad_alloc / system_error ->
  Unavailable, other -> Internal) instead of letting a worker's rethrown exception or a thread
  creation failure escape; test seam `detail::ScopedExpiryTaskHook` (expiry_schedule.hpp).
  F3: `longest_first` asserts its precondition (debug builds).
- review-L2a minor: `DeAmIvView::band` fills a local copy and publishes it with the kBand bit only
  when complete (a throw mid-fill no longer leaves a half-filled entry served as final).
- Policy key `graded.rung_memo` (bool, strict; provenance only when true). Policies (keys ON):
  `graded-v1-rungmemo.json`, `knot-ladder-v2-{point,strict}-rungmemo.json` in the evidence folder.

## 3. Not built (findings)

- **Pre-screen beyond today's refusal (item 3).** The certificate is on the FITTED curve's
  American prices, and graded admission has no containment rule, so no condition on the quotes'
  cached IVs (mid-IV band crossings, a vertical-spread bound on bid/ask) is NECESSARY for a
  certificate failure: such a screen would refuse slices today's path serves. For the ladder's
  containment, a crossed-market lower bound on outside quotes is exact in principle, but the
  surface flow (B1/B2/repair/retry each record their own refusal and diagnostics) cannot be
  short-cut without changing refused-row detail; 0 such SPY chains are expected (clean quotes).
  The screen therefore types today's exact pre-solve refusal and saves no work.
- **Certification screen wiring (item 5, `graded.cert_screen`): left undone.** Every cohort5
  graded and ladder profile sets `channel.repair_half_spreads 0.5` (and the ladders the channel
  repair). On a screen refusal `certify_and_serve` still needs all 2N ACCURATE grid prices for
  the bounded-move and channel repairs, so the screen cannot shrink the price bucket on these
  policies; it only adds Fast prices (a prefix on refusal, 2N on acceptance). It saves work only
  with both repairs off. Also, per the L2b review (m1), it needs a Fast-vs-Reference pin on
  certification-like inputs (short T, extreme moneyness) before any key turns it on.

## 4. Parity (keys OFF) and identity (keys ON), lane binary 9982fac0 (sha256 `0fb68463...`)

`scripts\run_parity.ps1` (runner `quote_benchmark.py run ... --dividends funding-pv`,
`ATX_VOL_FIT_WORKERS=2`), every row MATCH:

| run | sha256 | expected | s (unqualified) |
| --- | --- | --- | --- |
| dev200 default (auto), keys OFF | `c89d0739` | `c89d0739` | 20 |
| cohort5 graded-v1 raw, keys OFF | `171fa2d2` | `171fa2d2` | 78 |
| cohort5 knot-ladder-v2-point, keys OFF | `232d2c0d` | `232d2c0d` (W0 keys-ON set) | 157 |
| cohort5 knot-ladder-v2-strict, keys OFF | `21f4ee58` | `21f4ee58` | 143 |
| cohort5 graded-v1 + vol cache + rung_memo | `171fa2d2` | = OFF | 33 |
| cohort5 ladder point + vol cache + rung_memo | `232d2c0d` | = OFF | 45 |
| cohort5 ladder strict + vol cache + rung_memo | `21f4ee58` | = OFF | 41 |

Keys-ON CSVs are byte-identical to keys-OFF, so the served/refused sets, every refusal reason and
every served price are unchanged (no new served row, no newly refused row): the served-set diff
is empty by hash. SPY graded-v1 keys ON = `087e6a54` (section 6).

## 5. Tests (suite `QuoteSurfaceRungMemo`, quote-slice exe + monolith; `DeAmIvCache` +1)

- `ThreeRungsOfOneExpiryShareOneInversionPassOnAnyThread`: knot rung, point rung and a prior
  refit of one chain, each on a FRESH thread with the slot memo: 1 miss + 2 hits, the hits invert
  nothing and price no band edge, every fit bit-identical to the unmemoised one; without the slot
  memo the thread-local memo misses on a fresh thread.
- `ARepairRungReusesTheGridOfAnUnchangedCandidate`: zig-zag carry chain, pass-A config then the
  channel-repair rung config (knot and point): grid 1 miss + 1 hit, both fits bit-identical to
  unmemoised fits; a prior refit (changed candidate) misses.
- `OneChangedBidMissesTheMemoAndChangesTheServedPrices` (Review Focus #2): one ATM bid moved by
  half a spread: IV cache and grid both miss, the fit equals the unmemoised fit of the changed
  chain, its served prices differ from the original; the original chain still hits afterwards.
- `SurfacesAreBitIdenticalAndInvertEachExpiryOnceAtAnyWidth`: graded and ladder, price and vol
  channel, memo at 1 / 2 / auto workers bit-identical to the serial memo-off surface; under vol
  keys the memo's deam AL count at every width equals the serial count and is below the
  thread-local memo's count at 2 workers.
- `VolSpaceBandFillsAreChargedToTheDeamBucket`: `al_by_bucket.other` = 0, `deam` > 0 per chain.
- `PreScreenRefusesAnEmptyBandSliceBeforeAnySolve` / `PreScreenPassesEverySliceTheSolveServes`.
- `AnExpiryTaskThatThrowsIsReturnedAsTheSurfaceError`: bad_alloc / system_error / runtime_error
  injected into expiry 1's task at 1 and 2 workers -> Err (Unavailable / Unavailable / Internal),
  no throw; the next fit is bit-identical to the reference.
- `DeAmIvCache.ABandFillThatThrowsIsRefilledNotServedHalfFilled`.
- CLI: `test_native_rung_memo_is_byte_identical_and_opt_in`, two strict-key cases.
- quote-slice ctest filter (`tests/CMakeLists.txt`) gains `AmericanCertification.*` and
  `QuoteSurfaceRungMemo.*`: 136 cases / 12 suites -> 150 / 14 (= the exe's 14 suites).

## 6. COUNT and wall (SPY cohort5, lane binary 9982fac0, one run each, UNQUALIFIED, load 15-78%)

`scripts\run_wall.ps1` / `scripts\summarize.py`; OFF = plain graded-v1 / knot-ladder-v2-point,
ON = vol cache + rung_memo; every CSV = OFF (`087e6a54` graded, `dd1d42dd` ladder).

| run | workers | deam AL | price AL | fit+reprice s | chain p50 ms |
| --- | --- | --- | --- | --- | --- |
| graded-v1 OFF | 1 / all | 191,511 | 34,694 | 95.3 / 11.9 | 2,939 / 3,929 |
| graded-v1 ON | 1 / all | 91,212 | 28,908 | 48.4 / 6.6 | 1,512 / 2,182 |
| ladder point OFF | 1 / all | 346,130 | 58,817 | 153.4 / 48.1 | 3,928 / 5,521 |
| ladder point ON | 1 / all | 98,695 | 44,680 | 49.6 / 9.0 | 1,510 / 2,539 |

COUNT: ladder ON / graded ON = 98,695 / 91,212 = 1.08 (<= 1.1); graded ON 91,212 <= L2a-alone
(prebase, 2 workers) 191,511. Still ~7-50x off the 1 s SPY target: the one inversion pass
(~91k AL solves) is what remains (L2e's lever).

Fail-on-revert (each by an inverse Edit, restored blob = HEAD; outputs in `reverts\`): slot cache
ignored -> 3 tests fail; grid lookup off -> 2; prescreen untyped -> 1; Deam scope removed -> 1;
surface catch removed -> 1 (throws bad_alloc); kBand bit set before the fill -> 1; policy key
dropped -> CLI provenance test fails.
