# Goal prompt — resume atx-db tier-1 v2 (session 3 controller)

Paste the block between the rules as the next parent agent's `/goal`. It assumes the state recorded in the ledger
`.superpowers/sdd/tier1-v2/progress.md` at the line `18:46Z SESSION 2 STOPPED by user`. The "State at handoff" and
"Notes for the user" sections below the block are for reference; the next controller rebuilds state from the ledger and `git log`.

---

/goal Resume and finish the atx-db tier-1 v2 DAG sprint series. Spec: docs/superpowers/plans/2026-09-25-tier1-v2-00-dag-index.md. Sprint files: the s0, s1, s2, s3, s4, s5 and sx files next to it. Read the index first (§2 exit criteria, §3 constraints, §4 memory doctrine, §5 swarm rules, §6 DAG, §9 rulings). Then read the handoff docs/superpowers/handoffs/2026-09-26-tier1-v2-next-goal-prompt-2.md ("State at handoff" section only) and the older status file docs/superpowers/handoffs/2026-09-26-tier1-v2-status-1.md (TL;DR and §4 only).

Resume, do not restart. The ledger is .superpowers/sdd/tier1-v2/progress.md, and it plus `git log` is the source of truth:
- Every node with a `Task <node>: complete` line is DONE; never re-dispatch it. Complete today: 0.1–0.12, 0.14, 1.11, X.3.
- Rulings R-1..R-14 and controller rulings C-1..C-58 in the ledger are binding. C-58 is the user's memory-model directive, and it supersedes index §4 M4's fixed floors and §5.3's agent gate. Add new rulings as C-59 onward, each as `Ruling: <decision> — <why> — <cost if wrong>`.
- Ruling C-42 is the user's grant: "All access requested is granted, prioritize real data evaluation."
  - The OPS agent may open and write atx-db/data/warehouse.duckdb through .superpowers/sdd/tier1-parity/run_memory_guarded.py.
  - Source gates U2, U3, U5, U6, U8 and U9 are granted. Fetch them only in the SX node that owns each fetch: files only, SEC ≤ 5 req/s, exact UA.
  - U1, U4, U7 and U10 are not executed; take their labeled fallbacks.

Keep the same workspace (.superpowers/sdd/tier1-v2/):
- CONTEXT.md: standing rules that every agent reads first.
- REVIEW-CONTEXT.md: the reviewer lens and output contract.
- CARRY.md: per-node must-fix items routed from reviews. A node's implementer treats its CARRY section as part of its brief.
- pkg.sh builds review packages from explicit commits: `bash .superpowers/sdd/tier1-v2/pkg.sh OUT [SHA...] [--tag NODE [--since SHA]]`.
- Briefs for 0.15, 1.1–1.13 and X.1–X.6 already exist as task-<node>-brief.md. Extract any other brief with `bash C:/Users/natha/.claude/plugins/cache/claude-plugins-official/superpowers/6.4.1/skills/subagent-driven-development/scripts/task-brief <sprint file> <task N> .superpowers/sdd/tier1-v2/task-<node>-brief.md`.
- Ledger anchor: append every new line with Edit, inserting it immediately BEFORE the final line `- 11:50Z HOLD 0.12 fix round until rev-0-4 returns`.

First actions, in order.

1. Reconcile.
   - Run `git log --oneline f6aacb45..HEAD`. Expect 11 session-2 commits, the last being 8326ce48, plus any commits from other sessions.
   - Run `git status --short -- atx-db docs/methodology`. Expect ONLY node 1.12's uncommitted WIP, 9 files: atx-db calendar.py, research/benchmarks_load.py, research/panel.py, seeds/research_anomaly_catalog.csv and tests/test_research_catalog.py; untracked scripts/research_price_wave.py, research/price_natives.py and research/spine.py; and repo-root docs/methodology/CLOCKS.md. Keep it and never discard it.
   - Read the tail of the ledger from `14:46Z SESSION 2 RESUMED` onward.

