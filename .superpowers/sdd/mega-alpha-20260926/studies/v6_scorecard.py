"""v6 scorecard generator (docs lane; read-only over TRAIN 2020-2022 summaries and metadata).

Reads: build-equity/mega-nav-v6-summ-n28.{json,txt} (nav_summ over 13 v5 + 15 v6 cells, --dsr-n 28, reference v5 REF),
per-cell bounded-runner receipts (<cell>-run/receipt.json: exe SHA, wall, RSS), library v6 JSON + recipe, the v6l / v6u
admission.json and composition_weights.json. Never opens a daily/events CSV, a validation role or any 2023+ file.
Writes docs/plans/2026-09-28-mega-alpha-scorecard-v6.md and, with --fragment PATH, the handoff cell table.
Lo-null DSR per cell = nav_summ.dsr_rows single-cell formula at N = 28 on the n28 net_moments (arithmetic only; the
final cell reproduces the ledger's .499). Paired dSR vs each cell's own parent is copied from the ledger (progress.md);
nav_summ n28 pairs every cell against the v5 REF instead.
Usage (from C:/atx-wt/pool-2): python .superpowers/sdd/mega-alpha-20260926/studies/v6_scorecard.py [--fragment PATH]
"""
import importlib.util
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
B = ROOT / 'build-equity'
STUDIES = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('nav_summ', STUDIES / 'nav_summ.py')
assert spec is not None and spec.loader is not None
NS = importlib.util.module_from_spec(spec)
spec.loader.exec_module(NS)

J = json.load(open(B / 'mega-nav-v6-summ-n28.json'))
txt = open(B / 'mega-nav-v6-summ-n28.txt', encoding='utf-8').read()
blocks = [b for b in re.split(r'(?m)^rule=', txt) if b.strip()]
SC = {'S1': 'linear-6bps-stale5-v1+swap-fin-v1', 'S2': 'modeled-1bn-stale5-v1+swap-fin-v1',
      'S3': 'modeled-1bn-terminal-adverse-v1+sw', 'flat': 'modeled-1bn-stale5-v1+flat-300-v0',
      'tiers': 'modeled-1bn-stale5-v1+engine-tiers'}
PAT = (r'\S*\s+net ([+-][\d.]+) gross ([+-][\d.]+) hac ([+-][\d.]+) mu ([+-][\d.]+) vol ([\d.]+) mdd ([\d.]+)'
       r'.*?tc ([\d.]+) brw ([\d.]+).*?\| 2020:([+-][\d.]+) 2021:([+-][\d.]+) 2022:([+-][\d.]+)')


def parse(b):
    out = {}
    for k, p in SC.items():
        m = re.search(re.escape(p) + PAT, b)
        if m:
            out[k] = [float(x) for x in m.groups()]
    return out


P = [parse(b) for b in blocks]
assert len(P) == len(J) == 28, (len(P), len(J))
assert all(len(p) == 5 for p in P), [len(p) for p in P]


def nm(e):
    return e['dir'].replace(chr(92), '/').split('/')[-1].replace('mega-nav-', '')


for e, p in zip(J, P):  # the txt block order is the json order; check on the S2 net
    assert abs(p['S2'][0] - e['net_sharpe']) < 6e-4, (nm(e), p['S2'][0], e['net_sharpe'])

# Lo (2002) single-cell null at N = 28 (nav_summ.dsr_rows with one dir), from the n28 net moments.
LO = {}
for e in J:
    m = e['net_moments']
    LO[nm(e)] = NS.dsr_rows([m], 28)[0]
FINAL = 'v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247'
L1 = 'v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc'
LOC = 'v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc'
assert abs(LO[FINAL]['dsr'] - 0.499) < 6e-4, LO[FINAL]['dsr']

