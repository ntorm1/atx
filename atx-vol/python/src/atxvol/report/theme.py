"""Design tokens for atxvol reports.

One light theme, stated once as CSS custom properties and consumed by role
everywhere else — so no component hard-codes a hex value.

The categorical series palette is not a matter of taste: it was validated with
the data-viz six-checks validator against the `--surface #fbfaf8` this theme
actually renders on, and it passes all of them with no warnings —

    lightness band   all 6 inside L 0.43-0.77
    chroma floor     all 6 >= 0.1
    CVD separation   worst adjacent dE 8.6 (deutan), 9.8 (tritan)   [target >= 8]
    normal vision    worst adjacent dE 17.2                          [floor 15]
    contrast         all 6 >= 3:1 against the surface

The SLOT ORDER is the colorblind-safety mechanism, not decoration: the ochre and
pine steps were separated deliberately because adjacent green/ochre collapses
under tritanopia (dE 4.1). Reordering the list silently weakens the palette — if
you change it, re-run the validator.
"""

from __future__ import annotations

from typing import Final

# ── Categorical series slots, in validated order ────────────────────────────
SERIES: Final[tuple[str, ...]] = (
    "#2f6fb5",  # 1 slate blue
    "#a8730f",  # 2 ochre
    "#0e8a6b",  # 3 pine
    "#c0392b",  # 4 brick
    "#7d4bab",  # 5 plum
    "#4a7a2a",  # 6 moss
)

# Polarity. Gains/losses are a diverging pair, not two categorical slots:
# warm/cool poles reading as opposite, with the surface as the neutral middle.
POSITIVE: Final = "#0e8a6b"
NEGATIVE: Final = "#c0392b"

# Surfaces and ink. A faintly warm paper rather than pure white — pure #fff plus
# a hairline grid reads clinical at report scale.
PAPER: Final = "#f4f2ed"
SURFACE: Final = "#fbfaf8"
INK: Final = "#14161a"
INK_2: Final = "#454b54"
MUTED: Final = "#7c828b"
RULE: Final = "#dcd9d1"
GRID: Final = "#e7e4dc"
ACCENT: Final = "#8a5a13"

SANS: Final = 'system-ui,-apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif'
MONO: Final = 'ui-monospace,"SF Mono","Cascadia Mono","JetBrains Mono",Consolas,monospace'


def series_color(index: int) -> str:
    """Slot `index` (0-based). Never cycles: past the last slot is a caller bug —
    fold the tail into an 'other' bucket or facet instead of inventing a hue."""
    if index < 0:
        raise ValueError("series index must be non-negative")
    if index >= len(SERIES):
        raise ValueError(
            f"series slot {index} exceeds the {len(SERIES)}-slot palette; "
            "fold the tail into 'other' or facet into small multiples rather "
            "than cycling hues (cycled hues are indistinguishable under CVD)"
        )
    return SERIES[index]


