"""Deterministic 48-candidate research library v7.1 = library v7.0 + wave 2 (four SEC / 13F / FTD literature members).

Library v7.1 implements the platform-v7 pre-registration section "Library v7.1 = parent + wave 2" (declared 2026-09-29
before any IC pass of any wave-2 candidate; .superpowers/sdd/platform-20260928/v7-prereg.md), which registers by
reference library-v7-wave2-prereg.md sections 1-5 as committed at e3880d83 (P4 text; draft library-v7-draft.md
section 2 at f43e54d9 minus eap_8k). Wave 1 was accepted, so by Ruling W2-a the parent is library v7.0
(fund_industry_ic_v70.json, pinned by SHA-256 and re-derived from its own generator): its 44 candidate entries stay
byte-identical and in the same order, including its non-admitted members, and the four wave-2 members are appended in
the registered order:

  ins_opp          ownership_flow     B-  rank(ins_opportunistic_net)                             (Cohen-Malloy-Pomorski)
  inst_best_ideas  ownership_flow     C+  rank(decay_linear(inst_best_ideas, 21))                 (Cohen-Polk-Silli)
  ftd_fail         short_interest     C+  rank((-1 * ftd_shares_ratio21))                         (Evans-Geczy-Musto-Reed)
  ea_overdue       earnings_momentum  C+  rank((-1 * max(sign((-1 * ea_days_to_expected)), 0)))  (Johnson-So)

Each DSL string is the registered text verbatim (v7-prereg.md "Library v7.1"). Two mechanical respellings versus the
draft were decided by field semantics before the registration and are recorded, not re-decided, here: ins_opp drops
the draft's division by shares_out (ins_opportunistic_net is already per share outstanding), and ea_overdue uses the
draft's alignment rule 1{ea_days_to_expected < 0} (W5a's field is signed), which drops ea_days_since.

Changes against v7.0 (prereg section 5 identity (i)): the 44 parent entries are a byte-identical prefix; the library
declares the family ownership_flow (10th; the IC runner refuses an empty declared family, so it enters with its first
members), the short_interest and earnings_momentum descriptions name the new members, and four field declarations are
appended (ins_opportunistic_net, inst_best_ideas, ftd_shares_ratio21, ea_days_to_expected: fields-v9 as built,
manifest sha256 8fd00e9f...d7b8769b, 63 rows; Ruling W2-d keeps the 64-row cap). No budget exception: every member is
inside the house budget (5 extra fields, 7 estimated peak slots, 314 prior bars, 4,096 bytes), and every operator
(rank, decay_linear, max, sign) is already in the v7.0 validator, so no registry row is added.

No TRAIN return statistic, no v7.0 per-candidate result and no 2023+ data were read. Run with --check to verify the
committed exact JSON bytes.
"""
from __future__ import annotations

import argparse
import functools
import hashlib
import importlib.util
import json
import sys
from pathlib import Path
from types import ModuleType

HERE = Path(__file__).resolve().parent
V70_GENERATOR = 'generate_fund_ic_v70.py'
V70_LIBRARY = 'fund_industry_ic_v70.json'
V70_RECIPE = 'fund_industry_ic_v70.recipe.json'
V70_LIBRARY_SHA256 = 'e7bae75c9dc3c4ac6b826289f39a50e26dc9c41ca80df4b272395a3e7362c162'
V70_RECIPE_SHA256 = '60b823004cc40b105781262c530cc7c23b77c149d1135d3886d61a47b69820fc'
V70_CANDIDATES = 44
LIBRARY = 'fund_industry_ic_v71.json'
RECIPE = 'fund_industry_ic_v71.recipe.json'
LIBRARY_ID = 'fund_industry_ic_v71'
PREREG_PATH = '.superpowers/sdd/platform-20260928/v7-prereg.md'
WAVE2_PATH = '.superpowers/sdd/platform-20260928/library-v7-wave2-prereg.md'
DRAFT_PATH = '.superpowers/sdd/platform-20260928/library-v7-draft.md'
PREREG_COMMIT = 'e3880d83'  # v7-prereg.md "Library v7.1" + library-v7-wave2-prereg.md sections 1-5 (rulings W2-a..e)
DRAFT_COMMIT = 'f43e54d9'   # library-v7-draft.md section 2 (wave 2), the draft the P4 text refines
PREREG = (f'{PREREG_PATH} section "Library v7.1 = parent + wave 2" (declared 2026-09-29 before any IC pass of any wave-2 '
          f'candidate and before the wave-1 read), registering {WAVE2_PATH} sections 1-5 as committed at '
          f'{PREREG_COMMIT}; rulings W2-a..e; task brief task-L8-brief.md')
MAX_ROSTER = 56                     # prereg v7.0 ruling (generator roster cap 48 -> 56), unchanged
MAX_EXTRAS_PER_CANDIDATE = 5        # house budget (wave 2: no exception)
MAX_SLOTS_PER_CANDIDATE = 7         # house budget (wave 2: no exception)
SMOOTHING = 21
FIELDS_V9_MANIFEST_SHA256 = '8fd00e9f44b475116f483e133c03fe031618390060b8374180278f1cd7b8769b'
FIELDS_V9_DIR = 'build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9'
FIELDS_V9_ROWS = 63
NEW_THEME = 'ownership_flow'
DSR_N = 35                          # ledger lines when declared (34) + 1; root re-derives it at run time (prereg)

