# Task T12 report: role factor-break repair and factor-jump QA scan (t12-role, 2026-09-27)

**Status: DONE_WITH_CONCERNS.**

- Commit `f822dd42` on `feat/mega-alpha-role-repair-20260927` (pool-8, base `c099cade`).
- Files: `atx-impl/tools/repair_role_factor_breaks.py` (tool) and `atx-impl/tools/test_repair_role_factor_breaks.py`
  (10 synthetic unittest fixtures, all passing).
- Python only. Nothing to build, no CMake registration needed.
- I read no role payload (.f64/.u8) and no vendor or lake data. I ran synthetic fixtures only.

**Concern.** Repairing `close.f64` does not repair `shares_out`. The field producer restates vendor shares with the raw
vendor `cumulReturnFactor` ratio, so for about 62 TRAIN sessions after 2021-01-04 it carries the same break. See §6.1.
This needs a routing decision before the swap-fin tier flags or any size alpha is measured on TRAIN.

## 1. Root cause

### 1.1 Provenance, from manifests and run records

| Stage | Artifact | Binding |
|---|---|---|
| Vendor file | `C:/Users/natha/Downloads/TickerHistory3.parquet`: SpiderRock TickerHistory3 snapshot of 2026-09-20, 32.3M rows, 2012-03-26..2026-09-18 | sha `0ed96b26…` (role `source_sha256`) |
| Projection | `build-equity/recent-projection-v1` from `prepare_recent_research.py project` (run `recent-projection-v1-run`, 2018-06-01..2025-01-01) | manifest sha `25a96be8…` (role `projection_manifest_sha256`) |
| TRAIN role | `prepare_recent_research.py role --start 2018-06-01 --score-start 2020-01-01 --end 2023-01-01` | manifest sha `3f53ee9a…` |
| Validation role | same command with `--start 2021-06-01 --score-start 2023-01-01 --end 2025-01-01` | manifest sha `0c757c41…` |

The projection query (`prepare_recent_research.py:171`) computes `raw*factor AS close` from the same row's raw close
and vendor `cumulReturnFactor`. The role writer copies that value into `close.f64` unchanged. Nothing chains the
factor, and nothing checks it for continuity.

### 1.2 Mechanism

The vendor factor is not chained across 2021-01-04:

- Bars dated before that session carry a factor that omits corporate actions dated on or after it.
- Bars dated on or after it carry the full factor, anchored at the 2026-09-18 as-of.

