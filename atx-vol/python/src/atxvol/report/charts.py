"""Hand-rolled SVG chart primitives.

No matplotlib, no JS charting library, no network — every figure is inline SVG
built from stdlib string formatting, so a report is one self-contained file.

The mark specs are fixed (and deliberate):

* lines are 2px with round joins; area fills are the series hue at ~10% opacity
* markers are >= 8px diameter and carry a 2px ring in the surface color, so they
  stay legible where they cross a line
* gridlines and axes are solid 1px hairlines one step off the surface — never
  dashed, which reads as "threshold" when it is just a grid
* bars cap at 24px thick with a 4px rounded data-end and a square baseline end
* labels are placed selectively (endpoint / extremum), never on every point
* axis ticks and table figures are tabular; large standalone values are not

Every chart is paired with a table view by the components layer, so no value is
reachable only by reading a color or hovering a tooltip.
"""

from __future__ import annotations

import html
import math
from dataclasses import dataclass, field
from typing import Sequence

from . import theme

__all__ = [
    "Series",
    "line_chart",
    "bar_chart",
    "small_multiple",
    "scatter_chart",
    "paired_bar_chart",
]


@dataclass
class Series:
    """One plotted series. `color` defaults to the next validated palette slot."""

    name: str
    values: Sequence[float]
    color: str = ""
    area: bool = False
    label_end: bool = False


@dataclass
class _Box:
    width: float
    height: float
    top: float = 18.0
    right: float = 62.0
    bottom: float = 34.0
    left: float = 66.0

    @property
    def plot_w(self) -> float:
        return self.width - self.left - self.right

    @property
    def plot_h(self) -> float:
        return self.height - self.top - self.bottom


def _esc(text: object) -> str:
    return html.escape(str(text), quote=True)


def _fmt(value: float, unit: str = "$") -> str:
    """Compact axis/label money. 1_284 -> $1.3k, -1_116.87 -> -$1.1k."""
    if value != value or value in (float("inf"), float("-inf")):
        return "--"
    sign = "-" if value < 0 else ""
    v = abs(value)
    if v >= 1_000_000:
        body = f"{v / 1_000_000:.1f}M".replace(".0M", "M")
    elif v >= 1_000:
        body = f"{v / 1_000:.1f}k".replace(".0k", "k")
    elif v >= 10:
        body = f"{v:,.0f}"
    elif v >= 1:
        body = f"{v:.1f}"
    elif v == 0:
        return f"{unit}0" if unit else "0"
    else:
        body = f"{v:.2f}"
    return f"{sign}{unit}{body}"


def nice_ticks(lo: float, hi: float, target: int = 5) -> list[float]:
    """Round tick values (1/2/2.5/5 x 10^n) spanning [lo, hi], zero included when
    the range straddles it — a P&L axis that omits zero misstates the story."""
    if not math.isfinite(lo) or not math.isfinite(hi):
        return [0.0]
    if hi - lo < 1e-12:
        pad = max(abs(hi) * 0.1, 1.0)
        lo, hi = lo - pad, hi + pad
    raw = (hi - lo) / max(target, 1)
    mag = 10.0 ** math.floor(math.log10(raw)) if raw > 0 else 1.0
    for mult in (1.0, 2.0, 2.5, 5.0, 10.0):
        if raw / mag <= mult:
            step = mult * mag
            break
    else:
        step = 10.0 * mag
    start = math.floor(lo / step) * step
    ticks: list[float] = []
    v = start
    while v <= hi + step * 0.5 and len(ticks) < 40:
        ticks.append(0.0 if abs(v) < step * 1e-9 else v)
        v += step
    return ticks


def _x_tick_indices(n: int, target: int = 6) -> list[int]:
    """Evenly spaced tick positions, always including the last sample.

    Appending the final index can leave it a sample or two from the previous
    tick, which renders as overprinted labels ("04-2830") at the right edge — so
    drop the penultimate tick when the append crowds it.
    """
    if n <= 1:
        return [0] if n else []
    if n <= target:
        return list(range(n))
    stride = max(1, round((n - 1) / (target - 1)))
    idx = list(range(0, n, stride))
    if idx[-1] != n - 1:
        if len(idx) >= 2 and (n - 1 - idx[-1]) < stride * 0.6:
            idx.pop()
        idx.append(n - 1)
    return idx