# The four field declarations (fields-v9 as built; basis and clock condensed from the manifest rows, which are
# authoritative). origin: the fields manifest that first carried the field.
NEW_FIELDS = [
    dict(name='ins_opportunistic_net', origin='fields_v8', formula_id='sec-ins-opportunistic-net126-cmp3y-v1',
         sha256='1b2566dd255897a4b6c2e0a590db9d32be429ab04a9a67425f33f17452b2c59e',
         basis=('(P - S shares) of Form 4 open-market trades by insiders classified opportunistic for the trade year Y '
                '(Cohen-Malloy-Pomorski 2012: traded in each of Y-1..Y-3 with no calendar month repeated in all three; '
                'routine and unclassified owners excluded) with transaction_date in the 126 sessions t-126..t-1 and '
                'visible at t, divided by shares_out[t]; directors and officers on original Form 4s, joint filings '
                'with a 10% owner dropped (W5a rule 1); NaN outside [-1, 1] (W5a rule 2), without a visible insider '
                'row within 365 days (Section 16 presence) or before 2018 (formula id '
                'sec-ins-opportunistic-net126-cmp3y-v1; producer atx-engine/tools/research_fields_sec.py, fields-v8)'),
         clock=('sec-acceptance-lag1-v1: a Form 4 row is usable at session t iff its EDGAR acceptance is before '
                '22:00 UTC of session t-1 (lag 1 session)')),
    dict(name='inst_best_ideas', origin='fields_v9', formula_id='13f-asof45-best-ideas-cps-v1',
         sha256='043ec02026b65dce54df2f8715a6ecf075c4cc32f8c55a8616f76375c03414df',
         basis=('Cohen-Polk-Silli conviction at the anchor 13F quarter P: sum over filers m holding the security of '
                'max(0, w_m - mw), w_m = the security\'s screened value / m\'s total screened long value, mw = its '
                'share of the total screened mapped 13F value of P (13F-universe market weight); all 13F filers '
                'pooled; NaN without a screened mapped row in a fresh quarter (150-day staleness) (formula id '
                '13f-asof45-best-ideas-cps-v1; producer atx-engine/tools/research_fields_holdings.py, fields-v9)'),
         clock=('13f-quarter-asof45-v1: quarter P is visible at V(P) = the latest deadline filing date + 46 h, uniform '
                'across securities; session t reads the latest P with V(P) before 22:00 UTC of t-1 and date(t) - P '
                '<= 150 days')),
    dict(name='ftd_shares_ratio21', origin='fields_v9', formula_id='sec-ftd-sum21-over-shares-v1',
         sha256='fa5a48a85777bed79aae1e3c31f550f24ea32cb314bb71cb5d8faaa5f938106f',
         basis=('SEC CNS fails-to-deliver quantity summed over the 21 settlement dates ending at S*(t), the latest '
                'settlement date up to which every date is visible (no row = 0 fail), / shares_out at the last role '
                'session on or before S*(t); NaN when date(t) - S*(t) > 60 days or shares_out is not finite and '
                'positive (formula id sec-ftd-sum21-over-shares-v1; producer atx-engine/tools/'
                'research_fields_holdings.py, fields-v9)'),
         clock=('sec-ftd-latest-visible-21-v1: FTD rows visible by the nominal half-month publication + 7 d (or a '
                'later HTTP Last-Modified) before 22:00 UTC of t-1; S*(t) trails t by about 22-37 days')),
    dict(name='ea_days_to_expected', origin='fields_v8', formula_id='sec-ea-days-to-expected-yoy364-consume45-fb91-v1',
         sha256='86df20c01841a3f0902a6c9e85ed600b5c2cb92c5e92f96a180a69bcd06c39b9',
         basis=('signed sessions from t to the expected next earnings announcement E* (0 = expected today, negative = '
                'overdue): E* = the earliest pending yoy_364 date of the CIK\'s visible primary 8-K item 2.02 rows '
                'more than 45 days after the latest announcement A, else A + 91 days; NaN without a visible primary '
                '2.02 within 200 days or when overdue by more than 63 sessions (formula id '
                'sec-ea-days-to-expected-yoy364-consume45-fb91-v1; producer atx-engine/tools/research_fields_sec.py, '
                'fields-v8)'),
         clock=('sec-acceptance-lag1-v1: an 8-K row is usable at session t iff its EDGAR acceptance is before 22:00 '
                'UTC of session t-1 (lag 1 session)')),
]

# Registered DSL strings (v7-prereg.md "Library v7.1", backticked; tests compare them with the committed text).
DSL = {
    'ins_opp': 'rank(ins_opportunistic_net)',
    'inst_best_ideas': f'rank(decay_linear(inst_best_ideas, {SMOOTHING}))',
    'ftd_fail': 'rank((-1 * ftd_shares_ratio21))',
    'ea_overdue': 'rank((-1 * max(sign((-1 * ea_days_to_expected)), 0)))',
}
# Draft section 2 strings (f43e54d9) and the registered mechanical respellings (prereg section 1).
DRAFT_DSL = {
    'ins_opp': 'rank((ins_opportunistic_net / shares_out))',
    'inst_best_ideas': 'rank(decay_linear(inst_best_ideas, 21))',
    'ftd_fail': 'rank((-1 * ftd_shares_ratio21))',
    'ea_overdue': 'rank((-1 * max(sign(((ea_days_since + ea_days_to_expected) - 94.5)), 0)))',
}
RESPELLINGS = {
    'ins_opp': ('the field is already (P - S shares of opportunistic insiders, trade dates t-126..t-1, visible by '
                'acceptance) / shares_out[t] (units ratio, domain [-1, 1]), so the draft\'s own conditional ("if W5a '
                'already scales by shares_out, the division is dropped") applies; a second division would rank net / '
                'shares_out^2, a size tilt'),
    'ea_overdue': ('the draft\'s alignment rule: W5a\'s field is signed (negative = overdue), so the predicate is '
                   '1{ea_days_to_expected < 0} = max(sign(-x), 0) and ea_days_since is dropped; E* stays at a missed '
                   'date, so since + to is constant within a cycle (TRAIN p50 62, max 95) and the draft cut 94.5 would '
                   'fire on .011% of cells and on 0 of 52,476 overdue cells'),
}

