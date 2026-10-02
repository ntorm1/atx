# Orientation consumers: findings for Ruling PM5-16 / PM5-18 (read-only)

Source read: `C:/atx-wt/pool-2`. HEAD there is now `d19f9638`. It differs from the briefed `807af678` only in three
sprint docs (progress.md, integration-log.md, review-6c-fix4.md; `git diff --stat 807af678 HEAD`), so every code line
cited below is the same at `807af678`. I opened nothing under `build-equity/`. I read no IC level, sign, weight or
return, and I ran nothing.

## Plain answer to question 3

**(i).** In the book that B0a trades, each member's sign is the literature prior: `prior_sign` = +1, with the direction
built into the DSL, taken from the pinned library and recipe. It is not the sign of the run's own window. The path:

1. **Fit.** `scripts/specs/v8/base-lo1.json` `fit.flags` = `--orientation prior --screen v4-prior-v1 --composition
   ew-theme-v1`.
   - `fit_composition_weights.py:2287` sets `sign = prior_signs[k]`.
   - `:2381` writes the weights file with `"signs": {row["id"]: row["sign"] ...}`.
   - `:2249-2250` records the rule text "s_k=prior_sign=+1 (sign embedded in the DSL); no sign estimation or flip; ...
     runner sign reported not used".
2. **W pass.** `research_cycle.py:978-982` runs the w pass with `--composition-weights {w_dir}/composition_weights.json`.
   - `strategy_ic_admission.cpp:423-446` loads the `signs` block.
   - `strategy_ic_runner.cpp:433`: `const int blend_sign=blend_signs.empty()?sign:blend_signs[k];`
   - `:437`: `composition->add(k,signal,blend_sign,...)`.
   - `:502` saves the result as `train_combined.*`.
3. **NAV.** `research_cycle.py:987` sets `comb = f"{wt_out}/train_combined.json"`, and `:1171` passes `--combined comb`
   to the targets exe.

The run's own whole-window IC sign reaches the B0a book nowhere. It reaches:
- diagnostics and reports;
- the u-pass blend, which nothing reads;
- provenance hashes;
- the `sign_agrees` gate. B0a's gate lists no members (`base-lo1.json` `gate.admitted: []`), so for B0a the gate does
  nothing.

Which members are in the book does depend on data from the 4-year window: the v4-prior-v1 HAC veto, redundancy and tau
are computed on that window. That is the same registered rule the ledgered cells used. It is not orientation.

---

## Q1. Orientation as coded

**Statistic, horizon, window**
- `strategy_ic_runner.cpp:419-421`:
  - `orientation = scored.screen.horizons[1].rank` (the rank IC at horizon 21);
  - `fit = valid_dates>0 && isfinite(mean) && mean!=0`;
  - `sample_sign = fit ? (mean>0 ? 1 : -1) : 0`.
- The mean is the arithmetic mean of the finite daily rank ICs over the role's scored window. It is computed before any
  `min_dates` or coverage test, so a candidate with too little evidence still gets a sign:
  - `atx-engine/src/factory/ic_screen.cpp:194-204`: the sum of finite days divided by `valid_dates`;
  - window `[score_begin, score_end)`: `strategy_ic_runner.cpp:293` (`research_window_ic_config(role.score_begin,
    role.score_end, ...)`) and `ic_screen.cpp:519-525` (`maturity_end = end`).
- So the window is the run's whole TRAIN role: 2020-2022 on the 3-year role, 2020-2023 on the 4-year role.
- Daily rank IC:
  - a day whose paired names fall below `--min-names` (1000 here) is NaN and is excluded;
  - labels are `close[d+1+h]/close[d+1]-1` with strictly positive observed endpoints and role maturity, under the return
    guard;
  - the recipe states this at `strategy_ic_admission.cpp:43-45`, orientation text
    "TRAIN21h-nonzero-sample-rank-mean;undefined=0;screen-diagnostic-only;freeze-before-validation".

**Ties, zero, NaN**
- Ties are inside the rank IC: Pearson of tie-averaged ranks, as described at `alpha_report_card.py:1021`.
- A mean that is exactly 0, NaN, or has no valid date gives sign 0. Then:
  - `oriented_rank_ic` is written empty (`strategy_ic_runner.cpp:182`: `if (sign!=0 && isfinite(r)) out<<sign*r`);
  - `oriented_mean` is null (`:149`);
  - the blend treats the candidate as neutral (`strategy_ic_composition.cpp:229`: `if (sign == 0 || weight == 0)` skip).