# Per-cell meta from the ledger (progress.md, v6 sections): lever, parent, paired dSR vs parent (Memmel SE), verdict.
V6 = [
    ('v6l-ew-t.05-d.1-fixed', 'V6-L library v6 (parent of the grid)', 'v51 ew (+0.759)', '+0.156 (.189)', 'ACCEPTED'),
    ('v6l-ew-t.05-d.1-fixed-x.05', 'C2 exit .05, target orders', 'v6l parent', '+0.023 (.050)', 'accepted (sign)'),
    ('v6l-ew-t.05-d.1-fixed-x.1', 'C2 exit .1, target orders', 'v6l parent', '+0.012 (.030)', 'accepted (sign)'),
    ('v6l-ew-t.05-d.1-fixed-obdelta', 'C1 delta orders, exit 1', 'v6l parent', '+0.019 (.095)', 'accepted (sign)'),
    ('v6l-ew-t.05-d.1-fixed-obdelta-x.05', 'C1 delta + C2 exit .05', 'v6l parent', '+0.044 (.125)',
     'ACCEPTED (best grid cell)'),
    ('v6l-ew-t.05-d.1-fixed-obdelta-x.1', 'C1 delta + C2 exit .1', 'v6l parent', '+0.036 (.114)', 'accepted (sign)'),
    (LOC, 'C3 locate-in-aim on delta x.05', 'v6l parent', '+0.060 (.101)', 'ACCEPTED (fixes the |net| gate)'),
    ('v6l-ew-t.03-d.1-fixed-obdelta-x.05', 'theta .03 on delta x.05', 'v6l parent', '-0.001 (.133)',
     'REJECTED (sign)'),
    ('v6l-ew-t.05-d.05-fixed-obdelta-x.05', 'dust .05 on delta x.05', 'v6l parent', '+0.043 (.125)',
     'not adopted (-0.001 vs dust .1)'),
    ('v6l-ew-t.05-d.2-fixed-obdelta-x.05', 'dust .2 on delta x.05', 'v6l parent', '+0.051 (.124)',
     'sign +, not carried (+0.007 vs dust .1; ruling)'),
    ('v6l-ew6-t.05-d.1-fixed-obdelta-x.05-loc', 'V6-W ew-theme-v6 composition', 'loc cell', '-0.055 (.297)',
     'REJECTED (sign)'),
    ('v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc-nind-v1', 'C5 price-risk-ind-v1 (FF12 FWL)', 'loc cell',
     '-0.072 (.156)', 'REJECTED (sign)'),
    ('v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc-nind-v2', 'C5 price-risk-ind-v2 (vol126/ladv252)', 'loc cell',
     '-0.048 (.176)', 'REJECTED (sign)'),
    (L1, 'V6-U linked-operating-v1 universe', 'loc cell', '+0.217 (.227)', 'ACCEPTED'),
    (FINAL, 'V6-F final: V6-U stack at L 1.247', 'V6-U L 1', '-0.010 (.007)', 'FINAL CELL (C4 L re-derivation)'),
]
d = {nm(e): (e, p) for e, p in zip(J, P)}
assert all(k in d for k, *_ in V6), [k for k, *_ in V6 if k not in d]


def receipt(cell):
    r = json.load(open(B / f'mega-nav-{cell}-run' / 'receipt.json'))
    return r['executable_sha256'][:8], r['wall_seconds'], r['sampled_peak_tree_rss_bytes'] / 2 ** 20


TAG = {'212d9e22': 'v6-0', 'f55537fc': 'v6-2', 'fb2d3e94': 'v5-1', '59b4e944': 'v5-2'}


def mech(e):
    return (.90 <= e['mean_gross_leverage_all_rows'] <= 1.05 and e['mean_abs_net_leverage'] <= .02
            and e['tau_gmv_mean'] <= .2 and e['tau_gmv_p95'] <= .3)