MEMBERS = [
    dict(id='ins_opp', theme=NEW_THEME, tier='B-', ranking='cross_section', raw_prior_direction=1,
         citation='Cohen, Malloy and Pomorski (2012, JF 67(3)) Decoding Inside Information: opportunistic insider '
                  'net buying',
         prior_sign_source='Cohen-Malloy-Pomorski 2012',
         formula='ins_opportunistic_net: net (P - S) shares of opportunistic insiders over trade dates t-126..t-1, '
                 'visible by Form 4 acceptance, per share outstanding (W5a sec-ins-opportunistic-net126-cmp3y-v1)',
         form='R(x)', smoothing=1,
         domain='NaN when the field is NaN (no visible insider row within 365 days, foreign private issuers and ADRs, '
                'windows before 2018-01-01, |ratio| > 1 under W5a rule 2); exactly 0 when no opportunistic trade is in '
                'the window (66.6% of TRAIN member cells: zeros tie at an average rank of about .64)',
         deviation='respelled from the draft rank((ins_opportunistic_net / shares_out)) (field semantics, prereg '
                   'section 1); shares, not Cohen-Malloy-Pomorski\'s dollar volume over market cap; directors and '
                   'officers on original Form 4s with 10%-owner joint filings dropped (W5a rule 1); routine = a month '
                   'traded in each of Y-1..Y-3, opportunistic = each year traded but no repeated month; Form 4 shares '
                   'are not split-restated (rule 2 catches the extreme cases); no decay: the 126-session aggregate is '
                   'its own smoothing; seller-dominated (per date median 40 buyers vs about 530 sellers of about 1,779 '
                   'members), so the member mostly shorts net sellers; Ruling W2-c keeps the net measure (a buy-only '
                   'leg is a separate later trial)',
         prior='Cohen-Malloy-Pomorski (2012 JF), 1986-2007: the opportunistic long-short portfolio earns 82 bps/mo '
               'value-weighted abnormal return (t n/v); routine trades earn about 0; thinner evidence after 2007 '
               '(literature-v7)',
         expected_turnover='low (.01-.03/day [est]); max expected abs(rho) .10-.20 with mom_12_1 (negative) [est]'),
    dict(id='inst_best_ideas', theme=NEW_THEME, tier='C+', ranking='cross_section', raw_prior_direction=1,
         citation='Cohen, Polk and Silli (2010, LSE WP) Best Ideas; Anton, Cohen and Polk (2021 WP); crowding: Brown, '
                  'Howard and Lundblad (2022, RFS)',
         prior_sign_source='Cohen-Polk-Silli 2010',
         formula='inst_best_ideas: sum over 13F filers of max(0, manager weight - 13F-universe market weight) at the '
                 'anchor quarter, visible at the quarter\'s deadline filing + 46 h (13f-asof45-best-ideas-cps-v1)',
         form='R(decay_linear(x, 21))', smoothing=SMOOTHING,
         domain='NaN without a screened mapped 13F row in a fresh quarter (150-day staleness); a NaN inside the '
                '21-session window blanks the decay for 21 sessions (coverage .998); > 0 on every finite TRAIN cell',
         deviation='no respelling; all 13F filers pooled (filer type unclassified; Cohen-Polk-Silli use active mutual '
                   'funds); the summed overweight replaces each manager\'s single best idea (Ruling W2-c keeps it; a '
                   'per-holder mean is a separate later trial); the sum grows with breadth and single-security filers '
                   '(w = 1) form the tail (TRAIN p50 .54, p99 9.1, max 56.1; the rank bounds it); one visibility '
                   'session for all names per quarter (values 47-150 days old): the decay spreads the step; late '
                   '13F-HR/A amendments excluded',
         prior='Cohen-Polk-Silli (2010 WP), US active funds 1991-2005: best ideas beat the market by about '
               '1-2.5%/quarter (t n/v); Anton-Cohen-Polk (2021 WP): 2.8-4.5%/yr; crowding tail risk: Brown-Howard-'
               'Lundblad (2022 RFS)',
         expected_turnover='very low (.01-.02/day [est]); max expected abs(rho) .20-.30 with mom_12_1, liquidity '
                           '[est]'),
    dict(id='ftd_fail', theme='short_interest', tier='C+', ranking='cross_section', raw_prior_direction=-1,
         citation='Evans, Geczy, Musto and Reed (2009, RFS) Failure Is an Option; Miller (1977, JF); Autore, Boulton '
                  'and Braga-Alves (2015, FinRev); "Informed short selling, fails-to-deliver, and abnormal returns" '
                  '(JEF 2016)',
         prior_sign_source='Evans-Geczy-Musto-Reed 2009; Autore-Boulton-Braga-Alves 2015',
         formula='ftd_shares_ratio21: SEC CNS fails-to-deliver summed over the 21 settlement dates ending at the '
                 'latest fully visible one, per share outstanding (sec-ftd-sum21-over-shares-v1); prior sign negative',
         form='R(x)', smoothing=1,
         domain='NaN when the latest visible settlement date is more than 60 days old (2020-11-30..2020-12-21: every '
                'member cell NaN for 16 sessions; NaN, never 0, no fill or carve-out: ew-theme-v1 treats the member as '
                'missing, no redistribution, 740 of 756 IC days remain) or shares_out is NaN; unmapped symbols count as '
                'no fail; unit defects (max 51.9) are bounded by the rank',
         deviation='no respelling; publication lag: S*(t) trails t by about 22-37 days (half-month publication + 7 d); '
                   'files are re-posted without revision history (vintage_risk); ETF and ADR operational fails are '
                   'outside the universe; the short leg is mostly borrow fee (Muravyev-Pearson-Pollet 2025), so net '
                   'value is expected in the long leg; no decay (the 21-date sum is its own smoothing)',
         prior='Evans-Geczy-Musto-Reed (2009 RFS): fails concentrate where borrowing is scarce or costly; Miller '
               '(1977 JF): binding shorting constraints let prices run high and later returns fall; Autore-Boulton-'
               'Braga-Alves (2015 FinRev): threshold-level FTD stocks overvalued, then reversing (t n/v); JEF 2016: '
               'informed shorting that cannot borrow; Reg SHO close-out buy-ins push prices up over days (+), '
               'dominated at a weeks-to-months horizon, so the prior is negative',
         expected_turnover='moderate (.04-.08/day [est]); max expected abs(rho) .25-.40 with si_ratio, dtc [est]'),
    dict(id='ea_overdue', theme='earnings_momentum', tier='C+', ranking='cross_section', raw_prior_direction=-1,
         citation='Johnson and So (2018, JFQA 53(6)) Time Will Tell: information in the timing of scheduled earnings '
                  'news; Bagnoli, Kross and Watts (2002, JAR)',
         prior_sign_source='Johnson-So 2018; Bagnoli-Kross-Watts 2002',
         formula='overdue flag 1{ea_days_to_expected < 0} = max(sign(-ea_days_to_expected), 0): the expected '
                 'announcement date has passed with no primary 8-K 2.02 visible since (sec-ea-days-to-expected-'
                 'yoy364-consume45-fb91-v1); prior sign negative',
         form='R(x)', smoothing=1,
         domain='NaN when ea_days_to_expected is NaN (no visible primary 2.02 within 200 days, overdue by more than 63 '
                'sessions, foreign private issuers on 6-K, REITs without 2.02); a binary flag: overdue names tie at '
                'the bottom rank and the rest tie above (4.08% of finite member cells flagged, 9-369 names per date)',
         deviation='respelled by the draft\'s alignment rule (prereg section 1): 1{ea_days_to_expected < 0}, '
                   'ea_days_since dropped; the cut < 0 stays (Ruling W2-b): .67% of cells read -1 for one session '
                   'from the 8-K acceptance lag (disclosed); the 8-K is the only clock (no advance scheduling notice), '
                   'so only the overdue state is observable; is_primary uses a later-10-Q label (small presence '
                   'look-ahead, W5a concern 2); COVID delays kept (overdue share peaks at 21.7% on 2020-05-01, no '
                   'carve-out)',
         prior='Johnson-So (2018 JFQA): later-than-expected scheduling precedes worse news, which prices reflect only '
               'at the announcement (t n/v); Bagnoli-Kross-Watts (2002 JAR): about 1 cent/share below consensus per '
               'day of delay and lower announcement returns',
         expected_turnover='high (event, sparse) [est]; max expected abs(rho) about .1 with sue, droe [est]'),
]
THEME_DESCRIPTIONS = {
    'earnings_momentum': ('Earnings news: SUE, change in ROE, change in tax expense, the 3-day earnings announcement '
                          'return, the streak of year-on-year quarterly earnings increases and an overdue earnings '
                          'announcement (the expected 8-K 2.02 date passed); good news predicts continuation and a late '
                          'announcement predicts lower returns.'),
    'short_interest': ('FINRA short interest ratio, days to cover, one-month change in short interest, the 126-session '
                       'FINRA shorting flow (short sale volume / volume, within FF12) and SEC fails-to-deliver per share '
                       'outstanding (21 settlement dates); heavy or rising shorting and delivery failures predict lower '
                       'returns.'),
    NEW_THEME: ('Informed-owner flows from SEC ownership filings: opportunistic insider net buying (Form 4) and 13F '
                'manager conviction; informed buying predicts higher returns.'),
}
NEW_THEME_TURNOVER = ('low to very low: the 126-session opportunistic insider aggregate (no decay) and quarterly 13F '
                      'conviction (s21 decay)')
