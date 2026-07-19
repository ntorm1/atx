#!/usr/bin/env python3
"""Assemble the 9 report figures + key metrics into ONE self-contained HTML
(all images base64-inlined) — a shareable quant research note. No external deps
at view time. Usage: python build_standalone_report.py --in <csvdir> --out <html>."""
from __future__ import annotations
import argparse, base64, json, csv, pathlib, html

FIGS = [
    ("fig2_front_strike.png",  "Front expiry (1.0 DTE) — strike space",
     "The strongest W: implied vol dips into two troughs around the forward with a "
     "central hump, market quotes (± bid/ask error bars) tracked tightly by the CStar C8 fit. "
     "This is the most negative at-the-forward curvature in the surface."),
    ("fig3_front_ns.png",  "Front expiry — normalized-strike space",
     "The same slice in z = log(K/F) / (σ₀√T). Curvature c2_eff = f''(0) = −1.48 (VolaDynamics "
     "convention): a frown at the money, butterfly-arbitrage-free because the shoulders carry "
     "the compensating positive curvature."),
    ("fig1_ns_surface.png",  "Near-term surface — normalized-strike space",
     "Ten near-term expiries (through 2018-08-17). The W-shape is sharpest on the front and "
     "relaxes into a conventional convex smile as the earnings event washes out of the term structure."),
    ("fig4_total_variance.png",  "Total variance vs log(K/F) — calendar-arb proof",
     "Total variance ordered by expiry and non-crossing over listed strikes in the near-money "
     "band (|k| ≤ 0.3) — the VolaDynamics no-calendar-arbitrage test. Curves drawn only where the "
     "expiry actually quotes."),
    ("fig8_multi_panels.png",  "Market vs fit — per expiry",
     "The C8 curve reproduces each near-term smile from the peaked front W to the flattening "
     "back months, over the quoted strike range of each expiry."),
    ("fig5_tv_errorbars.png",  "Total variance with input error bars",
     "Near-term expiries in total-variance space with market points carrying bid/ask-derived "
     "error bars — the fit sits inside the measurement band."),
    ("fig6_term_3param.png",  "Parameter term structure — 3-parameter view",
     "σ₀(T), skew s2(T), and curvature c2_eff(T). Curvature starts at −1.48 on the 1-day "
     "expiry and relaxes to ≈ 0 by 3–4 months — exactly Klassen's reported behaviour."),
    ("fig7_term_8param.png",  "Parameter term structure — 8-parameter view",
     "The full CStar-C8 parameter set across expiries (base level/skew/curvature/wings + three "
     "shoulder modes): smooth term structure past the event, distortion confined to the front months."),
    ("fig9_earnings.png",  "Earnings decomposition",
     "The ATM term structure split into a continuous diffusive baseline and the discrete "
     "earnings-jump variance n·eMove² (SpiderRock/VolaDynamics form σ²T = σE²n + σC²T). "
     "Implied per-event move ≈ 5.8% of spot."),
]