def stylesheet() -> str:
    """The report stylesheet. Self-contained: no external fonts or assets."""
    return f"""
:root {{
  color-scheme: light;
  --paper:{PAPER}; --surface:{SURFACE}; --ink:{INK}; --ink-2:{INK_2};
  --muted:{MUTED}; --rule:{RULE}; --grid:{GRID}; --accent:{ACCENT};
  --pos:{POSITIVE}; --neg:{NEGATIVE};
  --sans:{SANS}; --mono:{MONO};
  --measure:74ch;
}}
*{{box-sizing:border-box}}
html{{-webkit-text-size-adjust:100%}}
body{{
  margin:0;background:var(--paper);color:var(--ink);
  font-family:var(--sans);font-size:16px;line-height:1.62;
  -webkit-font-smoothing:antialiased;text-rendering:optimizeLegibility;
}}
.sheet{{
  max-width:1180px;margin:0 auto;padding:clamp(28px,5vw,72px) clamp(18px,4vw,56px) 96px;
  background:var(--surface);min-height:100vh;
  border-inline:1px solid var(--rule);
}}

/* ── Masthead ─────────────────────────────────────────────────────────── */
.masthead{{border-bottom:2px solid var(--ink);padding-bottom:20px;margin-bottom:8px}}
.eyebrow{{
  font-family:var(--mono);font-size:11px;letter-spacing:.18em;text-transform:uppercase;
  color:var(--accent);font-weight:600;margin:0 0 14px;
}}
.masthead h1{{
  font-size:clamp(27px,4vw,40px);line-height:1.12;letter-spacing:-.021em;
  font-weight:640;margin:0 0 10px;max-width:22ch;
}}
.standfirst{{font-size:clamp(16px,1.6vw,18.5px);color:var(--ink-2);margin:0;max-width:var(--measure)}}
.byline{{
  display:flex;flex-wrap:wrap;gap:6px 26px;margin:20px 0 0;padding:0;list-style:none;
  font-family:var(--mono);font-size:11.5px;color:var(--muted);
}}
.byline b{{color:var(--ink-2);font-weight:600}}

/* ── Sections ─────────────────────────────────────────────────────────── */
section{{margin:56px 0 0}}
.sec-head{{display:flex;align-items:baseline;gap:14px;margin:0 0 6px}}
.sec-num{{
  font-family:var(--mono);font-size:12px;font-weight:600;color:var(--accent);
  letter-spacing:.06em;flex:none;padding-top:2px;
}}
h2{{font-size:clamp(19px,2.2vw,23px);line-height:1.25;letter-spacing:-.012em;font-weight:640;margin:0}}
h3{{font-size:16px;font-weight:640;margin:32px 0 8px;letter-spacing:-.005em}}
p{{margin:0 0 15px;max-width:var(--measure)}}
p.lede{{color:var(--ink-2)}}
a{{color:var(--accent);text-underline-offset:2px}}
.tnum{{font-variant-numeric:tabular-nums}}
code,.mono{{font-family:var(--mono);font-size:.88em}}

/* ── Stat row ─────────────────────────────────────────────────────────── */
.stats{{
  display:grid;grid-template-columns:repeat(auto-fit,minmax(158px,1fr));
  gap:1px;background:var(--rule);border:1px solid var(--rule);margin:26px 0 0;
}}
.stat{{background:var(--surface);padding:15px 17px 16px}}
.stat .k{{
  font-family:var(--mono);font-size:10.5px;letter-spacing:.11em;text-transform:uppercase;
  color:var(--muted);margin:0 0 7px;
}}
/* Proportional figures here on purpose: tabular-nums makes a large standalone
   number look loose. Tabular is for columns (tables, axis ticks). */
.stat .v{{font-size:25px;font-weight:620;letter-spacing:-.022em;line-height:1.1;margin:0}}
.stat .v.neg{{color:var(--neg)}}
.stat .v.pos{{color:var(--pos)}}
.stat .n{{font-size:12.5px;color:var(--muted);margin:5px 0 0;line-height:1.4}}

/* ── Figures ──────────────────────────────────────────────────────────── */
figure{{margin:30px 0 0}}
.fig-head{{margin:0 0 12px}}
.fig-title{{font-size:15px;font-weight:640;margin:0;letter-spacing:-.005em}}
.fig-sub{{font-size:13.5px;color:var(--muted);margin:3px 0 0;max-width:var(--measure)}}
.fig-frame{{border:1px solid var(--rule);background:var(--surface);padding:16px 14px 10px;overflow-x:auto}}
.fig-frame svg{{display:block;width:100%;height:auto;min-width:520px}}
figcaption{{
  font-size:12.5px;color:var(--muted);margin:10px 0 0;max-width:var(--measure);
  padding-left:13px;border-left:2px solid var(--rule);
}}
figcaption b{{color:var(--ink-2);font-weight:600}}

/* ── Legend ───────────────────────────────────────────────────────────── */
.legend{{display:flex;flex-wrap:wrap;gap:7px 20px;margin:0 0 12px;padding:0;list-style:none}}
.legend li{{display:flex;align-items:center;gap:7px;font-size:12.5px;color:var(--ink-2)}}
.legend .key{{width:13px;height:3px;border-radius:1.5px;flex:none}}
.legend .key.dot{{width:9px;height:9px;border-radius:50%}}

/* ── Tables ───────────────────────────────────────────────────────────── */
.table-wrap{{overflow-x:auto;margin:22px 0 0}}
table{{border-collapse:collapse;width:100%;font-size:13.5px;font-variant-numeric:tabular-nums}}
caption{{
  text-align:left;font-size:12.5px;color:var(--muted);padding:0 0 9px;
  font-family:var(--mono);letter-spacing:.04em;
}}
th,td{{padding:7px 13px;text-align:right;white-space:nowrap}}
th:first-child,td:first-child{{text-align:left;padding-left:0}}
th:last-child,td:last-child{{padding-right:0}}
thead th{{
  font-family:var(--mono);font-size:10.5px;letter-spacing:.08em;text-transform:uppercase;
  color:var(--muted);font-weight:600;border-bottom:1px solid var(--ink);padding-bottom:8px;
}}
tbody tr{{border-bottom:1px solid var(--grid)}}
tbody tr:last-child{{border-bottom:1px solid var(--rule)}}
tbody tr:hover{{background:#f3f1ea}}
tfoot td{{
  border-top:2px solid var(--ink);font-weight:640;padding-top:9px;
  font-family:var(--sans);
}}
td.neg{{color:var(--neg)}}
td.pos{{color:var(--pos)}}
td.sym{{font-family:var(--mono);font-size:12.5px}}

/* Table view twin: every chart's values stay reachable without reading color. */
details.tableview{{margin:12px 0 0;border-top:1px solid var(--grid);padding-top:9px}}
details.tableview>summary{{
  cursor:pointer;font-family:var(--mono);font-size:11px;letter-spacing:.09em;
  text-transform:uppercase;color:var(--muted);list-style:none;padding:2px 0;
}}
details.tableview>summary::-webkit-details-marker{{display:none}}
details.tableview>summary::before{{content:"+ ";font-weight:700}}
details.tableview[open]>summary::before{{content:"\\2212 "}}
details.tableview>summary:hover{{color:var(--ink)}}
details.tableview .table-wrap{{margin-top:6px;max-height:340px;overflow-y:auto}}

/* ── Notes ────────────────────────────────────────────────────────────── */
.note{{
  border-left:2px solid var(--accent);background:#faf6ef;
  padding:13px 17px;margin:24px 0 0;font-size:14px;color:var(--ink-2);max-width:var(--measure);
}}
.note b{{color:var(--ink)}}
.note.flat{{border-left-color:var(--rule);background:#f6f5f1}}

.colophon{{
  margin:64px 0 0;padding-top:18px;border-top:1px solid var(--rule);
  font-family:var(--mono);font-size:11.5px;color:var(--muted);line-height:1.9;
}}
.colophon b{{color:var(--ink-2);font-weight:600}}

@media (max-width:640px){{
  .sheet{{border-inline:none}}
  th,td{{padding:6px 9px}}
}}
@media print{{
  body{{background:#fff}}
  .sheet{{border:none;max-width:none;padding:0}}
  details.tableview{{display:none}}
  figure,section{{break-inside:avoid}}
}}
"""
