# AL speed lane: WIP handoff (2026-10-17) — CLOSED

This handoff recorded the resume-5 user stop of lane `al-speed` (tier 2 at
`5a0a4d21`, timing pass unfinished). Resume 6 lane `al-speed-finish` completed
every open step: the interleaved base / tier 1 / tier 2 A/B (6 rounds), per-tier
Greeks timing, the fast-profile error rerun on the final binary, the bench
`--atx-self-check`, the candidate baseline JSON and the carry-memo profile key
(`d77beb03`).

All results, numbers and the carry-memo decision are in
`2026-10-17-al-speed-sprint.md`, which supersedes this note. One correction to
the stop-time numbers this note used to carry: the interleaved A/B did not reproduce the
tier-1 "bulk 17.7 -> 16.8 us" figure (tier 1 is below this host's resolution),
and the Fast/Reference ratio is x1.8-2.3 on p50, not the single run's 2.5x.