At the boundary the factor steps by k = (product of the name's later actions as seen by the new block) divided by the
old block's factor. The raw close does not move.

- **Dividend payers:** k < 1 by about their cumulative later dividends. These are root's ~1073 "small" 1-10% jumps.
- **Names with a later forward split:** k is the inverse split ratio, x1/4..x1/10.
- **Names with a later consolidation (reverse split):** k is the consolidation ratio, x10..x80 and larger. The earlier
  nav-recon example `1816199` has adjusted ratio about 199 at a raw ratio of 1.013.

This is not a per-year re-basing. Root's scan and nav_recon section E find the 2019, 2020 and 2022 year starts clean
and only 2021-01-04 affected. It is a one-time block boundary in this vendor snapshot.

### 1.3 Independent corroboration: this artifact is already known in atx-db

`C:/atx/atx-db/src/atx_db/_vendor_artifact.py` (VA1, `vendor_artifact_repair_v2`, ruling C-35) documents the same
snapshot:

> "a factor decrease that is not a split is a vendor artifact … one on its artifact session 2021-01-04 for ~3,430 lines
> (median 1.8%, up to 96%) … 9 of its exact-ratio steps are the inverse of a later forward split of the line applied
> from 2021-01-04 instead of from the line's start (CVNA x0.2, CRWD x0.25, MNST x0.5, …)."

The warehouse bar builders apply VA1. The research role builder reads the vendor Parquet directly and bypasses it.

VA1 v2 repairs factor **decreases only**, so the large **increases** (later consolidations: x10..x80) are unrepaired
even in the warehouse. That gap is worth reporting to the atx-db owner.

### 1.4 What is verified and what is inferred

- **Verified by others:** the break exists, its size, and that it is on 2021-01-04 (root's measurement, nav_recon,
  and VA1's snapshot analysis).
- **Inferred:** that the pre-break block is anchored at its own end. The scan tests this:
  - `f1prev` on 2021-01-04 is the share of steps whose prior-observation factor is exactly 1.0. Near 1.0 means the old
    block was re-based to 1 on 2020-12-31.
  - `f1` is the share of present names with factor exactly 1 on the session.

  The repair does not depend on which anchoring the vendor used.

## 2. Rule `factor-break-v1` (declared before any real-data use; constants in the tool, recorded in the manifest)

All moves are in logs, over consecutive present observations p < t of one name at most 10 calendar days apart.

- **Quantities.** f = close/raw. s = ln(f_t/f_p) is the factor step, r = ln(raw_t/raw_p) is the raw move, and a = r + s
  is the adjusted move.
- **Detector: which sessions.**
  - A **jump cell** is a step ending at t with |s| > 0.01 and |a| > |r| + 0.01. This is the brief's cell condition.
  - Session t is a **MASS** session when it has ≥ 50 jump cells.
- **Classification: which steps.** On a mass session b, every step with p < b ≤ t and |s| > 1e-9 is exactly one of:

  | Class | Condition | Action |
  |---|---|---|
  | `kept_gap` | gap > 10 calendar days | kept |
  | `kept_split_follow` | split-like, and the raw close followed on the same step: s<0 and r ≥ max(\|s\|/2, ln 1.25), or s>0 and −r ≥ max(s/2, ln 1.25) | kept |
  | `kept_distribution` | 0 < s < ln 1.25 | kept |
  | `repaired` | everything else: every factor decrease, and every split-like increase whose raw close did not follow | repaired |

- **Repair.** For a repaired step, with k = f_t/f_p, set close[0:t, j] *= k.
  - The adjusted return across the step then equals the raw return. The measured maximum error is 1.8e-15 in logs.
  - Sessions ≥ t keep the vendor's current anchoring, so the repaired TRAIN closes after the break are bit-identical
    to the source. Post-2021 levels also stay consistent with the validation role, which is cut from the same
    projection.
  - A step that crosses two mass sessions is repaired once. Classification depends only on (p, t).

### 2.1 Thresholds, from first principles (none chosen from strategy outcomes)

- **0.01 step.** Well above an ordinary quarterly cash dividend: a single 1% payment means a yield of at least 4% a
  year. Far below any split (≥ ln 1.2).
- **0.01 excess, and why the detector needs a mass count.**
  - A genuine action moves the raw close against the factor, so a ≈ the name's own market move m.
  - A legitimate dividend of yield y is a jump cell only if the name also rises by more than about (y+0.01)/2.
  - So legitimate jump cells come from high-yield ex-dates on an up move. That is a few per session, a few tens at a
    quarter-end peak.
- **Mass count 50.** A re-anchoring hits every name with any later corporate action: hundreds to thousands (1178 here).
  50 is at least 2x above a pessimistic legitimate peak, more than 20x below the observed break, and about 1% of the
  role's present names. The scan prints the largest non-mass count on the role as the margin to this threshold.
- **Noise 1e-9.** This is atx-db's `FACTOR_NOISE`. With close = f64(raw) × F, a constant F gives |s| ≈ 1e-16.
- **ln 1.25 and the |s|/2 raw-follow veto.**
  - 1.25 is the smallest common split (5:4) and VA1's `ARTIFACT_SESSION_MIN_RISE`.
  - The veto is VA1 v2's artifact-session veto, mirrored for increases. A genuine split moves the raw close by the full
    inverse ratio plus the day's move. The artifact moves the raw close by nothing.
- **Small increases kept.** The artifact lowers the factor for every later distribution or forward split. It raises
  the factor only for a later consolidation, which is ≥ 1.25x. So a sub-25% increase on the session is a same-day
  distribution. VA1 also never repairs increases.
- **10-day gap.** This is VA1's `COVERAGE_MAX_GAP_DAYS`. A step across a longer gap mixes unknown genuine actions into
  the step, so it is not repaired, only listed.

### 2.2 How this differs from the brief's example rule (a declared decision)

I kept the brief's cell condition as the **detector** but did not use it to choose **which steps to repair**, for two
reasons:

1. **It misses breaks that oppose the raw move.** A −3% artifact on a +3% raw day gives a ≈ 0, so |a| < |r| and the
   cell is not flagged. About half of the small artifacts on a mixed day would be missed.
2. **The 0.01 floor leaves every sub-1% dividend artifact.** Across all payers that is a residual step on one day that
   is correlated with dividend yield (VA1 measured the bias as tracking dividends). It would contaminate any price
   signal whose window spans the date.

The sign and raw-follow classification above fixes both, and it matches the reviewed VA1 rule on the session.

**Expected on TRAIN:** "repaired" should be at least root's 105 + 1073 = 1178. `kept_split_follow` and
`kept_distribution` should be about 0 to tens.

### 2.3 Declared residuals

- **Genuine same-day cash dividend combined with an artifact decrease.** The step is repaired as a whole, so that day's
  dividend (about 1% or less) is booked as a price move for that name. This is the same as VA1.
- **Genuine split combined with an artifact on the same step.** Kept as a whole, so the artifact remains. Expected
  about 0 cases, and each is listed as `kept_split_follow`.
- **Steps across gaps longer than 10 days.** Kept and listed.
- **Stock distributions under 25%.** Indistinguishable from market moves in close/raw alone.

The exact alternative is to rebuild the role from raw plus the vendor `returnFactor` with a re-chained factor, which is
nav-recon's recommendation 1. That is a builder change that reads the lake, and it is out of this lane.

## 3. Tool

`atx-impl/tools/repair_role_factor_breaks.py` (Python 3.12 + numpy).

```
--role DIR --role-sha256 HEX --scan-only [--csv NEW.csv] [--top 10]
--role DIR --role-sha256 HEX --out NEW_DIR [--expect-sessions 2021-01-04,... | none] [--allow-noop]
```

### 3.1 Input binding

- The manifest must match `--role-sha256`.
- Every payload read must match the manifest's bytes and SHA-256. Scan-only reads sessions, ids, close, raw and
  present. Repair reads all seven payloads.
- It enforces the loader contract: present means finite and positive, absent means NaN.

### 3.2 Scan output

The table prints these sessions: every mass session, the top-N sessions by jump count, every year start, and every
session with a big jump.

| Column | Meaning |
|---|---|
| `big` / `small` | nav_recon section E's columns, adjacent sessions only. A fixture checks them against a literal port of nav_recon lines 171-186. |
| `jump` | The detector count. |
| `dec` / `inc` | Factor decreases and increases beyond 1e-9. |
| `longgap` | Factor steps across gaps longer than 10 days. |
| `f1prev` / `f1` | The anchoring test from §1.4. |

After the table it prints:

- Summary lines, including the largest non-mass count as the margin to 50, and the big-cell totals as in nav_recon.
- For each mass session: the class counts, |ln k| quantiles, the largest repaired steps, and the kept steps.
- A final line: `verdict: CLEAN` or `verdict: MASS n session(s): …`.

`--csv` writes all per-session counts to a new file.

### 3.3 Repair output: a new directory, exclusive

- **Directory creation.** `--out` must not exist and its parent must exist. Every file is opened with `xb`. The
  manifest is published last, using the pending-file-plus-hard-link pattern the builder uses. A directory without
  `manifest.json` is never a role.
- **Payloads.** `close.f64` is repaired. The other six payloads are written from the verified source bytes, then
  re-hashed from disk and required to equal the source manifest entries.
- **Manifest.** It has the same keys and values as the source except `files.close.f64.sha256`, plus a new `repair`
  block. `close_basis` is kept verbatim because `strategy_data.cpp:85` pins the string.
- **The `repair` block records:**
  - rule id, rule statement and parameters
  - the source role's path, manifest sha and close sha
  - tool identity: path, sha256, and git blob, which equals `git rev-parse HEAD:<path>` (checked: `4f5502a2…`)
  - the defect statement and a `close_basis_note`
  - per-mass-session counts: jump, big, small, dec/inc, each class, gap-spanning steps, and the post-repair jump count
  - `detector_jump_cells_by_session` for every session
  - repaired-name and changed-cell counts, and the maximum return error
  - the sidecar `repair_cells.csv`: bytes, sha and row count. It holds every crossing step, repaired and kept, with
    session, previous session, gap, column, security id, s, k, r, a and action. It is not in `files`, so no loader
    touches it.
- **Publication gates.** The tool refuses to publish unless:
  - every repaired step's return error is ≤ 1e-12;
  - no cell outside a repaired name's pre-break rows changed;
  - the loader contract holds;
  - the post-repair scan finds no mass session;
  - the manifest is within the loader's 1 MiB limit (about 26 KB for a TRAIN-size role).
- **`--expect-sessions`.** The tool refuses if the detected mass set differs from the expected one.
- **No mass session.** It exits 3 and writes nothing. With `--allow-noop` it publishes an unchanged copy that carries a
  `repair.noop: true` block.
- **Exit codes:** 0 done, 2 refused, 3 clean without `--allow-noop`.

### 3.4 Budget

Measured on a full-size synthetic role (1155 × 5627, 1200 artifact names, in my scratchpad):

| Mode | Wall time | Peak RSS |
|---|---|---|
| Scan | 0.86 s | 181 MiB |
| Repair | 1.86 s | 311 MiB |

A bounded-runner limit of 60 s and 1024 MiB is ample.

## 4. Tests (synthetic only)

Command:

```
"C:/Program Files/Python312/python.exe" -m unittest discover -s atx-impl/tools -p test_repair_role_factor_breaks.py -v
```

Result: 10/10 OK in 1.2 s.

The fixture is 80 sessions from 2020-11-02 across 2021-01-04, 100 names, f32-widened raw, and a pre-break block
anchored at 1.

| Fixture | What it checks |
|---|---|
| mass break repaired exactly | 65 dividend-like, 5 opposing-sign, 8 big (x10..x80, x0.2..x0.5) and 1 gap-spanning name. The repaired close equals raw × k with rtol 2e-15. Rows ≥ t are bit-identical. The step return equals the raw return within 1e-12. Every other column is bit-identical. Manifest counts match. |
| genuine split and dividend untouched | A 2:1 split, a 1:10 consolidation and a 2% dividend on the mass session are kept. A split and a dividend on normal days are untouched and unlisted. The >10-day gap is `kept_gap`. |
| other files and manifest schema preserved | Six payloads are byte-identical. Manifest keys are the source set plus `repair`. Sidecar sha and tool blob match. The output re-loads under `load_role` with its own sha. |
| output bytes deterministic | Two runs produce identical bytes for every file, including the manifest and sidecar. |
| exclusive output | An existing `--out` is refused and its contents are untouched. A missing parent is refused. |
| sha refusal | A wrong manifest sha is refused (both modes). A tampered close byte is refused. Nothing is written. |
| scan-only | Writes nothing. big/small equal a literal nav_recon section E port for every session. `f1prev` > 0.9 on the break. `--csv` is exclusive. |
| `--expect-sessions` mismatch | Refused, nothing written. |
| no mass session | Exit 3 and no output. `--allow-noop` gives close bytes identical to the source and `noop: true`. |
| two mass sessions | The multipliers compound: close' = raw × k1 × k2. A step across both sessions is listed once. |

## 5. Decisions recorded

1. **Repair set differs from the brief's example** (§2.2). The brief's cell condition is still the mass detector.
2. **The pre-break history is rescaled,** as the brief says. VA1 instead rescales the post-break data by 1/k. Returns
   are identical either way; only the side that keeps vendor levels differs. Keeping post-break levels matches the
   validation role and the current vendor basis.
3. **`close_basis` kept verbatim** for the C++ loader pin, with the change disclosed in `repair`. Changing the string
   would need a C++ change and a rebuild across every consumer.
4. **`--role-sha256` is required even for the scan,** so every QA line is bound to exact bytes.
5. **A clean role is not rewritten by default** (exit 3), so it does not get a new sha and force a cache cold pass.
   `--allow-noop` exists if root prefers uniform provenance.

## 6. Downstream consequences

### 6.1 Concern: `shares_out` carries the same break

`prepare_research_fields.py` builds `shares_out` as vendor shares from the 90-day lag row × cumulReturnFactor(session) /
cumulReturnFactor(lag row), taken straight from the vendor file (lines 636-645 and 709). Its own caveat at line 149
says it uses "the raw vendor cumulReturnFactor ratio (not the VA1-repaired factor; vendor artifact breaks are not
detected)".

**Affected window.** Every TRAIN session from 2021-01-04 until the lag row passes the break, about 2021-04-04 (≈ 62
sessions). The restated share count is off by the same k.

| Names | Error |
|---|---|
| Dividend payers | about 1-10% (harmless) |
| Later consolidations | x10..x80 |
| Later forward splits | x1/4..x1/10 |

**Consumers hit:**

- swap-fin-v1 tier flags: mcap = shares_out × raw_close < $1bn, and SI/shares_out > 10%
- any v3 size or SI-ratio candidate

The T10 ruling that treats `shares_out` outside [1e5, 5e10] as missing does not catch a x10 error inside that range.

The validation role is essentially unaffected: its sessions start 2021-06-01, so the lag rows are after the break
except for rows more than 5 months stale.

**Suggested fix** (the T6 file, not this lane): in `tickerhistory_fields`, when the lag row date is before a repaired
step's session and the field session is on or after it, divide by that step's k. The ids, sessions and k come from the
repaired role's `repair_cells.csv`, whose sha is in its manifest. Alternatively, declare `shares_out` NaN on those
cells. Root should route this before any TRAIN swap-fin or size measurement.

### 6.2 Everything bound to the TRAIN role sha must be regenerated

The repaired role has a new manifest sha. Every consumer binds the role sha and refuses a mismatch, so stale artifacts
cannot be mixed in silently.

| Consumer | How it binds | Consequence |
|---|---|---|
| Field producer | `--role-sha256` | TRAIN fields-v1 are stale. `mkt_ret` changes, since it is computed from role close. |
| IC runner | `--train-sha256`; candidate cache keyed `<cache>/<role sha>/` | The cache goes cold automatically in a new subdirectory, about 400 s in chunks per progress.md. The old `3f53ee9a…` entries become dead disk. |
| Orientations, combined blends | `role_manifest_sha256` | v6 blend, `mega-v1-train-cache-b` and every IC artifact are superseded. |
| `fit_composition_weights.py` | `--train-sha256` and cache `role_manifest_sha256` | `mega-weights-v1` is superseded. |
| NAV / target replay | `--role-sha256`; `strategy_target_replay.cpp:366` also requires combined.role_manifest_sha256 == role sha | The old blend cannot be NAV'd on the repaired role. |

`nav_recon.py` checks axes but not the blend's role sha. Pass a blend built on the repaired role.

Price features of affected names are clean across the boundary only after this regeneration. They used lookbacks up
to 252 sessions spanning 2021-01-04.

### 6.3 Root command order

Run from `C:/atx-wt/pool-2` after cherry-picking `f822dd42`. `PY` is `"C:\Program Files\Python312\python.exe" -B`.
Bounded limits of 60 s and 1024 MiB are ample for steps 1-4.

```
# 0 (optional) synthetic fixtures
PY -m unittest discover -s atx-impl/tools -p test_repair_role_factor_breaks.py -v

# 1 scan TRAIN
PY atx-impl/tools/repair_role_factor_breaks.py --scan-only --role build-equity/recent-fast-train-2020-2022-v1 --role-sha256 3f53ee9aa1b674d3f5022cbb22d40e5c043e7add8c9422cd2456299ce3662493

# 2 scan validation (data-quality scan only: counts of factor steps, no returns/signals)
PY atx-impl/tools/repair_role_factor_breaks.py --scan-only --role build-equity/recent-fast-validation-2023-2024-v1 --role-sha256 0c757c41a363659664c96359a2d2288e10f792e2b91ab38ca8bf5e064dfbbda7

# 3 repair TRAIN (if step 1 lists exactly 2021-01-04; otherwise pass the listed set after review)
PY atx-impl/tools/repair_role_factor_breaks.py --role build-equity/recent-fast-train-2020-2022-v1 --role-sha256 3f53ee9aa1b674d3f5022cbb22d40e5c043e7add8c9422cd2456299ce3662493 --out build-equity/recent-fast-train-2020-2022-v2 --expect-sessions 2021-01-04
#   -> last lines: "output manifest sha256 <TRAIN_V2_SHA>"

# 4 validation
#   step 2 verdict CLEAN -> keep recent-fast-validation-2023-2024-v1 (sha 0c757c41..., recommended), or for uniform provenance:
PY atx-impl/tools/repair_role_factor_breaks.py --role build-equity/recent-fast-validation-2023-2024-v1 --role-sha256 0c757c41a363659664c96359a2d2288e10f792e2b91ab38ca8bf5e064dfbbda7 --out build-equity/recent-fast-validation-2023-2024-v2 --expect-sessions none --allow-noop
#   step 2 verdict MASS -> same command with --expect-sessions <the listed dates> and without --allow-noop

# 5 fields-v2 TRAIN on the repaired role (after routing the shares_out fix of 6.1, or with that caveat recorded)
PY atx-engine/tools/prepare_research_fields.py --role build-equity/recent-fast-train-2020-2022-v2 --role-sha256 <TRAIN_V2_SHA> --output build-equity/recent-fast-train-2020-2022-v2-fields-v2 --fields si_shares,si_dtc,iv_atm_21d,iv_atm_63d,iv_atm_126d,earn_recent,shares_out,mkt_ret --finra C:/atx/data/finra_short_interest --tickerhistory C:/Users/natha/Downloads/TickerHistory3.parquet --lake C:/atx/atx-db/data/research/lake/price-wave-0ed96b2696f1-5b596288cf23 --max-rss-mib 700 --max-seconds 175

# 6 fields-v2 validation: same with the validation role chosen in step 4 and its sha

# 7 IC runner TRAIN (library v3; add the T7 extra-fields flags pointing at the step-5 fields)
build-equity/bin/atx-equity-strategy-ic.exe --library <v3 lib> --library-sha256 <..> --train build-equity/recent-fast-train-2020-2022-v2/manifest.json --train-sha256 <TRAIN_V2_SHA> <T7 field flags> --output build-equity/<ic-v3-train-on-v2> --max-memory-mib 1536 --min-names 2000 --workers 4 --save-combined --candidate-cache build-equity/mega-candidate-cache

# 8 weights / admission screen (T9/T11 fitter)
PY atx-impl/tools/fit_composition_weights.py --library <v3 lib> --library-sha256 <..> --train build-equity/recent-fast-train-2020-2022-v2/manifest.json --train-sha256 <TRAIN_V2_SHA> --orientations build-equity/<ic-v3-train-on-v2>/orientations.json --orientations-sha256 <..> --candidate-cache build-equity/mega-candidate-cache --output build-equity/<weights-on-v2>/composition_weights.json

# 9 NAV TRAIN
build-equity/bin/atx-equity-strategy-targets.exe nav --combined <step 7/8 combined>.json --combined-sha256 <..> --role build-equity/recent-fast-train-2020-2022-v2/manifest.json --role-sha256 <TRAIN_V2_SHA> --output build-equity/<nav-train-on-v2> --rule baseline-v1 --cadence 1 --trade-fraction 1

# 10 sanity: section E on the repaired role (2021-01-04 big/small should fall to ~0)
PY .superpowers/sdd/mega-alpha-20260926/studies/nav_recon.py --role build-equity/recent-fast-train-2020-2022-v2 --blend <step-7 dir> --nav <step-9 dir>
```

**What to read in steps 1-2:**

- **Verdict line.**
- **`repaired` versus 1178.** Expect repaired ≥ 1178.
- **`kept_*` counts.** Each kept step is listed by id. Check them against the vendor `returnFactor` if more than a few.
- **`f1prev` on 2021-01-04.** Near 1.0 confirms the old block was anchored at its end.
- **Max non-mass jump count.** This is the margin to 50.
- **For validation:** any MASS line, especially at 2023-01-03 or 2024-01-02.

## 7. Housekeeping

- One commit, `f822dd42`, in pool-8. No builds, no subagents, no real data, and no writes to C:/atx or the lake.
- The benchmark files are in my scratchpad and are not committed.
- Suggested ledger or owner note: the TickerHistory3 2026-09-20 factor re-anchoring on 2021-01-04 has increases
  (later consolidations) that VA1 v2 does not repair. This is atx-db scope.

## Fix round 1 (review `task-T12-review.md`: 0 Critical, 1 Important, 5 Minor)

**Status: DONE.**

- Commit `5093dd5c` on `feat/mega-alpha-role-repair-fix1-20260927`, in pool-9, based on root `e885687b`.
- Files: `atx-impl/tools/repair_role_factor_breaks.py` and `atx-impl/tools/test_repair_role_factor_breaks.py`.
- Python only: no build and no CMake changes.
- Tests: 16/16 synthetic fixtures pass in 2.4 s. The run used no real data.

### Important 1: rule `factor-break-v2` (v1 kept unchanged)

`--rule {v1,v2}` selects the rule. The default stays **v1**.

**v1 reproduces the committed tool exactly.** On a two-session fixture, `test_v1_reproduces_the_committed_tool` runs
`git show 99421a5f:…/repair_role_factor_breaks.py` against the current code. Every payload and `repair_cells.csv` are
byte-identical. The manifest is identical except `repair.tool`, which records the code itself. v1's statement,
parameters, manifest keys, detector and gate are untouched. So TRAIN role v2 (`210fff96…`) stays reproducible and keeps
its honest v1 provenance.

**v2 detector.** An *unexplained step* is a step ending at t (gap ≤ 10 days) that is either:

- a factor decrease (s < −1e-9) whose raw close did not follow, or
- a split-like increase (s ≥ ln 1.25) whose raw close did not follow.

This is exactly the classifier's `repaired` predicate, implemented as the shared functions `raw_followed` and
`unexplained`.

Session t is **MASS** when unexplained(t) ≥ 20 **and** unexplained(t) ≥ ⌈1% × steps(t)⌉. Classification and repair
are unchanged, so on the main fixture v1 and v2 produce byte-identical `close.f64` and cell lists.

**v2 post-repair gate.**

- No unexplained step may remain on a repaired session.
- No v2 mass session may remain anywhere.
- This replaces the jump-count gate for v2 only. It holds by construction: a repaired step's post-repair |s| is about
  1e-15.

**Threshold derivation.** It uses the structural baseline only. No count from TRAIN or validation was used, and the
44 and 47 figures were not inputs.

- **Why the legitimate baseline is structurally about 0.** With f = adjusted/raw, every distribution raises f on its
  ex-date: cash or stock dividend, forward split, spin-off, rights. In each case the raw close falls and the adjusted
  close is continuous. Only a consolidation lowers f, and then the raw close rises by the same ratio on the same bar,
  which the follow test explains. A legitimate unexplained step therefore needs a vendor error, such as a split factor
  dated one session off its price move (VA1's ARCM x5 example) or a reversed distribution. These are independent
  single-name events, far below one per session.
- **Floor of 20.** Assume a generous rate of one isolated error per session. A Poisson count reaches 20 with
  probability below 1e-18 per session, and below 1e-14 over a 4096-session role. So isolated errors never trip the
  floor, even in small universes where 1% of steps is only a few names.
- **Share of 1%.** A re-anchoring moves f for every name with any corporate action between the two anchors. Dividend
  payers alone are a large share of listed names, so even anchors one quarter apart move tens of percent of the cross
  section. The 1% share keeps the rule independent of universe size and still flags a re-anchoring about 20x smaller
  than that.
- **A dividend cluster can never trip v2.** A cluster of genuine ex-dates, of any size, contains no unexplained step.

**Scan output now reports both rules per session.**

- New columns: `unexpl` (the v2 count) and `thr` (the v2 threshold), next to v1's `jump`.
- Flags: `MASS-v1`, `MASS-v2`, `year-start`.
- Rows shown: both rules' mass sessions, the top-N sessions by jump and by unexplained count, year starts, and sessions
  with any big step.
- Summary lines:
  - v1: jump-cell sessions, median, and the largest count outside a v1 mass session.
  - v2: sessions with unexplained steps, the total and maximum outside a v2 mass session, and the threshold there.
- Classification is printed for the union of both rules' mass sessions.
- One verdict line per rule: `verdict factor-break-v1: …` and `verdict factor-break-v2: …`. The old single `verdict:`
  line is replaced.
- `--csv` gains `unexplained`, `v2_threshold`, `mass_v1` and `mass_v2`. The old `mass` column is now `mass_v1`.

**v2 manifest block.** v1 blocks keep exactly their original keys. v2 blocks add:

- `rule: factor-break-v2`, plus its own rule statement and `parameters`
- per mass session: `unexplained_steps`, `unexplained_threshold` and `post_repair_unexplained_steps`
- `detector_unexplained_steps_by_session`

**Docstring and report.** The v1 margin claim is corrected. The earlier "≥ 2x above a pessimistic legitimate peak"
(report §2.1, tool docstring) was wrong: real legitimate peaks are 44 (TRAIN 2022-12-29) and 47 (validation), so v1
has about 6% headroom. v1 is now documented as kept only to reproduce roles already built with it.

**Validation disposition** is unchanged and stands by construction: the validation window starts 2021-06-01, after
the only known break. It is not re-decided from numbers seen on validation. Root's v2 rescan is a data-quality check
only.

### Minors

| # | Resolution |
|---|---|
| M1 | **Documented** in the docstring. The kept_distribution premise has a limit: a later consolidation combined with later dividends or forward splits can net to k in (1, 1.25), which would be kept, leaving the break. TRAIN is empirically clean (repaired increases k ≥ 1.95, kept k ≤ 1.030). Row **35139** (k 1.030, raw +0.58%) is named in the docstring for a vendor `returnFactor` check. |
| M2 | **Listed, not fixable** with close/raw alone. A same-day dividend combined with an artifact is repaired as a whole. The exact route is a rebuild from `returnFactor`. No fixture was added, because the tool cannot separate the two cases. |
| M3 | **Fixed** in the docstring. CELL_STEP is now described as "below any split or stock distribution of 6:5 (ln 1.2) or more". 1.25 is VA1's floor. A 6:5 forward split on a mass session is kept. A 5:6 consolidation there (raw +20%, below 25%) would be repaired; this is a declared, rare limit. |
| M4 | **Fixed.** `TypeError` and `AttributeError` are now refused with exit 2. Fixture: `test_malformed_manifest_is_refused_not_a_traceback`. |
| M5 | **Listed.** `close_basis` stays verbatim, because the C++ loader and replay pin it. This is disclosed in `repair.close_basis_note` and bound by the role SHA. |

### New fixtures (6; the 10 original fixtures still pass after updating their verdict and CSV assertions)

| Fixture | What it checks |
|---|---|
| `test_dividend_cluster_is_not_a_v2_break` | A 60-name quarter-end cluster: 2% dividends on a +3% day, no artifact. v1 scans a false MASS and its repair is **refused** at the post-repair gate; this is the previously untested failure mode. v2 scans CLEAN with 0 unexplained steps, and a v2 repair exits 3 with nothing written. |
| `test_small_unexplained_break_is_a_v2_mass` | A 30-name re-anchoring. v1 scans CLEAN (30 jump cells, under 50) and exits 3 on repair. v2 scans MASS, and `--rule v2` repairs exactly: close' = raw × k to 2e-15, other columns bit-identical. Manifest: unexplained 30, threshold 20, repaired 30, post-repair unexplained 0. |
| `test_v2_threshold_is_a_share_of_steps` | The threshold is max(20, ⌈1% × steps⌉). |
| `test_v1_and_v2_agree_on_the_main_scenario` | Payloads and cell lists are byte-identical across rules. The v1 manifest has no v2 keys. |
| `test_v1_reproduces_the_committed_tool` | Byte-identity against `99421a5f`, as described above. |
| `test_malformed_manifest_is_refused_not_a_traceback` | M4. |

Existing fixture additions: the main scenario checks the `unexplained` column. The break session has 78 (the 79
repaired steps minus the one gap-spanning step, which lands a session later), and the whole role has 79.

**Budget** on the full-size synthetic role (1155 × 5627):

| Mode | Wall time | Peak RSS |
|---|---|---|
| Scan | 1.2 s | 182 MiB |
| Repair (`--rule v2`) | 3.2 s | 312 MiB |

### Root commands

Run from pool-2 after cherry-picking `5093dd5c`. `PY` is `"C:\Program Files\Python312\python.exe" -B`.

```
PY -m unittest discover -s atx-impl/tools -p test_repair_role_factor_breaks.py -v     # 16 tests
# v2 rescan of the validation role (data-quality scan only; the scan always reports v1 and v2 side by side)
PY atx-impl/tools/repair_role_factor_breaks.py --scan-only --rule v2 --role build-equity/recent-fast-validation-2023-2024-v1 --role-sha256 0c757c41a363659664c96359a2d2288e10f792e2b91ab38ca8bf5e064dfbbda7
# optional: TRAIN source role under v2 (expect MASS-v2 on 2021-01-04 with unexpl about 2114)
PY atx-impl/tools/repair_role_factor_breaks.py --scan-only --rule v2 --role build-equity/recent-fast-train-2020-2022-v1 --role-sha256 3f53ee9aa1b674d3f5022cbb22d40e5c043e7add8c9422cd2456299ce3662493
```

**What to read in the output:**

- the `verdict factor-break-v2` line;
- `summary v2`: the unexplained steps outside mass sessions should be about 0-3 per session;
- the validation rows for 2024-03-27 and the year starts.

For any future role repair, pass `--rule v2`. `--rule v1` (the default) exists only to reproduce TRAIN role v2.
