# Task L2: research-cycle driver, field reuse, and backtest-integrity tooling (Python)

**Pool:** C:/atx-wt/pool-11. `git status` must be clean, then `git checkout -B feat/platform-v7-l2-cycle-20260928 <BASE>`
where BASE = `git -C C:/atx-wt/pool-2 rev-parse HEAD`. Work only in pool-11. Never build C++, never run the pipeline or any
real data (DRY=1 / `--dry-run` only), never spawn subagents, never touch C:/atx. You may read C:/atx-wt/pool-2/build-equity/**
by absolute path (TRAIN outputs only; never anything named validation/VAL/2023/2024/2025; never a per-candidate VAL
statistic). Python: "C:/Program Files/Python312/python.exe"; stdlib + numpy (+ pandas/pyarrow if already used by the tool
you extend). Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Report:
C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/task-L2-report.md (<= 40 lines). Reply in chat < 15 lines.
Do not edit atx-impl/src/strategy_ic_runner.cpp (lane L1 owns it) or any C++.

**Read first:** code-review-v7.md findings A3, A4, C2, S4 Lane 2; literature-v7.md S5 (R5.1-R5.3) and the DSR/PBO
references; plan-v7.md S2-S3; .superpowers/sdd/mega-alpha-20260926/studies/v6u_train.sh, v61_train.sh, nav_summ.py,
v4-prereg.md (the statistics protocol: paired dSR Memmel SE, CBB, LW, cross-cell DSR vs Lo); scripts/run_bounded_research.py
(receipt format); atx-engine/tools/prepare_research_fields.py (field builder, manifest format).

**Deliverables**
1. `scripts/research_cycle.py` with `specs/<name>.json` (start with specs/v61.json reproducing the v6.1 ladder exactly).
   A spec pins: library path+sha, recipe path+sha, base role, universe rule, fields list + builder args, IC runner flags,
   fit flags (screen, composition), NAV rule + flags + L, reference cell, dsr_n, output dir names. Verbs: `plan` (print the
   exact command lines with all pins resolved, no execution), `run` (execute phase by phase through run_bounded_research.py
   with the standard 180 s / 1536 MiB / 512 MiB caps, never overwrite an existing output, resume from the first missing
   phase), `status`. Every phase reads the runner receipt and HARD-STOPS on a non-zero exit or a refused admission (the
   current scripts grep exit_code and keep going). Pins are computed from files, never hand-typed. Tests: unit tests on
   spec parsing, pin resolution, phase ordering, stop-on-refusal, dry-run command equality with v61_train.sh (string
   compare of the resolved lines against the existing script's DRY=1 output captured into a fixture).
2. `prepare_research_fields.py --reuse <prior-fields-dir>`: copy unchanged field payloads (same field id, same formula id,
   same inputs sha) instead of recomputing, record `reused_from` per field in the manifest, build only the new ones.
   Test: synthetic role, build A (n fields) then B (n+1, --reuse A): the n payloads byte-identical to A, manifest marks them
   reused, the new one computed.
3. Backtest-integrity tooling in studies/nav_summ.py (or a sibling module it imports), all pure Python + numpy:
   a. Trial ledger `build-equity/trials.jsonl`: one line per cell/run (kind: admission|composition|construction|universe|
      data; cell dir; pins; window; daily net return series path; S2 net SR). `nav_summ.py --ledger` appends; `--ledger-n`
      counts trials by kind and window for the Appendix A block.
   b. Effective-N DSR: ONC clustering (Lopez de Prado-Lewis 2019) of the ledger's daily net series -> N_eff clusters;
      DSR computed with N_eff and the cross-cluster SR variance; printed beside the cell-count DSR and the Lo null (do not
      change the existing numbers; add columns).
   c. CSCV PBO (Bailey et al. 2017) for any grid of >= 4 cells: 16 blocks over the TRAIN sessions, C(16,8) splits
      (12,870; subsample deterministically if you must, seeded, and say so), logit distribution, PBO; `--pbo <cells...>`.
   d. PSR and MinTRL (Bailey-Lopez de Prado 2012) for the final cell.
   Tests on synthetic series with known answers (e.g. iid normal cells: PBO ~ .5; a strictly dominant cell: PBO ~ 0).
4. Legacy check: running the new nav_summ on the v6.1 cell vs its v6 reference with `--dsr-n 29` must print the same
   numbers as before (+1.239 / DSR .9108 / Lo .531) -- compare to build-equity/*n29*.json outputs (read-only).
5. Stretch (only if 1-4 are done): fit_composition_weights.py WorkStore keyed on the L1 sidecar `field_payload_sha256`
   so an unchanged candidate's admission stats are reused (report "computed k, reused m").

**Root acceptance:** `research_cycle.py plan specs/v61.json` command lines equal the v61_train.sh lines pin-for-pin; root
then runs `run` on a fresh output-name suffix and the S2 daily CSV SHA equals the v6.1 cell; `--reuse` from lo1-fields-v6b
computes only sv_ratio126 with payload SHAs equal to lo1-fields-v7; a deliberately refused receipt stops the cycle;
ledger/PBO/N_eff outputs reproduce on the existing 29 cells.

Report: files with line counts, the spec schema, the exact root command lines, test counts, and what you could not verify.