def v6_table():
    o = ['| # | cell (`mega-nav-...`) | lever | S2 net | gross SR | HAC t | vol | tau mean/p95 | cost bps/$ | '
         'gross all rows | net all rows | dSR vs parent (SE) [parent] | verdict | NAV exe |',
         '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for i, (k, lever, parent, dsr, verdict) in enumerate(V6, 1):
        e, p = d[k]
        exe, wall, rss = receipt(k)
        net = f"**{e['net_sharpe']:+.3f}**" if k == FINAL else f"{e['net_sharpe']:+.3f}"
        o.append(f"| {i} | `{k}` | {lever} | {net} | {e['gross_sharpe']:.3f} | {e['hac_t']:.2f} | "
                 f"{p['S2'][4] * 100:.2f}% | {e['tau_gmv_mean']:.4f}/{e['tau_gmv_p95']:.4f} | "
                 f"{e['cost_bps_traded']:.2f} | {e['mean_gross_leverage_all_rows']:.4f} | "
                 f"{e['mean_net_leverage_all_rows']:+.4f} | {dsr} [{parent}] | {verdict} | "
                 f"`{exe}` ({TAG.get(exe, '?')}) |")
    return o


o = []
o.append('# Mega-alpha scorecard v6 and alpha DSL (as of 2026-09-28)\n')
o.append('Integration checkout `C:/atx-wt/pool-2`, branch `feat/aes-codex-integration-20260925`. All numbers are TRAIN '
         '2020-2022 (no 2023+ file was read in v6). Primary cost scenario **S2** = `modeled-1bn-stale5-v1` impact costs at '
         '$1bn NAV x `swap-fin-v1` financing; daily rebalance (cadence 1); 252 sessions/yr; Sharpe on excess returns. '
         'Sources: `build-equity/mega-nav-v6-summ-n28.{txt,json}` (nav_summ over 13 v5 + 15 v6 cells, `--dsr-n 28`, '
         'reference = v5 REF `v5-ew-t.05-d.1-fixed`), per-cell runner receipts, `mega-weights-v6{l,u}-ew/`, the ledger '
         '`progress.md` and handoff 5 (`docs/plans/2026-09-28-mega-alpha-v6-handoff-5.md`). Generator: '
         '`.superpowers/sdd/mega-alpha-20260926/studies/v6_scorecard.py`. The v5 scorecard '
         '(`2026-09-27-mega-alpha-scorecard.md`) stays the reference for library v4/v5.1.\n')
o.append('## 1. Headline\n')
o.append('| | cell | S2 net SR | gross SR | gross lev (all rows) | DSR N=28 cross-cell / Lo | status |\n'
         '|---|---|---|---|---|---|---|')


def head(label, k, status):
    e, p = d[k]
    o.append(f"| {label} | `{k}` | **{e['net_sharpe']:+.3f}** | {e['gross_sharpe']:.3f} | "
             f"{e['mean_gross_leverage_all_rows']:.3f} | {e['deflated']['dsr']:.3f} / {LO[k]['dsr']:.3f} | {status} |")


head('**Final cell (V6-F)**', FINAL, "net >= 1.0 PASS; R6' mechanics PASS; pre-registered DSR >= .95 **FAIL** (.904)")
head('Highest TRAIN net SR', L1, 'L 1: under-deployed (gross .78); fails mechanics')
head('Best library v6 cell on role v2', LOC, 'L 1: gross .76; fails mechanics')
head('v5 deployable book (handoff 4)', 'v5-ew-t.05-d.1-fixed-L1.279', 'v5 best mechanics-pass cell')
o.append('| Best out-of-sample (VAL 2023-2024, trial #2) | v4.1 `b2 f.25` | **+0.641** | 0.802 | 0.235 | | '
         'a 24%-gross book (ruling R-1) |')
o.append('| VAL trial #1 | v3 (b1 f1) | -1.27 | | 0.763 | | TRAIN 1.81 -> VAL -1.27 (overfit) |')
e, p = d[FINAL]
o.append(f"\n**The TRAIN objective is met, but the pre-registered freeze condition is not.** The final cell nets "
         f"S2 {e['net_sharpe']:+.3f} at all-rows gross {e['mean_gross_leverage_all_rows']:.3f}, "
         f"mean net {e['mean_net_leverage_all_rows']:+.4f}, tau {e['tau_gmv_mean']:.4f}/{e['tau_gmv_p95']:.4f}. "
         f"Its cross-cell DSR over N = 28 is {e['deflated']['dsr']:.3f} (SR0 {e['deflated']['sr0_annual']:.3f} ann), "
         f"below the .95 required by the v6 prereg (V6-F). The Lo-null DSR is {LO[FINAL]['dsr']:.3f} "
         f"(SR0 {LO[FINAL]['sr0_annual']:.3f} ann). No freeze is proposed by the controller; validation trial #3 is "
         f"unspent and is the owner's call (U1). Paired vs the v5 REF: dSR {e['paired']['dsr']:+.3f}, Memmel SE "
         f"{e['paired']['memmel_se']:.3f} (t {e['paired']['t']:.2f}), CBB 95% [{e['paired']['cbb_ci95'][0]:+.3f}, "
         f"{e['paired']['cbb_ci95'][1]:+.3f}], LW p {e['paired']['lw']['p_value']:.3f}. Year net returns 2020 "
         f"{p['S2'][8] * 100:+.1f}% / 2021 {p['S2'][9] * 100:+.1f}% / 2022 {p['S2'][10] * 100:+.1f}%: the 2020 "
         f"contribution is about zero.\n")

o.append('## 2. TRAIN cells (S2 primary)\n')
o.append('### 2a. The 15 v6 cells\n')
o.append('Rule aim-partial-v5, theta .05, dust .1, fixed rate unless named; neutralization price-risk-v1 unless named; '
         'library v6 (`5ee66d13`) and ew-theme-v1 on every cell except ew6. `x` = exit rate for nonmembers, `obdelta` = '
         'delta order basis, `loc` = locate-in-aim, `v6u` = linked-operating-v1 universe. Acceptance = sign of the paired '
         'dSR vs the named parent matches the pre-registered "+" prior (magnitude is not a criterion). dSR vs parent is '
         'the ledger value (Memmel SE). Gross/net leverage are means over every daily row (the R6\' gate basis); vol is '
         'the S2 annualised net vol; cost bps/$ = `cost_bps_traded`. NAV exe = bounded-runner receipt.\n')
o.extend(v6_table())
o.append('\nNot trials (identity checks): `v6-ew-t.05-d.1-fixed-obtarget-x1` (D2: v6-0 default path, all 12 files '
         'byte-identical to `v51-ew-t.05-d.1-fixed`) and `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc-v62` (branch review I1: '
         'the loc cell on v6-2, all 10 CSVs byte-identical to the v6-0 cell).\n')
o.append('### 2b. All 28 cells: DSR, paired vs v5 REF, mechanics\n')
o.append('"DSR x-cell" uses V[SR_n] across the 28 cells (1.403e-04 per session; SR0 .385 ann; nav_summ n28). "DSR Lo" is '
         'the Lo (2002) single-cell null at N = 28 recomputed from the n28 net moments with nav_summ\'s own formula '
         '(SR0 ~ the cell\'s own SR). dSR vs REF is paired against `v5-ew-t.05-d.1-fixed` (Memmel SE; Ledoit-Wolf '
         'studentized block-21 bootstrap, 2000 draws). Post-ramp gross = mean over rows after the first 63 (C4).\n')
o.append('| cell | net SR | gross SR | HAC t | mu/yr | vol/yr | MDD | 2020 / 2021 / 2022 | gross all | gross post-ramp | '
         'net all | tau mean/p95 | cost bps/$ | held names | dSR vs REF (SE) | LW 95% (p) | DSR x-cell | DSR Lo | '
         'mechanics |')
o.append('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
for e, p in zip(J, P):
    k = nm(e)
    s = p['S2']
    pr = e['paired']
    ds = '-' if pr['t'] is None else f"{pr['dsr']:+.3f} ({pr['memmel_se']:.3f})"
    lw = '-' if pr['t'] is None else f"[{pr['lw']['ci95'][0]:+.3f}, {pr['lw']['ci95'][1]:+.3f}] ({pr['lw']['p_value']:.3f})"
    name = f"**`{k}`**" if k == FINAL else f"`{k}`"
    o.append(f"| {name} | {e['net_sharpe']:+.3f} | {e['gross_sharpe']:.3f} | {e['hac_t']:.2f} | {s[3] * 100:+.2f}% | "
             f"{s[4] * 100:.2f}% | {s[5] * 100:.1f}% | {s[8] * 100:+.1f}% / {s[9] * 100:+.1f}% / {s[10] * 100:+.1f}% | "
             f"{e['mean_gross_leverage_all_rows']:.3f} | {e['mean_gross_leverage_post_ramp']:.3f} | "
             f"{e['mean_net_leverage_all_rows']:+.4f} | {e['tau_gmv_mean']:.4f}/{e['tau_gmv_p95']:.4f} | "
             f"{e['cost_bps_traded']:.2f} | {e['mean_held_names']:.0f} | {ds} | {lw} | {e['deflated']['dsr']:.3f} | "
             f"{LO[k]['dsr']:.3f} | {'PASS' if mech(e) else 'FAIL'} |")
o.append('\nThe n28 `netting_ratio` column is not reproduced: the run passed the v6u weights to every cell, so the ratio is '
         'matched only for the two v6u cells (the others print UNMATCHED).\n')

o.append('## 3. Cost and financing stresses (net SR by scenario)\n')
o.append('Rows: the final cell, its L 1 parent (V6-U), the loc cell (best role-v2 construction), the V6-L parent and the '
         'v5 references. Every scenario prices the same shared construction (locate-in-aim applies to every book).\n')
o.append('| cell | S1 linear 6 bps | **S2 modeled $1bn** | S2 x engine-tiers | S2 x flat-300 | S3 terminal-adverse (K = 1) |'
         '\n|---|---|---|---|---|---|')
for k in [FINAL, L1, LOC, 'v6l-ew-t.05-d.1-fixed', 'v51-ew-t.05-d.1-fixed', 'v5-ew-t.05-d.1-fixed-L1.279',
          'v5-ew-t.05-d.1-fixed']:
    e, p = d[k]
    name = f"**`{k}`**" if k == FINAL else f"`{k}`"
    o.append(f"| {name} | {p['S1'][0]:+.3f} | **{p['S2'][0]:+.3f}** | {p['tiers'][0]:+.3f} | {p['flat'][0]:+.3f} | "
             f"{p['S3'][0]:+.3f} |")
o.append('\nAll 15 v6 cells (S1 / S2 / tiers / flat-300 / S3):\n')
o.append('| cell | S1 | S2 | tiers | flat-300 | S3 |\n|---|---|---|---|---|---|')
for k, *_ in V6:
    e, p = d[k]
    o.append(f"| `{k}` | {p['S1'][0]:+.3f} | {p['S2'][0]:+.3f} | {p['tiers'][0]:+.3f} | {p['flat'][0]:+.3f} | "
             f"{p['S3'][0]:+.3f} |")
o.append('\nS3 writes delisted holdings off adversely; delisting returns are not modelled in S1/S2 (lane parked: T33a '
         'NO-GO, owner gate U3). S3 is positive on every v6 cell except the two C5 (industry-neutral) cells; the V6-L '
         'parent was the first positive S3 of any cell (+0.128) and the two V6-U cells reach +0.34.\n')

# ---------- section 4 ----------
L6 = json.load(open(ROOT / 'atx-impl/strategies/fund_industry_ic_v6.json'))
R6 = json.load(open(ROOT / 'atx-impl/strategies/fund_industry_ic_v6.recipe.json'))
chg = {c['id']: c for c in R6['v6_changes']}
Au = json.load(open(B / 'mega-weights-v6u-ew/admission.json'))
Al = json.load(open(B / 'mega-weights-v6l-ew/admission.json'))
Wu = json.load(open(B / 'mega-weights-v6u-ew/composition_weights.json'))
Wl = json.load(open(B / 'mega-weights-v6l-ew/composition_weights.json'))
au = {c['id']: c for c in Au['candidates']}
al = {c['id']: c for c in Al['candidates']}
lin = {r['id']: r for r in R6['lineage']}
M = json.load(open(B / 'recent-fast-train-2020-2022-v2-lo1/manifest.json'))['universe']
fe, fp = d[FINAL]
o.append('## 4. How the alphas become the book (final cell)\n')
o.append('1. **Library v6** `atx-impl/strategies/fund_industry_ic_v6.json` (sha `5ee66d13`, recipe `36c08452`, generator '
         '`generate_fund_ic_v6.py`): 38 candidates in 9 themes, each one DSL expression evaluated daily per name by the '
         'atx-engine alpha DSL VM. Vs v5.1: +`value_composite`, +`res_mom_12_1`; `cfoa`->`cbop`, `low_beta`->`bac`, '
         '`low_max`->`smax`; -`low_ivol`, -`lowvol_ind`; 27 members switched from `decay_linear(R(x),21)` to '
         '`R(decay_linear(x,21))` (removes the 21-session membership blackout); the 4 fast sleeves (standalone tau >= .08 '
         'in v5.1: `ind_adj_rev_5`, `seasonality_same_month`, `si_change`, `iv_rv_spread`) lose the decay.')
o.append(f"2. **Universe `linked-operating-v1`** (V6-U; role `recent-fast-train-2020-2022-v2-lo1`, manifest `3e79978a`): a "
         f"base member (top 3000 by prior-63-session $ADV, price > $5) is kept only if it has exactly one point-in-time "
         f"identity-bridge link on the issuer's primary line, class_status common, and a visible SIC (lag 1 session, "
         f"<= 550 days old) outside {M['non_operating_sic']} (pooled vehicles, blank checks, royalty trusts; REITs stay). "
         f"Score-window dropped member share {M['dropped_member_share_score_window']:.3f}; dropped cells by reason "
         f"(score window): {json.dumps(M['dropped_by_reason_score_window'])}; kept member cells "
         f"{M['kept_member_cells']:,} of {M['base_member_cells']:,}; min kept members per scored day 1675. Fields are "
         f"rebuilt on the restricted role (`lo1-fields-v6b`, manifest `c69b9c0f`, 40 fields): `mkt_ret` and group ranks "
         f"are now over the restricted members. The bridge is the r4 rehearsal (`scope_complete false`): unbridged "
         f"operating stocks drop; ADRs of linked filers stay.")
o.append(f"3. **Admission** `v4-prior-v1` re-run on the restricted role (TRAIN only): literature prior sign embedded in "
         f"the DSL, never flipped; reject if redundant (|rho| > .90 vs a stronger member by (tier, roster order)), "
         f"turnover > .7, HAC t < -2.0 (veto) or insufficient data. Restricted role: {Au['counts']['admitted']} of 38 "
         f"admitted ({Au['counts']['reject_redundant']} redundant, {Au['counts']['reject_veto']} veto: `si_change`). "
         f"Role v2: {Al['counts']['admitted']} of 38.")
o.append(f"4. **Composition** `ew-theme-v1` (weights `490c3836`): 9 themes at 1/9, equal weight within a theme across "
         f"its admitted members; no mean or covariance estimation. weighted standalone turnover "
         f"{Wu['provenance']['weighted_standalone_turnover']:.4f}. `ew-theme-v6` (drop low_risk, merge options into "
         f"short_interest, fast x1/3) was tested and rejected (V6-W).")
o.append('5. **Combined signal**: per candidate, unsigned centered tied rank over members, times the prior sign, weighted '
         'sum (`mega-v6uw-train-ew-1/train_combined.json`, sha `1e146796`; IC exe `b1c1ba07`).')
o.append('6. **Desired target**: tied rank of the combined signal; **locate-in-aim** (C3): a member in the special borrow '
         'tier at decision d with a negative desired weight is set to 0 *before* neutralisation (about 100 names per '
         'decision on role v2); then `price-risk-v1` (OLS residual on [1, z beta252, z vol63, z log ADV63], rescaled to '
         'the zeroed entry gross); gross 1; nonmembers 0.')
o.append('7. **Construction** `aim-partial-v5`: each session a member moves next = cur + theta (L x desired - cur) '
         'unless |L x desired - cur| <= dust / N_d; theta .05, dust .1, fixed rate, **L 1.247** (= 1 / post-ramp gross '
         '.8019 of the L 1 parent, C4). **Exit rate** (C2): a present nonmember decays next = cur x (1 - .05) and snaps to '
         '0 inside the dust band; an absent nonmember exits at once. **Delta orders** (C1): a changed nonzero plan '
         'becomes an order for (planned - current) x NAVpost; price drift between decision and fill rides instead of '
         'being traded back. Liquidity cache on (fixed-rate path bit-identical).')
o.append(f"8. **Execution and costs**: decide at d after the mark, fill at the close of d+1, first return row d+2. "
         f"S2 = `sqrt-impact-v1` at $1bn (half-spread 5 bps + commission 1 bp + Y .6 x sigma x sqrt(participation), "
         f"max participation 1% of ADV63) x `swap-fin-v1` (ACT/360: long +40 bps, short +20 bps + tier fee GC 30 / "
         f"warm 100 / special 500 bps; special shorts blocked). Result: held names {fe['mean_held_names']:.0f}, held share "
         f"{fe['held_share']:.3f} (exit decay lingers ~60 sessions), tau {fe['tau_gmv_mean']:.4f}, cost "
         f"{fe['cost_bps_traded']:.2f} bps per traded $.\n")

# ---------- section 5 ----------
o.append('## 5. Alpha table (library v6, 38 candidates)\n')
o.append('Every candidate embeds prior sign +1 (raw literature direction in column "raw dir"). "lo1" = the restricted '
         'role (the final cell\'s admission and weights, `mega-weights-v6u-ew`); "v2" = role v2 (`mega-weights-v6l-ew`, '
         'the V6-L / V6-C cells). HAC t and tau are the TRAIN admission statistics of the standalone neutralized gross-1 '
         'factor (forward r[d+2]; tau = daily one-way turnover). `reject_redundant` names the member it duplicates.\n')
o.append('| id | theme | tier | raw dir | change vs v5.1 | status lo1 | HAC t lo1 | tau lo1 | **w_ew lo1** | status v2 | '
         'HAC t v2 | w_ew v2 |\n|---|---|---|---|---|---|---|---|---|---|---|---|')
THEMES = [t['theme'] for t in R6['themes']]
order = sorted(L6['candidates'], key=lambda c: (THEMES.index(c['theme']), lin[c['id']]['roster_order']))


def st(a):
    s = a['status']
    if s == 'reject_redundant' and a.get('redundant_with'):
        s += f" ({a['redundant_with']})"
    return s


def change(i):
    c = chg[i]
    if c['change'] == 'smoothing':
        return 'decay removed (fast sleeve)' if 'fast sleeve' in c['detail'] else 'R(decay(x))'
    return c['change']


for c in order:
    i = c['id']
    u, v = au[i], al[i]
    o.append(f"| `{i}` | {c['theme']} | {c['tier']} | {lin[i]['raw_prior_direction']:+d} | {change(i)} | {st(u)} | "
             f"{u['hac_t']:+.2f} | {u['tau']:.4f} | **{Wu['weights'].get(i, 0):.4f}** | {st(v)} | {v['hac_t']:+.2f} | "
             f"{Wl['weights'].get(i, 0):.4f} |")
o.append('\nRemoved v5.1 ids: ' + '; '.join(f"`{c['id']}` ({c['change']})" for c in R6['v6_changes']
                                          if c['id'] not in au) + '.\n')
th = Wu['provenance']['themes']
o.append('Theme mass on the restricted role (ew-theme-v1): ' + '; '.join(
    f"{t} {th[t]['admitted_count']} x {th[t]['member_weight']:.4f}" for t in THEMES if t in th) + '.\n')

# ---------- section 6 ----------
fam = {f['id']: f.get('description', '') for f in L6['families']}
o.append('## 6. Alpha DSL strings (verbatim from `fund_industry_ic_v6.json`)\n')
cur = None
for c in order:
    if c['theme'] != cur:
        cur = c['theme']
        o.append(f"\n### {cur}\n\n{fam.get(cur, '')}\n")
    i = c['id']
    o.append(f"- **`{i}`** ({st(au[i])}; w_ew lo1 {Wu['weights'].get(i, 0):.4f}; {change(i)}) - {c.get('citation', '')}"
             f"\n  ```\n  {c['dsl']}\n  ```")

# ---------- section 7 ----------
fd = fe['deflated']
o.append('\n## 7. Trial accounting (Appendix A)\n')
o.append('```\nTrial accounting (TRAIN 2020-2022 only; no 2023+ read in v6; per-candidate VAL statistics never read):\n'
         '  v3 era: admission 48 + 121; composition 4 + 7; construction 14.\n'
         '  since run #1 to v5: libraries v4 (37), v4.2 (40), v5.1 (38); compositions v4, v4.2, ew-theme-aim-v1, v5.1 x2;\n'
         '    construction v4 1 + v4.1 grid 5 + v4.2 2 + v5 grid 10 + 2 L re-run + 1 v5.1; studies T26 5 paper books, T16,\n'
         '    T28 audit; stat-arb cluster study (separate family, negative).\n'
         '  v6: admission 38 (library v6 on role v2; res_mom_12_1 = re-trial of v4.2) + 38 (re-admission on the\n'
         '    linked-operating-v1 role); compositions 3 (ew-theme-v1 on library v6 x2 [role v2, role lo1], ew-theme-v6);\n'
         '    universe 1 (linked-operating-v1); construction cells 15 (V6-L parent, 5 C1/C2 grid, 4 conditional [loc,\n'
         '    theta .03, dust .05, dust .2], ew6, ind-v1, ind-v2, V6-U, V6-F); studies: Phase A reviews and the v6-explore\n'
         '    aggregation read existing TRAIN outputs (disclosed diagnostics).\n'
         '  Not trials: D2 identity cell (v6-0 default path = v51 cell, 12/12 files identical); I1 identity cell (-v62,\n'
         '    10/10 CSVs identical); m1 IC re-run (mega-v6lw-train-ew-2, 6/6 train_combined.* identical); refused or\n'
         '    colliding runs that produced no statistic (v6l u-run1..3 cache mismatch; v6u u-run1..4 field list / dir\n'
         '    collisions; ew6 w-run1..3 admit refusal).\n'
         '  validation: #1 (v3, book level), #2 (v4.1, 24%-gross book); #3 unspent (needs U1).\n'
         f"  DSR (final cell, N = 28 = 13 v5 + 15 v6 NAV cells, T {fd['sessions']}, skew {fd['skew']:.3f}, "
         f"kurtosis {fd['kurtosis']:.2f}):\n"
         f"    cross-cell V[SR_n] = {fd['variance_sr']:.3e}/session, SR0 {fd['sr0_annual']:.3f} ann -> DSR "
         f"{fd['dsr']:.3f}  (prereg V6-F requires >= .95: FAIL)\n"
         f"    Lo (2002) null V = (1 + SR^2/2)/T, SR0 {LO[FINAL]['sr0_annual']:.3f} ann -> DSR {LO[FINAL]['dsr']:.3f}\n"
         '    N counts NAV cells only; it omits the 38 + 38 admission trials and the library-design degrees of freedom.\n```')
out = ROOT / 'docs/plans/2026-09-28-mega-alpha-scorecard-v6.md'
out.write_text('\n'.join(o) + '\n', encoding='utf-8')
print(out, len(o), 'blocks')

if len(sys.argv) > 2 and sys.argv[1] == '--fragment':
    f = ['## v6 cell table (handoff fragment)'] + v6_table()
    f.append('')
    f.append('## Lo DSR N=28 per v6 cell')
    for k, *_ in V6:
        f.append(f"{k}: lo {LO[k]['dsr']:.3f} x-cell {d[k][0]['deflated']['dsr']:.3f} years "
                 f"{d[k][1]['S2'][8]:+.3f}/{d[k][1]['S2'][9]:+.3f}/{d[k][1]['S2'][10]:+.3f} mu {d[k][1]['S2'][3]:+.4f} "
                 f"mdd {d[k][1]['S2'][5]:.3f} postramp {d[k][0]['mean_gross_leverage_post_ramp']:.4f} "
                 f"held {d[k][0]['mean_held_names']:.0f} wall/rss {receipt(k)[1]:.1f}s/{receipt(k)[2]:.0f}MiB")
    Path(sys.argv[2]).write_text('\n'.join(f) + '\n', encoding='utf-8')
    print('fragment', sys.argv[2])
