"""Prose engine of the pitch report: paragraphs keyed by section id (config ``narrative``), with placeholders
filled from the data at build time so no number in the prose can go stale.

Placeholder grammar ``{expr|spec}`` (spec optional, default ``g``):
- ``summ.x`` / ``sum.x`` / ``scen[:KEY].x`` / ``lo.x`` / ``parent.x`` / ``rec.x`` / ``recsc:KEY.x``: a metric of the
  final cell (``Ctx.metric``);
- ``@CELL:path``: the same metric paths on another configured cell (name without the cell prefix);
- ``a:NAME.path``: an analysis result (``pitch.ANALYSES``), e.g. ``a:e2e.dsr``;
- ``j:KEY.path``: a JSON input declared in ``inputs.extra_json``; ``n:KEY:CELL.path``: a nav_summ-style row list
  (``extra_json`` KEY) looked up by the cell's directory basename;
- ``cfg.path``: a config value.
Spec: any ``components.fmt`` spec, plus ``bps<d>`` (x 1e4, ' bps'), ``x100:<spec>`` and ``date`` / ``str``.
Inline markup in literal text: ``code`` and **bold**. An unresolved placeholder renders an explicit n/a and is
recorded in ``ctx.unresolved`` (the CLI prints the count).
"""
from __future__ import annotations

import datetime as dt
import re

from . import components as C
from . import data as D

PH = re.compile(r'\{([^{}|]+?)(?:\|([^{}]+))?\}')
CODE = re.compile(r'`([^`]+)`')
BOLD = re.compile(r'\*\*([^*]+)\*\*')


def nfmt(v, spec: str | None):
    spec = spec or 'g'
    if v is None:
        return None
    if isinstance(v, (dt.date, dt.datetime)):
        return v.isoformat()
    if isinstance(v, bool):
        return 'yes' if v else 'no'
    if isinstance(v, (list, tuple)):
        parts = [nfmt(x, spec) for x in v]
        return None if any(p is None for p in parts) else ', '.join(parts)
    if isinstance(v, str):
        return v
    if spec.startswith('bps'):
        d = int(spec[3:] or 1)
        return None if not C.is_num(v) else C.fmt(v * 1e4, f'.{d}f') + ' bps'
    if spec.startswith('x100:'):
        return C.fmt(v * 100 if C.is_num(v) else v, spec[5:])
    if spec == 'd' and C.is_num(v):
        return C.fmt(int(round(v)), 'd')
    return C.fmt(v, spec)


def resolve(ctx, expr: str):
    expr = expr.strip()
    if expr.startswith('a:'):
        name, _, path = expr[2:].partition('.')
        res = ctx.analysis(name)
        return D.dig(res, path) if path else res
    if expr.startswith('@'):
        cell, _, path = expr[1:].partition(':')
        return ctx.metric(ctx.by.get(cell), path)
    if expr.startswith('j:'):
        key, _, path = expr[2:].partition('.')
        return D.dig(ctx.extra_json(key), path)
    if expr.startswith('n:'):
        key, _, rest = expr[2:].partition(':')
        cell, _, path = rest.partition('.')
        return D.dig(ctx.nsumm_row(key, cell), path)
    if expr.startswith('cfg.'):
        return D.dig(ctx.cfg, expr[4:])
    return ctx.metric(ctx.final, expr)


def _inline(s: str) -> str:
    s = C.esc(s)
    s = CODE.sub(lambda m: f'<code>{m.group(1)}</code>', s)
    return BOLD.sub(lambda m: f'<strong>{m.group(1)}</strong>', s)


def fill_html(ctx, text: str) -> str:
    """Escape the literal text (with inline markup) and substitute every placeholder by its formatted value."""
    if text is None:
        return ''
    out, pos = [], 0
    for m in PH.finditer(text):
        out.append(_inline(text[pos:m.start()]))
        try:
            v = nfmt(resolve(ctx, m.group(1)), m.group(2))
        except Exception as e:  # noqa: BLE001 - an unresolved value renders n/a
            v = None
            ctx.unresolved.append(f'{m.group(0)} ({type(e).__name__})')
        if v is None:
            if not any(u.startswith(m.group(0)) for u in ctx.unresolved):
                ctx.unresolved.append(m.group(0))
            out.append(C.na(f'unresolved {m.group(1)}'))
        else:
            out.append(C.esc(v))
        pos = m.end()
    out.append(_inline(text[pos:]))
    return ''.join(out)


def fill_text(ctx, text: str) -> str:
    """Plain-text substitution (for captions and SVG labels): unresolved -> 'n/a'."""
    def sub(m):
        try:
            v = nfmt(resolve(ctx, m.group(1)), m.group(2))
        except Exception:  # noqa: BLE001
            v = None
        if v is None:
            ctx.unresolved.append(m.group(0))
            return C.NA_TEXT
        return v
    return PH.sub(sub, text or '')


def paragraphs(ctx, items) -> str:
    """A list of paragraphs: str -> <p>; {"list": [...], "ordered": bool} -> a list; {"h": text} -> sub-head."""
    parts = []
    for it in items or []:
        if isinstance(it, str):
            parts.append(f'<p>{fill_html(ctx, it)}</p>')
        elif isinstance(it, dict) and 'list' in it:
            tag = 'ol' if it.get('ordered') else 'ul'
            lis = ''.join(f'<li>{fill_html(ctx, x)}</li>' for x in it['list'])
            parts.append(f'<{tag}>{lis}</{tag}>')
        elif isinstance(it, dict) and 'h' in it:
            parts.append(f'<h3 class="sub">{fill_html(ctx, it["h"])}</h3>')
    return f'<div class="prose">{"".join(parts)}</div>' if parts else ''


def check_list(ctx, items) -> str:
    if not items:
        return ''
    lis = ''.join(f'<li>{fill_html(ctx, x)}</li>' for x in items)
    return f'<div class="check"><p class="panel-title">What to check</p><ul>{lis}</ul></div>'


def section_intro(ctx, sec_id: str) -> str:
    """The section's opening prose and its "what to check" list (config ``narrative[sec_id]``)."""
    nar = (ctx.cfg.get('narrative') or {}).get(sec_id)
    if not nar:
        return ''
    return paragraphs(ctx, nar.get('paragraphs')) + check_list(ctx, nar.get('check'))