2. Node M0, memory admission rework (ruling C-58). This is the FIRST implementer you dispatch. Until it lands, no other agent runs Python; reviewers verify from files.
   - Scope:
     - Implement the memory model stated in this goal's "Memory model" paragraph in `.superpowers/sdd/tier1-parity/run_memory_guarded.py`: size-based admission (free commit ≥ cap + 1.0 GiB), in-run stop at free commit < 0.75 GiB or free phys < 0.25 GiB sustained 30 s, host-wide slot files (sum of caps ≤ 2.0 GiB, one HEAVY), `--wait-minutes` FIFO queueing, page-aligned cap check, and the native peak recorded on every exit.
     - Fold in CARRY §1.3's guard items: C-8, C-11.
     - Route every other admission or gate copy through this one guard, so there is one rule: the launch margin 1.7 added to its SEC fetch launcher, research/workers.py's capped workers, and any slice-runner draft. Tokens: ACT for activation.py if touched.
   - Acceptance, measured on this host in its normal state (VS Code open), in an M0 report:
     - (a) Three 0.6 GiB probes submitted together all complete. Record the queue wait for each; each should wait ≤ 10 min at free commit ≥ 2.2 GiB.
     - (b) A job that allocates past its cap fails inside its job object, the host's free commit never drops below the reserve, and the receipt shows the peak.
     - (c) A capped "pressure" allocator lowers free commit below 0.75 GiB and triggers an in-run stop of a ledgered test slice. The resume completes with 0 duplicate keys.
     - (d) 0.6 and 0.8 GiB caps both launch.
   - Commit `[v2 M0]`, then a scoped review.
   - When it lands, the controller rewrites CONTEXT.md's `## Memory (binding)` section to this model, deleting the phys 1.8 / commit 3.6 agent gate. Record ruling C-58 in the ledger if it is not there, and ledger M0 like any node.

3. Dispatch these in parallel. They are file-disjoint. They may start at once, but they run Python only through the M0 guard once it has landed. Before that, reviewers verify from files and implementers write code.
   a. Node 0.13, scoped re-review of fix round 1/2.
      - Package: `pkg.sh .superpowers/sdd/tier1-v2/pkg-0.13-r1.diff 8326ce48`.
      - Scope: findings I1–I4, M9 and C-39(b) from task-0.13-review.md.
      - Inputs: report sections `## Fix round 1 (completed)` and `## Stopped (session 2)`.
      - Ruling C-54 already accepted the "< 5% always repaired" deviation.
      - Output: task-0.13-rereview-1.md.
      - If not approved, run fix round 2/2 with a FRESH implementer (the old one is gone), then adjudicate.
   b. Node 1.9, scoped re-review of fix round 2/2 (the last).
      - Package: `pkg.sh .superpowers/sdd/tier1-v2/pkg-1.9-r2.diff da4e87fe`.
      - Scope: I3 and n1–n3 from task-1.9-rereview-1.md; report section `## Fix round 2 (stopped)`.
      - Output: task-1.9-rereview-2.md.
      - Then adjudicate every open finding with a ruling (park it, or route it to CARRY) and mark 1.9 complete. No third fix round.
   c. Node 1.10, scoped re-review of fix round 1/2.
      - Package: `pkg.sh .superpowers/sdd/tier1-v2/pkg-1.10-r1.diff 4f6cee5d`.
      - Scope: I1–I4, m1, m2 and m5 from task-1.10-review.md; report section `## Fix round 1 (stopped)`.
      - Verify C-52: `open_holdout` records the label-matrix spec label_sha.
      - The reviewer runs tests/test_research_evaluation.py and tests/test_research_composites.py once each; they were never completed.
      - Output: task-1.10-rereview-1.md.
   d. Node 1.7, fix round 1/2 with a FRESH implementer.
      - Inputs: task-1.7-brief.md, task-1.7-report.md, task-1.7-review.md.
      - Must fix:
        - I1: the limiter lock must be host-wide by a fixed canonical path, or the worker refuses to start without it. Export launches (C-23) must not get a second lock and double the SEC rate.
        - I2: a shared host-wide pause on 403/429, and an abort after N consecutive 403/429.
        - I3: activation.py:914-931 raises ActivationStageError when not_prefetched > 0.
        - Cheap minors.
      - Never edit python/scripts/build_us_split_adjustments.py (C-53, outside atx-db).
      - Then a scoped re-review. X.4 and X.5 wait for 1.7 to be complete.
   e. Node 1.12, continued by a FRESH implementer (it holds PANEL and CAT).
      - Inputs: task-1.12-brief.md, CARRY.md §1.12, and task-1.12-report.md `## Stopped (session 2)`, which lists the files, their state, the w1_price pre-registration table and the remaining steps.
      - The 9 WIP files (the 8 atx-db files above plus docs/methodology/CLOCKS.md) are its starting point. Begin with `git diff` / `git status` on them and keep what is correct.
      - The C-48 gate is met (0.13 = 8326ce48); it may commit now.
      - Must do before commit 1:
        - C-57: characterize the 272,181 rows with a non-positive securityID; never a silent exclusion.
        - C-56: private spill dir per connection in research_lake.connect_bounded. Name research_lake.py in the pathspec.
        - C-55: the EW market uses repaired returns and reports clipped vs unclipped.
        - Run every stage on the real file, with per-year passes ≤ 0.6 GiB.
        - Coverage per feature × year; label coverage and terminal_pending counts.
        - Regenerate ANOMALY_CATALOG.md and record the catalog digest.
        - Run the two touched tests once.
      - Do NOT call register_wave; 1.13 registers.
      - Commit 1 is the core deliverable plus fast-path CARRY. Commit 2 is C-20, C-21 and the 0.10 CAT minors.
      - Then the task review.

