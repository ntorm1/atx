"""Self-contained HTML reporting for atxvol.

A small component library — masthead, numbered sections, stat rows, tables,
figures, small-multiple grids — plus hand-rolled SVG chart primitives, over one
light theme whose series palette is validated for colorblind separation and
contrast (see `theme`). Pure stdlib: a rendered report is a single HTML file
with no external assets and no network access.

    from atxvol.report import Report, Section, Prose, Figure, Table, Column
    from atxvol.report import charts

    svg = charts.line_chart([charts.Series("NAV", nav, area=True)], dates)
    report = Report(title="Backtest", eyebrow="atx-vol")
    report.add(Section("Result", body=[Figure(svg, title="Cumulative P&L")]))
    report.write("report.html")
"""

from __future__ import annotations

from . import charts, theme
from .charts import Series, bar_chart, line_chart, small_multiple
from .components import (
    Column,
    FacetGrid,
    Figure,
    Note,
    Prose,
    Raw,
    Report,
    Section,
    Stat,
    StatRow,
    Subhead,
    Table,
)
from .parity import (
    ParityStats,
    build_parity_report,
    build_parity_report_from_archive,
    compute_parity_stats,
)

__all__ = [
    "charts", "theme",
    "Series", "line_chart", "bar_chart", "small_multiple",
    "Report", "Section", "Prose", "Subhead", "Raw", "Note",
    "Stat", "StatRow", "Table", "Column", "Figure", "FacetGrid",
    "ParityStats", "build_parity_report", "build_parity_report_from_archive",
    "compute_parity_stats",
]
