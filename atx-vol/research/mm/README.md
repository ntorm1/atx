# `research/mm` — Options Market-Making System Design

Research on how top options MM firms (Citadel Securities, Jump, SIG, Jane Street, Optiver, IMC, Wolverine, DRW) build their pricing / surface / quoting / risk stacks, and what `atx-vol` needs to reach that frontier.

## Reports

- **[2026-07-18 — Options Market-Making System & Algorithm Design](2026-07-18-options-market-making-system-design.md)**
  Deep-research report (5 angles, 25 sources, 25 claims adversarially verified) + `atx-vol` codebase gap analysis + prioritized roadmap. Covers pricing/numerics, vol-surface construction, quoting & hedging, portfolio risk at scale, low-latency systems.

## One-line conclusions

- **Numerics core (ALO American + Choi/LBR IV) is already frontier** — compete on implementation latency, not algorithm choice.
- **Measured perf gaps:** IV inversion (~218 ns vs LBR ~180 ns; SIMD lane efficiency) and streaming-fast surface fitting. Published fixes exist (FlashIV branch-light; Mingone unconstrained eSSVI; direct-conic SVI).
- **Biggest *feature* gaps → turn library into an MM system:** (1) quoting engine, (2) unified cross-underlier portfolio risk, (3) delta-hedging schedule.
- **Firm-specific architecture is largely unpublished** — the low-latency-systems angle is inferential; needs an engineering-talk research pass.

See §7 (gap table), §8 (prioritized roadmap), §9 (caveats), §10 (open questions) in the main report.
