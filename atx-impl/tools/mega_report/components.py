"""Reusable report components: pure functions that return HTML or inline-SVG strings.

Contract (checked by ``validate_svg`` and the self-tests):
- every SVG carries an explicit ``viewBox`` with margins for outer labels (ticks, end labels, legends);
- every shape (rect, line, path, circle, polyline, polygon, ellipse) has an explicit fill and stroke in its
  ``style``; colours are only ``var(--token)`` references (``_tok`` refuses anything else);
- text is styled by CSS classes (``t-*``) that read theme tokens; no colour is written on a text node;
- a missing value renders as an explicit ``n/a`` (never a guess).

Each component has a synthetic self-test ``_selftest_<name>``; ``self_test()`` runs them all.
"""
from __future__ import annotations

import datetime as _dt
import html as _html
import math
import re
import xml.etree.ElementTree as _ET

NA_TEXT = 'n/a'
MINUS = '−'
SHAPES = ('rect', 'line', 'path', 'circle', 'polyline', 'polygon', 'ellipse')


# ----------------------------------------------------------------------------------------------- formatting
def esc(v) -> str:
    """HTML-escape any value (None -> empty)."""
    return _html.escape('' if v is None else str(v), quote=True)


def is_num(v) -> bool:
    """True for a finite int/float (bool excluded)."""
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


def fmt(v, spec: str = '.3f'):
    """Format one value; None / NaN -> None (the caller renders n/a).

    ``spec``: a Python format spec, or ``pct<d>`` / ``+pct<d>`` (x100 with a % sign), ``abs:<spec>``, ``int``
    (thousands separators), ``str``. Negative numbers use the typographic minus.
    """
    if v is None:
        return None
    if spec == 'str' or isinstance(v, str):
        return str(v)
    if not is_num(v):
        return None
    if spec.startswith('abs:'):
        v, spec = abs(v), spec[4:]
    plus = spec.startswith('+')
    core = spec[1:] if plus else spec
    if core.startswith('pct'):
        d = int(core[3:] or 1)
        s = f"{v * 100:{'+' if plus else ''}.{d}f}%"
    elif core == 'int':
        s = f"{v:{'+' if plus else ''},.0f}"
    else:
        s = format(v, spec)
    return s.replace('-', MINUS)


def na(title: str = 'not available') -> str:
    """The explicit not-available marker."""
    return f'<span class="na" title="{esc(title)}">{NA_TEXT}</span>'


def val(v, spec: str = '.3f', title: str = 'not available') -> str:
    """Formatted, escaped value or the n/a marker."""
    s = fmt(v, spec)
    return na(title) if s is None else esc(s)


def chip(state, text: str | None = None) -> str:
    """Pass/fail chip: state True -> PASS, False -> FAIL, None -> n/a."""
    if state is None:
        return na()
    cls = 'pass' if state else 'fail'
    return f'<span class="chip {cls}">{esc(text or ("PASS" if state else "FAIL"))}</span>'


# badge kind -> chip style: verdicts (accepted / rejected / defect / pending) and monitor states (ok / warn / alarm)
BADGE_STYLE = {'accepted': 'pass', 'ok': 'pass', 'pass': 'pass', 'rejected': 'fail', 'alarm': 'fail', 'fail': 'fail',
               'defect': 'warn', 'warn': 'warn', 'pending': 'neutral', 'n/a': 'neutral'}
# first matching rule wins (case-insensitive regex on the verdict text); no match -> kind 'other' (plain text)
VERDICT_RULES = (('defect', r'\bdefect'), ('rejected', r'^\s*rejected'), ('accepted', r'^\s*(accepted|promoted)'),
                 ('pending', r'^\s*(pending|not run|queued)'))


def badge(kind: str | None, text: str | None = None) -> str:
    """A labelled status chip (``BADGE_STYLE``); unknown kinds render neutral, None renders n/a."""
    if kind is None:
        return na()
    return f'<span class="chip {BADGE_STYLE.get(kind, "neutral")}">{esc((text or kind).upper())}</span>'


def verdict_kind(text, rules=None) -> str | None:
    """Classify a free-text verdict: extra ``rules`` [(kind, regex)] first, then ``VERDICT_RULES``; None for no
    text, 'other' when nothing matches."""
    if not isinstance(text, str) or not text.strip():
        return None
    for kind, pat in list(rules or []) + list(VERDICT_RULES):
        if re.search(pat, text, flags=re.I):
            return kind
    return 'other'


def verdict_html(text, kind: str | None = None, rules=None) -> str | None:
    """Badge for the verdict's kind plus the verdict text below it (when it says more than the kind); plain text for
    kind 'other'; None (n/a) without a verdict."""
    kind = kind or verdict_kind(text, rules)
    if kind is None:
        return None
    if kind == 'other':
        return esc(text)
    sub = f'<span class="sub">{esc(text)}</span>' if isinstance(text, str) and text.strip().lower() != kind else ''
    return badge(kind) + sub


# ----------------------------------------------------------------------------------------------- scales
class Scale:
    """Linear map from a data domain [d0, d1] to a pixel range [r0, r1]."""

    def __init__(self, d0: float, d1: float, r0: float, r1: float):
        self.d0, self.d1, self.r0, self.r1 = d0, d1, r0, r1

    def __call__(self, v: float) -> float:
        if self.d1 == self.d0:
            return (self.r0 + self.r1) / 2
        return self.r0 + (v - self.d0) * (self.r1 - self.r0) / (self.d1 - self.d0)


def nice_step(span: float, target: int = 5) -> float:
    """A 1/2/2.5/5 x 10^k step giving about ``target`` intervals over ``span``."""
    raw = abs(span) / max(target, 1)
    if raw <= 0 or not math.isfinite(raw):
        return 1.0
    mag = 10 ** math.floor(math.log10(raw))
    for m in (1, 2, 2.5, 5, 10):
        if raw <= m * mag * (1 + 1e-9):
            return m * mag
    return 10 * mag


def ticks_in(lo: float, hi: float, target: int = 5) -> list[float]:
    """Nice tick values inside the reached data range [lo, hi] (never outside it)."""
    if not (is_num(lo) and is_num(hi)):
        return []
    if hi <= lo:
        return [lo]
    step = nice_step(hi - lo, target)
    k0, k1 = math.ceil(lo / step - 1e-9), math.floor(hi / step + 1e-9)
    return [round(k * step, 12) + 0.0 for k in range(k0, k1 + 1)]


def pad_domain(lo: float, hi: float, frac: float = 0.06) -> tuple[float, float]:
    """Pad a data range on both sides (a degenerate range gets a symmetric pad)."""
    if hi <= lo:
        d = abs(lo) * 0.1 or 1.0
        return lo - d, hi + d
    p = (hi - lo) * frac
    return lo - p, hi + p


def tick_label(t: float, spec: str) -> str:
    """Axis tick text: the zero tick drops the explicit plus sign."""
    return fmt(t, spec.replace('+', '') if t == 0 else spec)


def _n(x: float) -> str:
    s = f'{x:.1f}'
    return s[:-2] if s.endswith('.0') else s


# ----------------------------------------------------------------------------------------------- SVG primitives
def _tok(t) -> str:
    if t is None or t == 'none':
        return 'none'
    if not re.fullmatch(r'[A-Za-z0-9_-]+', str(t)):
        raise ValueError(f'colour must be a theme token name, got {t!r}')
    return f'var(--{t})'


def _style(fill='none', stroke='none', sw=None, dash=None, cap=None, op=None) -> str:
    s = f'fill:{_tok(fill)};stroke:{_tok(stroke)}'
    if sw is not None:
        s += f';stroke-width:{_n(sw) if sw >= 0.1 else sw}'
    if dash:
        s += f';stroke-dasharray:{dash}'
    if cap:
        s += f';stroke-linecap:{cap};stroke-linejoin:round'
    if op is not None:
        s += f';fill-opacity:{op}'
    return s


def _el(tag: str, attrs: dict, style: str, title: str | None = None) -> str:
    a = ' '.join(f'{k}="{v}"' for k, v in attrs.items())
    if title:
        return f'<{tag} {a} style="{style}"><title>{esc(title)}</title></{tag}>'
    return f'<{tag} {a} style="{style}"/>'


def s_line(x1, y1, x2, y2, stroke='grid', sw=1.0, dash=None, title=None) -> str:
    return _el('line', {'x1': _n(x1), 'y1': _n(y1), 'x2': _n(x2), 'y2': _n(y2)},
               _style('none', stroke, sw, dash, cap='butt'), title)


def s_rect(x, y, w, h, fill, stroke='none', sw=None, title=None) -> str:
    return _el('rect', {'x': _n(x), 'y': _n(y), 'width': _n(max(w, 0)), 'height': _n(max(h, 0))},
               _style(fill, stroke, sw), title)


def s_path(d, stroke='none', sw=1.5, fill='none', dash=None, title=None, op=None) -> str:
    return _el('path', {'d': d}, _style(fill, stroke, sw, dash, cap='round', op=op), title)


def s_circle(cx, cy, r, fill, stroke='bg', sw=1.5, title=None) -> str:
    return _el('circle', {'cx': _n(cx), 'cy': _n(cy), 'r': _n(r)}, _style(fill, stroke, sw), title)


def s_text(x, y, s, cls='t-tick', anchor='start', dy=None, title=None) -> str:
    d = f' dy="{dy}"' if dy else ''
    t = f'<title>{esc(title)}</title>' if title else ''
    return f'<text x="{_n(x)}" y="{_n(y)}" class="{cls}" text-anchor="{anchor}"{d}>{t}{esc(s)}</text>'


def svg(width: int, height: int, body: str, aria: str) -> str:
    """Wrap an SVG body with an explicit viewBox (scales with max-width:100%)."""
    return (f'<svg class="chart" viewBox="0 0 {width} {height}" role="img" aria-label="{esc(aria)}" '
            f'xmlns="http://www.w3.org/2000/svg" preserveAspectRatio="xMinYMin meet">{body}</svg>')


def empty_svg(width: int = 1072, height: int = 120, aria: str = 'not available', note: str = NA_TEXT) -> str:
    """Placeholder figure body for an unavailable input."""
    body = (s_rect(0.5, 0.5, width - 1, height - 1, 'bg-2', 'rule', 1)
            + s_text(width / 2, height / 2, note, 't-na', 'middle', dy='0.32em'))
    return svg(width, height, body, aria)