PROMOTION_TESTS = [
    'P1 admission v4-prior-v1 (unchanged) on the restricted TRAIN role lo1: prior sign +1, reject below 250 finite '
    'TRAIN days or tau above .70, veto on HAC t below -2.0 (NW lag 5), redundancy greedy at |rho| <= .90 by '
    '(tier_rank, roster_order) against every admitted member, the 44 v7.0 members included (their status unchanged), '
    'the four wave-2 members last',
    'P2 book: library v7.1 x ew-theme-v1 refit on lo1 (10 themes when ins_opp or inst_best_ideas is admitted, else 9) '
    'x the reference construction (aim-partial-v5 theta .05 dust .1 fixed, delta orders, exit .05, locate-in-aim, '
    'liquidity cache, price-risk-v1) on fields-v9 with L FIXED at 1.247; paired S2 net dSR vs '
    'mega-nav-v70u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247 (Ruling W2-a: wave 1 accepted) must be > 0 (sign-only '
    'inside one SE; Memmel SE and LW CBB reported)',
    'P3 R6\' mechanics on that cell (all-rows gross in [.90, 1.05], |mean net| <= .02, tau mean <= .20 / p95 <= .30) '
    'and S2 net >= 1.0; freeze still needs cell-count DSR >= .95 at N = ledger lines at run time + 1 (35 when declared), '
    'effective-N DSR (ONC) and PBO reported beside it',
    'Identity before the cell (prereg section 5): (i) the 44 v7.0 entries are a byte-identical prefix; (ii) fields-v9 '
    'pinned (manifest sha256 8fd00e9f...d7b8769b); (iii) the u pass reproduces the parent\'s orientation and '
    'train_daily_ic member rows byte for byte; (iv) the reference construction on fields-v9 reproduces the v7.0 '
    'cell\'s S2 daily CSV bit for bit (Ruling W2-e); a miss aborts without a trial',
    'All-or-nothing: otherwise wave 2 is rejected whole, its members are not re-proposed in v7, the ranking is not '
    're-tuned on TRAIN, and admitted-but-losing members are disclosed, never dropped one by one',
]


