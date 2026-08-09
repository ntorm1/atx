# GOAL PROMPT — Implement the pf1 Fundamentals-Parity series

> Paste below the line into a fresh agent. Detail lives in the plans; this is the driver.

---

Bring `atx-impl/db` to point-in-time parity with FactSet / S&P GMI Compustat for **US equity
fundamentals**: every metric, ratio, and derived ratio, linked through a strong schema, driven by real
job management. Implement all 8 sprints **PF-S1…PF-S8** end-to-end.

**Use `superpowers:subagent-driven-development`** — you are the controller; fresh subagents implement +
review (implementers use TDD + verification-before-completion; final whole-branch review + finish-branch
at the end). Do not write code yourself.

**Read `atx-impl/plans/pf1/ROADMAP.md` first** — it holds the shared PIT/determinism contract, ownership,
reserved migration ranges, sequencing, and the north-star acceptance. Each `sprint-N-*.md` is the plan
file for that sprint; its `S{N}-0, S{N}-1, …` tasks are the task list (one implementer + one review each).
Follow the ROADMAP sequencing (S1‖S2 → S3‖S5 → S4‖S7 → S6 → S8; sequential in one tree). Track progress
in `.superpowers/sdd/progress.md`; resume from the first incomplete task.

**Non-negotiables** (also in ROADMAP §Shared contract — put them in every implementer + reviewer block):
- Offline tests only (no SEC/FRED/FINRA/OpenFIGI/GLEIF network in pytest; live connectors are injectable
  `--*-file`/`--*-zip`, smoke is operator-run + recorded in the ledger).
- Bitemporal PIT, no lookahead: `available_at = max(input.available_at)`; as-of readers gate on valid
  window **and** `available_at ≤ as_of_ts`.
- Append-only idempotent migrations in the sprint's reserved range; catalog every new table/view in the
  same migration; split schema vs index for WAL safety.
- PF-S1 and PF-S4 refactors keep the ratio rebuild **byte-identical** (regression test gates it).
- Don't touch C++/CMake or the non-fundamental domains (13F/insider/macro/short-interest/off-exchange).
- `python -m pytest atx-impl\db\tests -q` green before each commit; never `git add -A`; commit trailer
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`; per sprint, append a
  `WAREHOUSE_PARITY_TRANCHES.md` row + update `PARITY_GAP.md`.

**Autonomy:** run for hours; don't check in between tasks/sprints. Stop only on unresolvable BLOCKED,
genuine ambiguity, destructive risk, or all 8 done. Done = the ROADMAP §North star acceptance.
