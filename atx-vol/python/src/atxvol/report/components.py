"""Composable report components.

Each component is a small object that renders to an HTML fragment; a `Report`
collects them and emits one self-contained document (styles and the hover
runtime inlined, no external assets, no network).

    from atxvol.report import Report, Section, StatRow, Stat, Figure, Table

    report = Report(title="...", eyebrow="...", standfirst="...")
    report.add(Section("Result", body="..."))
    report.write("out.html")

Composition rules worth knowing:

* `Figure` takes an optional `table` so every chart ships a table-view twin —
  values stay reachable without reading a color or landing a hover.
* `Figure` requires a legend for two or more series and omits it for one (a
  single-swatch legend just restates the title).
* Section numbering is automatic; `Figure` numbering is continuous across the
  document, so captions can be cross-referenced from prose.
"""

from __future__ import annotations

import html
from dataclasses import dataclass, field
from typing import Iterable, Sequence

from . import theme

__all__ = [
    "Report", "Section", "Stat", "StatRow", "Figure", "Table", "Column",
    "Note", "Prose", "Subhead", "Raw", "FacetGrid",
]


def esc(text: object) -> str:
    return html.escape(str(text), quote=True)


class _Node:
    """A renderable. `render(ctx)` returns an HTML fragment."""

    def render(self, ctx: "_Ctx") -> str:  # pragma: no cover - interface
        raise NotImplementedError


@dataclass
class _Ctx:
    """Document-wide counters, so numbering is continuous across components."""

    section: int = 0
    figure: int = 0
    table: int = 0

    def next_figure(self) -> int:
        self.figure += 1
        return self.figure

    def next_table(self) -> int:
        self.table += 1
        return self.table


# ── Text ────────────────────────────────────────────────────────────────────

@dataclass
class Prose(_Node):
    """One or more paragraphs. `lede=True` sets the softer opening voice."""

    text: str | Sequence[str]
    lede: bool = False

    def render(self, ctx: _Ctx) -> str:
        paras = [self.text] if isinstance(self.text, str) else list(self.text)
        cls = ' class="lede"' if self.lede else ""
        return "".join(f"<p{cls}>{p}</p>" for p in paras if p)


@dataclass
class Subhead(_Node):
    text: str

    def render(self, ctx: _Ctx) -> str:
        return f"<h3>{esc(self.text)}</h3>"


@dataclass
class Raw(_Node):
    """Escape hatch for pre-built markup."""

    html_text: str

    def render(self, ctx: _Ctx) -> str:
        return self.html_text


@dataclass
class Note(_Node):
    """A call-out. `flat=True` drops the accent rule for a neutral aside."""

    text: str
    flat: bool = False

    def render(self, ctx: _Ctx) -> str:
        return f'<div class="note{" flat" if self.flat else ""}">{self.text}</div>'


# ── Stats ───────────────────────────────────────────────────────────────────

@dataclass
class Stat:
    label: str
    value: str
    note: str = ""
    tone: str = ""  # "", "pos", "neg"


@dataclass
class StatRow(_Node):
    """A row of figures. The number *is* the chart — no one-bar bar charts."""

    stats: Sequence[Stat]

    def render(self, ctx: _Ctx) -> str:
        cells = []
        for s in self.stats:
            tone = f" {s.tone}" if s.tone in ("pos", "neg") else ""
            note = f'<p class="n">{esc(s.note)}</p>' if s.note else ""
            cells.append(
                f'<div class="stat"><p class="k">{esc(s.label)}</p>'
                f'<p class="v{tone}">{esc(s.value)}</p>{note}</div>'
            )
        return f'<div class="stats">{"".join(cells)}</div>'


# ── Tables ──────────────────────────────────────────────────────────────────

@dataclass
class Column:
    """A table column. `tone` picks a per-cell class from the value's sign when
    set to "sign"; `mono` renders the cell in the monospace face."""

    header: str
    tone: str = ""
    mono: bool = False