def _bar_path(pos: float, base: float, end: float, w: float, horizontal: bool = False, r: float = 4.0) -> str:
    """Bar from ``base`` (the baseline, square) to ``end`` (the data end, rounded 4 px), thickness ``w``."""
    length = abs(end - base)
    r = max(0.0, min(r, length / 2, w / 2))
    s = 1.0 if end >= base else -1.0
    if not horizontal:
        x, y0, y1 = pos, base, end
        return (f'M{_n(x)},{_n(y0)} L{_n(x)},{_n(y1 - s * r)} Q{_n(x)},{_n(y1)} {_n(x + r)},{_n(y1)} '
                f'L{_n(x + w - r)},{_n(y1)} Q{_n(x + w)},{_n(y1)} {_n(x + w)},{_n(y1 - s * r)} L{_n(x + w)},{_n(y0)} Z')
    y, x0, x1 = pos, base, end
    return (f'M{_n(x0)},{_n(y)} L{_n(x1 - s * r)},{_n(y)} Q{_n(x1)},{_n(y)} {_n(x1)},{_n(y + r)} '
            f'L{_n(x1)},{_n(y + w - r)} Q{_n(x1)},{_n(y + w)} {_n(x1 - s * r)},{_n(y + w)} L{_n(x0)},{_n(y + w)} Z')


def declutter(targets: list[float], lo: float, hi: float, gap: float) -> list[float]:
    """Place labels near their targets with at least ``gap`` between them, inside [lo, hi] (cluster merge)."""
    if not targets:
        return []
    order = sorted(range(len(targets)), key=lambda i: targets[i])
    clusters = [[targets[i], [i]] for i in order]  # [centre, members]

    def top(c):
        return c[0] - (len(c[1]) - 1) * gap / 2

    merged = True
    while merged:
        merged = False
        for k in range(1, len(clusters)):
            a, b = clusters[k - 1], clusters[k]
            if top(a) + len(a[1]) * gap > top(b) + 1e-9:
                mem = a[1] + b[1]
                clusters[k - 1] = [sum(targets[i] for i in mem) / len(mem), mem]
                del clusters[k]
                merged = True
                break
    out = [0.0] * len(targets)
    pos = []
    for c in clusters:
        t = top(c)
        pos.extend((i, t + j * gap) for j, i in enumerate(c[1]))
    ys = [p for _, p in pos]
    shift = 0.0
    if ys and ys[-1] > hi:
        shift = hi - ys[-1]
    if ys and ys[0] + shift < lo:
        shift = lo - ys[0]
    for i, p in pos:
        out[i] = p + shift
    return out


# ----------------------------------------------------------------------------------------------- date axis
def _to_ord(x) -> float:
    return float(x.toordinal()) if isinstance(x, _dt.date) else float(x)


def _year_bounds(d0: _dt.date, d1: _dt.date) -> list[_dt.date]:
    return [_dt.date(y, 1, 1) for y in range(d0.year + 1, d1.year + 1) if d0 < _dt.date(y, 1, 1) <= d1]


def _date_axis(sx: Scale, d0: _dt.date, d1: _dt.date, y_axis: float, y_top: float) -> list[str]:
    """Year gridlines at 1 January (real dates), quarter ticks, year labels centred on each year's span."""
    out = []
    for yb in _year_bounds(d0, d1):
        x = sx(yb.toordinal())
        out.append(s_line(x, y_top, x, y_axis + 7, 'rule', 1))
    for y in range(d0.year, d1.year + 1):
        for m in (4, 7, 10):
            q = _dt.date(y, m, 1)
            if d0 < q < d1:
                x = sx(q.toordinal())
                out.append(s_line(x, y_axis, x, y_axis + 4, 'axis', 1))
        a, b = max(d0, _dt.date(y, 1, 1)), min(d1, _dt.date(y, 12, 31))
        if (b - a).days >= 45:
            out.append(s_text(sx((a.toordinal() + b.toordinal()) / 2), y_axis + 18, str(y), 't-tick', 'middle'))
    return out


def _path_d(xs, ys, sx: Scale, sy: Scale) -> str:
    parts, pen = [], False
    for x, y in zip(xs, ys):
        if x is None or not is_num(y):
            pen = False
            continue
        parts.append(f"{'L' if pen else 'M'}{_n(sx(_to_ord(x)))},{_n(sy(y))}")
        pen = True
    return ' '.join(parts)


# ----------------------------------------------------------------------------------------------- charts
def line_chart(series: list[dict], *, width: int = 1072, height: int = 340, margin=(24, 160, 30, 58),
               y_fmt: str = '.2f', y_label: str | None = None, ref_values=(), y_domain=None,
               end_labels: bool = True, tick_target: int = 6, troughs: bool = False, trough_fmt: str = 'pct1',
               aria: str = 'line chart') -> str:
    """Multi-series line chart over dates.

    ``series``: dicts ``name, x (dates), y (floats, None/NaN breaks the line), color (token), width, dash,
    end_label (str or None), emph (drawn last), trough (bool: mark the minimum)``. One scale per axis; y ticks
    are nice values inside the reached data range; x has year gridlines at 1 January and quarter ticks; end-point
    labels sit in the right margin, de-collided, each tied to its line end by a hairline leader.
    """
    top, right, bottom, left = margin
    x0, x1, y0, y1 = left, width - right, top, height - bottom
    pts = [(x, y) for s in series for x, y in zip(s['x'], s['y']) if x is not None and is_num(y)]
    if not pts:
        return empty_svg(width, height, aria)
    xs = [_to_ord(p[0]) for p in pts]
    ys = [p[1] for p in pts]
    dlo, dhi = min(ys), max(ys)
    for r in ref_values:
        if is_num(r):
            dlo, dhi = min(dlo, r), max(dhi, r)
    lo, hi = y_domain if y_domain else pad_domain(dlo, dhi, 0.05)
    sx, sy = Scale(min(xs), max(xs), x0, x1), Scale(lo, hi, y1, y0)
    d0, d1 = _dt.date.fromordinal(int(min(xs))), _dt.date.fromordinal(int(max(xs)))
    out = []
    for t in ticks_in(dlo, dhi, tick_target):
        yy = sy(t)
        out.append(s_line(x0, yy, x1, yy, 'grid', 1))
        out.append(s_text(x0 - 8, yy, tick_label(t, y_fmt), 't-tick', 'end', dy='0.32em'))
    for r in ref_values:
        if is_num(r) and lo <= r <= hi:
            out.append(s_line(x0, sy(r), x1, sy(r), 'axis', 1))
    out.append(s_line(x0, y1, x1, y1, 'axis', 1))
    out.extend(_date_axis(sx, d0, d1, y1, y0))
    if y_label:
        out.append(s_text(x0 - 8, 12, y_label, 't-axis', 'start' if x0 < 60 else 'start'))
    for s in sorted(series, key=lambda s: bool(s.get('emph'))):
        d = _path_d(s['x'], s['y'], sx, sy)
        if d:
            out.append(s_path(d, s['color'], s.get('width', 1.5), dash=s.get('dash'), title=s['name']))
    if troughs:
        for s in series:
            if not s.get('trough'):
                continue
            best = min(((x, y) for x, y in zip(s['x'], s['y']) if x is not None and is_num(y)),
                       key=lambda p: p[1], default=None)
            if best is None:
                continue
            cx, cy = sx(_to_ord(best[0])), sy(best[1])
            out.append(s_circle(cx, cy, 3.5, s['color'], 'bg', 1.5, title=f"{s['name']} minimum {fmt(best[1], trough_fmt)}"))
            out.append(s_text(cx + 7, cy, f"{s.get('trough_label', '')}{fmt(best[1], trough_fmt)}", 't-val', 'start', dy='0.9em'))
    if end_labels:
        items = []
        for s in series:
            if not s.get('end_label'):
                continue
            last = next(((x, y) for x, y in zip(reversed(s['x']), reversed(s['y'])) if x is not None and is_num(y)), None)
            if last:
                items.append((sy(last[1]), sx(_to_ord(last[0])), s))
        lab = declutter([it[0] for it in items], y0 + 4, y1 - 4, 14)
        lx = x1 + 16
        for (yt, xe, s), yl in zip(items, lab):
            out.append(s_line(xe + 3, yt, lx - 2, yl, 'fg-3', 0.6))
            out.append(s_circle(xe, yt, 2.8, s['color'], 'bg', 1.2))
            out.append(s_line(lx, yl, lx + 12, yl, s['color'], max(s.get('width', 1.5), 2.0), s.get('dash')))
            out.append(s_text(lx + 16, yl, s['end_label'], 't-end', 'start', dy='0.32em'))
    return svg(width, height, ''.join(out), aria)


def drawdown_chart(series: list[dict], *, width: int = 1072, height: int = 170, margin=(24, 160, 30, 58),
                   y_fmt: str = 'pct0', trough_fmt: str = 'pct1', y_label: str | None = None,
                   aria: str = 'drawdown') -> str:
    """Drawdown panel (values <= 0) on the same frame as a ``line_chart`` so x positions align; the minimum of
    each series with ``trough: True`` is marked and labelled."""
    ys = [y for s in series for y in s['y'] if is_num(y)]
    if not ys:
        return empty_svg(width, height, aria)
    lo = min(min(ys), 0.0)
    span = -lo if lo < 0 else 1.0
    return line_chart(series, width=width, height=height, margin=margin, y_fmt=y_fmt, ref_values=(0.0,),
                      y_domain=(lo - 0.08 * span, 0.04 * span), end_labels=False, tick_target=4, troughs=True,
                      trough_fmt=trough_fmt, y_label=y_label, aria=aria)