- Signs are fitted only on the role named `train` (`:342`, `:422-423`). Any other role uses the frozen signs
  (`:343-344`, `:431`).

**Written to**
- `orientations.json` (`:708-724`). Schema `atx.dsl-ic-orientations/v1`, keys:
  - `recipe_sha256`, `library_sha256`, `train_manifest_sha256`;
  - `candidates[]`, each with `id`, `family`, `dsl_sha256`, `sign`, `sample_orientation_sign`, `orientation_defined`,
    `orientation_horizon` (21), `orientation_dates`, `diagnostic_keep`, `composition_selection`, `reason`, `fit_status`,
    and `ic` (full `result_json`);
  - optionally `research_fields` (`:424-429`, `:710-720`).
- `summary.json` keys `orientations_artifact_sha256` (the file's SHA) and `orientation_recipe_sha256` (`:721`, `:724`).
- Per candidate:
  - `frozen_train_sign` in `summary.json` and `train_candidates.jsonl` (`:444`);
  - `oriented_mean` in each estimate (`:149`);
  - the `oriented_rank_ic` column of `train_daily_ic.csv` (`:341`, `:182`).

**Existing options to take orientations from elsewhere**
- **IC runner `--orientations FILE --orientations-sha256 SHA`** (`:797-798`) exists, but it only switches the run to
  validation-only mode (`:568`). In that mode:
  - it needs `--validation` (`:579`);
  - the TRAIN payload is never opened (`:610-615`, `roles.erase`);
  - the file must carry the same `train_manifest_sha256` as `--train-sha256` (`strategy_ic_admission.cpp:136-140`);
  - TRAIN and validation must not overlap (`strategy_ic_runner.cpp:600-603`).
  - So it cannot orient a scored TRAIN role, and it cannot carry the 3-year signs into the 4-year role.
- **IC runner `--composition-weights` with a `signs {id: ±1}` block** (`strategy_ic_admission.cpp:421-446`; help text at
  `strategy_ic_runner.cpp:767-768`) pins the blend sign only. "Pinned signs orient the blend only; every IC diagnostic
  keeps `sign`" (`:432-433`). This is how the w pass applies the prior signs.
- **Fitter `--orientation prior`** (`fit_composition_weights.py:262` `ORIENTATIONS = ("train","prior")`):
  - must be paired with `--screen v4-prior-v1/v2` (`:1981`);
  - reads `prior_sign` from the pinned library and recipe (`load_priors`, `:514-560`; -1 is refused at `:558`, 0 leads to
    reject_no_prior);
  - writes the pinned `signs`.
- **Registry and library.** `atx-impl/strategies/alphas/registry.json` `alphas[].prior_sign`, and the v7.1 library and
  recipe lineage, carry `prior_sign` = 1 for all 48 candidates. The recipe's `orientation` block reads:
  - "prior sign embedded in the DSL ... every candidate has prior_sign +1";
  - `pinned_signs`: "T23 fitter --orientation prior writes pinned +1 signs; no TRAIN sign flips";
  - `sign_estimation: false`.
- **The IC runner ignores `prior_sign`.** It requires every library row's `sign_policy` to equal "train-rank-ic21"
  (`strategy_ic_library.cpp:144`).

**What `orientations_artifact_sha256` pins** (in the combined, "composition", manifest `<role>_combined.json`,
`strategy_ic_runner.cpp:110`)
- It is the SHA-256 of the orientations.json file whose frozen signs were used.
- It is set only for a role scored with frozen signs: the validation-only run's `--orientations` (`:694`, `:701-702`),
  or the validation role of a TRAIN+validation run (`:724`, then `:701-702`).
- For a TRAIN role (the u and w passes, and B0a) it is null: the report key does not exist yet when TRAIN is scored.
- Its sibling `orientation_candidates_sha256` (`:102`, `:109`) is the SHA of the in-memory candidates array (signs plus
  the full IC rows of this run). It is always present.
  - The replay exe requires it to be a valid hash (`strategy_target_replay.cpp:682-685`).
  - The NAV summary republishes it as `source_bindings` (`strategy_target_replay.cpp:1405`;
    `strategy_nav_replay.cpp:2685-2696`).
  - The ledger pins it (`backtest_integrity.py:602`); live deploy binds it (`strategy_live.cpp:398-400`).
  - It is a provenance pin only.
- The weights file pins the u pass's orientations file separately, in `provenance.orientations_sha256` and
  `orientations_recipe_sha256` (`fit_composition_weights.py:2399-2400`). The fitter checks that the runner summary
  names that file (`:657-659`).

## Q2. `__combined__`

**How it is built**
- Any run without `--no-composition` builds it, so both the u pass and the w pass do.
  - Built: `strategy_ic_runner.cpp:311-323`. Evaluated: `:476-483`, with `series(daily,"__combined__",...,1)`, so
    `oriented_rank_ic` equals `rank_ic` on this row.
- Inputs:
  - all library candidates, centred tied ranks per date over effective members (`:313-317`);
  - sign = `blend_sign` (`:433`), which with no pinned weights is the run's own whole-window IC sign;
  - weights: default equal-family/equal-within, `1/(F*n_family)` (`strategy_ic_composition.cpp:184`), or the pinned
    values (`:180`).

**u pass vs w pass**
- u pass: default weights and the run's own TRAIN signs.
- w pass: ew-theme-v1 weights and the pinned prior signs.

**Is the u-pass row consumed?** No. It is a diagnostic.
- Plan section 5 D4 says "u pass blends an unread book" (`docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md:133`).
- `research_cycle.py:949`: "B-1: a screen skips the u-pass blend nobody reads", and `run --screen` drops it.
- The u pass's saved `train_combined.*` (base-lo1 keeps `--save-combined`) is also unread:
  - every reader of `train_combined.json` uses the w pass (`research_cycle.py:987`, `research_spec.py:104`,
    `research_add_alpha.py:165`);
  - `mega_report/pitch.py:307-317` computes `u{h}` keys, but only `w{h}` keys are rendered (`:796-799`, `:833-835`).
- Its only reader is `compare_window_overlap.py` (R13).

## Q3. Consumers, end to end

Key: (a) = the data-derived sign; (b) = `oriented_rank_ic`; (c) = the u-pass `__combined__` row. "Book?" = whether
B0a's traded book depends on it.

| consumer | (a) | (b) | (c) | Book? |
|---|---|---|---|---|
| IC runner, u pass (`strategy_ic_runner.cpp:419-444`, `:182`, `:433`) | writes it; blend sign | writes it | writes it | no (u blend unread) |
| fit `v4-prior-v1` + `ew-theme-v1` (`fit_composition_weights.py:2004`, `:2193-2400`) | `runner_sign`, `sign_agrees`, `sign_conflicts(_weighted)` reported only (`:2232-2233`, `:2249-2250`, `:2376`); orientations SHA pinned in provenance (`:2399-2400`) | no | no | weights and signs no; weights-file bytes yes (provenance) |
| `--screen none` (T9) / `v3-admit-v1` | none uses runner signs as book signs (`:2096`); v3 uses the sign of the FIT factor mean | no | no | not in any v8 spec |
| `ew-theme-std-v1`, `-aim-v2`, `ic-shrink-*`, `theme-resid-v1` (`composition_rules.py`, `composition_ic_shrink.py:10,56`, `composition_resid.py:342-362`) | no: prior signs and the parent's signs; ic-shrink uses the prior-signed `train_mean` | no | no | no |
| w pass (same exe, pinned weights and signs) | diagnostics keep it (`:432-433`); `orientation_candidates_sha256` (`:102`) | writes its own | no | no (hash only) |
| targets/NAV exe (`strategy_target_replay.cpp:682-685`, `:1405`; `strategy_nav_replay.cpp:2685-2696`) | hash pin only | no | no | no |
| `nav_summ.py` (weights only, for the netting ratio, `:24-25`, `:487-508`) | no | no | no | no |
| `backtest_integrity.py:602` (ledger pins) | hash pin | no | no | no |
| `research_cycle.py` gate (`:1397-1436`, `:1418` `sign_agrees` default true) | yes: STOP (no w or nav) if listed members' runner sign disagrees with the prior | no | no | B0a: no (`admitted: []`). r2, r7, R-12: whether the cell runs at all |
| add-alpha identity compares `parent-orientations`, `parent-train-daily-ic` (`research_add_alpha.py:191-195`) | json-rows identity, same window | csv-rows identity | no | no (hard-stop identity) |
| `alpha_report_card.py` (`:305`, `:318`, `:325-326`, `:1024`) | `runner_sign` and `frozen_train_sign` as metadata; raw `rank_ic` | no | no | no |
| `book_monitor.py` (`:218`, `:226`) | no: admission `s_k`; raw `rank_ic` of members | no | no | no |
| `book_diagnostics.py` (`:745-750`, `:961-971`) | no: weights-file signs; orientations used for identity and cache only | no | no | no |
| marginal IC verb (`strategy_marginal_ic.cpp:140-160`, `:547-548`) | no: pinned or library prior sign | no | no | no |
| mega report (`pitch.py:86-93`, `:307-317`; `analysis.py:23`) | no: raw `rank_ic` | no | computed, not rendered | no |
| `compare_window_overlap.py` (`:18`, `:64`) | no | yes | yes | no (R13 only) |

The full-repo grep for `oriented_rank_ic`, `oriented_mean` and `frozen_train_sign` outside tests finds only the runner,
`alpha_report_card.py` and `compare_window_overlap.py`.

## Q4. Governing text and the ledgered cells

The book answer is (i), so the "if (ii)" branch does not apply to the book. The texts that make the book prior-signed:
- prereg rule 10: "prior: literature sign and canonical definition";
- prereg rule 11, which pulls in plan sections 7 and 9:
  - W0-4: B0a is "v7.1, ew-theme-v1 ... protocol as v7";
  - R-2: "read admission under v4-prior-v1 (unchanged rule)";
- plan section 13: "dropping or flipping a member on its TRAIN sign" is do-not-build;
- the v7.1 recipe's orientation block: "no TRAIN sign flips".

No v8 ruling before PM5-16 addresses the runner's orientation (grep of `progress.md`: only lines 1153-1192 hit).

**Ledgered cells on the 3-year window**
- Every spec-driven cell used `--orientation prior --screen v4-prior-v1 --composition ew-theme-v1`: `scripts/specs/v61`,
  `v61-ops`, `v70`, `v70-lo3`, `v71` and `v7u-lo3.json`. Their books were therefore prior-signed, not 3-year-data-signed.
- The v5 and v6 cells (N 1-28) used ew-theme-v1, ew-theme-aim-v1 or ew-theme-v6 prior fits
  (`.superpowers/sdd/mega-alpha-20260926/progress.md:390-395`, `:466-470`). The runner's pinned blend signs predate
  v4-prior-v1:
  - pinned signs: `git log` `a9ef7175`, 2026-09-27 08:59;
  - v4-prior-v1: `791f1e4b`, 12:26 the same day.
- The 3-year own-window runner sign did enter those cells in two places:
  - the diagnostics;
  - the P1 promotion gate on new members: v4-prereg P1 "runner sign agrees with the prior"
    (`mega-alpha-20260926/v4-prereg.md:150`), implemented as spec gates `p1` / `p1-v70` / `p1-v71` with
    `sign_agrees: true`.

**Consistency of a 4-year sign**
- Taking a 4-year sign into the book's member signs would contradict plan section 13 and how the cells were run.
- The code as written does not do that.
- Using the 4-year own-window sign in diagnostics and the P1 gate is the same rule the ledgered cells used, applied to
  the new window.

**Flag for the PM.** W0-4 step 4 re-runs v6.1, v7.0 and v7.1 from their specs on the 4-year role. Through
`research_cycle.py`, their `sign_agrees` gates would be re-evaluated with 4-year signs, and a failed gate stops before
w/nav (`:1436`). I did not cross the gate lists with the three flipped names, because doing so could imply a sign.

## Q5. Cause test commands for PM5-16

Setup (as in runbook R13):
- `PY="C:/Program Files/Python312/python.exe"`, `IC=build-equity/bin/atx-equity-strategy-ic.exe`;
- `LIB=atx-impl/strategies/fund_industry_ic_v71.json`,
  `LIBS=787c802ed1cdf7cfc507500b014525c3d4dff148f44e77ebd8368c0a686c2259`;
- `LO1` = full SHA of `build-equity/train-2020-2023-lo1/manifest.json` (`2ff9d771...`); `F1` = fields v9 lo1
  (`888e6616...`);
- old u pass `build-equity/mega-v71-train-u-1`; new u pass `build-equity/w0-2-v71-u-lo1-1`.

### (i) Exact negation for the three, equality for the other 45

`compare_window_overlap.py` cannot show this:
- the columns are fixed (`:64-65`);
- the cell rule is bitwise (`:225-248`);
- `max_rel_diff` is a maximum (`:240-245`): 2.0 is consistent with negation but does not prove it on every cell;
- there is no option for absolute values, negation or column choice (`:577-591`).

Smallest read-only script. Save it outside the repo, e.g. as `$S/pm516_i.py`. It prints only counts and booleans:

```python
import csv, datetime as dt, math, struct, sys
ns = lambda d: (d - dt.date(1970, 1, 1)).days * 86_400_000_000_000
SEAL, BEFORE = ns(dt.date(2024, 1, 1)), ns(dt.date(2022, 9, 30))
HEADER = ["id", "horizon", "decision_index", "session_ns", "pearson", "rank_ic", "oriented_rank_ic"]
bits = lambda x: struct.unpack("<Q", struct.pack("<d", x))[0]
def load(path):
    out = {}
    with open(path, newline="", encoding="utf-8") as f:
        r = csv.reader(f); assert next(r) == HEADER
        for row in r:
            s = int(row[3]); assert s < SEAL, "on/after seal: refuse"
            if s < BEFORE and row[0] != "__combined__":
                out.setdefault(row[0], {})[(s, int(row[1]))] = tuple(float(v) if v else math.nan for v in row[4:])
    return out
same = lambda a, b: (math.isnan(a) and math.isnan(b)) or bits(a) == bits(b)
neg = lambda a, b: not math.isnan(a) and not math.isnan(b) and bits(b) == bits(a) ^ (1 << 63)
old, new, expect = load(sys.argv[1]), load(sys.argv[2]), set(sys.argv[3].split(","))
eq_ids, neg_ids, other, raw_ok, cells = set(), set(), set(), True, 0
for cid, o in old.items():
    n = new.get(cid, {}); keys = [k for k in o if k in n]; assert keys and len(keys) == len(o)
    e = g = bad = 0
    for k in keys:
        (p0, r0, x0), (p1, r1, x1) = o[k], n[k]; cells += 1
        raw_ok &= same(p0, p1) and same(r0, r1)
        if same(x0, x1): e += 1
        elif neg(x0, x1): g += 1
        else: bad += 1
    finite = sum(1 for k in keys if not math.isnan(o[k][2]))
    (eq_ids if bad == 0 and g == 0 else neg_ids if bad == 0 and g == finite else other).add(cid)
print({"ids": len(old), "cells": cells, "raw_columns_identical": raw_ok, "ids_equal": len(eq_ids),
       "ids_negated_every_finite_cell": len(neg_ids), "ids_other": len(other),
       "negated_are_expected": neg_ids == expect, "equal_are_the_rest": eq_ids == set(old) - expect})
```

Run it under the bounded runner; output goes to `stdout.log`:

```
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
  --output build-equity/pm5-16-cause-i-run --bind $S/pm516_i.py -- \
  "$PY" $S/pm516_i.py build-equity/mega-v71-train-u-1/train_daily_ic.csv \
  build-equity/w0-2-v71-u-lo1-1/train_daily_ic.csv chtax,ind_adj_rev_5,ind_mom_12_1
```

- Expected under the hypothesis: `raw_columns_identical` true, `ids_equal` 45, `ids_negated_every_finite_cell` 3,
  `ids_other` 0, both booleans true.
- Binds are capped at 16 MiB (`run_bounded_research.py:107-111`); bind the two CSVs as well only if each is under that.
- By code, `oriented = sign*rank_ic` (`:182`), and multiplying by ±1 is exact, so a single per-candidate sign product
  explains every cell. The script tests this on the data.

### (ii) Combined row bit-identical under the 3-year orientation

There is no runner option to orient a scored TRAIN role from a file (Q1). The runner's own option for the blend sign
is `--composition-weights` with `signs`. Pinning the default equal-family/equal-within values reproduces the default
blend bit for bit:
- `strategy_ic_composition.hpp` comment on pinned weights; `strategy_ic_composition.cpp:180-184`, `:229`;
- tests `StrategyIcRunner.PinnedDefaultCompositionWeightsReproduceDefaultBlendBytes` (`strategy_ic_runner_test.cpp:679`)
  and `PinnedSignOppositeToIcOrientationFlipsOnlyThatBlendContribution` (`:1886`), which ends "A pinned sign equal to
  the IC orientation reproduces the unsigned blend bytes".

So (ii) needs one JSON file and one bounded IC run. No code change.

1. **Weights file** (prints only its SHA and counts; never prints the signs). Save as `$S/pm516_w.py`:

```python
import hashlib, json, sys
from collections import Counter
from pathlib import Path
lib_path, lib_sha, orient3, role4, out = sys.argv[1:6]
lb = Path(lib_path).read_bytes(); assert hashlib.sha256(lb).hexdigest() == lib_sha
lib = json.loads(lb)["candidates"]; o = json.loads(Path(orient3).read_bytes())
assert o["schema"] == "atx.dsl-ic-orientations/v1" and o["library_sha256"] == lib_sha
rows = o["candidates"]; assert [r["id"] for r in rows] == [c["id"] for c in lib]
fam = Counter(c["family"] for c in lib); F = len(fam); w, s = {}, {}
for c, r in zip(lib, rows):
    w[c["id"]] = 1.0 / (float(F) * float(fam[c["family"]])) if r["sign"] != 0 else 0.0
    if r["sign"] != 0: s[c["id"]] = r["sign"]
doc = {"schema": "atx.dsl-composition-weights/v1", "library_sha256": lib_sha, "train_manifest_sha256": role4,
       "weights": w, "signs": s}
p = Path(out); p.parent.mkdir(parents=True, exist_ok=False); data = (json.dumps(doc, indent=1) + "\n").encode()
p.write_bytes(data)
print({"sha256": hashlib.sha256(data).hexdigest(), "candidates": len(lib), "families": F, "unoriented": len(lib) - len(s)})
```

   Run: `"$PY" $S/pm516_w.py $LIB $LIBS build-equity/mega-v71-train-u-1/orientations.json $LO1
   build-equity/pm5-16-pin3y/composition_weights.json`. A sign-0 candidate gets weight 0, which skips it exactly as
   the default path does. `1/(F*n)` is the runner's own expression, and repr round-trips through nlohmann.
2. **IC run** with the u pass's flags, without `--save-combined`, under the W0-c caps:

```
"$PY" scripts/run_bounded_research.py --seconds 300 --max-rss-mib 2560 --min-free-mib 512 \
  --output build-equity/pm5-16-pin3y-u-lo1-run1 --bind $IC --bind $LIB \
  --bind build-equity/train-2020-2023-lo1/manifest.json --bind build-equity/train-2020-2023-lo1-fields-v9/manifest.json \
  --bind build-equity/pm5-16-pin3y/composition_weights.json -- \
  $IC --library $LIB --library-sha256 $LIBS --train build-equity/train-2020-2023-lo1/manifest.json --train-sha256 $LO1 \
  --train-fields build-equity/train-2020-2023-lo1-fields-v9 --train-fields-sha256 $F1 \
  --output build-equity/pm5-16-pin3y-u-lo1-1 --max-memory-mib 2560 --min-names 1000 --workers 4 \
  --candidate-cache build-equity/mega-candidate-cache-v8-lo1 \
  --composition-weights build-equity/pm5-16-pin3y/composition_weights.json --composition-weights-sha256 <sha from 1>
```

3. **Compare:**

```
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
  --output build-equity/pm5-16-overlap-daily-ic-pin3y-run --bind atx-impl/tools/compare_window_overlap.py -- \
  "$PY" atx-impl/tools/compare_window_overlap.py --kind daily_ic --per-key --before 2022-09-30 \
  --old build-equity/mega-v71-train-u-1 --new build-equity/pm5-16-pin3y-u-lo1-1 \
  --out build-equity/pm5-16-overlap-daily-ic-pin3y.json
```

   Read only `rows[key=="__combined__"].bit_identical` (expected true) and `differing_keys`.
   - `differing_keys` is expected to be exactly the three member keys: pinned signs do not change the member diagnostics
     (`:432-433`). The class therefore stays "stop" by design.
   - Optional: compare `--old build-equity/w0-2-v71-u-lo1-1 --new build-equity/pm5-16-pin3y-u-lo1-1` without
     `--before`. It is expected to give `differing_keys == ["__combined__"]`, which shows the pin touched only the blend.
   - Support for the expected result: the v8 exe already reproduced `mega-v71-train-u-1`'s `train_daily_ic.csv` and
     `train_combined.f64` byte for byte on the 3-year role (`integration-log.md:537`).

## Q6. Options for PM5-18 (no recommendation from data)

**(A) The code as is.**
- Correction to the brief's label: the code does not take the book's sign from the run's own window. The book uses the
  prior signs. The own-window sign goes only into diagnostics, the unread u-pass blend, the provenance hashes and the
  P1 `sign_agrees` gate (B0a has no listed members).
- Code path: existing; no executable change.
- Comparability with the 37 cells: the book uses the same prior-signed mechanism they used. Diagnostics and gates use the
  same rule on the new window.
- The 4-year provenance hashes differ from the 3-year ones, as any re-run's do:
  - `orientation_candidates_sha256` in the w-pass manifest, NAV `source_bindings` and the ledger;
  - the weights-file `runner_sign` and `sign_conflicts` fields.

**(B) Pin to the 3-year run's orientation artifact.**
- Book: no change, because the w pass already pins prior signs.
- Runner diagnostics, u blend and gate: no existing option. `--orientations` is validation-only and refuses a different
  train SHA or overlapping roles (`strategy_ic_admission.cpp:136-140`, `strategy_ic_runner.cpp:568`, `:600-603`,
  `:615`).
- New code would be needed:
  - a TRAIN-scored frozen-sign flag in the IC runner, with recipe semantics: exe rebuild and identity canary;
  - fitter changes: `load_orientations` requires the file's `train_manifest_sha256 == --train-sha256`
    (`fit_composition_weights.py:493`), and `CacheLayout` binds the summary to that file (`:657-659`).
- Undefined for v8.0, v8.1 and v8.2 members: there is no 3-year artifact for those libraries.
- Comparability: the book is unchanged; diagnostics would mix two windows.

**(C) Literature prior signs from the registry.**
- Book: this is already the case (Q3).
- Making the runner's own orientation prior-signed needs new runner code and an exe rebuild. The runner hard-requires
  `sign_policy == "train-rank-ic21"` (`strategy_ic_library.cpp:144`).
- It would also make `sign_agrees` identically true, which empties the registered P1 promotion test (v4-prereg P1)
  that the r2, r7 and R-12 gates rely on.
- Comparability: the book is unchanged; diagnostics and gates diverge from how the 37 cells were run.

**What the written registration supports**
- For the traded book: prior signs, i.e. the code as is. A and C are the same for the book. Sources: prereg rules 10 and
  11, plan sections 7 W0-4 and 9 R-2, plan section 13, and the v7.1 recipe orientation block.
- For the diagnostic orientation: no text supports B. W0-a covers old values, and PM5-16 itself says a window-dependent
  sign "is not an old value". The only registered use of the runner sign is v4-prereg P1, evaluated on the run's own
  TRAIN window, which A keeps.

## Could not establish by reading

- That every one of the 37 cells used prior-signed weights. This is verified for the spec-driven cells (fit flags) and,
  from the sprint logs, for v5 and v6. It is not verified for C1-C3, spo-v1 and spo-v2, whose argv is in
  `build-equity/trials.jsonl` and receipts, which I did not open.
- Whether any flipped name sits in a v6.1, v7.0 or v7.1 `sign_agrees` gate list. I did not cross them, because doing so
  could disclose a sign.
- The outcome of (i) and (ii): nothing was run.
- Whether the two daily-IC CSVs fit the bounded runner's 16 MiB bind cap.