def _grid_and_axes(box: _Box, ticks: Sequence[float], y_of, unit: str) -> list[str]:
    out: list[str] = []
    for t in ticks:
        y = y_of(t)
        zero = abs(t) < 1e-12
        out.append(
            f'<line x1="{box.left:.1f}" y1="{y:.2f}" x2="{box.left + box.plot_w:.1f}" '
            f'y2="{y:.2f}" stroke="{theme.RULE if zero else theme.GRID}" stroke-width="1" '
            'shape-rendering="crispEdges"/>'
        )
        out.append(
            f'<text x="{box.left - 11:.1f}" y="{y + 3.6:.2f}" text-anchor="end" '
            f'font-family="{theme.MONO}" font-size="10.5" fill="{theme.MUTED}" '
            f'style="font-variant-numeric:tabular-nums">{_esc(_fmt(t, unit))}</text>'
        )
    return out


def _x_axis(box: _Box, labels: Sequence[str], x_of) -> list[str]:
    out = [
        f'<line x1="{box.left:.1f}" y1="{box.top + box.plot_h:.1f}" '
        f'x2="{box.left + box.plot_w:.1f}" y2="{box.top + box.plot_h:.1f}" '
        f'stroke="{theme.RULE}" stroke-width="1" shape-rendering="crispEdges"/>'
    ]
    for i in _x_tick_indices(len(labels)):
        out.append(
            f'<text x="{x_of(i):.2f}" y="{box.top + box.plot_h + 17:.1f}" text-anchor="middle" '
            f'font-family="{theme.MONO}" font-size="10.5" fill="{theme.MUTED}" '
            f'style="font-variant-numeric:tabular-nums">{_esc(labels[i])}</text>'
        )
    return out


def line_chart(
    series: Sequence[Series],
    x_labels: Sequence[str],
    *,
    width: float = 1000.0,
    height: float = 340.0,
    unit: str = "$",
    zero_rule: bool = True,
    chart_id: str = "c",
) -> str:
    """Multi-series line chart with an optional area wash and endpoint labels.

    A hover crosshair is attached by the report's small runtime; the values also
    live in the table view, so the tooltip enhances and never gates.
    """
    if not series or not x_labels:
        return ""
    n = len(x_labels)
    box = _Box(width, height)

    flat = [v for s in series for v in s.values if math.isfinite(v)]
    lo, hi = (min(flat), max(flat)) if flat else (0.0, 1.0)
    if zero_rule:
        lo, hi = min(lo, 0.0), max(hi, 0.0)
    ticks = nice_ticks(lo, hi)
    axis_lo, axis_hi = min(ticks + [lo]), max(ticks + [hi])
    span = axis_hi - axis_lo or 1.0

    def x_of(i: int) -> float:
        return box.left + (box.plot_w * i / (n - 1) if n > 1 else box.plot_w / 2)

    def y_of(v: float) -> float:
        return box.top + box.plot_h * (1.0 - (v - axis_lo) / span)

    parts = [
        f'<svg viewBox="0 0 {width:.0f} {height:.0f}" role="img" '
        f'preserveAspectRatio="xMidYMid meet" data-chart="{_esc(chart_id)}">'
    ]
    parts += _grid_and_axes(box, ticks, y_of, unit)
    parts += _x_axis(box, x_labels, x_of)

    for slot, s in enumerate(series):
        color = s.color or theme.series_color(slot)
        pts = [(x_of(i), y_of(v)) for i, v in enumerate(s.values) if math.isfinite(v)]
        if not pts:
            continue
        if s.area:
            base = y_of(max(axis_lo, min(0.0, axis_hi)) if zero_rule else axis_lo)
            poly = " ".join(f"{x:.2f},{y:.2f}" for x, y in pts)
            parts.append(
                f'<polygon points="{pts[0][0]:.2f},{base:.2f} {poly} '
                f'{pts[-1][0]:.2f},{base:.2f}" fill="{color}" fill-opacity="0.10"/>'
            )
        parts.append(
            '<polyline fill="none" stroke="{c}" stroke-width="2" stroke-linejoin="round" '
            'stroke-linecap="round" points="{p}"/>'.format(
                c=color, p=" ".join(f"{x:.2f},{y:.2f}" for x, y in pts)
            )
        )
        if s.label_end:
            ex, ey = pts[-1]
            # End marker: >= 8px across, with a 2px surface ring so it stays
            # readable where it lands on the line or another series.
            parts.append(
                f'<circle cx="{ex:.2f}" cy="{ey:.2f}" r="4.5" fill="{color}" '
                f'stroke="{theme.SURFACE}" stroke-width="2"/>'
            )
            parts.append(
                f'<text x="{ex + 10:.2f}" y="{ey + 4:.2f}" font-family="{theme.SANS}" '
                f'font-size="12.5" font-weight="640" fill="{theme.INK}">'
                f"{_esc(_fmt(s.values[-1], unit))}</text>"
            )

    # Hover layer: one full-height band per index, wider than any mark.
    band = box.plot_w / max(n - 1, 1)
    parts.append(f'<g class="hoverbands" data-n="{n}">')
    for i in range(n):
        cx = x_of(i)
        rows = "&#10;".join(
            f"{s.name}: {_fmt(s.values[i], unit)}"
            for s in series
            if i < len(s.values) and math.isfinite(s.values[i])
        )
        parts.append(
            f'<rect x="{max(box.left, cx - band / 2):.2f}" y="{box.top:.1f}" '
            f'width="{band:.2f}" height="{box.plot_h:.1f}" fill="transparent" '
            f'data-x="{cx:.2f}" data-i="{i}">'
            f"<title>{_esc(x_labels[i])}&#10;{rows}</title></rect>"
        )
    parts.append("</g>")
    parts.append(
        f'<line class="crosshair" x1="0" y1="{box.top:.1f}" x2="0" '
        f'y2="{box.top + box.plot_h:.1f}" stroke="{theme.INK}" stroke-width="1" '
        'stroke-opacity="0" shape-rendering="crispEdges"/>'
    )
    parts.append("</svg>")
    return "".join(parts)