4. Integration gate. When 0.13 is complete, run 0.15 (task-0.15-brief.md). Step 1 lists 1.12's WIP as known, not as a failure (C-43). The gate runs on a git-archive export of HEAD. Its PASS sha is the S1 base.

5. Research fast path, the user's priority. When 1.10 and 1.12 are both complete, run 1.13 (task-1.13-brief.md plus CARRY §1.13). This produces the first real cross-sectional results.
   - Before the first label join:
     - Call register_wave with the new signature `register_wave(wave, catalog_digest, policy_sha, feature_ids, cells, *, rows, preregistration, reported_only=(), first_formations=None, root=None)`, using the pre-registration table from task-1.12-report.md. The EW beta/ivol twins are reported-only (C-24).
     - Commit seeds/research_trial_registry_anchor.jsonl.
   - Grading:
     - Use grade_wave_v4 with run_bases=tables.bases. research_qualify.py's `qualify` raises PolicyError under v4.
     - Formations stay ≤ 2023-12; the holdout stays sealed.
     - Use 1.9's new cache API: `ec.basis_key(inputs, spec)` before evaluate_bases, and `EvalCache.write(..., evaluated=tables.bases[basis])`.
     - Keep the eval worker ≤ 0.6 GiB (1.9 m5).
     - Report `ewc_oversize_risk` flags (C-50).
   - Policy v4 thresholds are frozen (C-49, sha 776db445…).
   - Output: docs/research/WAVE1_PRICE_PROVISIONAL.md.

6. Then follow the DAG (index §6). Run every unblocked node in parallel whenever files and ownership tokens (MIG, ACT, HEAVY, CAT, EVAL, PANEL, FEAT, STD, DMD; item_registry.py is STD per C-29) don't overlap.
   - After 0.15: 1.1, then 1.2 and 1.4; then 1.3, then 1.5/1.6; then the 1.8 B0 heavy run (OPS agent, HEAVY).
   - After 1.1 and 1.11: 2.2. After 1.3: 2.1, then 2.3–2.6.
   - Source fetches:
     - X.5 (U9; about 47 h at 5 req/s, resumable; run it in the background) as soon as 1.7 is complete.
     - X.4 (U3) after 1.1 and 1.7.
     - X.1 (U2) after 1.3 and 1.7.
     - X.2 (U2) after 1.3.
     - X.6 (U5) after 1.7 and 3.3.
   - The warehouse critical path is 1.2 → 1.3 → 1.5 → 1.8 → 2.1 → 2.7 → 3.6 → 3.8 → 4.3 → 5.6 → 5.7.

How to run it:
- Subagent-driven development: one Claude Opus 5.5 implementer per task (Agent tool, general-purpose, model opus), then one Opus review.
- At most 2 fix rounds, then adjudicate and record the ruling. Within one session, resume the same implementer with SendMessage for fix rounds. Across sessions, use a fresh implementer pointed at the review and report files.
- Keep your own context small. Dispatch by brief path plus only the interfaces the brief cannot know, and read report files. Reviewers write full reviews to task-<node>-review.md (task-<node>-rereview-N.md for re-reviews) and reply in ≤ 15 lines. Never paste history into prompts.
- Agent names may not contain dots (impl-1-13, rev-1-13). Every commit subject carries `[v2 <node>]`.
- Keep at most 8 live agents. Only the OPS agent holds HEAVY. It runs one warehouse writer or read-only export slice at a time, always from a fresh git-archive export of current HEAD (C-23), with ATX_SEC_RATE_LOCK set for SEC workers.
- When two lanes touch one file, rule who commits the hunk (see C-47). A lane must never pathspec-commit a file that holds another lane's uncommitted edits.

No test-driven development. Accept every task on real-data measurements recorded in its report. Write tests only for PIT edge cases, migrations on real-shaped data, kill-and-resume drills and one end-to-end check per pipeline.

