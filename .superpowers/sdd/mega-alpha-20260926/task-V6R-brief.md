# Task V6-R: production visualization report (generator + static HTML, inline SVG, reusable components)

**Pool:** C:/atx-wt/pool-4. First `git status` clean, then `git checkout -B feat/mega-alpha-v6-report-20260928 <pool-2 HEAD>`
(`git -C C:/atx-wt/pool-2 rev-parse HEAD`). Work only in pool-4; read C:/atx-wt/pool-2/build-equity/** (outputs only: summary
JSON, nav_summ JSON/TXT, daily/events CSVs, weights/admission JSON) by absolute path. Never build, never run the pipeline,
never spawn subagents, never touch C:/atx or other pools, never read anything named validation/VAL/2023/2024/2025.
Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Report: C:/atx-wt/pool-2/.superpowers/sdd/
mega-alpha-20260926/task-V6R-report.md. Reply < 12 lines.

**Owner's words (binding):** "productionize the scorecard + the equity curves + any other visuals into a final html output
that is a high quality, production quality visualization report made of reusable components. Avoid ai slop, use clean
professional academic styling and svg figures. Make it clean and no prose - it should be dynamic to a generated output so a
change can be automatically handled."

## Deliverables
1. `atx-impl/tools/mega_report/` Python package (stdlib + numpy only; Python 3.12 at "C:/Program Files/Python312/python.exe"):
   - `components.py`: pure functions returning HTML/SVG strings, each with a docstring and a small synthetic self-test:
     `kpi_strip`, `table` (numeric formats, optional in-cell bars, sortable by a data attribute, sticky header, right-aligned
     tabular numerals), `line_chart` (multi-series, real axis scales, ticks that name reached values, year gridlines, end-point
     labels), `drawdown_chart`, `bar_chart` (grouped, signed), `waterfall` (signed steps with running total), `scatter`
     (labelled points, optional size), `dot_plot` (signed value with zero line, e.g. HAC t by candidate), `heatmap` (months x
     years), `gate_panel` (name, value, threshold, pass/fail chip), `figure` (numbered figure with caption), `section`.
     All SVG: explicit viewBox with room for outer labels, every shape has an explicit fill/stroke, text from theme tokens
     via CSS classes (no hard-coded colours inside SVG except via `var(--...)` on `fill`/`stroke`).
   - `data.py`: loaders for a nav cell dir (summary.json, daily_*.csv, events_*.csv), nav_summ `--json` files, weights /
     admission JSON, the library JSON; a `Cell` dataclass; derived series (NAV, drawdown, monthly and yearly returns from
     `net_return`, rolling 63-session Sharpe, cumulative cost drag = gross NAV minus net NAV).
   - `report.py`: `build(config_path) -> html` and CLI `python -m atx_impl_tools.mega_report --config X --out Y` (or an
     equivalent runnable path). It must not contain any number from this sprint: every value comes from the config or the
     files it names. Missing inputs render as an explicit "not available" cell, never a crash, never a guess.
2. `docs/plans/mega-alpha-v6-report.config.json`: the declarative input: root path, cells (dir, short label, group:
   final / parent / grid / conditional / rejected / v5-reference / v5-grid, parent dir for paired dSR, exe tag), scenario
   ids and display names, primary scenario, gate thresholds (gross [.90,1.05], |net| .02, tau .20/.30, net SR 1.0, DSR .95),
   nav_summ n28 JSON path, weights/admission/library paths, TRAIN window, title. Regenerating after any new cell = edit
   config, rerun.
3. `docs/plans/2026-09-28-mega-alpha-v6-report.html`: the generated, self-contained output (inline CSS + SVG; Google Fonts
   link with real fallback stacks; no JS required for reading; small optional JS for table sort and a theme toggle only).
4. `docs/plans/mega-alpha-v6-report.README.md`: 15 lines max: how to regenerate, config keys, component list.

## Content (no prose: titles, captions, labels, tables, figures only; captions state what is plotted and the data source)
- Header: title, TRAIN window, primary scenario, generated-at, config SHA, git HEAD, the list of input file SHAs (collapsed).
- KPI strip (final cell): S2 net SR, gross SR, HAC t, gross lev all rows, |net|, tau mean/p95, cost bps/$, MDD, DSR x-cell / Lo.
- Gate panel: the five pre-registered conditions with value, threshold, PASS/FAIL chips.
- Fig 1: NAV equity curves, final cell under every scenario (S2 emphasised) + its L 1 parent + v5 deployable book, with
  drawdown panel below; Fig 2: yearly and monthly net returns (heatmap months x years, final cell S2); Fig 3: rolling
  63-session Sharpe (final vs v5 deployable); Fig 4: cumulative cost drag (gross minus net NAV) per scenario; Fig 5: lever
  ladder waterfall from v5 REF to final (each accepted step's dSR vs its parent, with SE whiskers); Fig 6: all 28 cells,
  scatter tau mean vs cost bps/$, point colour = group, size = S2 net SR, final cell labelled; Fig 7: dot plot of the 38
  library-v6 candidates' TRAIN HAC t on the restricted role, grouped by theme, prior sign shown, admitted vs rejected;
  Fig 8: theme weights (ew-theme-v1 on the final weights) as a horizontal bar chart with member counts; Fig 9: per-cell
  daily turnover distribution (small multiples or box-style bars) for the 15 v6 cells.
- Tables: T1 all 28 cells (the scorecard's columns: net, gross SR, HAC t, mu, vol, MDD, per-year, gross/net lev all rows,
  tau, cost bps, dSR vs parent (SE), DSR x-cell, DSR Lo, verdict, exe); T2 stresses for the final cell (S1/S2/S3/flat-300/
  engine-tiers); T3 alpha table (id, theme, prior sign, change vs v5.1, status, HAC t, tau, weight); T4 DSL strings verbatim
  in a monospace block per theme with the citation from the generator; T5 recipe pins (SHAs, flags, exe tags); T6 Appendix A
  trial accounting (from config, with the source line quoted).

## Design (binding; owner asked for clean professional academic styling, no AI slop)
- Tokens in bare `:root` (complete light palette), redefined under `@media (prefers-color-scheme: dark)` guarded
  `:root:not([data-theme="light"])` and under `:root[data-theme="dark"]`, with `color-scheme: dark` there; `body` sets an
  explicit background from a token. Neutrals with a slight cool (blue-grey) bias; one accent (deep navy, e.g. #1F3A5F light /
  #8FB3E6 dark); scenario colours fixed and legible in both themes: S2 navy, S1 slate teal, S3 oxblood, flat-300 amber-brown,
  engine-tiers grey-violet; v5 reference lines in neutral grey. Semantic pass/fail green/red distinct from the accent.
- Type: Source Serif 4 (headings, captions) + IBM Plex Sans (tables, labels) + IBM Plex Mono (numbers in KPI/pins, DSL),
  all from Google Fonts with fallbacks; `font-variant-numeric: tabular-nums` on every numeric cell; uppercase labels with
  slight letter-spacing; one type scale.
- Layout: single column, max 1120 px, 24 px gutters, section numbering that IS the report's structure (1 Summary, 2 Equity,
  3 Construction, 4 Alphas, 5 Reproducibility, 6 Trial accounting), numbered figures/tables with captions above tables and
  below figures (academic convention), thin hairline rules, no cards/shadows/rounded boxes except the pass/fail chips,
  nothing centred except figure captions, no emoji, no gradients, no hero. Phone width: tables scroll in their own
  `overflow-x:auto` container; figures scale with `max-width:100%`.
- Charts to scale: one scale per axis, ticks at values the data reaches, year gridlines from real dates, end-point labels,
  legends as text rows not icons. `prefers-reduced-motion` respected (no animation anyway).

## Verification
- Run the generator against pool-2's outputs; the HTML must open with zero console errors (check by parsing with
  `html.parser` and by validating every `<svg>` with `xml.etree`); every number in the HTML must be traceable to a file
  (write a small test that regenerates with a mutated config, e.g. one cell removed, and asserts the output changes
  accordingly and nothing crashes); component self-tests pass; `python -m pytest` on your tests.
- Report: file list with sizes, the exact regenerate command, screenshot not possible (say so), and any input the report
  could not fill.
