### Task T34c: `studies/v51_train.sh` — pipeline script for library v5.1 (added task)

**Lane:** D · **Pool:** pool-8 (branch `feat/mega-alpha-v51-pipeline-20260927`, base `e620d3c1`) · **Model:** Opus 5.5 ·
**Depends on:** T34b (library v5.1 landed, `--plan-only` admitted), T31 (`v5_train.sh` landed) · **Tokens:** STUDIES · **Peak:** 0.1 GiB

Origin: ledger section "T39 step 2a" ruling — v5_train.sh pins L = v4 and has no `u` phase, so v5.1 needs its own script.
Brief reconstructed by the controller from that ruling and plan Task T39 step 2 after the original dispatch was lost at a
session pause (the committed script `b2c957ff` predates this file).

**Files:** Create `.superpowers/sdd/mega-alpha-20260926/studies/v51_train.sh` only. No Python changes. No C++.

**Requirements**
1. Start from `studies/v5_train.sh` (fit / w / nav phases; env COMP, COMBINED, THETA, DUST, RATE, LEV) and add the `u` phase
   of `studies/v4_train.sh` (unweighted TRAIN IC pass; bounded re-passes as v4_train.sh does).
2. Library L = `atx-impl/strategies/fund_industry_ic_v5.json`; assert its SHA-256 starts `9e5ea08c` before any phase runs
   (recipe `a26670b0`). Output directories use the `mega-v51-*` prefix so no v4/v5 artifact is overwritten
   (u dir, weights dirs per composition, weighted-pass dirs per composition, `mega-nav-v51-*`).
3. Phases chain through recorded SHA pins (the fit consumes the u pass it names; the w pass consumes the weights it names;
   nav consumes the combined it names), exactly as v5_train.sh chains fit -> w -> nav.
4. TRAIN only: role v2 `210fff96`, fields-v6 `32565c32`, dates 2020-2022. Refuse (non-zero exit) any argument or env value
   naming validation / VAL / 2023-2024.
5. Both compositions fit: `COMP=ew-theme-v1` and `COMP=ew-theme-aim-v1` (T31 fitter), each with its own weights dir.
6. NAV: reference cell only by default (theta .05, dust .1, rate fixed, LEV 1, `--rule aim-partial-v5 --cadence 1`), per
   composition. Any other cell must require an explicit opt-in (it is an undeclared trial otherwise).
7. Every real-data command goes through
   `"C:/Program Files/Python312/python.exe" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512
   --output build-equity/<new-dir> --bind <manifests> -- <cmd>` (same wrapper and binds as v5_train.sh / v4_train.sh).
8. `DRY=1` prints every bounded-runner command instead of running it (no file writes).

**Acceptance (root, T39):** `bash -n` clean; `DRY=1` output for u / fit (both COMP) / w (both) / nav (both) shows the right
library, pins, dirs and flags; the root then runs u -> fit x2 -> w x2 -> nav reference x2. Trials (disclosed): admission 38,
composition +2, construction +1 per composition run.