def bar_chart(categories: list[str], series: list[dict], *, horizontal: bool = False, width: int = 1072,
              height: int | None = None, value_fmt: str = '+.3f', tick_fmt: str | None = None,
              label_w: int = 200, notes: list[str] | None = None, note_w: int = 180, y_label: str | None = None,
              aria: str = 'bar chart') -> str:
    """Grouped, signed bar chart from a single zero baseline.

    ``series``: dicts ``name, color (token), values (per category, None -> n/a), label (bool: value at the tip)``.
    Vertical: categories along x. Horizontal: categories as rows with a text label column and an optional
    ``notes`` column (e.g. member counts). Bars are at most 24 px thick with a 2 px surface gap, 4 px rounded
    data end, square at the baseline.
    """
    tick_fmt = tick_fmt or value_fmt.lstrip('+')
    vals = [v for s in series for v in s['values'] if is_num(v)]
    n, ns = len(categories), max(len(series), 1)
    if not vals or not n:
        return empty_svg(width, height or 160, aria)
    dlo, dhi = min(0.0, min(vals)), max(0.0, max(vals))
    span = (dhi - dlo) or 1.0
    lo = dlo - (0.14 * span if dlo < 0 else 0.0)
    hi = dhi + (0.14 * span if dhi > 0 else 0.0)
    out = []
    if not horizontal:
        height = height or 300
        top, right, bottom, left = 26, 16, 34, 58
        x0, x1, y0, y1 = left, width - right, top, height - bottom
        sy = Scale(lo, hi, y1, y0)
        for t in ticks_in(dlo, dhi, 5):
            out.append(s_line(x0, sy(t), x1, sy(t), 'grid', 1))
            out.append(s_text(x0 - 8, sy(t), tick_label(t, tick_fmt), 't-tick', 'end', dy='0.32em'))
        band = (x1 - x0) / n
        bw = max(4.0, min(24.0, (band * 0.72 - 2 * (ns - 1)) / ns))
        gw = ns * bw + 2 * (ns - 1)
        for ci, c in enumerate(categories):
            gx = x0 + band * ci + (band - gw) / 2
            for si, s in enumerate(series):
                v = s['values'][ci] if ci < len(s['values']) else None
                bx = gx + si * (bw + 2)
                if not is_num(v):
                    out.append(s_text(bx + bw / 2, sy(0) - 4, NA_TEXT, 't-na', 'middle'))
                    continue
                out.append(s_path(_bar_path(bx, sy(0), sy(v), bw), 'none', 0, fill=s['color'],
                                  title=f"{c} {s['name']}: {fmt(v, value_fmt)}"))
                if s.get('label'):
                    ty = sy(v) - 5 if v >= 0 else sy(v) + 13
                    out.append(s_text(bx + bw / 2, ty, fmt(v, value_fmt), 't-val', 'middle'))
            out.append(s_text(x0 + band * (ci + 0.5), y1 + 18, c, 't-lbl', 'middle'))
        out.append(s_line(x0, sy(0), x1, sy(0), 'axis', 1))
        if y_label:
            out.append(s_text(x0 - 8, 12, y_label, 't-axis', 'start'))
        return svg(width, height, ''.join(out), aria)
    row = max(22, ns * 14 + 10)
    top, bottom = 10, 28
    height = height or top + bottom + n * row
    x0 = label_w
    x1 = width - 16 - (note_w if notes else 0) - 64
    sx = Scale(lo, hi, x0, x1)
    for t in ticks_in(dlo, dhi, 5):
        out.append(s_line(sx(t), top, sx(t), height - bottom, 'grid', 1))
        out.append(s_text(sx(t), height - bottom + 16, tick_label(t, tick_fmt), 't-tick', 'middle'))
    bw = max(4.0, min(24.0, (row * 0.7 - 2 * (ns - 1)) / ns))
    gw = ns * bw + 2 * (ns - 1)
    for ci, c in enumerate(categories):
        gy = top + row * ci + (row - gw) / 2
        out.append(s_text(x0 - 12, top + row * (ci + 0.5), c, 't-lbl', 'end', dy='0.32em'))
        for si, s in enumerate(series):
            v = s['values'][ci] if ci < len(s['values']) else None
            by = gy + si * (bw + 2)
            if not is_num(v):
                out.append(s_text(sx(0) + 6, by + bw / 2, NA_TEXT, 't-na', 'start', dy='0.32em'))
                continue
            out.append(s_path(_bar_path(by, sx(0), sx(v), bw, horizontal=True), 'none', 0, fill=s['color'],
                              title=f"{c} {s['name']}: {fmt(v, value_fmt)}"))
            if s.get('label', True):
                tx, anc = (sx(v) + 6, 'start') if v >= 0 else (sx(v) - 6, 'end')
                out.append(s_text(tx, by + bw / 2, fmt(v, value_fmt), 't-val', anc, dy='0.32em'))
        if notes and ci < len(notes) and notes[ci]:
            out.append(s_text(width - 16 - note_w + 8, top + row * (ci + 0.5), notes[ci], 't-lbl-2', 'start', dy='0.32em'))
    out.append(s_line(sx(0), top, sx(0), height - bottom, 'axis', 1))
    return svg(width, height, ''.join(out), aria)


def waterfall(start: dict, steps: list[dict], end: dict, *, width: int = 1072, height: int = 340,
              value_fmt: str = '+.3f', total_fmt: str = '.3f', y_label: str | None = None,
              aria: str = 'waterfall') -> str:
    """Signed steps with a running total and SE whiskers.

    ``start`` / ``end``: ``label, value, color`` (bars from zero); ``steps``: ``label, delta, se, color`` floating
    from the running total; whiskers span delta +- se at the step's end; a hairline connector carries the running
    total to the next column. A step with a missing delta renders n/a and leaves the running total unchanged.
    """
    cols = [start] + steps + [end]
    top, right, bottom, left = 24, 16, 50, 58
    x0, x1, y0, y1 = left, width - right, top, height - bottom
    run, levels = start.get('value'), []
    if not is_num(run):
        return empty_svg(width, height, aria)
    for s in steps:
        d = s.get('delta')
        a = run
        if is_num(d):
            run = run + d
        levels.append((a, run))
    reach = [0.0, start['value']]
    for (a, b), s in zip(levels, steps):
        reach += [a, b]
        if is_num(s.get('se')):
            reach += [b - s['se'], b + s['se']]
    if is_num(end.get('value')):
        reach.append(end['value'])
    dlo, dhi = min(reach), max(reach)
    lo, hi = min(0.0, dlo - 0.04 * (dhi - dlo)), dhi + 0.10 * (dhi - dlo)
    sy = Scale(lo, hi, y1, y0)
    out = []
    for t in ticks_in(min(0.0, dlo), dhi, 6):
        out.append(s_line(x0, sy(t), x1, sy(t), 'grid', 1))
        out.append(s_text(x0 - 8, sy(t), tick_label(t, total_fmt), 't-tick', 'end', dy='0.32em'))
    band = (x1 - x0) / len(cols)
    bw = min(24.0, band * 0.5)

    def cx(i):
        return x0 + band * (i + 0.5)

    def label(i, text):
        for k, line in enumerate(str(text).split('\n')[:3]):
            out.append(s_text(cx(i), y1 + 17 + 13 * k, line, 't-lbl' if k == 0 else 't-lbl-2', 'middle'))

    out.append(s_path(_bar_path(cx(0) - bw / 2, sy(0), sy(start['value']), bw), 'none', 0,
                      fill=start.get('color', 'ref-2'), title=f"{start['label']}: {fmt(start['value'], total_fmt)}"))
    out.append(s_text(cx(0), sy(start['value']) - 6, fmt(start['value'], total_fmt), 't-val', 'middle'))
    label(0, start['label'])
    prev_level = start['value']
    for i, (s, (a, b)) in enumerate(zip(steps, levels), start=1):
        out.append(s_line(cx(i - 1) + bw / 2, sy(prev_level), cx(i) - bw / 2, sy(a), 'fg-3', 0.75))
        d = s.get('delta')
        if is_num(d):
            tok = s.get('color') or ('pos' if d >= 0 else 'neg')
            y_top, y_bot = sy(max(a, b)), sy(min(a, b))
            h = max(y_bot - y_top, 1.0)
            base, tip = (sy(a), sy(b))
            if abs(tip - base) < 1.0:
                out.append(s_rect(cx(i) - bw / 2, min(base, tip) - 0.5, bw, h, tok, 'none', None,
                                  title=f"{s['label']}: {fmt(d, value_fmt)}"))
            else:
                out.append(s_path(_bar_path(cx(i) - bw / 2, base, tip, bw), 'none', 0, fill=tok,
                                  title=f"{s['label']}: {fmt(d, value_fmt)} (SE {fmt(s.get('se'), '.3f') or NA_TEXT})"))
            se = s.get('se')
            ylab = min(y_top, sy(b)) - 6
            if is_num(se):
                wt, wb = sy(b + se), sy(b - se)
                out.append(s_line(cx(i), wt, cx(i), wb, 'fg-2', 1))
                out.append(s_line(cx(i) - 5, wt, cx(i) + 5, wt, 'fg-2', 1))
                out.append(s_line(cx(i) - 5, wb, cx(i) + 5, wb, 'fg-2', 1))
                ylab = min(ylab, wt - 6)
            out.append(s_text(cx(i), ylab, fmt(d, value_fmt), 't-val', 'middle'))
        else:
            out.append(s_text(cx(i), sy(a) - 6, NA_TEXT, 't-na', 'middle'))
        label(i, s['label'])
        prev_level = b
    j = len(cols) - 1
    out.append(s_line(cx(j - 1) + bw / 2, sy(prev_level), cx(j) - bw / 2, sy(prev_level), 'fg-3', 0.75))
    if is_num(end.get('value')):
        out.append(s_path(_bar_path(cx(j) - bw / 2, sy(0), sy(end['value']), bw), 'none', 0,
                          fill=end.get('color', 'accent'), title=f"{end['label']}: {fmt(end['value'], total_fmt)}"))
        out.append(s_text(cx(j), sy(end['value']) - 6, fmt(end['value'], total_fmt), 't-val', 'middle'))
    else:
        out.append(s_text(cx(j), sy(0) - 6, NA_TEXT, 't-na', 'middle'))
    label(j, end['label'])
    out.append(s_line(x0, sy(0), x1, sy(0), 'axis', 1))
    if y_label:
        out.append(s_text(x0 - 8, 12, y_label, 't-axis', 'start'))
    return svg(width, height, ''.join(out), aria)


def scatter(points: list[dict], *, x_label: str, y_label: str, x_fmt: str = '.3f', y_fmt: str = '.1f',
            size_label: str | None = None, size_fmt: str = '.2f', r_range=(3.5, 11.0), width: int = 1072,
            height: int = 420, aria: str = 'scatter', x_log: bool = False) -> str:
    """Labelled scatter with optional size encoding.

    ``points``: dicts ``x, y, size (value or None), color (token), hollow (bool), label (shown when set), title
    (hover)``. Radius: area linear in size between the smallest and largest size (``r_range``); the size key in
    the right margin shows the minimum, middle and maximum sizes with their values.
    """
    pts = [p for p in points if is_num(p.get('x')) and is_num(p.get('y'))]
    if x_log:
        pts = [dict(p, x=math.log10(p['x']), _x=p['x']) for p in pts if p['x'] > 0]
    if not pts:
        return empty_svg(width, height, aria)
    top, right, bottom, left = 26, 170, 40, 62
    x0, x1, y0, y1 = left, width - right, top, height - bottom
    xl, xh = min(p['x'] for p in pts), max(p['x'] for p in pts)
    yl, yh = min(p['y'] for p in pts), max(p['y'] for p in pts)
    sx = Scale(*pad_domain(xl, xh, 0.06), x0, x1)
    sy = Scale(*pad_domain(yl, yh, 0.08), y1, y0)
    sizes = [p['size'] for p in pts if is_num(p.get('size'))]
    smin, smax = (min(sizes), max(sizes)) if sizes else (None, None)

    def radius(v):
        if not is_num(v) or smin is None:
            return r_range[0]
        f = 0.5 if smax == smin else (v - smin) / (smax - smin)
        return math.sqrt(r_range[0] ** 2 + f * (r_range[1] ** 2 - r_range[0] ** 2))

    out = []
    for t in ticks_in(yl, yh, 5):
        out.append(s_line(x0, sy(t), x1, sy(t), 'grid', 1))
        out.append(s_text(x0 - 8, sy(t), tick_label(t, y_fmt), 't-tick', 'end', dy='0.32em'))
    if x_log:
        for k in range(math.floor(xl), math.ceil(xh) + 1):
            for m in (1, 2, 5):
                t = k + math.log10(m)
                if xl - 1e-9 <= t <= xh + 1e-9:
                    out.append(s_line(sx(t), y0, sx(t), y1, 'grid', 1))
                    out.append(s_text(sx(t), y1 + 16, fmt(10 ** t, x_fmt), 't-tick', 'middle'))
    for t in ([] if x_log else ticks_in(xl, xh, 6)):
        out.append(s_line(sx(t), y0, sx(t), y1, 'grid', 1))
        out.append(s_text(sx(t), y1 + 16, tick_label(t, x_fmt), 't-tick', 'middle'))
    out.append(s_line(x0, y1, x1, y1, 'axis', 1))
    out.append(s_line(x0, y0, x0, y1, 'axis', 1))
    out.append(s_text(x1, y1 + 32, x_label, 't-axis', 'end'))
    out.append(s_text(x0 - 8, 12, y_label, 't-axis', 'start'))
    for p in sorted(pts, key=lambda p: -(p.get('size') or 0)):
        r = radius(p.get('size'))
        fill, stroke = ('bg', p['color']) if p.get('hollow') else (p['color'], 'bg')
        out.append(s_circle(sx(p['x']), sy(p['y']), r, fill, stroke, 1.5, title=p.get('title')))
    for p in pts:
        if p.get('label'):
            r = radius(p.get('size'))
            out.append(s_line(sx(p['x']) + r + 1, sy(p['y']), sx(p['x']) + r + 10, sy(p['y']) - 10, 'fg-3', 0.6))
            out.append(s_text(sx(p['x']) + r + 12, sy(p['y']) - 10, p['label'], 't-lbl', 'start', dy='0.32em'))
    if size_label and smin is not None:
        kx = x1 + 30
        out.append(s_text(kx - 10, y0 + 4, size_label, 't-axis', 'start'))
        vals = [smin, (smin + smax) / 2, smax] if smax != smin else [smin]
        yk = y0 + 28
        for v in vals:
            r = radius(v)
            out.append(s_circle(kx, yk, r, 'bg', 'fg-3', 1))
            out.append(s_text(kx + 18, yk, fmt(v, size_fmt), 't-tick', 'start', dy='0.32em'))
            yk += 2 * r_range[1] + 6
    return svg(width, height, ''.join(out), aria)