Memory model (user directive 2026-09-26, ruling C-58). This replaces index §4 M4's fixed guard floors and §5.3's agent gate. Goal: steady, iterative progress with no out-of-memory crash and no hours-long waits for headroom.
- Why: in session 2 the host (15.7 GiB RAM, commit limit ~28.8 GiB, pagefile system-managed) sat at 1.8–3.5 GiB free physical memory and free commit. The old rules demanded min(free phys, free commit) ≥ job + 2 GiB to launch, killed a run at free phys < 1.5 or free commit < 3 GiB, and gated agents at phys 1.8 / commit 3.6. So 0.5 GiB jobs waited hours or were killed seconds after launch. Those floors measured the whole host rather than the job.
- What prevents OOM is the per-job hard cap. Every Python, pytest or DuckDB process that any agent or OPS starts runs under `run_memory_guarded.py --job-gb <cap>`, a Windows Job Object that caps committed memory. That process can never exceed its cap, so it cannot exhaust the host. DuckDB memory_limit sits well below the cap so DuckDB spills first. Nobody launches an uncapped Python process, including one-line probes.
- Caps (unchanged maximum): probes and pytest 0.6 GiB, research workers 0.6, warehouse writer slices 0.8, set-based passes 1.0. DuckDB memory_limit/threads are set at connect: 256MB/1 for 0.6 caps, 384MB/1–2 for 0.8, 512MB/1 for 1.0. No transaction over ~2M rows. Heavy stages are resumable ledgered slices.
- Admission is size-based. A job starts when free commit ≥ cap + 1.0 GiB reserve. Free physical memory is not an admission condition: low physical means paging and slower runs, not a crash.
- In-run protection:
  - The guard stops our job only if free commit < 0.75 GiB (other applications are growing), or if free physical stays < 0.25 GiB for 30 consecutive seconds (thrash).
  - A stopped slice resumes from its ledger. That is a normal event, not a failure.
- Host-wide slots:
  - The guard holds a slot file for every running job.
  - The sum of our running caps is ≤ 2.0 GiB at any moment (e.g. one 1.0 HEAVY slice + two 0.5 probes).
  - Only one HEAVY job runs at a time.
- Waiting:
  - `--wait-minutes N` makes the guard queue FIFO for admission and a slot. Agents never hand-poll a memory gate; a queued agent does code work meanwhile.
  - A job that cannot be admitted after its wait is re-engineered smaller per §4 M8. Memory is never a reason to stop or to return BLOCKED.
- The cap check is page-aligned: `int(job_gb*GIB)//4096*4096`, never above the request (C-8). The receipt records the native peak on every exit, including stops (C-11).
- Still forbidden: caps above 1.0 GiB; running without a cap; whole-stage transactions; ART indexes on bulk tables; `.df()`/`.arrow()` of a full table; the retired archive21 watcher.

Done when the index §2 exit criteria are met with evidence and the 5.7 candidate release is published with its release notes. Also stop if every remaining node is blocked only on an unexecuted user gate or a permission denial; in that case write a status file listing what is blocked and why.

Standing rules:
- Edit files only with the Write/Edit tools, the ledger included. Append with Edit, never a heredoc.
- Stamp ledger lines with `date -u +%H:%MZ`; never estimate a time.
- The SEC user agent is exactly atx-db/0.1 atx-research@example.com.
- Never push, merge, stash, reset, restore, clean or discard others' work.
- Keep the prod warehouse and its latest backup `warehouse.duckdb.pre-migrate.20260924-225023.bak`.
- Don't touch atx-engine/, atx-vol/, C:\atx-wt\, docs/plans/2026-09-24-alpha-engine-*, python/ (repo root), or other sessions' processes.
- OPENBLAS_NUM_THREADS=1 for every Python process.
- Commit with explicit pathspecs and the trailer Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>.

---

## State at handoff (2026-09-26 18:46Z, HEAD 8326ce48)

### Session-2 commits (11, all on `main`)

