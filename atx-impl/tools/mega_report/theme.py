"""Theme tokens, stylesheet and the optional script (table sort + theme toggle) of the report.

Light palette on bare ``:root``; the dark palette redefines the same tokens under
``@media (prefers-color-scheme: dark) :root:not([data-theme="light"])`` and under ``:root[data-theme="dark"]``.
Categorical slots were checked with the dataviz palette validator (all-pairs CVD and normal-vision separation);
the deep navy accent and the neutral greys sit outside its chroma band by design and always carry a text label.
"""
from __future__ import annotations

FONTS_HREF = ('https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:'
              'wght@400;500;600&family=Source+Serif+4:opsz,wght@8..60,400;8..60,600&display=swap')

LIGHT = {
    'bg': '#FBFCFD', 'bg-2': '#F1F3F6', 'hl': '#EEF2F8', 'fg': '#18202B', 'fg-2': '#465262', 'fg-3': '#6A7584',
    'rule': '#C9D0D9', 'rule-2': '#E3E7EC', 'grid': '#E8EBEF', 'axis': '#8E98A5',
    'accent': '#1F3A5F', 's2': '#1F3A5F', 's1': '#2E8C87', 's3': '#8E2C2C', 'flat': '#C0802A', 'tiers': '#7C5FB6',
    'ref': '#A2A9B2', 'ref-2': '#6B7480', 'pos': '#1F3A5F', 'neg': '#8E2C2C',
    'pass': '#2E7D4F', 'pass-bg': '#E4F1E8', 'fail': '#B3261E', 'fail-bg': '#F8E4E2',
    'div-n3': '#8E2C2C', 'div-n2': '#C4746D', 'div-n1': '#EBCBC7', 'div-0': '#ECEFF3',
    'div-p1': '#C9D6E8', 'div-p2': '#7C98C0', 'div-p3': '#1F3A5F', 'hm-ink-3': '#FFFFFF', 'hm-ink-2': '#FFFFFF',
}
DARK = {
    'bg': '#13171D', 'bg-2': '#1B2028', 'hl': '#1B2533', 'fg': '#E3E7ED', 'fg-2': '#AEB7C3', 'fg-3': '#8791A0',
    'rule': '#353D49', 'rule-2': '#262D37', 'grid': '#212830', 'axis': '#6C7684',
    'accent': '#8FB3E6', 's2': '#8FB3E6', 's1': '#3A9E86', 's3': '#D2605A', 'flat': '#E3B566', 'tiers': '#8C76BE',
    'ref': '#5B636E', 'ref-2': '#8A93A0', 'pos': '#8FB3E6', 'neg': '#D2605A',
    'pass': '#6FCF97', 'pass-bg': '#16301F', 'fail': '#F28B82', 'fail-bg': '#3A1D1B',
    'div-n3': '#D2605A', 'div-n2': '#93453F', 'div-n1': '#4A2C2B', 'div-0': '#20262E',
    'div-p1': '#273A55', 'div-p2': '#4C6FA0', 'div-p3': '#8FB3E6', 'hm-ink-3': '#10141A', 'hm-ink-2': '#F0F3F7',
}
DERIVED = {  # follow the base tokens in both themes
    'g-final': 'var(--s2)', 'g-parent': 'var(--s1)', 'g-grid': 'var(--flat)', 'g-conditional': 'var(--tiers)',
    'g-rejected': 'var(--s3)', 'g-v5-reference': 'var(--ref-2)', 'g-v5-grid': 'var(--ref)',
    'font-serif': "'Source Serif 4', 'Source Serif Pro', Georgia, 'Times New Roman', serif",
    'font-sans': "'IBM Plex Sans', 'Helvetica Neue', Helvetica, Arial, system-ui, sans-serif",
    'font-mono': "'IBM Plex Mono', ui-monospace, SFMono-Regular, Menlo, Consolas, 'Liberation Mono', monospace",
}