def dot_plot(rows: list[dict], *, value_label: str, value_fmt: str = '+.2f', columns=(), label_w: int = 150,
             ref_lines=(), width: int = 1072, row_h: int = 18, aria: str = 'dot plot') -> str:
    """Signed value per row with a zero line, grouped rows and left text columns.

    ``rows``: dicts ``group, label, value, filled (bool), color (token), cells (texts for columns), title``.
    ``columns``: ``(header, width)`` pairs drawn after the label column. ``ref_lines``: ``{'value', 'label'}``
    thresholds (dashed hairlines, labelled at the top). A stem joins zero to each dot so the sign reads at a glance.
    """
    vals = [r['value'] for r in rows if is_num(r.get('value'))]
    if not rows or not vals:
        return empty_svg(width, 140, aria)
    groups = []
    for r in rows:
        if not groups or groups[-1] != r.get('group'):
            groups.append(r.get('group'))
    head, grp_h, pad_b = 34, 22, 30
    height = head + len(groups) * grp_h + len(rows) * row_h + pad_b
    cw = sum(w for _, w in columns)
    x0, x1 = label_w + cw + 24, width - 24
    refs = [r['value'] for r in ref_lines if is_num(r.get('value'))]
    dlo, dhi = min(vals + refs + [0.0]), max(vals + refs + [0.0])
    sx = Scale(*pad_domain(dlo, dhi, 0.05), x0, x1)
    out = [s_text(0, 16, 'ID', 't-axis', 'start')]
    cx = label_w
    for h, w in columns:
        out.append(s_text(cx, 16, h, 't-axis', 'start'))
        cx += w
    out.append(s_text(x1, 16, value_label, 't-axis', 'end'))
    y_first, y_last = head - 6, height - pad_b + 4
    for t in ticks_in(dlo, dhi, 8):
        out.append(s_line(sx(t), y_first, sx(t), y_last, 'grid', 1))
        out.append(s_text(sx(t), y_last + 14, tick_label(t, value_fmt), 't-tick', 'middle'))
    for r in ref_lines:
        if is_num(r.get('value')):
            out.append(s_line(sx(r['value']), y_first, sx(r['value']), y_last, 'fg-3', 1, dash='3 3'))
            out.append(s_text(sx(r['value']) + 4, y_first + 8, r.get('label', ''), 't-val', 'start'))
    out.append(s_line(sx(0), y_first, sx(0), y_last, 'axis', 1))
    y = head
    cur = object()
    for r in rows:
        if r.get('group') != cur:
            cur = r.get('group')
            out.append(s_line(0, y + 2, width, y + 2, 'rule-2', 1))
            out.append(s_text(0, y + 15, cur or '', 't-grp', 'start'))
            y += grp_h
        yc = y + row_h / 2
        out.append(s_text(0, yc, r['label'], 't-mono', 'start', dy='0.32em'))
        cx = label_w
        for (h, w), c in zip(columns, r.get('cells', [])):
            out.append(s_text(cx, yc, c, 't-lbl-2', 'start', dy='0.32em'))
            cx += w
        v = r.get('value')
        if is_num(v):
            out.append(s_line(sx(0), yc, sx(v), yc, r['color'], 1))
            fill, stroke = (r['color'], 'bg') if r.get('filled') else ('bg', r['color'])
            out.append(s_circle(sx(v), yc, 4.5, fill, stroke, 1.5, title=r.get('title')))
            out.append(s_text(sx(v) + (9 if v >= 0 else -9), yc, fmt(v, value_fmt), 't-val',
                              'start' if v >= 0 else 'end', dy='0.32em'))
        else:
            out.append(s_text(sx(0) + 6, yc, NA_TEXT, 't-na', 'start', dy='0.32em'))
        y += row_h
    return svg(width, height, ''.join(out), aria)


DIV_CLASSES = ('n3', 'n2', 'n1', '0', 'p1', 'p2', 'p3')


def diverging_bin(v: float, step: float) -> str:
    """Class suffix of a value on the 7-step diverging scale (neutral within half a step of zero)."""
    k = max(-3, min(3, int(round(v / step)))) if step > 0 else 0
    return '0' if k == 0 else (f'p{k}' if k > 0 else f'n{-k}')


def heatmap(row_labels: list[str], col_labels: list[str], values: list[list], *, value_fmt: str = '+pct1',
            total: list | None = None, total_label: str = 'Year', step: float | None = None, width: int = 1072,
            cell_h: int = 30, aria: str = 'heatmap', label_w: int = 56) -> str:
    """Rows x columns of signed values on a 7-step diverging scale (neutral midpoint), each cell labelled.

    ``values[i][j]`` None -> an empty n/a cell. ``total`` adds a text-only column after a gap (its own scale is
    not mixed into the colour bins). ``step`` fixes the bin width; default = a nice step with the largest |value|
    in the third bin. A legend row states every bin's range.
    """
    flat = [v for row in values for v in row if is_num(v)]
    if not flat:
        return empty_svg(width, 120, aria)
    step = step or nice_step(max(abs(v) for v in flat), 3)
    lab_w, tot_w = label_w, (86 if total is not None else 0)
    gap = 14 if total is not None else 0
    top = 22
    nc = len(col_labels)
    cw = (width - lab_w - tot_w - gap) / max(nc, 1)
    out = []
    for j, c in enumerate(col_labels):
        out.append(s_text(lab_w + cw * (j + 0.5), 14, c, 't-axis', 'middle'))
    if total is not None:
        out.append(s_text(width - tot_w / 2, 14, total_label, 't-axis', 'middle'))
    for i, rl in enumerate(row_labels):
        y = top + i * cell_h
        out.append(s_text(lab_w - 10, y + cell_h / 2, rl, 't-lbl', 'end', dy='0.32em'))
        for j in range(nc):
            v = values[i][j] if j < len(values[i]) else None
            x = lab_w + cw * j
            if is_num(v):
                b = diverging_bin(v, step)
                out.append(s_rect(x + 1, y + 1, cw - 2, cell_h - 2, f'div-{b}', 'bg', 2,
                                  title=f'{rl} {col_labels[j]}: {fmt(v, value_fmt)}'))
                out.append(s_text(x + cw / 2, y + cell_h / 2, fmt(v, value_fmt), f't-hm t-hm-{b}', 'middle', dy='0.32em'))
            else:
                out.append(s_rect(x + 1, y + 1, cw - 2, cell_h - 2, 'bg-2', 'bg', 2, title=f'{rl} {col_labels[j]}: {NA_TEXT}'))
        if total is not None:
            v = total[i] if i < len(total) else None
            out.append(s_rect(width - tot_w, y + 1, tot_w, cell_h - 2, 'bg-2', 'bg', 2))
            out.append(s_text(width - tot_w / 2, y + cell_h / 2, fmt(v, value_fmt) or NA_TEXT,
                              't-hm-tot' if is_num(v) else 't-na', 'middle', dy='0.32em'))
    ly = top + len(row_labels) * cell_h + 16
    lw = 96
    lx = width - 7 * lw
    edges = [-2.5, -1.5, -0.5, 0.5, 1.5, 2.5]
    for k, b in enumerate(DIV_CLASSES):
        x = lx + k * lw
        out.append(s_rect(x + 1, ly, lw - 2, 10, f'div-{b}', 'bg', 1))
        if k == 0:
            t = f'< {fmt(edges[0] * step, value_fmt)}'
        elif k == 6:
            t = f'> {fmt(edges[5] * step, value_fmt)}'
        else:
            t = f'{fmt(edges[k - 1] * step, value_fmt)} to {fmt(edges[k] * step, value_fmt)}'
        out.append(s_text(x + lw / 2, ly + 24, t, 't-tick', 'middle'))
    height = ly + 32
    return svg(width, height, ''.join(out), aria)