@dataclass
class Table(_Node):
    columns: Sequence[Column | str]
    rows: Sequence[Sequence[object]]
    caption: str = ""
    footer: Sequence[object] | None = None
    numbered: bool = True

    def _cols(self) -> list[Column]:
        return [c if isinstance(c, Column) else Column(str(c)) for c in self.columns]

    def render(self, ctx: _Ctx, *, bare: bool = False) -> str:
        cols = self._cols()
        head = "".join(f"<th>{esc(c.header)}</th>" for c in cols)
        body = []
        for row in self.rows:
            cells = []
            for value, col in zip(row, cols):
                classes = []
                if col.mono:
                    classes.append("sym")
                if col.tone == "sign":
                    try:
                        num = float(str(value).replace(",", "").replace("$", ""))
                        classes.append("neg" if num < 0 else "pos" if num > 0 else "")
                    except ValueError:
                        pass
                elif col.tone:
                    classes.append(col.tone)
                cls = f' class="{" ".join(c for c in classes if c)}"' if any(classes) else ""
                cells.append(f"<td{cls}>{esc(value)}</td>")
            body.append(f"<tr>{''.join(cells)}</tr>")
        foot = ""
        if self.footer:
            foot = "<tfoot><tr>" + "".join(f"<td>{esc(v)}</td>" for v in self.footer) + "</tr></tfoot>"
        cap = ""
        if self.caption and not bare:
            label = f"Table {ctx.next_table()}. " if self.numbered else ""
            cap = f"<caption>{esc(label)}{esc(self.caption)}</caption>"
        elif self.caption:
            cap = f"<caption>{esc(self.caption)}</caption>"
        return (
            f'<div class="table-wrap"><table>{cap}<thead><tr>{head}</tr></thead>'
            f"<tbody>{''.join(body)}</tbody>{foot}</table></div>"
        )


# ── Figures ─────────────────────────────────────────────────────────────────

@dataclass
class Figure(_Node):
    """A chart with its title, legend, caption, and table-view twin.

    `legend` is a sequence of (label, color) pairs, or (label, color, "dot").
    Pass it whenever the chart carries two or more series; omit it for one.
    """

    svg: str
    title: str = ""
    subtitle: str = ""
    caption: str = ""
    legend: Sequence[tuple] = ()
    table: Table | None = None
    table_label: str = "Show underlying values"

    def render(self, ctx: _Ctx) -> str:
        num = ctx.next_figure()
        head = ""
        if self.title or self.subtitle:
            sub = f'<p class="fig-sub">{self.subtitle}</p>' if self.subtitle else ""
            head = (
                f'<div class="fig-head"><p class="fig-title">Figure {num}. '
                f"{esc(self.title)}</p>{sub}</div>"
            )
        legend = ""
        if len(self.legend) >= 2:
            items = []
            for entry in self.legend:
                label, color = entry[0], entry[1]
                shape = entry[2] if len(entry) > 2 else "line"
                cls = "key dot" if shape == "dot" else "key"
                items.append(
                    f'<li><span class="{cls}" style="background:{esc(color)}"></span>'
                    f"{esc(label)}</li>"
                )
            legend = f'<ul class="legend">{"".join(items)}</ul>'
        caption = ""
        if self.caption:
            caption = f"<figcaption>{self.caption}</figcaption>"
        table_view = ""
        if self.table is not None:
            table_view = (
                f'<details class="tableview"><summary>{esc(self.table_label)}</summary>'
                f"{self.table.render(ctx, bare=True)}</details>"
            )
        return (
            f"<figure>{head}{legend}"
            f'<div class="fig-frame">{self.svg}</div>{caption}{table_view}</figure>'
        )