def _block(tokens: dict) -> str:
    return ''.join(f'--{k}:{v};' for k, v in tokens.items())


BASE_CSS = """
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 var(--font-sans)}
.page{max-width:1120px;margin:0 auto;padding:40px 24px 72px}
a{color:var(--accent)}
.lbl-up,.meta dt,.kpi dt,.panel-title,.panel-label,.eyebrow{font:500 10.5px/1.4 var(--font-sans);text-transform:uppercase;letter-spacing:.08em;color:var(--fg-3)}
.doc-head{padding-bottom:18px;border-bottom:1px solid var(--rule)}
.doc-head .row{display:flex;justify-content:space-between;align-items:flex-start;gap:16px;flex-wrap:wrap}
h1{font:600 30px/1.2 var(--font-serif);margin:6px 0 16px;letter-spacing:-.005em}
h2{font:600 24px/1.25 var(--font-serif);margin:44px 0 6px;padding-top:18px;border-top:1px solid var(--rule)}
h2 .sec-num{font:500 18px/1 var(--font-mono);color:var(--accent);margin-right:14px}
h3{font:600 18px/1.3 var(--font-serif);margin:28px 0 8px}
.meta{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px 28px;margin:0}
.meta dd{margin:3px 0 0;font:12.5px/1.4 var(--font-mono);color:var(--fg);overflow-wrap:anywhere}
details.files{margin-top:16px;border-top:1px solid var(--rule-2);padding-top:10px}
details.files summary{cursor:pointer;font:500 10.5px/1.4 var(--font-sans);text-transform:uppercase;letter-spacing:.08em;color:var(--fg-2)}
nav.toc{display:flex;flex-wrap:wrap;gap:6px 22px;padding:12px 0;border-bottom:1px solid var(--rule-2);font:13px/1.4 var(--font-sans)}
nav.toc a{color:var(--fg-2);text-decoration:none}
nav.toc a:hover,nav.toc a:focus{color:var(--accent);text-decoration:underline}
nav.toc .n{font-family:var(--font-mono);color:var(--accent);margin-right:6px}
.theme-btn{font:500 10.5px/1 var(--font-sans);text-transform:uppercase;letter-spacing:.08em;color:var(--fg-2);background:var(--bg);border:1px solid var(--rule);border-radius:0;padding:7px 10px;cursor:pointer}
.theme-btn:hover{color:var(--accent);border-color:var(--accent)}
.kpi-strip{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));margin:18px 0 0;border-top:1px solid var(--rule);border-bottom:1px solid var(--rule)}
.kpi{padding:12px 16px 12px 0;border-bottom:1px solid var(--rule-2)}
.kpi-val{margin:5px 0 0;font:500 21px/1.2 var(--font-mono);color:var(--fg)}
.kpi-sub{margin:3px 0 0;font:11.5px/1.3 var(--font-sans);color:var(--fg-3)}
.panel{margin:28px 0 0}
.panel-title{margin:0 0 8px}
.panel-label{margin:18px 0 6px}
figure.fig{margin:32px 0 36px}
.fig-body svg.chart{display:block;width:100%;height:auto;max-width:100%}
.fig-body .panel-label:first-child{margin-top:0}
figcaption{text-align:center;font:14px/1.45 var(--font-serif);color:var(--fg-2);margin:12px auto 0;max-width:900px}
.fig-num,.tbl-num{font-weight:600;color:var(--fg)}
.tbl-block{margin:32px 0 0}
.tbl-cap{font:14px/1.45 var(--font-serif);color:var(--fg-2);margin:0 0 8px}
.tbl-wrap{overflow-x:auto;max-width:100%;border-top:1px solid var(--rule);border-bottom:1px solid var(--rule)}
.tbl-wrap.tall{max-height:640px;overflow-y:auto}
table{border-collapse:collapse}
table.data,table.gates{width:100%;font:12.5px/1.35 var(--font-sans)}
table.data th,table.gates th{position:sticky;top:0;z-index:1;background:var(--bg);font:600 10px/1.3 var(--font-sans);text-transform:uppercase;letter-spacing:.07em;color:var(--fg-2);padding:8px 10px;border-bottom:1px solid var(--rule);white-space:nowrap;vertical-align:bottom;text-align:left}
table.sortable th[data-type]{cursor:pointer}
table.sortable th[aria-sort=ascending]::after{content:" \\2191"}
table.sortable th[aria-sort=descending]::after{content:" \\2193"}
table.data td,table.gates td{padding:5px 10px;border-bottom:1px solid var(--rule-2);vertical-align:top;white-space:nowrap}
table.data tbody tr:last-child td,table.gates tbody tr:last-child td{border-bottom:0}
th.n,td.n{text-align:right;font-variant-numeric:tabular-nums}
td.m,.m{font-family:var(--font-mono);font-size:12px;font-variant-numeric:tabular-nums}
td.c,th.c{text-align:left}
td.wrap{white-space:normal;min-width:260px}
td.dsl{white-space:normal;min-width:460px}
code.dsl{display:block;font:11.5px/1.5 var(--font-mono);white-space:pre-wrap;overflow-wrap:anywhere;color:var(--fg)}
pre.quote{margin:0;font:11.5px/1.5 var(--font-mono);white-space:pre-wrap;overflow-wrap:anywhere;color:var(--fg)}
tr.hl td{background:var(--hl)}
tr.grp th{position:static;text-align:left;background:var(--bg-2);color:var(--fg);padding:7px 10px;border-bottom:1px solid var(--rule)}
.sub{display:block;font:10.5px/1.35 var(--font-mono);color:var(--fg-3)}
.na{color:var(--fg-3);font-style:italic}
.chip{display:inline-block;padding:1px 8px;border-radius:999px;font:600 10px/1.7 var(--font-sans);letter-spacing:.07em}
.chip.pass{color:var(--pass);background:var(--pass-bg)}
.chip.fail{color:var(--fail);background:var(--fail-bg)}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:7px;vertical-align:baseline;background:var(--c);border:1.5px solid var(--c)}
.dot.hollow{background:transparent}
.cbar{position:relative;display:inline-block;width:48px;height:7px;margin-right:8px;vertical-align:middle;background:var(--bg-2)}
.cbar-f{position:absolute;top:0;bottom:0}
.cbar-f.pos{background:var(--pos)}
.cbar-f.neg{background:var(--neg)}
.cbar-z{position:absolute;top:-2px;bottom:-2px;width:1px;background:var(--axis)}
.legend{list-style:none;display:flex;flex-wrap:wrap;gap:4px 20px;margin:0 0 10px;padding:0;font:12px/1.5 var(--font-sans);color:var(--fg-2)}
.legend li{display:inline-flex;align-items:center;gap:7px}
.legend svg.key{width:22px;height:12px;flex:none}
.lg-val{font-family:var(--font-mono);color:var(--fg)}
.unavailable{margin:24px 0;padding:10px 0;border-top:1px solid var(--rule);border-bottom:1px solid var(--rule);color:var(--fg-3);font-style:italic}
svg.chart text{font-family:var(--font-sans)}
.t-tick{font-size:11px;fill:var(--fg-3);font-variant-numeric:tabular-nums}
.t-axis{font-size:10px;fill:var(--fg-2);text-transform:uppercase;letter-spacing:.08em;font-weight:500}
.t-lbl{font-size:12px;fill:var(--fg)}
.t-lbl-2{font-size:11px;fill:var(--fg-2)}
.t-end{font-size:11.5px;fill:var(--fg);font-variant-numeric:tabular-nums}
.t-val{font-size:10.5px;fill:var(--fg-2);font-variant-numeric:tabular-nums}
.t-grp{font-size:10px;fill:var(--fg);text-transform:uppercase;letter-spacing:.08em;font-weight:600}
svg.chart .t-mono{font-family:var(--font-mono);font-size:11px;fill:var(--fg)}
.t-na{font-size:12px;fill:var(--fg-3);font-style:italic}
.t-hm{font-size:11px;fill:var(--fg);font-variant-numeric:tabular-nums}
.t-hm-n3,.t-hm-p3{fill:var(--hm-ink-3)}
.t-hm-n2,.t-hm-p2{fill:var(--hm-ink-2)}
.t-hm-tot{font-size:11.5px;font-weight:600;fill:var(--fg);font-variant-numeric:tabular-nums}
@media (max-width:640px){
 .page{padding:24px 16px 48px}
 h1{font-size:24px}
 h2{font-size:20px}
 .kpi-val{font-size:18px}
}
@media (prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
@media print{.theme-btn{display:none}.tbl-wrap{overflow:visible}table.data th{position:static}}
"""


