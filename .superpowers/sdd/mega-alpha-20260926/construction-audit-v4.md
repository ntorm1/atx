# T28 — Construction audit of the v3/v4 NAV outputs (explorer, read-only)

Status: **DONE_WITH_CONCERNS** (concerns are about disclosure only; every acceptance number reproduces).
Worktree `C:/atx-wt/pool-2`, branch `feat/aes-codex-integration-20260925`, HEAD `41fb5e39`. Nothing was built, and
nothing was run except the read-only script below. Nothing was committed (the controller commits).

- Script: `.superpowers/sdd/mega-alpha-20260926/studies/construction_audit.py` (sha256 `6ba72885…6a18`)
- Output: `.superpowers/sdd/mega-alpha-20260926/construction-audit-v4.json` (sha256 `24d5a2b9…4668`, 18 runs, no NaN)
- Command, run from `C:/atx-wt/pool-2`, took 0.4 s and reads only:
  `"C:/Program Files/Python312/python.exe" .superpowers/sdd/mega-alpha-20260926/studies/construction_audit.py .superpowers/sdd/mega-alpha-20260926/construction-audit-v4.json`
- Inputs: `build-equity/mega-nav-v[34]*/daily_modeled-1bn-stale5-v1+swap-fin-v1.csv`, `recipe.json` and `summary.json`.
  The glob also catches `mega-nav-v42-*`, and each row is labelled by its directory. The script skips `*-run` directories
  (bounded-runner receipts with no scenario CSV). Both reads of the code were at pool-2 HEAD.

## 1. Column adaptations (brief sketch vs actual CSV)

All the columns the brief uses (`rebalance`, `neutralize_used`, `banded_names`, `held_names`, `gross_leverage`,
`net_leverage` and `one_way_turnover_gmv`) exist, under the same names, in all 18 v3/v4/v4.2 CSVs (72 columns each).
`banded_names` is present in every v3 run too, and is 0 for the b0 runs. Nothing is NaN. Adaptations:

1. **Member count.** The brief divides by `neutralize_used`. That undercounts members: `neutralize_target`
   (`strategy_price_exposures.cpp:395-404`) puts every member in exactly one of `used` or `excluded`. Excluded members
   get desired weight 0, are held at 0 and so are always banded. The result is that `banded / used` goes **above 1**
   for every b2 run (VAL 1.001, TRAIN b2 1.008). The script therefore reports `banded_share = banded / (used + excluded)`,
   i.e. divided by the true N_d that sets the band. It keeps the brief's ratio as `banded_share_used`. The §0 percentages
   match the member-count version.
2. **`band_multiple`** is missing from the v3 `b0` recipes (the band is off), so it is reported as `null`.
3. **Additions** (these give extra evidence and are not needed for acceptance):
   - `members_mean`.
   - The gross path: median, max, last value, and mean GMV in dollars.
   - The entry filter at the deployment decision.
   - Bounds on the entry rate.
   - A `summary.json` cross-check: mean gross, held names, banded names, net/gross SR, and the official τ mean/p95.
     The CSV means agree with the summary to within 0.002, because the CSV means include the zero-gross pre-deployment
     decision row.

## 2. Acceptance — the five v4 rows vs plan §0 (tolerance ±0.01)