def _load(name: str, file: str) -> ModuleType:
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    assert spec is not None and spec.loader is not None, file
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclass annotation resolution needs the module registered
    spec.loader.exec_module(module)
    return module


@functools.cache
def pinned_v70() -> tuple[ModuleType, bytes, bytes]:
    """The v7.0 generator, its library and recipe bytes re-derived and checked against the pins and the files."""
    module = _load('_frozen_generate_fund_ic_v70_for_v71', V70_GENERATOR)
    docs = module.documents()
    library_bytes, recipe_bytes = docs[V70_LIBRARY], docs[V70_RECIPE]
    assert hashlib.sha256(library_bytes).hexdigest() == V70_LIBRARY_SHA256, 'frozen v7.0 library bytes changed'
    assert hashlib.sha256(recipe_bytes).hexdigest() == V70_RECIPE_SHA256, 'frozen v7.0 recipe bytes changed'
    for name, blob in docs.items():
        assert (HERE / name).read_bytes() == blob, f'committed {name} differs from its generator'
    return module, library_bytes, recipe_bytes


def v70() -> ModuleType:
    return pinned_v70()[0]


def v4() -> ModuleType:
    """The pinned v4 validator copy (through v6.1 and v7.0); not modified."""
    return v70().v4()


def field_table() -> dict:
    """The v7.0 field table plus the four wave-2 fields (plain f64, no prior bar beyond the session)."""
    m = v4()
    extra = {f['name']: m.Field(f['name'], f['basis'], f['clock'], f['origin']) for f in NEW_FIELDS}
    assert not set(extra) & set(v70().field_table())
    return dict(v70().field_table(), **extra)


def parse(text: str):
    """The v7.0 parser (v4 grammar + the six v7.0 registry rows) over the extended field table."""
    return v70().parse(text, field_table())


def validate(cid: str, dsl: str) -> dict:
    """House static budget, no exception: <= 314 prior bars, <= 7 estimated peak slots, <= 5 extra fields,
    <= 4,096 bytes; the parser is the v7.0 one, so every operator is an existing registry row."""
    m = v4()
    tree, shape, _, lookback, native, text = parse(dsl)
    nodes, slots = m.grammar().peak_slots(tree)
    assert text == dsl and shape == 'panel' and lookback <= m.MAX_PRIOR_BARS, (cid, lookback)
    assert native <= lookback and nodes <= m.MAX_SLOTS and slots <= MAX_SLOTS_PER_CANDIDATE, (cid, nodes, slots)
    assert len(dsl.encode()) <= m.MAX_DSL_BYTES, cid
    fields = sorted(m.fields_of(tree))
    extras = [f for f in fields if f not in m.BASE_FIELDS]
    assert len(extras) <= MAX_EXTRAS_PER_CANDIDATE, (cid, extras)
    return dict(prior_bars=lookback, native_prior_bars=native, dag_nodes=nodes, estimated_peak_slots=slots,
                dsl_bytes=len(dsl.encode()), fields=fields, extra_fields=extras)


def base_of(x: dict, dsl: str) -> str:
    """The member's x: inside rank(.) for the R(x) form, inside rank(decay_linear(., 21)) for the smoothed one."""
    head, tail = ('rank(decay_linear(', f', {SMOOTHING}))') if x['form'] != 'R(x)' else ('rank(', ')')
    assert dsl.startswith(head) and dsl.endswith(tail), x['id']
    return dsl[len(head):-len(tail)]