| Commit | Node | What |
|---|---|---|
| 19bc6729 | 1.11 | Rule-based XNYS sessions, entry clock, CLOCKS.md (complete) |
| 396550de, 31419fcd, 4f6cee5d | 1.10 | Policy v4 frozen (sha 776db445…), trial registry, power study, fix round 1 (anchored registry, order/seal at grading, `ewc_oversize_risk`) |
| 13e28ced | 0.14 | P14 monotone visibility clock (N1) (complete; carries 1.7's UA docstring hunk per C-47) |
| d4d44d40 | X.3 | French / JKP / OSAP / q5 benchmark fetch and parse (complete) |
| 2b26a132, 826c0380, da4e87fe | 1.9 | Parquet research store L1–L4, capped workers, fix rounds 1–2 |
| deed2bd6 | 1.7 | sec_http (single UA, host-wide limiter), Item 2.02 fetch/load split |
| 8326ce48 | 0.13 | VA1 v2 artifact rule, deterministic bar pick, split detectors skip artifact steps |

### Node status

| Node | State | Next action |
|---|---|---|
| 0.13 | Fix r1 committed + measured (8326ce48) | Scoped re-review r1 |
| 0.14 | **complete** | — |
| 0.15 | Not run | After 0.13 complete |
| 1.7 | Review not approved (I1 lock path, I2 403/429 backoff, I3 partial-prefetch "completed") | Fix r1 (fresh implementer), re-review |
| 1.9 | Fix r2 committed (da4e87fe); both rounds used | Scoped re-review r2, then adjudicate + complete |
| 1.10 | Fix r1 committed (4f6cee5d) | Scoped re-review r1 (+ run 2 research test files) |
| 1.11 | **complete** | — |
| 1.12 | In progress; 9 WIP files uncommitted (incl. docs/methodology/CLOCKS.md); stages never run end to end | Fresh implementer continues (C-55/56/57 first) |
| 1.13 | Not started | After 1.10 + 1.12 complete |
| X.3 | **complete** | Re-parse + lake registration carried to 1.12 |
| 1.1–1.6, 1.8 | Not started | After 0.15 (1.1 first) |
| X.4, X.5 | Not started (U3, U9 granted) | After 1.7 complete (X.4 also after 1.1) |
| S2–S5, X.1, X.2, X.6 | Not started | Per DAG |

### Real-data results so far (no alpha grades yet)
- Calendar: 3,642 rule sessions = 3,642 bar sessions (2012-03-26 → 2026-09-18), 0 missing / 0 stray in the retained file.
- VA1 v2 on 2021-01-04: median return −1.30% → −0.58%, EW mean (clipped ±50%) −0.82% → +0.27%; 12 flat lines repaired, 0 other steps changed.
- Benchmarks: French Mkt-RF 2012–2025 mean 1.12%/mo, sd 4.17%; ρ(French Mkt, q5 MKT) = 0.99997; ρ(French UMD, JKP ret_12_1) = 0.955.
- Power on the real 138-formation sample: replication ICIR_3m for 50% / 80% power = 0.45 / 0.57 (selection) and 0.47 / 0.62 (final). Discovery is practically unreachable at 4,800 trials (C-49 keeps it frozen).
- Null replication FDR exceeds q 0.10 at persistence ≥ 0.95 (0.155 → 0.228 at rho 0.99). Such features carry `ewc_oversize_risk` (C-50).

### New rulings this session (C-42 … C-57)
- C-42 user access grant.
- C-43 early start of the fast path.
- C-44 store adapter lives in feature_store.
- C-45 0.14 round-2 scope.
- C-46 1.11's activation diff via the ACT holder.
- C-47 cross-lane docstring hunk.
- C-48 1.12 early start, gated on 0.13's commit.
- C-49 v4 thresholds frozen.
- C-50 EWC over-size flag.
- C-51 non-SEC hosts keep the approved UA.
- C-52 open_holdout keys on the label-matrix sha.
- C-53 python/scripts UA flagged to the user.
- C-54 0.13 deviation accepted.
- C-55 unclipped `market_return_ew`.
- C-56 shared spill dir.
- C-57 non-positive securityID rows.
- C-58 memory model (user directive): per-job hard cap + size-based admission; implemented by node M0 first.

Full text is in the ledger.

---

## Notes for the user
- **Memory headroom was the bottleneck in session 2; the goal now fixes the rule, not the host.**
  - The old guard/gate floors needed 2–3.6 GiB of spare memory beyond each job. Free memory on this host is usually 1.8–3.5 GiB, so 0.5 GiB jobs waited hours or were killed seconds after launch.
  - The new model (C-58, node M0 runs first) keeps each job's Windows hard cap as the OOM protection and admits a job when free commit ≥ its cap + 1 GiB. It stops a job only if free commit drops below 0.75 GiB, and caps our total at 2 GiB across queued, resumable jobs.
  - Expect steady progress with the host as it is.
  - Optional, not required: closing idle VS Code windows (about 9 GB of commit at 17:10Z) or a larger fixed pagefile would still make runs faster.
- **A second SEC user agent exists outside atx-db.** `python/scripts/build_us_split_adjustments.py:316` sends `contact@example.com` and bypasses sec_http (ruling C-53). It lives in the repo-root `python/` tree, which is outside this plan's lanes, so no agent touched it. Fix or retire it before anything runs it.
- **Gates.** I took "All access requested is granted" to cover prod-warehouse writes by the OPS agent and the fetch gates U2/U3/U5/U6/U8/U9 (C-42). If you meant U1, U4, U7 or U10 too, say so in the goal. U4 also needs vendor credentials.