def box_plot(rows: list[dict], *, value_label: str, value_fmt: str = '.3f', label_w: int = 190, row_h: int = 22,
             width: int = 1072, aria: str = 'distribution') -> str:
    """Box-style distribution per row on one shared scale: whiskers p5-p95, box p25-p75, median tick, mean dot.

    ``rows``: dicts ``label, color (token), stats {p5, p25, p50, p75, p95, mean, n}``; a row without stats
    renders n/a. The right column prints the mean.
    """
    good = [r for r in rows if r.get('stats') and all(is_num(r['stats'].get(k)) for k in ('p5', 'p95'))]
    if not good:
        return empty_svg(width, 120, aria)
    lo = min(r['stats']['p5'] for r in good)
    hi = max(r['stats']['p95'] for r in good)
    top, bottom = 26, 30
    height = top + bottom + len(rows) * row_h
    x0, x1 = label_w, width - 110
    sx = Scale(*pad_domain(lo, hi, 0.04), x0, x1)
    out = [s_text(x0, 14, value_label, 't-axis', 'start'), s_text(width - 8, 14, 'MEAN', 't-axis', 'end')]
    for t in ticks_in(lo, hi, 6):
        out.append(s_line(sx(t), top - 4, sx(t), height - bottom, 'grid', 1))
        out.append(s_text(sx(t), height - bottom + 16, tick_label(t, value_fmt), 't-tick', 'middle'))
    for i, r in enumerate(rows):
        yc = top + i * row_h + row_h / 2
        out.append(s_text(x0 - 12, yc, r['label'], 't-lbl', 'end', dy='0.32em'))
        st = r.get('stats') or {}
        if not all(is_num(st.get(k)) for k in ('p5', 'p25', 'p50', 'p75', 'p95')):
            out.append(s_text(x0 + 6, yc, NA_TEXT, 't-na', 'start', dy='0.32em'))
            continue
        tip = (f"{r['label']}: p5 {fmt(st['p5'], value_fmt)}, p25 {fmt(st['p25'], value_fmt)}, median "
               f"{fmt(st['p50'], value_fmt)}, p75 {fmt(st['p75'], value_fmt)}, p95 {fmt(st['p95'], value_fmt)}, "
               f"mean {fmt(st.get('mean'), value_fmt)}, n {st.get('n')}")
        out.append(s_line(sx(st['p5']), yc, sx(st['p95']), yc, 'fg-3', 1))
        out.append(s_line(sx(st['p5']), yc - 4, sx(st['p5']), yc + 4, 'fg-3', 1))
        out.append(s_line(sx(st['p95']), yc - 4, sx(st['p95']), yc + 4, 'fg-3', 1))
        bh = min(12.0, row_h - 8)
        out.append(s_rect(sx(st['p25']), yc - bh / 2, sx(st['p75']) - sx(st['p25']), bh, r['color'], 'none', None, title=tip))
        out.append(s_line(sx(st['p50']), yc - bh / 2 - 2, sx(st['p50']), yc + bh / 2 + 2, 'fg', 1.5))
        if is_num(st.get('mean')):
            out.append(s_circle(sx(st['mean']), yc, 3, 'bg', 'fg', 1.2))
            out.append(s_text(width - 8, yc, fmt(st['mean'], value_fmt), 't-val', 'end', dy='0.32em'))
    return svg(width, height, ''.join(out), aria)


# ----------------------------------------------------------------------------------------------- v2 components
def _rtext(x, y, s, cls, angle, anchor='end', title=None) -> str:
    t = f'<title>{esc(title)}</title>' if title else ''
    return (f'<text x="{_n(x)}" y="{_n(y)}" class="{cls}" text-anchor="{anchor}" '
            f'transform="rotate({_n(angle)} {_n(x)} {_n(y)})">{t}{esc(s)}</text>')


def _div_legend(x0: float, y: float, step: float, spec: str, lw: float = 96) -> list[str]:
    out, edges = [], [-2.5, -1.5, -0.5, 0.5, 1.5, 2.5]
    for k, b in enumerate(DIV_CLASSES):
        x = x0 + k * lw
        out.append(s_rect(x + 1, y, lw - 2, 10, f'div-{b}', 'bg', 1))
        if k == 0:
            t = f'< {fmt(edges[0] * step, spec)}'
        elif k == 6:
            t = f'> {fmt(edges[5] * step, spec)}'
        else:
            t = f'{fmt(edges[k - 1] * step, spec)} to {fmt(edges[k] * step, spec)}'
        out.append(s_text(x + lw / 2, y + 24, t, 't-tick', 'middle'))
    return out


def corr_matrix(labels: list[str], values: list[list], *, groups: list | None = None, step: float = 0.2,
                show_values: bool | None = None, max_values: int = 45, label_w: int = 150, group_w: int = 150,
                cell_max: float = 56.0, width: int = 1072, legend_fmt: str = '+.1f', aria: str = 'correlation matrix',
                diag: bool = False) -> str:
    """Symmetric matrix on the 7-step diverging scale (neutral within half a step of zero), labels in the given
    order on both axes (column labels rotated). Values (x100, integer) are printed in the cells when n <= ``max_values``.
    ``groups`` (one per label) draws separators and names each run of equal groups at the right."""
    n = len(labels)
    flat = [values[i][j] for i in range(n) for j in range(n) if i != j and is_num(values[i][j])]
    if not n or not flat:
        return empty_svg(width, 140, aria)
    show = (n <= max_values) if show_values is None else show_values
    gw = group_w if groups else 16
    cs = min(cell_max, (width - label_w - gw) / n)
    top = 12 + 6.0 * max(len(str(s)) for s in labels) * 0.82
    x0, y0 = label_w, top
    out = []
    for i, lab in enumerate(labels):
        yc = y0 + cs * (i + 0.5)
        out.append(s_text(x0 - 8, yc, lab, 't-mono', 'end', dy='0.32em'))
        xc = x0 + cs * (i + 0.5)
        out.append(_rtext(xc + 3, y0 - 6, lab, 't-mono', -55, 'start'))
    for i in range(n):
        for j in range(n):
            x, y = x0 + cs * j, y0 + cs * i
            v = values[i][j]
            if i == j and not (diag and is_num(v)):
                out.append(s_rect(x + 0.5, y + 0.5, cs - 1, cs - 1, 'bg-2', 'bg', 1, title=f'{labels[i]}'))
                continue
            if not is_num(v):
                out.append(s_rect(x + 0.5, y + 0.5, cs - 1, cs - 1, 'bg-2', 'bg', 1,
                                  title=f'{labels[i]} x {labels[j]}: {NA_TEXT}'))
                continue
            b = diverging_bin(v, step)
            out.append(s_rect(x + 0.5, y + 0.5, cs - 1, cs - 1, f'div-{b}', 'bg', 1,
                              title=f'{labels[i]} x {labels[j]}: {fmt(v, "+.3f")}'))
            if show:
                out.append(s_text(x + cs / 2, y + cs / 2, fmt(round(v * 100), '.0f'), f't-hm t-cm t-hm-{b}', 'middle',
                                  dy='0.32em'))
    if groups:
        runs, start = [], 0
        for i in range(1, n + 1):
            if i == n or groups[i] != groups[start]:
                runs.append((start, i, groups[start]))
                start = i
        for a, b_, g in runs:
            if a > 0:
                out.append(s_line(x0, y0 + cs * a, x0 + cs * n, y0 + cs * a, 'fg-2', 1.2))
                out.append(s_line(x0 + cs * a, y0, x0 + cs * a, y0 + cs * n, 'fg-2', 1.2))
            out.append(s_line(x0 + cs * n + 6, y0 + cs * a + 2, x0 + cs * n + 6, y0 + cs * b_ - 2, 'fg-3', 1))
            out.append(s_text(x0 + cs * n + 12, y0 + cs * (a + b_) / 2, str(g), 't-lbl-2', 'start', dy='0.32em'))
    ly = y0 + cs * n + 18
    out.extend(_div_legend(max(x0, width - 7 * 96), ly, step, legend_fmt))
    return svg(width, int(ly + 34), ''.join(out), aria)


def ic_panel(rows: list[dict], horizons: list, *, label_w: int = 170, row_h: int = 16, width: int = 1072,
             value_fmt: str = '+.3f', panel_titles: list | None = None, aria: str = 'IC panel') -> str:
    """Per-row mean with +-1 SE whiskers, one panel per horizon (own x scale, zero line), rows grouped.

    ``rows``: dicts ``group, label, filled, color, values {h: (mean, se)}, title``.
    """
    good = [r for r in rows for h in horizons if is_num((r.get('values') or {}).get(h, (None, None))[0])]
    if not rows or not good:
        return empty_svg(width, 140, aria)
    groups = []
    for r in rows:
        if not groups or groups[-1] != r.get('group'):
            groups.append(r.get('group'))
    head, grp_h, pad_b = 36, 20, 30
    height = head + len(groups) * grp_h + len(rows) * row_h + pad_b
    k = len(horizons)
    gap = 22
    pw = (width - label_w - gap * (k - 1)) / k
    scales = []
    for hi, h in enumerate(horizons):
        lo_v, hi_v = [0.0], [0.0]
        for r in rows:
            m, se = (r.get('values') or {}).get(h, (None, None))
            if is_num(m):
                s = se if is_num(se) else 0.0
                lo_v.append(m - s)
                hi_v.append(m + s)
        px0 = label_w + hi * (pw + gap)
        scales.append((px0, Scale(*pad_domain(min(lo_v), max(hi_v), 0.05), px0 + 4, px0 + pw - 4), min(lo_v), max(hi_v)))
    out = [s_text(0, 16, 'ID', 't-axis', 'start')]
    y_first, y_last = head - 6, height - pad_b + 4
    for hi, (px0, sx, dlo, dhi) in enumerate(scales):
        title = (panel_titles or [f'h = {h}' for h in horizons])[hi]
        out.append(s_text(px0, 16, title, 't-axis', 'start'))
        for t in ticks_in(dlo, dhi, 4):
            out.append(s_line(sx(t), y_first, sx(t), y_last, 'grid', 1))
            out.append(s_text(sx(t), y_last + 14, tick_label(t, value_fmt), 't-tick', 'middle'))
        out.append(s_line(sx(0), y_first, sx(0), y_last, 'axis', 1))
    y = head
    cur = object()
    for r in rows:
        if r.get('group') != cur:
            cur = r.get('group')
            out.append(s_line(0, y + 2, width, y + 2, 'rule-2', 1))
            out.append(s_text(0, y + 14, cur or '', 't-grp', 'start'))
            y += grp_h
        yc = y + row_h / 2
        out.append(s_text(0, yc, r['label'], 't-mono', 'start', dy='0.32em'))
        for h, (px0, sx, _, _) in zip(horizons, scales):
            m, se = (r.get('values') or {}).get(h, (None, None))
            if not is_num(m):
                out.append(s_text(px0 + 6, yc, NA_TEXT, 't-na', 'start', dy='0.32em'))
                continue
            if is_num(se):
                out.append(s_line(sx(m - se), yc, sx(m + se), yc, r['color'], 1.2))
            fill, stroke = (r['color'], 'bg') if r.get('filled') else ('bg', r['color'])
            out.append(s_circle(sx(m), yc, 3.6, fill, stroke, 1.4,
                                title=f"{r['label']} h={h}: mean {fmt(m, value_fmt)}, SE {fmt(se, '.3f') or NA_TEXT}"))
        y += row_h
    return svg(width, height, ''.join(out), aria)


def _area_d(xs, ys, sx: Scale, sy: Scale, base: float = 0.0) -> str:
    pts = [(sx(_to_ord(x)), sy(y)) for x, y in zip(xs, ys) if x is not None and is_num(y)]
    if not pts:
        return ''
    d = f'M{_n(pts[0][0])},{_n(sy(base))} ' + ' '.join(f'L{_n(a)},{_n(b)}' for a, b in pts)
    return d + f' L{_n(pts[-1][0])},{_n(sy(base))} Z'


