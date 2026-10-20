# vol-sota sprint — parent (PM) handoff, resume 2 (2026-10-19, user stop ~23:05 EDT)

Read `2026-10-19-vol-sota-status-r2.md` first (goal, what landed, numbers, order on resume). This file is the process handoff for the next PM: where everything is, what is ready to dispatch, the rules that bind, the traps found. Resume only on a new user instruction.

## Start here on an authorized resume
1. Read, in order: the status-r2 file; `SHARED-BRIEF.md` (all, especially "Resume wave 2" — the user directive and rulings a-c); `progress.md` from the line "RESUME 2" (the ruling log; `2026-10-19-vol-sota-pm-ledger.md` in the tree is its copy at the stop); the three research/gap reports in the workspace.
2. Pool state: `powershell -NoProfile -File C:\atx-wt\pool-1\scripts\lease-worktree.ps1 -Status`. Lease pool-1 as root12: `-Branch feat/vol-resume4-20261016 -Base <branch tip> -Agent pm-root12 -RunId vol-<date>-root12 -HeartbeatId vol-<date>-root12-hb -MaxPool 40` (the lease script asserts the existing branch head equals `-Base`; take the tip from `git -C C:\atx-wt\pool-1 rev-parse feat/vol-resume4-20261016`). Pools 2, 8, 12-32 are foreign dead leases: ignore. Refresh SHARED-BRIEF's root SHA / RunId line.
3. Dispatch in the order of status-r2 §4. Every lane: Agent tool, `general-purpose`, `model: fable`, background, prompt = "read SHARED-BRIEF (all, incl. Resume wave 2) then <brief>" + the hard-rules block + "≤ 15-line hand-back". Reviews at the lane's exact final SHA with a fresh agent. One PM ruling per deviation, logged in `progress.md` with its cost-if-wrong.