def documents() -> dict[str, bytes]:
    m = v4()
    _, library_v70_bytes, recipe_v70_bytes = pinned_v70()
    library_v70, recipe_v70 = json.loads(library_v70_bytes), json.loads(recipe_v70_bytes)
    assert len(library_v70['candidates']) == V70_CANDIDATES
    table = field_table()
    for c in library_v70['candidates']:  # the extended table changes no parent parse
        assert parse(c['dsl']) == v70().parse(c['dsl']), c['id']
    declared70 = {f['name'] for f in library_v70['fields']}
    new_names = [f['name'] for f in NEW_FIELDS]
    assert not declared70 & set(new_names)
    declared = declared70 | set(new_names)
    themes71 = list(m.THEME_IDS) + [NEW_THEME]
    template_keys = list(recipe_v70['templates'][0])
    lineage_keys = list(recipe_v70['lineage'][0])
    new_candidates, templates, lineage, checks = [], [], [], {}
    for k, x in enumerate(MEMBERS):
        dsl = DSL[x['id']]
        check = validate(x['id'], dsl)
        checks[x['id']] = check
        assert set(check['fields']) <= declared, x['id']
        assert [f for f in check['extra_fields'] if f not in declared70] == check['extra_fields'], x['id']  # new field
        assert x['theme'] in themes71 and x['tier'] in m.TIER_RANK, x['id']
        assert x['theme'] not in m.WITHIN_INDUSTRY_THEMES and x['ranking'] == 'cross_section', x['id']  # rule R1
        tree = parse(dsl)[0]
        assert tree[:2] == ('call', 'CsRank'), x['id']  # the house cross-section rank is the only op on top
        base = base_of(x, dsl)
        labels = dict(theme=x['theme'], tier=x['tier'], tier_rank=m.TIER_RANK[x['tier']], prior_sign=1,
                      citation=x['citation'])
        candidate = dict(id=x['id'], family=x['theme'], dsl=dsl, horizons=[5, 21, 63],
                         sign_policy='train-rank-ic21', **labels)
        assert list(candidate) == list(library_v70['candidates'][0]), 'candidate key order differs from v7.0'
        new_candidates.append(candidate)
        template = dict(id=x['id'], theme=x['theme'], formula=x['formula'], base_dsl=base,
                        base_prior_bars=parse(base)[3], raw_prior_direction=x['raw_prior_direction'],
                        domain=x['domain'], deviation=x['deviation'], smoothing_form=x['form'],
                        smoothing_sessions=x['smoothing'])
        assert list(template) == template_keys
        templates.append(template)
        row = dict(id=x['id'], family=x['theme'], roster_order=V70_CANDIDATES + k + 1, **labels,
                   raw_prior_direction=x['raw_prior_direction'], prior_sign_source=x['prior_sign_source'],
                   ranking=x['ranking'], smoothing_form=x['form'], smoothing_sessions=x['smoothing'],
                   prior_bars=check['prior_bars'], native_prior_bars=check['native_prior_bars'],
                   dsl_sha256=hashlib.sha256(dsl.encode()).hexdigest(), fields=check['fields'],
                   v51_change='added in v7.1 (wave 2)')
        assert list(row) == lineage_keys
        lineage.append(row)
    candidates = library_v70['candidates'] + new_candidates
    ids = [c['id'] for c in candidates]
    assert len(ids) == len(set(ids)) == V70_CANDIDATES + len(MEMBERS) <= MAX_ROSTER
    assert len({c['dsl'] for c in candidates}) == len(candidates)
    families = [dict(f, description=THEME_DESCRIPTIONS[f['id']]) if f['id'] in THEME_DESCRIPTIONS else f
                for f in library_v70['families']]
    assert NEW_THEME not in {f['id'] for f in families}
    families.append(dict(id=NEW_THEME, description=THEME_DESCRIPTIONS[NEW_THEME]))
    assert len(families) <= m.MAX_FAMILIES and {x['theme'] for x in MEMBERS} == set(THEME_DESCRIPTIONS)
    assert {c['family'] for c in candidates} == {f['id'] for f in families}  # the runner refuses an empty family
    field_decls = [dict(name=f['name'], basis=f['basis']) for f in NEW_FIELDS]
    library = dict(schema=library_v70['schema'], id=LIBRARY_ID, fields=library_v70['fields'] + field_decls,
                   families=families, candidates=candidates)
    encode = lambda obj: (json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False) + '\n').encode()
    library_bytes = encode(library)
    # the 44 v7.0 entries are a byte-identical prefix of the v7.1 candidate list (same encoder, same depth)
    region = lambda blob: blob.split(b'"candidates": [\n', 1)[1]
    assert region(library_bytes).startswith(region(library_v70_bytes)[:-len(b'\n  ]\n}\n')] + b',\n')
    referenced = sorted({f for c in candidates for f in m.fields_of(parse(c['dsl'])[0])} - set(m.BASE_FIELDS))
    assert len(referenced) == 40 <= m.MAX_EXTRA_FIELDS and set(new_names) <= set(referenced)

    new_ids = [x['id'] for x in MEMBERS]
    themes = []
    for t in recipe_v70['themes']:
        added = [x for x in MEMBERS if x['theme'] == t['theme']]
        if added:
            t = dict(t, description=THEME_DESCRIPTIONS[t['theme']], members=t['members'] + [x['id'] for x in added],
                     expected_turnover_v71={x['id']: x['expected_turnover'] for x in added})
        themes.append(t)
    own = [x for x in MEMBERS if x['theme'] == NEW_THEME]
    themes.append(dict(theme=NEW_THEME, index=len(themes) + 1, description=THEME_DESCRIPTIONS[NEW_THEME],
                       members=[x['id'] for x in own], expected_turnover=NEW_THEME_TURNOVER,
                       expected_turnover_v71={x['id']: x['expected_turnover'] for x in own}))
    assert [t['theme'] for t in themes] == themes71
    sv = recipe_v70['static_validation']
    users = dict(sv['extra_field_users'])
    for x in MEMBERS:
        for f in checks[x['id']]['extra_fields']:
            users[f] = users.get(f, []) + [x['id']]
    static = dict(
        sv, rule=sv['rule'] + '; v7.1 adds the four fields-v9 fields ins_opportunistic_net, inst_best_ideas, '
                              'ftd_shares_ratio21, ea_days_to_expected; no registry row (rank, decay_linear, max and '
                              'sign are v7.0 rows); no budget exception',
        max_prior_bars=max(sv['max_prior_bars'], *(c['prior_bars'] for c in checks.values())),
        max_native_prior_bars=max(sv['max_native_prior_bars'], *(c['native_prior_bars'] for c in checks.values())),
        max_dag_nodes=max(sv['max_dag_nodes'], *(c['dag_nodes'] for c in checks.values())),
        max_estimated_peak_slots=max(sv['max_estimated_peak_slots'],
                                     *(c['estimated_peak_slots'] for c in checks.values())),
        extra_field_capacity=max(sv['extra_field_capacity'], *(len(c['extra_fields']) for c in checks.values())),
        declared_extra_fields=sv['declared_extra_fields'] + len(NEW_FIELDS), referenced_extra_fields=len(referenced),
        extra_field_users=dict(sorted(users.items())), v71=checks,
        v71_budget='house budget, no exception (library-v7-wave2-prereg.md section 2): 5 extra fields, 7 estimated '
                   'peak slots, 314 prior bars, 4,096 bytes')
    data70 = recipe_v70['data']
    data = dict(
        data70, fields=data70['fields'] + field_decls,
        field_clocks=dict(data70['field_clocks'], **{f['name']: f['clock'] for f in NEW_FIELDS}),
        field_origin=dict(data70['field_origin'], **{f['name']: f['origin'] for f in NEW_FIELDS}),
        requires=dict(data70['requires'], fields_v9=(
            f'the fields-v9 manifest as built ({FIELDS_V9_DIR}, sha256 {FIELDS_V9_MANIFEST_SHA256}): {FIELDS_V9_ROWS} '
            'rows = fields-v7 (41) + W5a SEC (14) + W5b holdings (8), every fields-v7 and fields-v8 payload '
            'byte-identical; 63 rows fit the IC runner cap of 64 (Ruling W2-d); v7.1 references 40 extras (cap 64); '
            'all four new fields point_in_time')),
        wave2_fields={f['name']: dict(formula_id=f['formula_id'], payload_sha256=f['sha256'], origin=f['origin'])
                      for f in NEW_FIELDS})
    hygiene = ('v7.1 appends exactly the four pre-registered wave-2 members (v7-prereg.md "Library v7.1", '
               f'library-v7-wave2-prereg.md sections 1-5 at {PREREG_COMMIT}) implemented literally: DSL strings '
               'verbatim from the registration (the two mechanical respellings versus the draft were registered, not '
               'decided here); the implementer read no TRAIN return statistic, no v7.0 per-candidate result, no '
               'validation statistic and no data dated 2023 or later, and did not relate any member to returns. The '
               '44 v7.0 entries are byte-identical.')
    gen70 = recipe_v70['generation']
    order = {x['id']: V70_CANDIDATES + k + 1 for k, x in enumerate(MEMBERS)}
    recipe = dict(
        schema=recipe_v70['schema'], id=f'{LIBRARY_ID}_initial',
        library=dict(path=LIBRARY, sha256=hashlib.sha256(library_bytes).hexdigest()),
        preregistration=PREREG,
        generation=dict(rule='v70-plus-wave2-v71', revision='initial', candidates=len(candidates),
                        families=len(families), family_is_theme=True, one_variant_per_hypothesis=True,
                        max_roster=MAX_ROSTER,
                        wrapper=('v7.0 members byte-identical; wave 2 takes the draft section 0 forms: R(x) = rank(x) '
                                 'for the aggregates and the event flag (ins_opp, ftd_fail, ea_overdue; like sv_flow) '
                                 'and R(decay_linear(x, 21)) for inst_best_ideas; no wave-2 theme is within-industry, '
                                 'so R = rank(.); DSL text verbatim from the registration'),
                        appended=new_ids, roster_order=order, v70_wrapper=gen70['wrapper'],
                        max_prior_bars=gen70['max_prior_bars'], complete_observations=gen70['complete_observations'],
                        grammar_helpers=gen70['grammar_helpers'],
                        prereg=dict(path=WAVE2_PATH, commit=PREREG_COMMIT, sections='1-5',
                                    by_reference_in=f'{PREREG_PATH} "Library v7.1 = parent + wave 2"'),
                        draft=dict(path=DRAFT_PATH, commit=DRAFT_COMMIT, sections='0 (conventions), 2 (wave 2) minus '
                                                                                   'eap_8k (dropped before any read)'),
                        parent_v70=dict(generator=V70_GENERATOR, library=V70_LIBRARY,
                                        library_sha256=V70_LIBRARY_SHA256, recipe=V70_RECIPE,
                                        recipe_sha256=V70_RECIPE_SHA256, candidates=V70_CANDIDATES,
                                        generator_sha256=gen70['generator_sha256'],
                                        ruling='W2-a: wave 1 accepted, so the parent is v7.0 (all 44 entries, including '
                                               'its non-admitted members) and the reference cell is the v7.0 cell'),
                        generator_sha256=hashlib.sha256(
                            Path(__file__).read_bytes().replace(b'\r\n', b'\n')).hexdigest()),
        orientation=recipe_v70['orientation'],
        labels=dict(recipe_v70['labels'], tier_basis_v71='wave-2 tiers from library-v7-wave2-prereg.md section 1 '
                                                         '(v6-literature scale): ins_opp B-, inst_best_ideas C+, '
                                                         'ftd_fail C+, ea_overdue C+'),
        themes=themes,
        within_industry=recipe_v70['within_industry'],
        industry_level_price_members=recipe_v70['industry_level_price_members'],
        industry_demeaned_members=recipe_v70['industry_demeaned_members'],
        v6_changes=recipe_v70['v6_changes'],
        v61_changes=recipe_v70['v61_changes'],
        v70_changes=recipe_v70['v70_changes'],
        v71_changes=[dict(id=x['id'], change='added', detail=f"prereg v7.1 wave 2 rank {k + 1}: theme {x['theme']}, "
                                                              f"tier {x['tier']}, raw prior direction "
                                                              f"{x['raw_prior_direction']:+d} (embedded)")
                     for k, x in enumerate(MEMBERS)] + [
            dict(id=NEW_THEME, change='family declared', detail='ownership_flow is declared in v7.1 with its first '
                                                                 'members (the runner refuses an empty declared family; '
                                                                 'the fitter\'s theme list carries it since v7.0)')],
        literature=dict(recipe_v70['literature'], priors_v71={x['id']: x['prior'] for x in MEMBERS},
                        respellings_v71={cid: dict(draft=DRAFT_DSL[cid], registered=DSL[cid], reason=why)
                                         for cid, why in RESPELLINGS.items()},
                        pending_before_freeze='root copies the JEF 2016 FTD paper\'s authors and the "t n/v" values '
                                              'from the papers (prereg Q6; not a variant)'),
        needs_new_field=recipe_v70['needs_new_field'],
        family_fixing=dict(fixed_before_measurement=True, measurement_consulted=False, statement=hygiene,
                           v70_statement=recipe_v70['family_fixing']['statement'],
                           v61_statement=recipe_v70['family_fixing']['v61_statement'],
                           v6_statement=recipe_v70['family_fixing']['v6_statement']),
        templates=recipe_v70['templates'] + templates,
        lineage=recipe_v70['lineage'] + lineage,
        data=data,
        admission=dict(policy='v4-prior-v1 (T23; prereg R3; v7-prereg "Library v7.1": unchanged)', trials=len(MEMBERS),
                       trials_basis='v7-prereg: 4 admission trials (only the wave-2 members are new; the 44 v7.0 '
                                    'members are identical definitions on identical payloads and keep their status; '
                                    'eap_8k dropped before any read; inst_breadth_chg, k8_item_material_21 and '
                                    'regsho_threshold_days63 at 0 trials)',
                       v70_policy=recipe_v70['admission']['policy']),
        composition=dict(policy='v7-prereg "Library v7.1": ew-theme-v1 refit on lo1 (+1 composition), unchanged rule',
                         themes='the 9 v7.0 families + ownership_flow (declared in v7.1); ew-theme-v1 counts only '
                                'themes with an admitted member: 9 -> 10 themes (1/9 -> 1/10 each) iff ins_opp or '
                                'inst_best_ideas is admitted, ownership_flow\'s share split equally among its admitted '
                                'members; an admitted ftd_fail (ea_overdue) takes 1/(n+1) of short_interest\'s '
                                '(earnings_momentum\'s) weight'),
        promotion_tests=PROMOTION_TESTS,
        trials=dict(generated_candidates=len(candidates), admission_trials=len(MEMBERS), new_candidates=len(MEMBERS),
                    unchanged_candidates=V70_CANDIDATES, orientation_fits=0, composition_recipes=1,
                    construction_recipes=1, variants_per_hypothesis=1, dsr_n=DSR_N,
                    dsr_n_rule='cross-cell N = trial-ledger lines at run time + 1 (34 ledgered when declared; the run '
                               'order of the v7.1, spo-v2 and U-lo3 cells decides the number, each adds one)',
                    family_or_template_selection=False, validation_selection=False),
        static_validation=static,
        qualification=dict(native_parse_vm='pending: root u pass compile against fields-v9 (44 cache hits + 4 '
                                           'evaluations expected)',
                           empirical='unmeasured at source freeze',
                           prior_phase='v1-v7.0 files and generators are unchanged by v7.1'))
    return {LIBRARY: library_bytes, RECIPE: encode(recipe)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    docs = documents()
    for name, expected in docs.items():
        path = HERE / name
        if args.check:
            if not path.is_file() or path.read_bytes() != expected:
                raise SystemExit(f'fixed artifact differs: {name}')
        else:
            path.write_bytes(expected)
        print(f'{name} {hashlib.sha256(expected).hexdigest()} {len(expected)} bytes')
    library = json.loads(docs[LIBRARY])
    recipe = json.loads(docs[RECIPE])
    print(f"candidates {len(library['candidates'])}: v7.0 {V70_CANDIDATES} byte-identical + "
          f"{[c['id'] for c in library['candidates'][V70_CANDIDATES:]]}; families {len(library['families'])}; "
          f"generator sha256 (LF) {recipe['generation']['generator_sha256']}")
    for cid, c in recipe['static_validation']['v71'].items():
        print(f"  {cid}: prior bars {c['prior_bars']}, dag nodes {c['dag_nodes']}, peak slots "
              f"{c['estimated_peak_slots']}, bytes {c['dsl_bytes']}, extras {c['extra_fields']}")


if __name__ == '__main__':
    main()