def bar_chart(
    values: Sequence[float],
    x_labels: Sequence[str],
    *,
    width: float = 1000.0,
    height: float = 260.0,
    unit: str = "$",
    positive: str = theme.POSITIVE,
    negative: str = theme.NEGATIVE,
    chart_id: str = "b",
) -> str:
    """Diverging column chart about a zero baseline (gains vs losses).

    Bars cap at 24px and carry a 4px rounded data-end with a square baseline end;
    adjacent bars are separated by a 2px surface gap rather than a stroke.
    """
    if not values:
        return ""
    n = len(values)
    box = _Box(width, height, bottom=34.0)
    finite = [v for v in values if math.isfinite(v)]
    lo, hi = min(finite + [0.0]), max(finite + [0.0])
    ticks = nice_ticks(lo, hi, target=4)
    axis_lo, axis_hi = min(ticks + [lo]), max(ticks + [hi])
    span = axis_hi - axis_lo or 1.0

    def y_of(v: float) -> float:
        return box.top + box.plot_h * (1.0 - (v - axis_lo) / span)

    slot = box.plot_w / n
    bw = min(24.0, max(2.0, slot - 2.0))  # 2px surface gap between neighbours
    zero_y = y_of(0.0)

    parts = [
        f'<svg viewBox="0 0 {width:.0f} {height:.0f}" role="img" '
        f'preserveAspectRatio="xMidYMid meet" data-chart="{_esc(chart_id)}">'
    ]
    parts += _grid_and_axes(box, ticks, y_of, unit)

    def x_of(i: int) -> float:
        return box.left + slot * (i + 0.5)

    parts += _x_axis(box, x_labels, x_of)

    r = 4.0
    for i, v in enumerate(values):
        if not math.isfinite(v):
            continue
        color = positive if v >= 0 else negative
        x = x_of(i) - bw / 2
        y_val = y_of(v)
        h = abs(y_val - zero_y)
        if h < 0.6:  # keep a hairline presence for ~zero days
            parts.append(
                f'<rect x="{x:.2f}" y="{zero_y - 0.5:.2f}" width="{bw:.2f}" height="1" '
                f'fill="{color}"><title>{_esc(x_labels[i])}: {_esc(_fmt(v, unit))}</title></rect>'
            )
            continue
        rad = min(r, h, bw / 2)
        top = min(y_val, zero_y)
        # Rounded at the data end, square where it meets the baseline.
        if v >= 0:
            d = (f"M{x:.2f},{zero_y:.2f} V{top + rad:.2f} Q{x:.2f},{top:.2f} {x + rad:.2f},{top:.2f} "
                 f"H{x + bw - rad:.2f} Q{x + bw:.2f},{top:.2f} {x + bw:.2f},{top + rad:.2f} "
                 f"V{zero_y:.2f} Z")
        else:
            bot = zero_y + h
            d = (f"M{x:.2f},{zero_y:.2f} V{bot - rad:.2f} Q{x:.2f},{bot:.2f} {x + rad:.2f},{bot:.2f} "
                 f"H{x + bw - rad:.2f} Q{x + bw:.2f},{bot:.2f} {x + bw:.2f},{bot - rad:.2f} "
                 f"V{zero_y:.2f} Z")
        parts.append(
            f'<path d="{d}" fill="{color}">'
            f"<title>{_esc(x_labels[i])}: {_esc(_fmt(v, unit))}</title></path>"
        )

    parts.append(
        f'<line x1="{box.left:.1f}" y1="{zero_y:.2f}" x2="{box.left + box.plot_w:.1f}" '
        f'y2="{zero_y:.2f}" stroke="{theme.RULE}" stroke-width="1" shape-rendering="crispEdges"/>'
    )
    parts.append("</svg>")
    return "".join(parts)