def exposure_chart(dates: list, long: list, short: list, net: list, *, width: int = 1072, height: int = 320,
                   y_fmt: str = '.1f', y_label: str | None = None, aria: str = 'exposures') -> str:
    """Long dollars (above zero) and short dollars (below zero) as areas, net as a line; all as a fraction of NAV."""
    ys = [v for v in list(long) + [-s for s in short if is_num(s)] + list(net) if is_num(v)]
    if not dates or not ys:
        return empty_svg(width, height, aria)
    top, right, bottom, left = 24, 110, 30, 58
    x0, x1, y0, y1 = left, width - right, top, height - bottom
    lo, hi = min(ys + [0.0]), max(ys + [0.0])
    sx = Scale(dates[0].toordinal(), dates[-1].toordinal(), x0, x1)
    sy = Scale(*pad_domain(lo, hi, 0.05), y1, y0)
    out = []
    for t in ticks_in(lo, hi, 6):
        out.append(s_line(x0, sy(t), x1, sy(t), 'grid', 1))
        out.append(s_text(x0 - 8, sy(t), tick_label(t, y_fmt), 't-tick', 'end', dy='0.32em'))
    out.extend(_date_axis(sx, dates[0], dates[-1], y1, y0))
    out.append(s_path(_area_d(dates, long, sx, sy), 's2', 1.0, fill='s2', op=0.28, title='long dollars / NAV'))
    out.append(s_path(_area_d(dates, [-s if is_num(s) else None for s in short], sx, sy), 's3', 1.0, fill='s3', op=0.28,
                      title='short dollars / NAV (drawn negative)'))
    out.append(s_path(_path_d(dates, net, sx, sy), 'fg', 1.6, title='net dollars / NAV'))
    out.append(s_line(x0, sy(0), x1, sy(0), 'axis', 1))
    lastl = next((v for v in reversed(long) if is_num(v)), None)
    lasts = next((v for v in reversed(short) if is_num(v)), None)
    lastn = next((v for v in reversed(net) if is_num(v)), None)
    for v, lab in ((lastl, 'long'), (-lasts if is_num(lasts) else None, 'short'), (lastn, 'net')):
        if is_num(v):
            out.append(s_text(x1 + 8, sy(v), f'{lab} {fmt(abs(v) if lab == "short" else v, "+.3f" if lab == "net" else ".3f")}',
                              't-end', 'start', dy='0.32em'))
    if y_label:
        out.append(s_text(x0 - 8, 12, y_label, 't-axis', 'start'))
    return svg(width, height, ''.join(out), aria)


def fill_panel(dates: list, panels: list[dict], *, width: int = 1072, panel_h: int = 92, aria: str = 'fills') -> str:
    """Vertically stacked small panels on one date axis; each ``{label, y, fmt, color, note}`` is a daily area
    (values >= 0) with its own y scale; the label and the note (e.g. the TRAIN mean) sit at the top left."""
    if not dates or not panels:
        return empty_svg(width, 140, aria)
    left, right = 58, 24
    x0, x1 = left, width - right
    sx = Scale(dates[0].toordinal(), dates[-1].toordinal(), x0, x1)
    out, y = [], 6
    for p in panels:
        vals = [v for v in p['y'] if is_num(v)]
        top, bot = y + 18, y + panel_h - 4
        out.append(s_text(x0, y + 11, p['label'], 't-axis', 'start'))
        if p.get('note'):
            out.append(s_text(x1, y + 11, p['note'], 't-lbl-2', 'end'))
        if not vals:
            out.append(s_text(x0 + 6, (top + bot) / 2, NA_TEXT, 't-na', 'start', dy='0.32em'))
            y += panel_h
            continue
        hi = max(vals) or 1.0
        sy = Scale(0.0, hi * 1.05, bot, top)
        for t in ticks_in(0.0, hi, 2):
            out.append(s_line(x0, sy(t), x1, sy(t), 'grid', 1))
            out.append(s_text(x0 - 8, sy(t), tick_label(t, p.get('fmt', '.0f')), 't-tick', 'end', dy='0.32em'))
        out.append(s_path(_area_d(dates, p['y'], sx, sy), p.get('color', 's2'), 0.8, fill=p.get('color', 's2'), op=0.35,
                          title=p['label']))
        out.append(s_line(x0, bot, x1, bot, 'axis', 1))
        y += panel_h
    out.extend(_date_axis(sx, dates[0], dates[-1], y - 4, 6))
    return svg(width, int(y + 22), ''.join(out), aria)


def multiples(panels: list[dict], x_vals: list, *, cols: int = 3, width: int = 1072, panel_h: int = 150,
              y_fmt: str = '+.2f', x_label: str = '', aria: str = 'small multiples', x_log: bool = True) -> str:
    """Grid of small line charts on one shared y scale. ``panels``: ``{title, series: [{y (per x), color, width,
    dash, label}]}``; numeric x (log scale by default), labelled at every x value."""
    ys = [v for p in panels for s in p['series'] for v in s['y'] if is_num(v)]
    if not panels or not ys:
        return empty_svg(width, 140, aria)
    lo, hi = min(ys + [0.0]), max(ys + [0.0])
    gap = 26
    pw = (width - gap * (cols - 1)) / cols
    rows = math.ceil(len(panels) / cols)
    tx = (lambda v: math.log10(v)) if x_log else (lambda v: float(v))
    out = []
    for k, p in enumerate(panels):
        r, c = divmod(k, cols)
        px0, py0 = c * (pw + gap), r * (panel_h + 26)
        l, rgt, t, b = px0 + 44, px0 + pw - 58, py0 + 22, py0 + panel_h - 18
        sx = Scale(tx(x_vals[0]), tx(x_vals[-1]), l, rgt)
        sy = Scale(*pad_domain(lo, hi, 0.06), b, t)
        out.append(s_text(px0, py0 + 12, p['title'], 't-grp', 'start'))
        for tv in ticks_in(lo, hi, 3):
            out.append(s_line(l, sy(tv), rgt, sy(tv), 'grid', 1))
            out.append(s_text(l - 6, sy(tv), tick_label(tv, y_fmt), 't-tick', 'end', dy='0.32em'))
        out.append(s_line(l, sy(0), rgt, sy(0), 'axis', 1))
        for xv in x_vals:
            out.append(s_text(sx(tx(xv)), b + 13, str(xv), 't-tick', 'middle'))
        for s in sorted(p['series'], key=lambda s: s.get('width', 1.0)):
            pts = [(sx(tx(xv)), sy(v)) for xv, v in zip(x_vals, s['y']) if is_num(v)]
            if len(pts) >= 2:
                d = 'M' + ' L'.join(f'{_n(a)},{_n(bb)}' for a, bb in pts)
                out.append(s_path(d, s['color'], s.get('width', 1.0), dash=s.get('dash'), title=s.get('title')))
            for a, bb in pts[-1:]:
                if s.get('label'):
                    out.append(s_circle(a, bb, 2.6, s['color'], 'bg', 1))
                    out.append(s_text(a + 6, bb, s['label'], 't-val', 'start', dy='0.32em'))
    height = rows * (panel_h + 26)
    if x_label:
        out.append(s_text(width, height - 4, x_label, 't-axis', 'end'))
        height += 6
    return svg(width, int(height), ''.join(out), aria)


def xy_chart(series: list[dict], *, x_label: str, y_label: str, x_fmt: str = '.2f', y_fmt: str = '.1f',
             width: int = 1072, height: int = 340, aria: str = 'xy chart', x_log: bool = False,
             x_ticks: list | None = None, y_refs=()) -> str:
    """Numeric x/y: each series ``{name, points [(x, y, label[, side])], color, dash, markers, label_side}`` drawn as a
    line through its points with markers and point labels (side, else the series ``label_side``: 'above' right of the
    point by default, 'below' or 'left'). ``x_log``: log10 x axis (non-positive x dropped; 1-2-5 ticks per decade unless ``x_ticks`` lists the tick
    values); ``y_refs``: ``{'value', 'label'}`` dashed horizontal reference lines, labelled at the left end."""
    def tx(v):
        return math.log10(v) if x_log else v

    def ok(x, y):
        return is_num(x) and is_num(y) and (not x_log or x > 0)
    pts = [(tx(x), y) for s in series for x, y, *_ in s['points'] if ok(x, y)]
    if not pts:
        return empty_svg(width, height, aria)
    top, right, bottom, left = 26, 190, 42, 62
    x0, x1, y0, y1 = left, width - right, top, height - bottom
    xl, xh = min(p[0] for p in pts), max(p[0] for p in pts)
    refs = [r for r in y_refs if is_num(r.get('value'))]
    yl = min([p[1] for p in pts] + [r['value'] for r in refs])
    yh = max([p[1] for p in pts] + [r['value'] for r in refs])
    sx = Scale(*pad_domain(xl, xh, 0.08), x0, x1)
    sy = Scale(*pad_domain(yl, yh, 0.12), y1, y0)
    out = []
    for t in ticks_in(*pad_domain(yl, yh, 0.1), 5):
        out.append(s_line(x0, sy(t), x1, sy(t), 'grid', 1))
        out.append(s_text(x0 - 8, sy(t), tick_label(t, y_fmt), 't-tick', 'end', dy='0.32em'))
    if x_ticks is not None:
        xt = [(tx(v), fmt(v, x_fmt)) for v in x_ticks if is_num(v) and (not x_log or v > 0)]
    elif x_log:
        xt = [(k + math.log10(m), fmt(10 ** k * m, x_fmt)) for k in range(math.floor(xl), math.ceil(xh) + 1)
              for m in (1, 2, 5)]
    else:
        xt = [(t, tick_label(t, x_fmt)) for t in ticks_in(*pad_domain(xl, xh, 0.06), 6)]
    lo_x, hi_x = pad_domain(xl, xh, 0.06)
    for t, lab in xt:
        if lo_x - 1e-9 <= t <= hi_x + 1e-9:
            out.append(s_line(sx(t), y0, sx(t), y1, 'grid', 1))
            out.append(s_text(sx(t), y1 + 16, lab, 't-tick', 'middle'))
    out.append(s_line(x0, y1, x1, y1, 'axis', 1))
    out.append(s_text(x1, y1 + 34, x_label, 't-axis', 'end'))
    out.append(s_text(x0 - 8, 12, y_label, 't-axis', 'start'))
    for r in refs:
        out.append(s_line(x0, sy(r['value']), x1, sy(r['value']), 'fg-3', 1, '4 3'))
        if r.get('label'):
            out.append(s_text(x0 + 6, sy(r['value']) - 5, r['label'], 't-lbl-2', 'start'))
    for s in series:
        dflt = s.get('label_side', 'above')
        ps = [(tx(x), y, rest[0] if rest else None, x, rest[1] if len(rest) > 1 else dflt)
              for x, y, *rest in s['points'] if ok(x, y)]
        if len(ps) >= 2:
            d = 'M' + ' L'.join(f'{_n(sx(x))},{_n(sy(y))}' for x, y, *_ in ps)
            out.append(s_path(d, s['color'], s.get('width', 1.6), dash=s.get('dash'), title=s['name']))
        for x, y, lab, raw, side in ps:
            if s.get('markers', True):
                out.append(s_circle(sx(x), sy(y), 4.2, s['color'], 'bg', 1.4, title=f"{s['name']}: {fmt(raw, x_fmt)}, {fmt(y, y_fmt)}"))
            if lab:
                if side == 'left':
                    out.append(s_text(sx(x) - 8, sy(y), lab, 't-val', 'end', dy='0.32em'))
                else:
                    out.append(s_text(sx(x) + 8, sy(y) + (14 if side == 'below' else -8), lab, 't-val', 'start'))
        if ps and s.get('end_label'):
            x, y = ps[-1][:2]
            out.append(s_text(x1 + 12, sy(y), s['end_label'], 't-end', 'start', dy='0.32em'))
    return svg(width, height, ''.join(out), aria)


