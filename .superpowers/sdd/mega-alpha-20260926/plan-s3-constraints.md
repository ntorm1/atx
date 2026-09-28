## 3. Global constraints (binding on every task; verbatim from the owner goal / handoff / ledger)

- "Work only in the integration checkout `C:/atx-wt/pool-2`… Never mutate, build, switch or commit in `C:/atx`; read-only
  inspection is okay." The tier1-v2 session owns `C:/atx` and atx-db: read-only, no locks, never kill its processes.
- "Root alone builds, using `build-equity/mega-build.ps1`: RAM-admitted, Jobs 2-4, target-scoped."
  `powershell -File build-equity/mega-build.ps1 -Tag <new-unique-tag> -Targets "<t1,t2>"` (refuses a reused tag; Jobs auto).
- "Root alone runs real data, under `scripts/run_bounded_research.py` with Python312 … <= 180 s, RSS 1536 MiB, free floor 512 MiB."
  Exact: `"C:/Program Files/Python312/python.exe" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 --output build-equity/<new-dir> --bind <manifests> -- <cmd>`.
  The guard needs `git status --porcelain` empty (untracked files included): **commit docs, briefs, prereg and scripts before every run.**
- "Child agents implement in their own pool worktrees and never build." Children follow `lane-contract.md`; reviewers
  `reviewer-contract.md` / `re-review-contract.md`; ≤ 5 fix rounds; children never spawn subagents; commit trailer
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; reply < 15 lines with status, commits, tests, concerns, report path, root targets.
- "Select everything … on TRAIN 2020-2022 only. Validation 2023-2024 is used once, on the frozen daily mega-alpha. 2025+ stays reserved."
  2023-2024 has been used twice: any new test there is **validation trial #3**, disclosed, and requires the owner's ruling (U1).
- "No pushes, warehouse writes or broker actions. Do not kill other owners' processes."
- "Do not use test-driven development; write focused postimplementation fixtures." "Do not let RAM slow progress." Run limits stay
  180 s / 1536 MiB (a 300 s / 2048 MiB proposal was refused: "efficiency fix, not a longer cap").
- Targets (owner ruling 2026-09-27): S2 `modeled-1bn-stale5-v1` × `swap-fin-v1` net Sharpe ≥ 1; combined-book daily one-way
  turnover mean ≤ 20% GMV, p95 ≤ 30% GMV (deployment excluded); per-alpha τ_k ≤ 70%/day; netting ratio τ_book / Σ w_k τ_k reported.
- Validation flow (binding): "unweighted TRAIN-only run (orientations O) -> fitter -> weights W (provenance O) -> ledger W sha ->
  ONE validation-only runner run with O + W -> NAV on the validation-only combined output."
- Pre-register in `v4-prereg.md` (new `## v5 revision` section, R1'-R7') and commit **before any v5 TRAIN read**. Rulings:
  `Ruling: <decision> — <why> — cost if wrong: …`, declared before any measurement they could bias.
- IC runner ≤ 1.5 GB: run `--plan-only` before any library change; ≤ 5 extras and ≤ 10 VM slots per candidate.
- House C++ rules: read `.agents/cpp/agent.md` before any C++ edit (children: the pool-2 copy).