def scatter_chart(
    xs: Sequence[float],
    ys: Sequence[float],
    *,
    x_title: str,
    y_title: str,
    point_labels: Sequence[str] = (),
    width: float = 520.0,
    height: float = 460.0,
    unit: str = "$",
    color: str = "",
    fit: tuple[float, float] | None = None,
    chart_id: str = "sc",
) -> str:
    """Agreement scatter with a square 1:1 frame.

    Both axes share one scale and the y=x diagonal is drawn, because the question
    a two-method scatter answers is "do these agree", not "how do these covary" —
    on independent scales a 30% bias still looks like a tight fit. `fit` draws the
    OLS line (slope, intercept) over the diagonal so the departure is visible.
    """
    if not xs or not ys or len(xs) != len(ys):
        return ""
    color = color or theme.series_color(0)
    box = _Box(width, height, top=18.0, right=22.0, bottom=44.0, left=66.0)

    pairs = [(x, y) for x, y in zip(xs, ys) if math.isfinite(x) and math.isfinite(y)]
    if not pairs:
        return ""
    flat = [v for pair in pairs for v in pair]
    ticks = nice_ticks(min(flat + [0.0]), max(flat + [0.0]), target=5)
    # One shared scale on both axes: the diagonal must be a true 45 degrees.
    axis_lo, axis_hi = min(ticks), max(ticks)
    span = axis_hi - axis_lo or 1.0

    def x_of(v: float) -> float:
        return box.left + box.plot_w * (v - axis_lo) / span

    def y_of(v: float) -> float:
        return box.top + box.plot_h * (1.0 - (v - axis_lo) / span)

    parts = [
        f'<svg viewBox="0 0 {width:.0f} {height:.0f}" role="img" '
        f'preserveAspectRatio="xMidYMid meet" data-chart="{_esc(chart_id)}">'
    ]
    parts += _grid_and_axes(box, ticks, y_of, unit)
    for t in ticks:
        x = x_of(t)
        parts.append(
            f'<line x1="{x:.2f}" y1="{box.top:.1f}" x2="{x:.2f}" '
            f'y2="{box.top + box.plot_h:.1f}" stroke="{theme.GRID}" stroke-width="1" '
            'shape-rendering="crispEdges"/>'
        )
        parts.append(
            f'<text x="{x:.2f}" y="{box.top + box.plot_h + 17:.1f}" text-anchor="middle" '
            f'font-family="{theme.MONO}" font-size="10.5" fill="{theme.MUTED}" '
            f'style="font-variant-numeric:tabular-nums">{_esc(_fmt(t, unit))}</text>'
        )

    # y = x reference. Dashed here is correct and not a grid: it is a claim
    # ("perfect agreement"), which is exactly what a dashed rule should mean.
    parts.append(
        f'<line x1="{x_of(axis_lo):.2f}" y1="{y_of(axis_lo):.2f}" '
        f'x2="{x_of(axis_hi):.2f}" y2="{y_of(axis_hi):.2f}" stroke="{theme.MUTED}" '
        'stroke-width="1.25" stroke-dasharray="5 4"/>'
    )
    parts.append(
        f'<text x="{x_of(axis_hi) - 6:.2f}" y="{y_of(axis_hi) + 16:.2f}" text-anchor="end" '
        f'font-family="{theme.MONO}" font-size="10.5" fill="{theme.MUTED}">y = x</text>'
    )

    if fit is not None:
        slope, intercept = fit
        if math.isfinite(slope) and math.isfinite(intercept):
            parts.append(
                f'<line x1="{x_of(axis_lo):.2f}" y1="{y_of(slope * axis_lo + intercept):.2f}" '
                f'x2="{x_of(axis_hi):.2f}" y2="{y_of(slope * axis_hi + intercept):.2f}" '
                f'stroke="{theme.ACCENT}" stroke-width="2" stroke-opacity="0.9"/>'
            )

    for i, (x, y) in enumerate(pairs):
        label = point_labels[i] if i < len(point_labels) else f"#{i}"
        parts.append(
            f'<circle cx="{x_of(x):.2f}" cy="{y_of(y):.2f}" r="4" fill="{color}" '
            f'fill-opacity="0.62" stroke="{theme.SURFACE}" stroke-width="1.5">'
            f"<title>{_esc(label)}&#10;{_esc(x_title)}: {_esc(_fmt(x, unit))}&#10;"
            f"{_esc(y_title)}: {_esc(_fmt(y, unit))}</title></circle>"
        )

    parts.append(
        f'<text x="{box.left + box.plot_w / 2:.1f}" y="{height - 6:.1f}" text-anchor="middle" '
        f'font-family="{theme.SANS}" font-size="11.5" fill="{theme.INK_2}">'
        f"{_esc(x_title)}</text>"
    )
    parts.append(
        f'<text transform="translate(15,{box.top + box.plot_h / 2:.1f}) rotate(-90)" '
        f'text-anchor="middle" font-family="{theme.SANS}" font-size="11.5" '
        f'fill="{theme.INK_2}">{_esc(y_title)}</text>'
    )
    parts.append("</svg>")
    return "".join(parts)