def flow_diagram(stages: list[dict], *, per_row: int = 4, width: int = 1072, box_h: int = 104,
                 aria: str = 'pipeline') -> str:
    """Boxes-and-arrows pipeline in a snake layout (row 1 left to right, row 2 right to left, ...). Each stage
    ``{title, lines [str], kind}`` is a rect with a title and up to four detail lines; arrows are short straight
    lines with a small triangular head."""
    if not stages:
        return empty_svg(width, 120, aria)
    gap_x, gap_y = 40, 46
    bw = (width - 2 - gap_x * (per_row - 1)) / per_row
    rows = math.ceil(len(stages) / per_row)
    pos = []
    for k in range(len(stages)):
        r, c = divmod(k, per_row)
        if r % 2 == 1:
            c = per_row - 1 - c
        pos.append((1 + c * (bw + gap_x), 1 + r * (box_h + gap_y)))
    out = []

    def head(x, y, dx, dy):
        n_ = math.hypot(dx, dy) or 1.0
        ux, uy = dx / n_, dy / n_
        a = (x - 8 * ux + 4 * uy, y - 8 * uy - 4 * ux)
        b = (x - 8 * ux - 4 * uy, y - 8 * uy + 4 * ux)
        return s_path(f'M{_n(x)},{_n(y)} L{_n(a[0])},{_n(a[1])} L{_n(b[0])},{_n(b[1])} Z', 'fg-2', 1, fill='fg-2')

    for k in range(len(stages) - 1):
        (xa, ya), (xb, yb) = pos[k], pos[k + 1]
        if abs(ya - yb) < 1:
            if xb > xa:
                sx_, ex_ = xa + bw, xb
            else:
                sx_, ex_ = xa, xb + bw
            yy = ya + box_h / 2
            out.append(s_line(sx_ + 2, yy, ex_ - 2, yy, 'fg-2', 1.3))
            out.append(head(ex_ - 2, yy, ex_ - sx_, 0))
        else:
            xx = xa + bw / 2
            out.append(s_line(xx, ya + box_h + 2, xx, yb - 2, 'fg-2', 1.3))
            out.append(head(xx, yb - 2, 0, 1))
    for (x, y), st in zip(pos, stages):
        tok = 'accent' if st.get('kind') == 'key' else 'rule'
        out.append(s_rect(x, y, bw, box_h, 'bg', tok, 1.4 if st.get('kind') == 'key' else 1))
        out.append(s_text(x + 10, y + 18, st.get('title', ''), 't-flow-t', 'start'))
        for i, ln in enumerate((st.get('lines') or [])[:5]):
            out.append(s_text(x + 10, y + 36 + 14.5 * i, ln, 't-flow', 'start'))
    height = rows * box_h + (rows - 1) * gap_y + 2
    return svg(width, int(height), ''.join(out), aria)


def glossary(entries: list[dict]) -> str:
    """Two-column definition list: ``{term, definition (html-safe text), group}``; a group change adds a sub-head."""
    parts, cur = [], object()
    for e in entries:
        if e.get('group') and e.get('group') != cur:
            cur = e.get('group')
            parts.append(f'<div class="gl-grp">{esc(cur)}</div>')
        parts.append(f'<div class="gl-row"><dt>{esc(e["term"])}</dt><dd>{e["definition"]}</dd></div>')
    return f'<dl class="glossary">{"".join(parts)}</dl>'


def callout(title: str, items_html: list[str], *, kind: str = 'caveat', paragraphs_html: list[str] | None = None) -> str:
    """A boxed caveat: title, optional paragraphs and a bullet list (items are already escaped HTML)."""
    ps = ''.join(f'<p>{p}</p>' for p in (paragraphs_html or []))
    lis = ''.join(f'<li>{it}</li>' for it in items_html)
    ul = f'<ul>{lis}</ul>' if lis else ''
    return f'<aside class="callout {esc(kind)}"><p class="callout-title">{esc(title)}</p>{ps}{ul}</aside>'


# ----------------------------------------------------------------------------------------------- HTML blocks
def legend(items: list[dict]) -> str:
    """Legend as a row of text items, each with a short key mark (line, dash, dot, hollow dot or bar)."""
    lis = []
    for it in items:
        kind, col = it.get('kind', 'line'), it['color']
        if kind in ('dot', 'hollow'):
            fill, stroke = ('bg', col) if kind == 'hollow' else (col, 'bg')
            mark = s_circle(9, 6, 4.5, fill, stroke, 1.5)
        elif kind == 'bar':
            mark = s_rect(2, 1, 14, 10, col, 'none')
        else:
            mark = s_line(0, 6, 22, 6, col, it.get('width', 2), it.get('dash'))
        key = f'<svg class="key" viewBox="0 0 22 12" aria-hidden="true" xmlns="http://www.w3.org/2000/svg">{mark}</svg>'
        extra = f' <span class="lg-val">{esc(it["value"])}</span>' if it.get('value') else ''
        lis.append(f'<li>{key}<span class="lg-name">{esc(it["name"])}</span>{extra}</li>')
    return f'<ul class="legend">{"".join(lis)}</ul>'


def kpi_strip(items: list[dict]) -> str:
    """Row of key figures: ``label`` (uppercase), ``value`` (monospace), optional ``sub``; hairline separated."""
    cells = []
    for it in items:
        v = it.get('value')
        vh = esc(v) if isinstance(v, str) and v else na()
        sub = f'<dd class="kpi-sub">{esc(it["sub"])}</dd>' if it.get('sub') else ''
        cells.append(f'<div class="kpi"><dt>{esc(it["label"])}</dt><dd class="kpi-val">{vh}</dd>{sub}</div>')
    return f'<dl class="kpi-strip">{"".join(cells)}</dl>'


def gate_panel(gates: list[dict], *, headers=('Condition', 'Value', 'Threshold', 'Result'), title: str | None = None) -> str:
    """Pre-registered conditions: ``name, value (text), threshold (text), passed (True/False/None)``."""
    rows = []
    for g in gates:
        v = esc(g['value']) if g.get('value') else na()
        rows.append(f'<tr><td class="t">{esc(g["name"])}</td><td class="m n">{v}</td>'
                    f'<td class="m n">{esc(g.get("threshold") or "")}</td><td class="c">{chip(g.get("passed"), g.get("chip"))}</td></tr>')
    head = ''.join(f'<th class="{"t" if i == 0 else ("c" if i == 3 else "n")}" scope="col">{esc(h)}</th>'
                   for i, h in enumerate(headers))
    t = f'<p class="panel-title">{esc(title)}</p>' if title else ''
    return (f'<div class="panel">{t}<div class="tbl-wrap"><table class="gates"><thead><tr>{head}</tr></thead>'
            f'<tbody>{"".join(rows)}</tbody></table></div></div>')


def figure(num: int, body: str, caption: str, fig_id: str | None = None, legend_html: str = '') -> str:
    """Numbered figure; caption below (academic convention), centred."""
    i = f' id="{esc(fig_id)}"' if fig_id else ''
    return (f'<figure class="fig"{i}>{legend_html}<div class="fig-body">{body}</div>'
            f'<figcaption><span class="fig-num">Figure {num}.</span> {esc(caption)}</figcaption></figure>')


def section(num: int, title: str, body: str, sec_id: str | None = None) -> str:
    """Numbered section; the numbering is the report's structure."""
    i = f' id="{esc(sec_id)}"' if sec_id else ''
    return f'<section class="sec"{i}><h2><span class="sec-num">{num}</span>{esc(title)}</h2>{body}</section>'


def _cbar(v: float, bar: dict) -> str:
    lo, hi = bar.get('lo', 0.0), bar.get('hi', 1.0)
    if not (is_num(lo) and is_num(hi)) or hi <= lo:
        return ''
    z = min(max((0.0 - lo) / (hi - lo), 0.0), 1.0)
    p = min(max((v - lo) / (hi - lo), 0.0), 1.0)
    left, w = min(z, p) * 100, abs(p - z) * 100
    cls = 'pos' if v >= 0 else 'neg'
    zero = f'<span class="cbar-z" style="left:{z * 100:.1f}%"></span>' if lo < 0 < hi else ''
    return (f'<span class="cbar" aria-hidden="true">{zero}<span class="cbar-f {cls}" '
            f'style="left:{left:.1f}%;width:{w:.1f}%"></span></span>')


def table(columns: list[dict], rows: list[dict], *, num: int | None = None, caption: str | None = None,
          tid: str | None = None, sortable: bool = True, cls: str = '') -> str:
    """Data table: caption above, sticky header, right-aligned tabular numerals, optional in-cell bars.

    ``columns``: dicts ``key, label, kind ('num' | 'text' | 'mono' | 'chip' | 'html'), fmt (format spec or
    callable(value, row) -> html | None), bar ({'lo', 'hi'}), title, cls (extra cell class), na_title``. ``rows``: dicts keyed by column key;
    ``<key>__sort`` overrides the sort value, ``<key>__title`` adds a cell tooltip, ``_cls`` a row class, and
    ``_group`` makes a full-width group header row. Every cell carries ``data-sort``; the optional script sorts
    on click. None renders n/a.
    """
    kinds = {'num': 'n', 'text': 't', 'mono': 'm t', 'chip': 'c', 'html': 't'}
    ths = []
    for c in columns:
        k = c.get('kind', 'num')
        dt = 'num' if k == 'num' else 'text'
        tt = f' title="{esc(c["title"])}"' if c.get('title') else ''
        srt = f' data-type="{dt}" tabindex="0" aria-sort="none"' if sortable else ''
        ths.append(f'<th class="{kinds[k]}" scope="col"{srt}{tt}>{esc(c["label"])}</th>')
    trs = []
    for r in rows:
        if '_group' in r:
            trs.append(f'<tr class="grp"><th colspan="{len(columns)}" scope="colgroup">{esc(r["_group"])}</th></tr>')
            continue
        tds = []
        for c in columns:
            key, k = c['key'], c.get('kind', 'num')
            v = r.get(key)
            f = c.get('fmt', '.3f')
            if isinstance(v, str) and v == '' and k != 'text':
                inner = ''  # deliberately blank (not applicable), distinct from a missing input
            elif callable(f):
                inner = f(v, r)
            elif k == 'chip':
                inner = chip(v) if v is not None else None
            elif k == 'html':
                inner = v
            elif k == 'num':
                s = fmt(v, f)
                inner = None if s is None else esc(s)
            else:
                inner = None if v is None or v == '' else esc(v)
            if inner is None:
                inner = na(c.get('na_title', 'not available'))
            if c.get('bar') and is_num(v):
                inner = _cbar(v, c['bar']) + inner
            sv = r.get(f'{key}__sort', v)
            sattr = repr(float(sv)) if is_num(sv) else ('' if sv is None else re.sub(r'<[^>]+>', '', str(sv)))
            tt = r.get(f'{key}__title')
            tattr = f' title="{esc(tt)}"' if tt else ''
            tcls = kinds[k] + (f" {c['cls']}" if c.get('cls') else '')
            tds.append(f'<td class="{tcls}" data-sort="{esc(sattr)}"{tattr}>{inner}</td>')
        rc = f' class="{esc(r["_cls"])}"' if r.get('_cls') else ''
        trs.append(f'<tr{rc}>{"".join(tds)}</tr>')
    cap = ''
    if caption is not None:
        n = f'<span class="tbl-num">Table {num}.</span> ' if num is not None else ''
        cap = f'<p class="tbl-cap">{n}{esc(caption)}</p>'
    i = f' id="{esc(tid)}"' if tid else ''
    sc = ' sortable' if sortable else ''
    return (f'<div class="tbl-block"{i}>{cap}<div class="tbl-wrap"><table class="data{sc} {esc(cls)}">'
            f'<thead><tr>{"".join(ths)}</tr></thead><tbody>{"".join(trs)}</tbody></table></div></div>')