| run | gross (§0) | net (§0) | held (§0) | banded / members = share (§0) | τ_GMV mean | net / gross SR (§0) |
|---|---|---|---|---|---|---|
| TRAIN b1 f1 | **0.813** (0.81) | **+0.025** (+0.025) | 2,717 (2,717) | 2,840 / 2,977 = **0.954** (95%) | 0.0347 | 0.431 / 1.100 (same) |
| TRAIN b1 f.25 | **0.687** (0.69) | **+0.027** (+0.028) | 2,720 (2,720) | 2,772 / 2,977 = **0.931** (93%) | 0.0267 | 0.677 / 1.132 (same) |
| TRAIN b2 f1 | **0.364** (0.36) | **+0.016** (+0.016) | 1,366 (1,366) | 2,958 / 2,977 = **0.994** (99%) | 0.0205 | 0.643 / 0.929 (same) |
| **TRAIN b2 f.25 (frozen v4.1)** | **0.256** (0.26) | **+0.041** (+0.041) | 1,369 (1,369) | 2,959 / 2,977 = **0.994** (99%) | 0.0136 | 0.687 / 0.815 (same) |
| **VAL b2 f.25 (trial #2)** | **0.235** (0.24) | **+0.038** (+0.038) | 1,237 (1,237) | 2,981 / 2,999 = **0.994** (99.7%) | 0.0133 | 0.641 / 0.802 (same) |

All five rows are within ±0.01 on gross, net and banded share. The largest gap is 0.005 (VAL gross 0.23506 vs 0.24; the
VAL banded share 0.994 vs 99.7% differs by 0.003). The counts match exactly. The SRs are read from `summary.json` and
match §0 to the digit. τ_GMV is the plain CSV mean. The official statistic, which excludes deployment, is in the JSON
and is the same to within 0.0002.

## 3. Full table (every NAV output under the glob)

Each cell's column meanings:
- `dep pass`: the share of members with |desired| > band at the deployment decision.
- `dep gross`: the desired gross carried by those names, which is `planned_gross / f` on the deployment row.
- `held/N`: mean held names ÷ mean members.

| dir | rule | f | gross mean / med / max | net | held (held/N) | banded share | τ_GMV | dep pass / dep gross | amp | net / gross SR |
|---|---|---|---|---|---|---|---|---|---|---|
| mega-nav-v3-plain-b0 | band off | 1 | 0.978 / 0.981 / 1.000 | +0.018 | 2,907 (0.976) | 0.000 | 0.1004 | 1.000 / 1.000 | 1.109 | 1.409 / 2.856 |
| mega-nav-v3-plain-b0.5 | band-0.5 | 1 | 0.913 / 0.919 / 0.973 | +0.014 | 2,849 (0.957) | 0.863 | 0.0665 | 0.758 / 0.942 | 1.109 | 1.611 / 2.869 |
| mega-nav-v3-plain-b1 | band-1 | 1 | 0.766 / 0.775 / 0.839 | +0.008 | 2,740 (0.920) | 0.939 | 0.0472 | 0.497 / 0.746 | 1.109 | 1.807 / 2.741 |
| mega-nav-v3-plain-b2 | band-2 | 1 | 0.289 / 0.325 / 0.386 | +0.008 | 1,391 (0.467) | 0.992 | 0.0265 | 0.031 / 0.067 | 1.109 | 1.105 / 1.405 |
| mega-nav-v3-netcost-b0 | band off | 1 | 0.985 / 0.988 / 1.007 | +0.012 | 2,913 (0.979) | 0.000 | 0.0435 | 1.000 / 1.000 | 1.102 | 1.570 / 2.102 |
| mega-nav-v3-netcost-b0.5 | band-0.5 | 1 | 0.931 / 0.939 / 1.042 | +0.015 | 2,778 (0.933) | 0.950 | 0.0195 | 0.747 / 0.934 | 1.102 | 1.710 / 2.098 |
| mega-nav-v3-netcost-b1 | band-1 | 1 | 0.775 / 0.789 / 0.877 | +0.019 | 2,520 (0.846) | 0.978 | 0.0135 | 0.511 / 0.758 | 1.102 | 1.646 / 1.938 |
| mega-nav-v3-netcost-b2 | band-2 | 1 | 0.195 / 0.226 / 0.277 | +0.062 | 643 (0.216) | 0.997 | 0.0127 | 0.016 / 0.034 | 1.102 | 1.396 / 1.553 |
| **mega-nav-v3-VAL (trial #1)** | band-1 | 1 | **0.763** / 0.771 / 0.815 | +0.001 | 2,790 (0.930) | 0.941 | 0.0492 | 0.494 / 0.740 | 1.072 | −1.271 / 0.161 |
| mega-nav-v4-train-b1 | band-1 | 1 | 0.813 / 0.825 / 0.931 | +0.025 | 2,717 (0.913) | 0.954 | 0.0347 | 0.482 / 0.744 | 1.216 | 0.431 / 1.100 |
| mega-nav-v4-train-b1-f.5 | band-1 | .5 | 0.737 / 0.748 / 0.853 | +0.028 | 2,719 (0.913) | 0.949 | 0.0291 | 0.482 / 0.744 | 1.216 | 0.589 / 1.135 |
| mega-nav-v4-train-b1-f.25 | band-1 | .25 | 0.687 / 0.697 / 0.800 | +0.027 | 2,720 (0.914) | 0.931 | 0.0267 | 0.482 / 0.744 | 1.216 | 0.677 / 1.132 |
| mega-nav-v4-train-b2-f1 | band-2 | 1 | 0.364 / 0.381 / 0.461 | +0.016 | 1,366 (0.459) | 0.994 | 0.0205 | 0.047 / 0.105 | 1.216 | 0.643 / 0.929 |
| mega-nav-v4-train-b2-f.5 | band-2 | .5 | 0.306 / 0.322 / 0.379 | +0.027 | 1,367 (0.459) | 0.994 | 0.0153 | 0.047 / 0.105 | 1.216 | 0.596 / 0.779 |
| **mega-nav-v4-train-b2-f.25 (v4.1)** | band-2 | .25 | **0.256** / 0.270 / 0.324 | +0.041 | 1,369 (0.460) | 0.994 | 0.0136 | 0.047 / 0.105 | 1.216 | 0.687 / 0.815 |
| **mega-nav-v4-VAL-b2-f.25 (trial #2)** | band-2 | .25 | **0.235** / 0.267 / 0.304 | +0.038 | 1,237 (0.412) | 0.994 | 0.0133 | 0.078 / 0.185 | 1.215 | 0.641 / 0.802 |
| mega-nav-v42-train-b1-f.25 | band-1 | .25 | 0.697 / 0.718 / 0.839 | +0.035 | 2,519 (0.846) | 0.967 | 0.0112 | 0.480 / 0.735 | 1.226 | 0.164 / 0.496 |
| mega-nav-v42-train-b2-f.25 | band-2 | .25 | 0.199 / 0.225 / 0.248 | +0.027 | 812 (0.273) | 0.996 | 0.0104 | 0.060 / 0.140 | 1.226 | 0.449 / 0.598 |

Every rule string is `baseline-target-v1+neutral-price-risk-v1[+band-b]`, at cadence 1 with neutralize `price-risk-v1`.

**v3 rows are present.** Validation trial #1 (`mega-nav-v3-VAL`: b1, f1, VAL combined `ba697121`) was a **76%-gross**
book: mean 0.763 and max 0.815, net +0.001, 93% of members held. It did deploy most of the book. The book that never
deployed is the v4.1 b2 book. Note that no rule with the band on reaches D1's gross of at least 0.90, except b0.5 in
some cells (0.913 and 0.931). Only b0 (band off) holds at least 97% of members.

## 4. Mechanism (quoted from `atx-impl/src/strategy_target_replay.cpp` at pool-2 HEAD; line numbers verified)

**Desired target** (`:133-154`). The tied rank is centred, then scaled to gross 1:

```cpp
133 void desired_target(std::span<const f64> signal, std::span<const u8> member,
134                     std::vector<Ranked>& row, std::vector<f64>& target) {
...
141     const f64 r = (static_cast<f64>(b) + static_cast<f64>(e - 1)) /
142                  (2.0 * static_cast<f64>(row.size() - 1)) - 0.5;
...
150   for (const auto& value : row) {
151     target[value.second] -= mean; gross += std::abs(target[value.second]);
152   }
153   if (gross > 0) for (const auto& value : row) target[value.second] /= gross;
```

**Band** (`:161-166`, exactly as the brief cites). The band is `band_multiple / N_d`, where N_d is the member count at d,
which is the same N the rank is taken over (a member must have a finite signal, `:124`):

```cpp
161   f64 band = -1;
162   if (rebalance && cfg.band_multiple > 0) {
163     usize members = 0;
164     for (usize i = 0; i < in.instruments; ++i) members += in.member[offset + i] ? 1U : 0U;
165     if (members) band = cfg.band_multiple / static_cast<f64>(members);
166   }
```

**Fraction and next weight** (`:175-184`, exactly as the brief cites). A banded name keeps `current`. The others move a
fraction `f` of the gap. Nothing re-grosses the result: the `next` weights are summed into `out.gross` as they are
(`:189`), and the comment at `:131-132` says explicitly that "no daily renormalization changes partial fills".

```cpp
175   f64 fraction = rebalance ? cfg.trade_fraction : 0;
176   if (cfg.rule == TargetReplayRule::MonthlyTargetBudgetV2 && distance > 0)
177     fraction = std::min(fraction, std::max(0.0, cfg.monthly_budget - spent - forced) / distance);
178   out.applied_fraction = fraction;
179   f64 squared = 0;
180   for (usize i = 0; i < in.instruments; ++i) {
181     const bool live = in.member[offset + i] != 0;
182     const bool banded = rebalance && live && std::abs(desired[i] - current[i]) <= band;
183     const f64 next = !live ? 0 : rebalance && !banded
184       ? current[i] + fraction * (desired[i] - current[i]) : current[i];
```

The banding at `:172` (`if (gap <= band) ++out.construction.banded_names`) is the counter behind the CSV's
`banded_names` column.

**Neutralization.** `price-risk-v1` (`strategy_price_exposures.cpp:407-412`) replaces the ranked target with the residual
of a cross-sectional exposure regression. It zeroes the excluded members and rescales the residual so that Σ|w| is
back to 1: `scale = stats.gross / stats.residual_gross`. The mean amplification is 1.216 on v4 TRAIN and 1.215 on VAL.
The mean |w| stays at 1/N_used, but the rescale reshapes the distribution and fattens its tails beyond 2/N.

### 4.1 Desired-weight scale: |w| ≤ 2/N, mean 1/N

Take N members with distinct signals. `:141-142` gives r_k = k/(N−1) − 1/2 for k = 0…N−1. These values are symmetric,
lie in [−1/2, 1/2] and have mean 0, so the demeaning at `:151` changes nothing. Their gross is G = Σ|r_k| = N²/(4(N−1))
for even N and (N+1)/4 for odd N, which is about N/4 because E|U(−½,½)| = ¼. Then w_k = r_k / G, so:

- **mean |w| = Σ|w| / N = 1/N exactly**, because `:153` normalizes to gross 1;
- **max |w| = (1/2)/G = 2(N−1)/N² (even N) or 2/(N+1) (odd N), which is < 2/N.** Ties only average ranks together and
  shrink the extremes;
- in general |w_k| ≈ 4|r_k|/N, and |r| is uniform on [0, ½].

The band b/N is therefore **b × the mean position**. So **b1 = the mean |w|** and **b2 = 2/N = the maximum |w|**.

### 4.2 Why the band became an entry filter

A member not currently held has `current = 0`, so its gap is |desired|. By `:182-184` it enters only if
|desired| > b/N. Before neutralization that means |r| > b/4, which gives:

- the share of members that can enter is **1 − b/2**;
- the share of desired gross they carry is **1 − (b/2)²**.

| b | pass (theory) | gross share (theory) | pass measured at deployment | desired gross measured at deployment |
|---|---|---|---|---|
| 0 (off) | 1 | 1 | 1.000 (v3 b0 ×2) | 1.000 |
| 0.5 | 0.75 | 0.9375 | 0.747-0.758 | 0.934-0.942 |
| 1 | 0.50 | 0.75 | 0.480-0.511 | 0.735-0.758 |
| 2 | **0** | **0** | **0.016-0.078** | **0.034-0.185** |

The measurement is direct. On the deployment decision every member has `current = 0`, so that row's `banded_names`
counts exactly the members with |desired| ≤ band, and `planned_gross / f` is exactly the desired gross that passes the
band. At b2, no rank-only weight can pass. The 1.6-7.8% of members that do pass (4.7% on v4 TRAIN, 7.8% on VAL) are
names whose neutralized weight was pushed past 2/N by the price-risk rescale. So at deployment the frozen v4.1 rule
planned **10.5%** of the book on TRAIN and 18.5% on VAL, times f = 0.25. Gross then builds up slowly, and only through
names whose desired weight moves more than 2/N away from their current weight. The quarterly mean gross on VAL was
0.144, 0.233, 0.274 and 0.289, the max over the whole run was 0.304, and the last value was 0.278. The book never got
near full size.

A partial fraction makes this worse. After a partial entry a name sits at f·d, so its next gap is (1−f)|d|. If that gap
is ≤ band, the position freezes short of target. That is why gross falls from f1 (0.364) to f.5 (0.306) to f.25 (0.256)
at b2, and from 0.813 to 0.737 to 0.687 at b1. Held names almost never leave. With f < 1, `next` is exactly 0 only when
a name drops out of membership (`!live`), so at b1 `held` accumulates to 91% of members while gross stays stuck at
0.69-0.81.

**Net exposure.** The desired book is demeaned and neutralized, but banding freezes stale positions. The frozen v4.1
book ran net/gross = 0.041/0.256 = **16%** on TRAIN and 0.038/0.235 = **16%** on VAL. It was not market-neutral.

**Cost scale.** The mean pre-trade GMV was $272m (TRAIN v4.1) and **$238m (VAL)**. The "$1bn S2" cost scenario was
therefore charged to a ~$240m book. Impact scales with √size, so per-dollar impact was roughly √0.24 ≈ 0.5 of what a
fully deployed book would pay.

## 5. Entry rate (bounds; no per-name file exists)

The NAV outputs have no per-name weight file. Each output has the daily scenario CSVs, an `events_*.csv` that lists
only write-off, gap-resolved, guarded and reappeared events, `recipe.json` and `summary.json`. The entry rate is
therefore **bounded from the `held_names` dynamics**, not measured.

Setup:
- The decision is made at row d, after that session's execution, so `held_d` = the names with current ≠ 0 at the
  decision.
- Fills land at row d+1.
- Unheld members ≈ N_d − held_d.

Bounds:
- **Lower bound:** max(0, held_{d+1} − held_d) / unheld. Held can rise only through entries.
- **Upper bound:** min(N_d − banded_d, fills_{d+1}, unheld) / unheld. An entry needs a gap = |desired| > band, and it
  needs a fill.
- Averaged over the ~753 non-deployment decision→execution pairs.

| run | entry rate / day, lower bound | entry rate / day, upper bound | note |
|---|---|---|---|
| TRAIN b1 f1 | 1.14% | 60.9% | the upper bound is uninformative: ≈ 137 non-banded names a day, mostly held re-trades, set against a small unheld pool of ≈ 260 |
| TRAIN b1 f.25 | 1.15% | 82.1% | uninformative for the same reason |
| TRAIN b2 f1 | 0.16% | 1.19% | |
| **TRAIN b2 f.25 (v4.1)** | **0.15%** | **1.14%** | only ≈ 18 of 2,977 members are non-banded per day, and those include held names |
| **VAL b2 f.25** | **0.16%** | **1.01%** | |
| v3-VAL (b1 f1) | 1.13% | 86.3% | uninformative |

At b2, **at most ≈ 1% of the ~1,600 unheld members can enter on any day**, and at least ≈ 0.15% net do. The whole
b2 book re-trades only about 18 names a day. Caveats:
- The unheld denominator N_d − held_d under-counts unheld members by the few held non-members that are awaiting a forced
  exit.
- A stale name that reprints can fill an order from an older decision. Both effects are small.

The upper bound is tight enough to be informative for b2 and loose for b1. Getting a true entry rate needs a per-name dump, which does not exist.

## 6. Conclusion

Ruling-ready sentence: **validation trial #2 was a 24%-gross book.**

The frozen v4.1 rule (baseline-target-v1 + price-risk-v1 + band-2, f = 0.25) set the no-trade band equal to the
**maximum** rank-target weight, 2/N. That turned the band into an entry filter: 95.3% of members (TRAIN; 92.2% on VAL) were
banded at 0 on the deployment decision, and nothing re-grossed the book afterwards. Mean gross was 0.256 on TRAIN and 0.235 on VAL. Net was
+0.041 and +0.038 (16% of gross). About 1,370 and 1,240 names were held, 46% and 41% of members. **Validation trial #2
was a 24%-gross book** (mean gross 0.235, max 0.304, mean GMV $238m on $1bn NAV). Its recorded net SR of +0.641 stands,
but it must be disclosed as the result of a book that was about a quarter deployed. Validation trial #1 (v3, b1 f1)
was a 76%-gross book.

## 7. Selection hygiene and concerns

- **VAL reads.** This audit reads the two existing validation NAV outputs (`mega-nav-v3-VAL` and
  `mega-nav-v4-VAL-b2-f.25`). They are book-level construction diagnostics, re-read from outputs that are already
  recorded (plan §0 and progress.md:76/253). Nothing is selected, fitted or signed from them. No new validation run and
  no per-candidate VAL statistic are involved. The lane contract's "never read validation" rule is overridden only by
  the brief and the controller's explicit request. The controller should record this in the trial accounting as a
  disclosure re-read, not as trial #3.
- **Rounding.** VAL gross 0.23506 rounds to 0.24 at two decimals. "24%" is the rounded figure; the precise value is
  23.5%.
- **Brief's formula.** The brief's `banded / neutralize_used` ratio goes above 1 for b2 and should not be reused. Use
  `neutralize_used + neutralize_excluded` as N_d.
- **Entry rate.** It is bounded, not measured (no per-name file). The b1 upper bound is uninformative.