def paired_bar_chart(
    categories: Sequence[str],
    series: Sequence[Series],
    *,
    width: float = 1000.0,
    height: float = 300.0,
    unit: str = "$",
    chart_id: str = "pb",
) -> str:
    """Grouped columns: one cluster per category, one bar per series.

    Series keep their palette slot across clusters — color follows the method,
    never the rank within a cluster, so a category where the order flips does not
    silently repaint.
    """
    if not categories or not series:
        return ""
    box = _Box(width, height, bottom=40.0)
    flat = [
        v for s in series for v in list(s.values)[: len(categories)] if math.isfinite(v)
    ]
    lo, hi = min(flat + [0.0]), max(flat + [0.0])
    ticks = nice_ticks(lo, hi, target=4)
    axis_lo, axis_hi = min(ticks + [lo]), max(ticks + [hi])
    span = axis_hi - axis_lo or 1.0

    def y_of(v: float) -> float:
        return box.top + box.plot_h * (1.0 - (v - axis_lo) / span)

    cluster = box.plot_w / len(categories)
    k = len(series)
    # 2px surface gap inside a cluster and a third of a slot between clusters.
    bw = min(26.0, max(3.0, (cluster * 0.72) / k - 2.0))
    zero_y = y_of(0.0)

    parts = [
        f'<svg viewBox="0 0 {width:.0f} {height:.0f}" role="img" '
        f'preserveAspectRatio="xMidYMid meet" data-chart="{_esc(chart_id)}">'
    ]
    parts += _grid_and_axes(box, ticks, y_of, unit)

    group_w = k * bw + (k - 1) * 2.0
    r = 4.0
    for ci, cat in enumerate(categories):
        cx = box.left + cluster * (ci + 0.5)
        for si, s in enumerate(series):
            if ci >= len(s.values):
                continue
            v = s.values[ci]
            if not math.isfinite(v):
                continue
            color = s.color or theme.series_color(si)
            x = cx - group_w / 2 + si * (bw + 2.0)
            y_val = y_of(v)
            h = abs(y_val - zero_y)
            title = f"<title>{_esc(cat)} — {_esc(s.name)}: {_esc(_fmt(v, unit))}</title>"
            if h < 0.6:
                parts.append(
                    f'<rect x="{x:.2f}" y="{zero_y - 0.5:.2f}" width="{bw:.2f}" height="1" '
                    f"fill=\"{color}\">{title}</rect>"
                )
                continue
            rad = min(r, h, bw / 2)
            if v >= 0:
                top = y_val
                d = (f"M{x:.2f},{zero_y:.2f} V{top + rad:.2f} Q{x:.2f},{top:.2f} "
                     f"{x + rad:.2f},{top:.2f} H{x + bw - rad:.2f} "
                     f"Q{x + bw:.2f},{top:.2f} {x + bw:.2f},{top + rad:.2f} V{zero_y:.2f} Z")
            else:
                bot = y_val
                d = (f"M{x:.2f},{zero_y:.2f} V{bot - rad:.2f} Q{x:.2f},{bot:.2f} "
                     f"{x + rad:.2f},{bot:.2f} H{x + bw - rad:.2f} "
                     f"Q{x + bw:.2f},{bot:.2f} {x + bw:.2f},{bot - rad:.2f} V{zero_y:.2f} Z")
            parts.append(f'<path d="{d}" fill="{color}">{title}</path>')
        parts.append(
            f'<text x="{cx:.2f}" y="{box.top + box.plot_h + 18:.1f}" text-anchor="middle" '
            f'font-family="{theme.SANS}" font-size="11.5" fill="{theme.INK_2}">'
            f"{_esc(cat)}</text>"
        )

    parts.append(
        f'<line x1="{box.left:.1f}" y1="{zero_y:.2f}" x2="{box.left + box.plot_w:.1f}" '
        f'y2="{zero_y:.2f}" stroke="{theme.RULE}" stroke-width="1" shape-rendering="crispEdges"/>'
    )
    parts.append("</svg>")
    return "".join(parts)