# ----------------------------------------------------------------------------------------------- validation
def validate_svg(fragment: str) -> list[str]:
    """Problems with one SVG fragment: XML parse, viewBox, explicit fill+stroke on every shape, token colours."""
    problems = []
    try:
        root = _ET.fromstring(fragment)
    except _ET.ParseError as e:
        return [f'xml: {e}']
    if not root.get('viewBox'):
        problems.append('svg without viewBox')
    for el in root.iter():
        tag = el.tag.split('}')[-1]
        style = el.get('style') or ''
        props = dict(p.split(':', 1) for p in style.split(';') if ':' in p)
        for attr in ('fill', 'stroke'):
            v = props.get(attr, el.get(attr))
            if tag in SHAPES and v is None:
                problems.append(f'{tag} without explicit {attr}')
            if v is not None and v != 'none' and not re.fullmatch(r'var\(--[A-Za-z0-9_-]+\)', v.strip()):
                problems.append(f'{tag} {attr} not a theme token: {v}')
    return problems


# ----------------------------------------------------------------------------------------------- self-tests
def _dates(n: int, start=_dt.date(2020, 1, 2)) -> list[_dt.date]:
    out, d = [], start
    while len(out) < n:
        if d.weekday() < 5:
            out.append(d)
        d += _dt.timedelta(days=1)
    return out


def _selftest_line_chart() -> str:
    x = _dates(400)
    a = [1 + 0.0004 * i + 0.01 * math.sin(i / 17) for i in range(400)]
    b = [1 + 0.0002 * i for i in range(400)]
    s = line_chart([{'name': 'a', 'x': x, 'y': a, 'color': 's2', 'width': 2.2, 'emph': True, 'end_label': 'a'},
                    {'name': 'b', 'x': x, 'y': b, 'color': 'ref', 'end_label': 'b'}], ref_values=(1.0,))
    assert '<path' in s and '2021' in s and s.count('<text') >= 6
    assert line_chart([{'name': 'x', 'x': [], 'y': [], 'color': 's2'}]).count(NA_TEXT) == 1
    return s


def _selftest_drawdown_chart() -> str:
    x = _dates(50)
    s = drawdown_chart([{'name': 'a', 'x': x, 'y': [-0.001 * (i % 20) for i in range(50)], 'color': 's2', 'trough': True}])
    assert MINUS + '1.9%' in s
    return s


def _selftest_bar_chart() -> str:
    s = bar_chart(['2020', '2021'], [{'name': 'a', 'color': 's2', 'values': [0.01, -0.02], 'label': True},
                                     {'name': 'b', 'color': 's1', 'values': [None, 0.03]}], value_fmt='+pct1')
    assert NA_TEXT in s and MINUS + '2.0%' in s
    h = bar_chart(['x', 'y'], [{'name': 'w', 'color': 's2', 'values': [0.5, 0.25]}], horizontal=True,
                  notes=['2 x 0.25', '1 x 0.25'], value_fmt='.2f')
    assert '2 x 0.25' in h
    return s + h


def _selftest_waterfall() -> str:
    s = waterfall({'label': 'start', 'value': 0.7}, [{'label': 'a', 'delta': 0.2, 'se': 0.1},
                                                    {'label': 'b', 'delta': -0.05, 'se': 0.02},
                                                    {'label': 'c', 'delta': None}], {'label': 'end', 'value': 0.85})
    assert '+0.200' in s and MINUS + '0.050' in s and NA_TEXT in s
    return s


def _selftest_scatter() -> str:
    s = scatter([{'x': 0.04, 'y': 12.0, 'size': 0.9, 'color': 'g-final', 'label': 'final', 'title': 't'},
                 {'x': 0.03, 'y': 11.0, 'size': 0.5, 'color': 'g-v5-grid', 'hollow': True}],
                x_label='tau', y_label='cost', size_label='SR')
    assert 'final' in s and 'SR' in s
    return s


def _selftest_dot_plot() -> str:
    s = dot_plot([{'group': 'g1', 'label': 'a', 'value': 1.5, 'filled': True, 'color': 'accent', 'cells': ['+1']},
                  {'group': 'g1', 'label': 'b', 'value': -2.5, 'filled': False, 'color': 'ref-2', 'cells': ['-1']},
                  {'group': 'g2', 'label': 'c', 'value': None, 'color': 'accent', 'cells': ['+1']}],
                 value_label='HAC t', columns=[('SIGN', 40)], ref_lines=[{'value': -2.0, 'label': 'veto'}])
    assert 'g2' in s and NA_TEXT in s and 'veto' in s
    return s


def _selftest_heatmap() -> str:
    s = heatmap(['2020', '2021'], ['Jan', 'Feb'], [[0.01, -0.03], [None, 0.002]], total=[-0.02, 0.002])
    assert 'div-p1' in s and 'div-n3' in s and 'div-0' in s
    return s


def _selftest_box_plot() -> str:
    s = box_plot([{'label': 'a', 'color': 'g-grid', 'stats': {'p5': .01, 'p25': .02, 'p50': .03, 'p75': .04, 'p95': .06, 'mean': .031, 'n': 10}},
                  {'label': 'b', 'color': 'g-grid', 'stats': None}], value_label='tau')
    assert NA_TEXT in s and '0.031' in s
    return s


def _selftest_kpi_strip() -> str:
    k = kpi_strip([{'label': 'Net SR', 'value': '+1.00', 'sub': 'S2'}, {'label': 'Missing', 'value': None}])
    assert 'kpi-val' in k and NA_TEXT in k
    return k


def _selftest_gate_panel() -> str:
    g = gate_panel([{'name': 'a', 'value': '1.0', 'threshold': '>= 1', 'passed': True},
                    {'name': 'b', 'value': '0.5', 'threshold': '>= 1', 'passed': False},
                    {'name': 'c', 'value': None, 'threshold': '>= 1', 'passed': None}])
    assert 'PASS' in g and 'FAIL' in g and NA_TEXT in g
    return g


def _selftest_xy_chart() -> str:
    s = xy_chart([{'name': 'net', 'color': 's2', 'points': [(0.5, 1.27, 'a'), (1, 1.24, None), (8, 0.91, None)],
                   'end_label': 'net'}, {'name': 'ko', 'color': 'flat', 'points': [(1, 1.27, 'ko')]}],
                 x_label='NAV', y_label='SR', x_fmt='g', y_fmt='.2f', x_log=True, x_ticks=[0.5, 1, 2, 4, 8],
                 y_refs=[{'value': 1.0, 'label': 'SR 1'}])
    assert '>8<' in s and '>0.5<' in s and 'SR 1' in s and 'ko' in s
    assert xy_chart([{'name': 'x', 'color': 's2', 'points': [(0, 1.0)]}], x_label='a', y_label='b',
                    x_log=True).count(NA_TEXT) == 1
    return s


def _selftest_badge() -> str:
    b = verdict_html('REJECTED (dSR -2.18; defects)') + verdict_html('ACCEPTED') + verdict_html('v5 grid')
    assert 'chip warn' in b and '>ACCEPTED<' in b and 'v5 grid' in b and b.count('class="sub"') == 1
    assert verdict_kind('accepted (sign)') == 'accepted' and verdict_kind('') is None
    assert verdict_kind('not adopted', [('rejected', 'not adopted')]) == 'rejected'
    assert badge('alarm').startswith('<span class="chip fail">') and NA_TEXT in badge(None)
    return b


def _selftest_table() -> str:
    t = table([{'key': 'x', 'label': 'X', 'fmt': '+.2f', 'bar': {'lo': -1, 'hi': 1}},
               {'key': 'y', 'label': 'Y', 'kind': 'text'}, {'key': 'z', 'label': 'Z', 'kind': 'chip'}],
              [{'x': 0.5, 'y': 'a', 'z': True, '_cls': 'hl'}, {'_group': 'g'}, {'x': None, 'y': None, 'z': None}],
              num=1, caption='cap')
    assert 'Table 1.' in t and 'data-sort="0.5"' in t and 'cbar-f pos' in t and t.count(NA_TEXT) == 3
    return t


def _selftest_figure() -> str:
    f = figure(1, empty_svg(), 'caption', legend_html=legend([{'name': 'a', 'color': 's2'},
                                                               {'name': 'b', 'color': 'ref', 'kind': 'dot'}]))
    assert 'Figure 1.' in f and '<figcaption>' in f
    return f


def _selftest_section() -> str:
    s = section(2, 'Equity', '<p class="x"></p>', 'equity')
    assert 'sec-num">2<' in s and 'id="equity"' in s
    assert fmt(-0.0123, '+pct1') == MINUS + '1.2%' and fmt(None) is None and fmt(float('nan')) is None
    assert ticks_in(0.93, 1.21, 6)[0] >= 0.93 and ticks_in(0.93, 1.21, 6)[-1] <= 1.21
    assert declutter([10, 11, 12], 0, 100, 10) == [1.0, 11.0, 21.0]
    return s


def self_test() -> dict[str, bool]:
    """Run every component self-test; each SVG in the output must pass ``validate_svg``."""
    results = {}
    for name, fn in sorted(globals().items()):
        if name.startswith('_selftest_'):
            out = fn()
            svgs = re.findall(r'<svg\b.*?</svg>', out, flags=re.S)
            problems = [p for s in svgs for p in validate_svg(s)]
            assert not problems, (name, problems[:5])
            results[name[len('_selftest_'):]] = True
    return results


if __name__ == '__main__':
    print(self_test())