## Briefs ready in `C:\atx\.superpowers\sdd\2026-10-18-vol-sota\` (state at the stop)
| Brief | State | Fill before dispatch |
| --- | --- | --- |
| `lane-review-L2e-brief.md` | SHA `9923ce4c` filled; PM notes (ruling-(a) acceptance, COUNT number not target, budget extension to read) | nothing — re-dispatch if the checkpoint review is UNDECIDED |
| `lane-integrate-W2b-brief.md` | Dispatch list filled (L2c `3d5fe7fa` APPROVE, L2e `9923ce4c`, L2f `4ba287c9` APPROVE); PM additions (caps, ruling-(a) parity wording, W2b-keys G2, docs incl. HFT rows + three ledger lines) | paste the review-L2e verdict into item 2 |
| `lane-L2g-screen-wiring-brief.md` | Additions section (R2 band-gated confirm key, R3 Fast-confirm arm, r = q guard, matrix A-D, caps, RunId r2) | `ROOT_SHA_PLACEHOLDER` → the W2b-integrated root SHA |
| `lane-L2h-sigma-family-brief.md` | Original brief (base `4ba287c9`) | write `lane-L2h-sigma-family-resume-brief.md` from the lane's checkpoint report (same pattern as the L2e/L2f resume briefs); base = the L2h tip |
| `lane-research-sota-brief.md`, `lane-research-cold-start-gap-brief.md` | done / checkpointed | — |
| `lane-review-L2c-brief.md`, `lane-review-L2f-brief.md`, `lane-L2e-deam-inversion-resume-brief.md`, `lane-L2f-al-fast-tier-resume-brief.md` | consumed | — |
Not yet written: `lane-L2i-family-wiring-brief.md` (family → Premium iterates + certification grid; the L2h note's "What L2i must assume" section is its input), `lane-integrate-W2c-brief.md` (L2g + L2h + L2i; clone integrate-W2b), the L3 accuracy briefs (plan W3; `lane-knot-union-clamp-brief.md` exists in the resume-6 workspace), and the lanes the cold-start gap report ranks as TAKE.

## Process that worked this session (keep)
- Fable 5.1 for every role. Implementer lanes 200-390k tokens / 20-55 min; reviews 250-265k / 22-25 min; research 280k / 21 min.
- Three build lanes at most, `-Jobs 2`, `ATX_VOL_FIT_WORKERS = 2`: the host has 12C/16T and ~3-4 GB free (VS Code + Chrome + several claude processes); load 64 % and 1.1 GB of pagefile at three lanes. Research/gap lanes do not build and can run beside them.
- Resume briefs that name the checkpoint SHA, what is done/tested, the PM rulings, and the remaining work in order; reviewers get the SHA filled and a "PM notes" section that overrides the generic items.
- The prebase trick again: L2h was based on L2f's tip (`4ba287c9`) so it merges clean after L2f; file-disjoint lanes (fitting layer vs kernel layer) run in parallel.
- Research lanes before kernel lanes: the research-sota report found the in-tree prior art (`andersen_lake_put_slice_sigma`) that shaped L2h in one hour instead of a day.

## Rulings this session (undo any you disagree with; each is in progress.md with its cost-if-wrong)
1. Accuracy criterion = distance to the accurate root / market / SpiderRock; "≤ 1e-9 USD vs the old path" withdrawn where the old path is less accurate (L2e first case).
2. Default promotion only at integration by PM ruling on dev200 + dev200b + cohort5 evidence, hash set re-baselined in the same commit; holdout closed.
3. Opt-in keys stay the dev mechanism; old paths may be deleted after promotion.
4. Provisional HFT rows (status-r2 §1) — research-sourced; the W2b integrator adds them to plan §1.1.
5. L2c `cert_screen` not shipped by finding (saves nothing while the repairs need all 2N accurate prices) — accepted; pre-screen typing today's refusal only — accepted.
6. L2e COUNT 0.392 (target ≤ 0.33) accepted as DONE_WITH_CONCERNS; row 466851 (AMDY) = Search's intrinsic-region answer, no guard.
7. L2h dispatched on L2f's tip before L2f's review finished (prebase trick).
8. Next wave after W2b = L2g (fitting layer, with R2 + R3 measurement arm + r = q guard) ∥ L2h (kernel), then L2i wiring; R3 promotion decided at integration from L2g's table.

## Deferred minors (new this session; older ones in the two earlier handoffs)
- review-L2c: `quote_slice.cpp` has five regions, not one hunk (all in scope); test-only process-global hook in `expiry_schedule.hpp`; evidence folder lacked `sha256.txt` (reviewer re-hashed).
- review-L2f: literal `kChainRowStatusCount = 7` pinned by static_assert (fine; could derive from the enum); the headline Reference 260 µs is a loaded-host artifact (160-170 µs quiet — say so when quoting "7-12×"); bench TUs are outside the `vol-hygiene` preset (PCH-off cannot cover `atx-vol/bench/*.cpp`).
- L2e: 381 `no_seed` fallbacks are deep-ITM put mids with the BAW leg at its intrinsic floor (a smooth seed leg is the follow-up); the SPY wall 29.9 → 15.6 s is not like-for-like (load); one cohort5 parity log line was appended by hand after `Get-Counter` threw post-run (hash verified).
- L2f probe: Screen dips at r = q (295 + 295 cells) where the accurate scheme has none → L2g guard requirement (routed to the accurate inversion, never a quote refusal).

## Ledger candidates (append at the next docs commit; the W2b brief already lists the first three)
- `| trap |` a missing `.atx-lease` record is reported LEASED by `-Status` but refuses both release and `-RecoverStale` ("inside the grace period": a missing record never ages); a fresh lease of the slot is the recovery (pool-1/3/33, 2026-10-19).
- `| trap |` bench TUs are not configurable under the `vol-hygiene` preset (L2f report).
- `| note |` the 2026-10-16 research report's "Healy 0.41 bp RMSE" is a 100× conversion error (4.1e-5 USD at K = 100 = 0.0041 bp of K).
- `| benchmark |` L2c, L2e, L2f headline lines (each lane's note carries its candidate text).
- `| trap |` RTK-wrapped `git status --short` / `ls -la` in Bash hide or garble dotfiles and status lines (the `.atx-lease` files and a ` M` line were invisible); use `rtk proxy git ...` or PowerShell `Get-ChildItem -Force` for lease and tree-state checks.

## Traps found this session
- Lease records: after a session ends, the keepers die and the per-tree `.atx-lease` files can be gone while `-Status` still prints LEASED from the slot's `.atx-lease.*` sidecars; plain release and `-RecoverStale` both refuse; a fresh lease of the (actually free) slot works and `-Status` then reports the others free. Verify keeper PIDs with `Get-Process` before trusting `owner=alive`.
- RTK Bash wrapper: `cat` on an absolute dotfile path reported "cannot find the file" and `ls -la | grep` hid dotfiles; `git log` output can lag the branch tip (reflog showed two newer commits) — use `rtk proxy git rev-parse` / PowerShell for anything that gates a decision.
- `Shell cwd was reset` after many Bash calls: always use absolute paths or a leading `cd <abs> &&` in the same command.
- Large tool outputs are persisted to a file and truncated at ~30 KB; read long files in line ranges.

## Artifacts (all with `sha256.txt` unless noted)
`C:\atx-cache\quote-benchmark\`: `L2c-rung-memo-20261019-evidence\` (no manifest; reviewer re-hashed), `L2c-3d5fe7fa-review-v1\`, `L2e-deam-inversion-20261019-evidence\`, `L2e-deam-inversion-20261019-resume\` (104 files), `L2f-al-fast-tier-20261019-{inputs,us,scan,fp,nonmonotone,gates}\`, `L2f-4ba287c9-review-v1\`, `L2h-sigma-family-20261019-*\` (if the lane created any; see its report), plus the W2 folders from session 2. Workspace: every brief/report above, `research-sota-american-hft-report.md`, `research-cold-start-gap-report.md`.

## Lease disposition at the stop
As FOUND by the resume-3 PM on 2026-10-20 (the session-3 PM never filled this): pool-1 LEASED root11 (keeper 18768 dead; clean at `f4a5028e`; NOT released; the status-r2 / handoff-r2 docs commit was never made — both files exist only in the workspace); pool-3 LEASED review-L2e (keeper 3440 dead; clean at `9923ce4c`; review report §8-11 unwritten); pool-4 LEASED L2h (keeper 13564 dead; 4 modified + 2 untracked files UNCOMMITTED at `4ba287c9`); pool-5 / 33-36 free; 24 foreign dead p9 / aes leases. Resume-3 disposition: pool-1 released and re-leased as root12 (RunId `vol-20261020-root12`, keeper 9452); pool-3 released; pool-4: the working tree committed verbatim as `850b6d4f` (ungated WIP checkpoint), then released. Fact for the ledger: a plain `-Release <pool> -RunId <recorded run_id>` succeeds on a dead-keeper lease when the tree is clean (owner state is checked only under `-RecoverStale`); a second release of the same slot reports "incomplete lease record" (= already released).

## Standing rules (unchanged)
No pushes, no PRs, no merges into main; nothing under `C:\atx-cache\quote-benchmark\` overwritten; SpiderRock marks are the yardstick, never inputs; holdout closed; evidence = command + exit + trimmed output; source/doc edits with Edit/Write only; focused build targets only; `.agents/cpp/agent.md` before any C++.