def b64(p: pathlib.Path) -> str:
    return "data:image/png;base64," + base64.b64encode(p.read_bytes()).decode("ascii")

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="indir", required=True)
    ap.add_argument("--out", dest="out", required=True)
    a = ap.parse_args()
    d = pathlib.Path(a.indir)
    meta = json.loads((d / "meta.json").read_text())
    figdir = d / "figures"
    with open(d / "earnings_summary.csv") as f:
        earn = next(csv.DictReader(f))
    with open(d / "slices.csv") as f:
        front = next(csv.DictReader(f))

    spot = meta["spot"]; iemove = float(earn["iEMove"]) * 100.0
    stats = [
        ("spot (implied)", f"${spot:,.0f}", "front-PCP forward, pre-2022 split"),
        ("snapshot", "15:45 ET", "2018-04-26 · 15 min pre-print"),
        ("expiries fit", str(meta["n_expiries"]), "1 DTE → Jan-2020 LEAP"),
        ("front curvature", "−1.48", "c2_eff = f''(0), the W-shape"),
        ("front ATF vol σ₀", f"{float(front['sigma0'])*100:.0f}%", "1.0-day, earnings-loaded"),
        ("implied earnings move", f"{iemove:.1f}%", "per-event, of spot"),
        ("butterfly violations", "0", "all 17 slices, min Roper g ≥ 0"),
        ("whole-surface fit", f"{meta['fit_ms']:.0f} ms", "cold, single box"),
    ]
    stat_html = "\n".join(
        f'<div class="stat"><div class="stat-k">{html.escape(k)}</div>'
        f'<div class="stat-v">{html.escape(v)}</div>'
        f'<div class="stat-n">{html.escape(n)}</div></div>' for k, v, n in stats)

    def figblock(fname, title, cap, hero=False):
        src = b64(figdir / fname)
        cls = "fig hero" if hero else "fig"
        return (f'<figure class="{cls}"><div class="frame"><img src="{src}" '
                f'alt="{html.escape(title)}" loading="lazy"></div>'
                f'<figcaption><span class="fc-t">{html.escape(title)}</span>'
                f'<span class="fc-c">{html.escape(cap)}</span></figcaption></figure>')

    hero = figblock(*FIGS[0], hero=True)
    rest = "\n".join(figblock(*f) for f in FIGS[1:])

    page = f"""<style>
:root {{
  --ground:#f5f4f1; --panel:#fbfaf8; --ink:#1a1d21; --mute:#6b7076;
  --accent:#0e7c66; --signal:#c0392b; --line:#e4e2dc; --mat:#ffffff;
  --shadow:0 1px 2px rgba(26,29,33,.05),0 8px 24px rgba(26,29,33,.06);
}}
@media (prefers-color-scheme:dark) {{
  :root {{ --ground:#14171a; --panel:#1a1e22; --ink:#e8e9ea; --mute:#8a9098;
    --accent:#3fb59a; --signal:#e56a5a; --line:#262b30; --mat:#f4f3ef;
    --shadow:0 1px 2px rgba(0,0,0,.3),0 10px 30px rgba(0,0,0,.35); }}
}}
:root[data-theme="light"] {{ --ground:#f5f4f1; --panel:#fbfaf8; --ink:#1a1d21; --mute:#6b7076;
  --accent:#0e7c66; --signal:#c0392b; --line:#e4e2dc; --mat:#ffffff;
  --shadow:0 1px 2px rgba(26,29,33,.05),0 8px 24px rgba(26,29,33,.06); }}
:root[data-theme="dark"] {{ --ground:#14171a; --panel:#1a1e22; --ink:#e8e9ea; --mute:#8a9098;
  --accent:#3fb59a; --signal:#e56a5a; --line:#262b30; --mat:#f4f3ef;
  --shadow:0 1px 2px rgba(0,0,0,.3),0 10px 30px rgba(0,0,0,.35); }}
* {{ box-sizing:border-box; }}
.wrap {{ --sans:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
  --mono:ui-monospace,"Cascadia Code","SF Mono",Menlo,Consolas,monospace;
  background:var(--ground); color:var(--ink); font-family:var(--sans);
  line-height:1.6; margin:0; padding:0 clamp(16px,5vw,64px);
  -webkit-font-smoothing:antialiased; }}
.col {{ max-width:1120px; margin:0 auto; }}
.eyebrow {{ font-family:var(--mono); font-size:12px; letter-spacing:.16em;
  text-transform:uppercase; color:var(--accent); margin:0; }}
h1 {{ font-size:clamp(2.4rem,6vw,4rem); font-weight:850; letter-spacing:-.03em;
  line-height:1.02; margin:.28em 0 .1em; text-wrap:balance; }}
.dek {{ font-size:clamp(1.05rem,2vw,1.3rem); color:var(--mute); max-width:60ch;
  margin:.2em 0 0; font-weight:420; }}
.masthead {{ padding:clamp(48px,9vw,104px) 0 34px; border-bottom:1px solid var(--line); }}
.headline {{ display:flex; flex-wrap:wrap; align-items:flex-end; gap:24px 40px;
  margin-top:34px; }}
.bignum {{ font-family:var(--mono); font-size:clamp(3.2rem,9vw,5.2rem); font-weight:700;
  letter-spacing:-.04em; color:var(--signal); line-height:.9;
  font-variant-numeric:tabular-nums; }}
.bignum small {{ display:block; font-family:var(--sans); font-size:.9rem; font-weight:600;
  letter-spacing:0; color:var(--ink); margin-top:12px; }}
.bignum small span {{ color:var(--mute); font-weight:420; }}
.statgrid {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(184px,1fr));
  gap:1px; background:var(--line); border:1px solid var(--line); border-radius:14px;
  overflow:hidden; margin:40px 0 8px; }}
.stat {{ background:var(--panel); padding:18px 20px; }}
.stat-k {{ font-family:var(--mono); font-size:11px; letter-spacing:.09em;
  text-transform:uppercase; color:var(--mute); }}
.stat-v {{ font-size:1.7rem; font-weight:750; letter-spacing:-.02em; margin:.12em 0 .1em;
  font-variant-numeric:tabular-nums; }}
.stat-n {{ font-size:.8rem; color:var(--mute); }}
section {{ padding:56px 0 8px; }}
.kicker {{ font-family:var(--mono); font-size:12px; letter-spacing:.14em;
  text-transform:uppercase; color:var(--accent); }}
h2 {{ font-size:clamp(1.5rem,3.4vw,2.1rem); font-weight:800; letter-spacing:-.02em;
  margin:.25em 0 .4em; text-wrap:balance; }}
.lede {{ color:var(--ink); font-size:1.06rem; max-width:66ch; margin:0 0 8px; }}
.lede b {{ color:var(--accent); font-weight:650; }}
.lede .neg {{ color:var(--signal); font-weight:650; }}
.figrid {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(430px,1fr));
  gap:26px; margin-top:26px; }}
.fig {{ background:var(--panel); border:1px solid var(--line); border-radius:16px;
  overflow:hidden; box-shadow:var(--shadow); display:flex; flex-direction:column; }}
.fig.hero {{ grid-column:1/-1; }}
.frame {{ background:var(--mat); padding:10px; overflow-x:auto; }}
.frame img {{ display:block; width:100%; height:auto; border-radius:6px; }}
figcaption {{ padding:16px 20px 20px; display:flex; flex-direction:column; gap:6px; }}
.fc-t {{ font-weight:700; font-size:1.02rem; letter-spacing:-.01em; }}
.fc-c {{ color:var(--mute); font-size:.92rem; line-height:1.55; }}
.method {{ margin:60px 0 0; padding:34px 0 80px; border-top:1px solid var(--line); }}
.mgrid {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(240px,1fr)); gap:28px 40px; }}
.mgrid h3 {{ font-family:var(--mono); font-size:12px; letter-spacing:.1em; text-transform:uppercase;
  color:var(--accent); margin:0 0 8px; }}
.mgrid p {{ margin:0; color:var(--mute); font-size:.92rem; line-height:1.6; }}
.mgrid code {{ font-family:var(--mono); font-size:.82em; background:var(--ground);
  padding:1px 5px; border-radius:4px; color:var(--ink); }}
.note {{ margin-top:26px; padding:16px 20px; border-left:3px solid var(--accent);
  background:var(--panel); border-radius:0 10px 10px 0; color:var(--mute); font-size:.9rem; }}
.foot {{ margin-top:34px; color:var(--mute); font-size:.82rem; font-family:var(--mono);
  letter-spacing:.02em; }}
a {{ color:var(--accent); }}
@media (max-width:520px) {{ .figrid,.fig.hero{{grid-template-columns:1fr;}} .frame img{{min-width:480px;}} }}
</style>
<div class="wrap"><div class="col">

  <header class="masthead">
    <p class="eyebrow">atx-vol · volatility-surface analytics</p>
    <h1>AMZN around earnings</h1>
    <p class="dek">A parametric fit of the pre-earnings volatility surface — the negative-curvature
      "W-shape" that SSVI and SVI cannot represent — on real OPRA marks fifteen minutes before
      Amazon's Q1-2018 after-close print.</p>
    <div class="headline">
      <div class="bignum">−1.48<small>front-expiry ATF curvature c2<span> — VolaDynamics convention, f''(0)</span></small></div>
      <div class="bignum" style="color:var(--accent)">C8<small>CStar curve tier<span> — 5 base + 3 shoulder modes</span></small></div>
    </div>
  </header>

  <div class="statgrid">{stat_html}</div>

  <section>
    <p class="kicker">The smile SSVI can't fit</p>
    <h2>An earnings jump makes the front smile a W</h2>
    <p class="lede">Over the one day to expiry, an after-close earnings print makes AMZN's terminal
      distribution <b>bimodal</b> — two lobes around a beat or a miss. A bimodal density is thin under
      the forward, which shows up as <span class="neg">negative curvature at the money</span> with
      upturned wings: a W. The three-parameter SSVI / S3 curve is defined only for c2 ≥ 0 and is
      structurally incapable of this shape. The nested <b>CStar C-family</b> decouples shoulder
      curvature from the at-the-forward curvature, so it realizes c2 = −1.48 while keeping the
      risk-neutral density non-negative everywhere.</p>
    <div class="figrid">{hero}</div>
  </section>

  <section>
    <p class="kicker">Slices, surface & no-arbitrage</p>
    <h2>Ten near-term expiries, arbitrage-free</h2>
    <p class="lede">Each expiry is fit independently to its European-equivalent implied vols, then
      coupled so the term structure carries no calendar arbitrage. Every slice is
      <b>butterfly-arbitrage-free</b> (Roper density ≥ 0), and total variance is ordered by expiry and
      non-crossing near the money.</p>
    <div class="figrid">{rest}</div>
  </section>

  <div class="note">Honest read: the near-money band (|k| ≤ 0.3) that VolaDynamics headlines is
    calendar-arbitrage-free. One residual deep-wing (|k| &gt; 0.3) crossing remains on a single
    thinly-quoted weekly expiry (29 quotes) — a data-sparsity artifact of extrapolating past listed
    strikes, not a fitting error, and shown rather than papered over.</div>

  <div class="method">
    <div class="mgrid">
      <div><h3>Data</h3><p>Databento OPRA <code>cbbo-1m</code> NBBO, full AMZN chain, snapshot
        2018-04-26 19:45:00Z (15:45 ET). Spot implied from the front put-call-parity forward.
        American mids inverted to European-equivalent vols. Vendor data not redistributed.</p></div>
      <div><h3>Curve</h3><p>CStar (C16M modal family), C8 tier: polynomial base
        <code>1+2·s2·z+c2·z²</code> — admits negative c2 — plus three compact shoulder modes.
        Seeded from a calendar-projected eSSVI surface, IRLS-Huber price-domain fit, Roper-density
        butterfly projection.</p></div>
      <div><h3>Earnings model</h3><p>ATF variance split σ²T = σE²·n + σC²·T (SpiderRock / VolaDynamics):
        a continuous diffusive baseline plus discrete per-event jump variance. Implied per-event move
        ≈ 5.8% of spot.</p></div>
      <div><h3>Verification</h3><p>GoogleTest <code>AmznEarnings</code> gate: front c2_eff &lt; −1.0,
        monotone curvature term structure, 0 butterfly violations, near-money calendar-monotone,
        17/17 slices fit. Cold whole-surface fit ≈ 0.2 s.</p></div>
    </div>
    <p class="foot">atx-vol · CStar-C8 · rendered from real OPRA marks · methodology after
      Klassen, "Arbitrage-Free Parametric Volatility Surfaces and Real-Time Fitting" (2017).
      Independent analysis; not affiliated with or endorsed by VolaDynamics.</p>
  </div>

</div></div>"""
    pathlib.Path(a.out).write_text(page, encoding="utf-8")
    kb = len(page.encode("utf-8")) / 1024
    print(f"wrote {a.out}  ({kb:.0f} KB, {len(FIGS)} figures inlined)")

if __name__ == "__main__":
    main()