def css() -> str:
    """Complete stylesheet: light tokens on :root, dark tokens in both dark selectors, then the base rules."""
    light = f':root{{{_block(LIGHT)}{_block(DERIVED)}color-scheme:light}}'
    dark_tokens = _block(DARK)
    dark = (f'@media (prefers-color-scheme:dark){{:root:not([data-theme="light"]){{{dark_tokens}color-scheme:dark}}}}'
            f':root[data-theme="dark"]{{{dark_tokens}color-scheme:dark}}'
            f':root[data-theme="light"]{{color-scheme:light}}')
    return light + dark + BASE_CSS


SCRIPT = r"""
(function () {
  var root = document.documentElement, KEY = 'mega-report-theme', btn = document.getElementById('theme-btn');
  function apply(t) {
    if (t === 'light' || t === 'dark') { root.setAttribute('data-theme', t); } else { root.removeAttribute('data-theme'); }
    if (btn) { btn.textContent = 'Theme: ' + (t || 'system'); }
  }
  var saved = null;
  try { saved = window.localStorage.getItem(KEY); } catch (e) { saved = null; }
  apply(saved);
  if (btn) {
    btn.hidden = false;
    btn.addEventListener('click', function () {
      var cur = root.getAttribute('data-theme');
      var next = cur === null ? 'light' : (cur === 'light' ? 'dark' : null);
      apply(next);
      try { if (next) { window.localStorage.setItem(KEY, next); } else { window.localStorage.removeItem(KEY); } } catch (e) { }
    });
  }
  Array.prototype.forEach.call(document.querySelectorAll('table.sortable'), function (tb) {
    if (!tb.tHead || !tb.tBodies.length) { return; }
    var ths = tb.tHead.rows[0].cells;
    Array.prototype.forEach.call(ths, function (th, ci) {
      if (!th.hasAttribute('data-type')) { return; }
      function go() {
        var body = tb.tBodies[0], rows = Array.prototype.slice.call(body.rows);
        var dir = th.getAttribute('aria-sort') === 'ascending' ? 'descending' : 'ascending';
        Array.prototype.forEach.call(ths, function (o) { if (o.hasAttribute('aria-sort')) { o.setAttribute('aria-sort', 'none'); } });
        th.setAttribute('aria-sort', dir);
        var num = th.getAttribute('data-type') === 'num', s = dir === 'ascending' ? 1 : -1;
        rows.sort(function (a, b) {
          var x = a.cells[ci] ? a.cells[ci].getAttribute('data-sort') || '' : '';
          var y = b.cells[ci] ? b.cells[ci].getAttribute('data-sort') || '' : '';
          if (num) {
            var fx = parseFloat(x), fy = parseFloat(y), nx = isNaN(fx), ny = isNaN(fy);
            if (nx && ny) { return 0; } if (nx) { return 1; } if (ny) { return -1; }
            return (fx - fy) * s;
          }
          return x.localeCompare(y) * s;
        });
        rows.forEach(function (r) { body.appendChild(r); });
      }
      th.addEventListener('click', go);
      th.addEventListener('keydown', function (e) { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); go(); } });
    });
  });
})();
"""