def small_multiple(
    values: Sequence[float],
    x_labels: Sequence[str],
    *,
    color: str,
    width: float = 470.0,
    height: float = 168.0,
    unit: str = "",
    chart_id: str = "s",
) -> str:
    """A compact single-series panel for a facet grid — same specs, less chrome.

    One series, so no legend box: the panel's own title says what is plotted.
    """
    if not values:
        return ""
    n = len(values)
    box = _Box(width, height, top=14.0, right=16.0, bottom=28.0, left=58.0)
    finite = [v for v in values if math.isfinite(v)]
    lo, hi = (min(finite), max(finite)) if finite else (0.0, 1.0)
    lo, hi = min(lo, 0.0), max(hi, 0.0)
    ticks = nice_ticks(lo, hi, target=3)
    axis_lo, axis_hi = min(ticks + [lo]), max(ticks + [hi])
    span = axis_hi - axis_lo or 1.0

    def x_of(i: int) -> float:
        return box.left + (box.plot_w * i / (n - 1) if n > 1 else box.plot_w / 2)

    def y_of(v: float) -> float:
        return box.top + box.plot_h * (1.0 - (v - axis_lo) / span)

    parts = [
        f'<svg viewBox="0 0 {width:.0f} {height:.0f}" role="img" '
        f'preserveAspectRatio="xMidYMid meet" data-chart="{_esc(chart_id)}">'
    ]
    parts += _grid_and_axes(box, ticks, y_of, unit)
    parts += _x_axis(box, x_labels, x_of)

    pts = [(x_of(i), y_of(v)) for i, v in enumerate(values) if math.isfinite(v)]
    if pts:
        base = y_of(max(axis_lo, min(0.0, axis_hi)))
        poly = " ".join(f"{x:.2f},{y:.2f}" for x, y in pts)
        parts.append(
            f'<polygon points="{pts[0][0]:.2f},{base:.2f} {poly} {pts[-1][0]:.2f},{base:.2f}" '
            f'fill="{color}" fill-opacity="0.10"/>'
        )
        parts.append(
            f'<polyline fill="none" stroke="{color}" stroke-width="2" stroke-linejoin="round" '
            f'stroke-linecap="round" points="{poly}"/>'
        )
        ex, ey = pts[-1]
        parts.append(
            f'<circle cx="{ex:.2f}" cy="{ey:.2f}" r="4" fill="{color}" '
            f'stroke="{theme.SURFACE}" stroke-width="2"/>'
        )
    for i, v in enumerate(values):
        if math.isfinite(v):
            parts.append(
                f'<rect x="{x_of(i) - 4:.2f}" y="{box.top:.1f}" width="8" '
                f'height="{box.plot_h:.1f}" fill="transparent">'
                f"<title>{_esc(x_labels[i])}: {_esc(_fmt(v, unit))}</title></rect>"
            )
    parts.append("</svg>")
    return "".join(parts)
