# Task W3: per-alpha report card, daily monitor, fitter WorkStore (Python)

**Pool:** C:/atx-wt/pool-11. `git status` clean, then `git checkout -B feat/platform-v7-w3-reportcard-20260928 <BASE>`,
BASE = `git -C C:/atx-wt/pool-2 rev-parse HEAD`. Rules: never build C++, never run the pipeline or real data (dry-run and
reading existing TRAIN outputs under C:/atx-wt/pool-2/build-equity/** by absolute path is allowed; never anything named
validation/VAL/2023/2024/2025; never a per-candidate VAL statistic), never spawn subagents, never touch C:/atx, no C++
edits. Python "C:/Program Files/Python312/python.exe", stdlib + numpy (pandas/pyarrow only where the extended tool already
uses them). Trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Report task-W3-report.md (<= 40 lines) in
the pool-2 sprint dir; reply < 12 lines.

**Read first:** literature-v7.md S5 R5.5-R5.6 (report card + admission rules at 100+ alphas) and S7 (monitor M1-M4,
retire/reweight rules); task-L1-report.md (cache v2 layout: candidate_cache.entries[] in summary.json, payload names
`<id>.<dsl_sha16>.*`, sidecar `field_payload_sha256`); task-L2-report.md (research_cycle spec, trials.jsonl, nav_summ
integrity flags); task-L3-report.md (holdings_days.csv, decision.json schema incl. health and transfer coefficient);
atx-impl/tools/fit_composition_weights.py (CacheLayout, admission computation), atx-impl/tools/mega_report/* (components).
Real inputs available now (TRAIN): build-equity/mega-v7l1-train-u-1 (u pass with cache v2: train_daily_ic.csv,
train_candidates.jsonl), build-equity/mega-candidate-cache-v7l1, build-equity/mega-weights-v61-ew/admission.json,
build-equity/v7-l3-holdings/holdings_days.csv, build-equity/v7-l3-decide-*/decision.json, build-equity/v7-l4-risk/*.

**Deliverables**
1. `atx-impl/tools/alpha_report_card.py`: for a u-pass dir + cache + admission file, one JSON + one HTML page per
   candidate (reuse mega_report components; no prose) with: IC and IR by year, IC by size tercile and by FF12 (from the
   cached signal payload and the role's returns as the runner defines them: read the runner's IC definition and match it;
   state where you cannot), IC(h) decay curve h = 1..63 with half-life, turnover (mean |rank change|), margin and
   fitness in the WorldQuant sense (define exactly), coverage by year, correlation to every admitted member and to the
   theme composites, max |rho| with whom, admission status and reasons. An index page ranks candidates. Deterministic
   output (sorted keys, fixed float format).
2. `atx-impl/tools/book_monitor.py` (M1-M4): from a decide output dir (or a holdings_days.csv + risk bias.csv) compute:
   bias statistic of the book vs forecast (from build-equity/v7-l4-risk/bias.csv family=book if present, else compute
   from holdings_days + risk outputs), sleeve IC and turnover CUSUM vs the TRAIN distribution (per admitted candidate,
   from train_daily_ic.csv), implementation shortfall vs model (planned vs filled cost from holdings_days), crowding
   proxy (comomentum-style: mean pairwise correlation of the book's members' residual returns, if residuals available;
   else state n/a). Output JSON with status ok/warn/alarm per check and the CUSUM thresholds (declared, from the
   literature: h = 5 sigma, k = .5 sigma). A `--baseline` mode writes the TRAIN reference distributions once; live mode
   compares to them. No retire/reweight action: flags only.
3. Fitter WorkStore (the L2 stretch): fit_composition_weights.py reuses an unchanged candidate's admission statistics
   keyed on (dsl sha, field_payload_sha256 map, role sha, screen id); prints "computed k, reused m"; admission.json
   byte-identical with and without the store (test on synthetic v2 caches).
4. research_cycle.py: a `card` phase (runs 1 after u) and a `monitor` phase (after nav, baseline mode); dry-run lines.
5. Tests: pytest on synthetic inputs for every metric with known answers; a determinism test (two runs, identical
   bytes); the WorkStore identity test.

**Root acceptance:** report cards generated for the 39 v6.1 candidates from the real u pass in < 60 s; monitor baseline
from the v6.1 holdings_days + risk outputs; fitter identity on the v6.1 admission (computed 0, reused 39 on the second
run). Report: metric definitions, files, test counts, the exact root command lines.