@dataclass
class FacetGrid(_Node):
    """Small multiples: a titled grid of single-series panels.

    The right answer when several measures share an x-axis but not a y-scale —
    the alternative, a second y-axis on one plot, invents a correlation.
    """

    panels: Sequence[tuple[str, str, str]]  # (title, svg, note)
    columns: int = 2
    caption: str = ""
    title: str = ""

    def render(self, ctx: _Ctx) -> str:
        num = ctx.next_figure()
        head = ""
        if self.title:
            head = (
                f'<div class="fig-head"><p class="fig-title">Figure {num}. '
                f"{esc(self.title)}</p></div>"
            )
        cells = []
        for title, svg, note in self.panels:
            sub = f'<p class="facet-note">{esc(note)}</p>' if note else ""
            cells.append(
                f'<div class="facet"><p class="facet-title">{esc(title)}</p>{sub}'
                f'<div class="fig-frame">{svg}</div></div>'
            )
        caption = f"<figcaption>{self.caption}</figcaption>" if self.caption else ""
        style = (
            "<style>"
            ".facets{display:grid;gap:20px 22px;margin:0}"
            f"@media(min-width:760px){{.facets{{grid-template-columns:repeat({self.columns},1fr)}}}}"
            ".facet .fig-frame{padding:12px 10px 6px}"
            ".facet .fig-frame svg{min-width:0}"
            ".facet-title{font-size:13px;font-weight:640;margin:0 0 2px}"
            ".facet-note{font-size:12px;color:var(--muted);margin:0 0 8px}"
            "</style>"
        )
        return f'<figure>{style}{head}<div class="facets">{"".join(cells)}</div>{caption}</figure>'


# ── Sections & document ─────────────────────────────────────────────────────

@dataclass
class Section(_Node):
    title: str
    body: Sequence[_Node] = field(default_factory=list)
    lede: str = ""
    numbered: bool = True

    def render(self, ctx: _Ctx) -> str:
        num = ""
        if self.numbered:
            ctx.section += 1
            num = f'<span class="sec-num">{ctx.section:02d}</span>'
        lede = f'<p class="lede">{self.lede}</p>' if self.lede else ""
        inner = "".join(node.render(ctx) for node in self.body)
        return (
            f'<section><div class="sec-head">{num}<h2>{esc(self.title)}</h2></div>'
            f"{lede}{inner}</section>"
        )


# Hover runtime: a crosshair on line charts. Values are also in the table view,
# so this enhances and never gates.
_RUNTIME = """
document.querySelectorAll('svg[data-chart]').forEach(function(svg){
  var bands=svg.querySelector('.hoverbands'), rule=svg.querySelector('.crosshair');
  if(!bands||!rule) return;
  bands.querySelectorAll('rect').forEach(function(r){
    r.addEventListener('mouseenter',function(){
      var x=parseFloat(r.getAttribute('data-x'));
      rule.setAttribute('x1',x); rule.setAttribute('x2',x);
      rule.setAttribute('stroke-opacity','0.28');
    });
  });
  svg.addEventListener('mouseleave',function(){rule.setAttribute('stroke-opacity','0');});
});
"""


@dataclass
class Report:
    """The document. Add components in order, then `render()` or `write()`."""

    title: str
    eyebrow: str = ""
    standfirst: str = ""
    meta: Sequence[tuple[str, str]] = ()
    colophon: Sequence[str] = ()
    body: list[_Node] = field(default_factory=list)

    def add(self, *nodes: _Node) -> "Report":
        self.body.extend(nodes)
        return self

    def render(self) -> str:
        ctx = _Ctx()
        eyebrow = f'<p class="eyebrow">{esc(self.eyebrow)}</p>' if self.eyebrow else ""
        stand = f'<p class="standfirst">{self.standfirst}</p>' if self.standfirst else ""
        meta = ""
        if self.meta:
            items = "".join(f"<li><b>{esc(k)}</b> {esc(v)}</li>" for k, v in self.meta)
            meta = f'<ul class="byline">{items}</ul>'
        content = "".join(node.render(ctx) for node in self.body)
        colophon = ""
        if self.colophon:
            colophon = f'<div class="colophon">{"<br>".join(self.colophon)}</div>'
        return (
            "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            '<meta name="viewport" content="width=device-width,initial-scale=1">'
            f"<title>{esc(self.title)}</title>"
            f"<style>{theme.stylesheet()}</style></head><body>"
            f'<div class="sheet"><header class="masthead">{eyebrow}'
            f"<h1>{esc(self.title)}</h1>{stand}{meta}</header>"
            f"{content}{colophon}</div>"
            f"<script>{_RUNTIME}</script></body></html>"
        )

    def write(self, path: str) -> str:
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(self.render())
        return path
