"""Stage ``events`` table ``guidance`` (S6.4): numeric EPS and revenue guidance from earnings-release EX-99 text.

Rule-based, no model. Inputs: the EX-99 exhibits of original 8-Ks carrying item 2.02 (and 7.01 guidance updates)
from 2019 (ruling D7): the landed v2 objects (``data/raw/sec-earnings-release``) first, then the exhibits located by
EDGAR full-text search and fetched into ``data/raw/sec_events`` (``events_sources``). Clock and fiscal period from
``earnings_calendar/announcements.parquet`` / ``sec_filings/filings.parquet``; issuer link from
``identity/link_table.parquet``.

Extraction (:func:`extract_guidance`, pure):

* The text is split into lines (block elements) and sentences. Table lines (`` | `` cells) inside an outlook
  section are read as guidance tables (:func:`_table_rows`): a header line with period labels, then rows whose label
  is a measure.
* A **value** is a US-dollar amount: a range (``$2.47 to $2.55``, ``$27.5-$28.5 million``, ``between $X and $Y``), a
  point (``of approximately $X``), a floor (``at least $X``) or a ceiling (``up to $X``). ``$(0.10)`` and
  ``loss per share`` are negative. Other currencies (``C$``, ``€``) and percentages are ignored.
* The **measure** of a value is the nearest measure keyword before it in the clause: ``EPS`` (earnings / income /
  loss per [diluted] share, EPS; a value followed by ``per [diluted] share`` is EPS unless the keyword is FFO, AFFO,
  dividend, distributable earnings, net investment income or book value) or ``REVENUE`` (revenue(s), net sales,
  sales, with total / net / consolidated / GAAP / adjusted / operating modifiers only; segment revenue is skipped).
  Any other measure keyword nearer the value (EBITDA, operating income, cash flow, capex, tax rate, FFO, margin,
  expenses...) blocks it. Change amounts (``increase by``, ``growth of``, ``impact``, ``headwind``) are skipped.
  Revenue needs a scale (million / billion) or a value >= 100,000; EPS must be below 1,000 in absolute value.
* The clause must carry a forward cue (expect, anticipate, guidance, outlook, forecast, project, target, sees,
  reaffirm, raise, lower, narrow, update, maintain, reiterate, initiate, introduce, provide) and no historical marker
  (exceeded, beat, achieved, came in, in line with / within / above / below guidance, was / were ...).
* **Prior guidance** in the same clause (``compared to prior guidance of``, ``previously``, ``from $A to $B`` after a
  change verb, ``from a range of X to a range of Y``) fills ``prior_low`` / ``prior_high`` and is never a new row.
* **Period**: the nearest period expression before the value in the sentence (else after it, else the section
  heading, else the line prefix): fiscal year (``fiscal 2025``, ``full-year 2025``, ``FY25``, ``2025 outlook``) or
  quarter (``second quarter of 2025``, ``Q2 2025``, ``Q2 FY25``); a year-less quarter or ``full year`` is inferred
  from the reported period (``period_basis = 'inferred'``). A period that is not after the reported fiscal period
  (``release_period_end``) is historical and dropped.
* **Basis**: ``ADJUSTED`` (non-GAAP, adjusted, core, operating, ongoing, excluding, pro forma, cash EPS), ``GAAP``
  (GAAP, reported), else ``UNSPECIFIED``.
* **Direction** in text (``text_direction``): raised / lowered / maintained / narrowed / initiated / updated; the
  numeric ``revision`` compares the midpoint with the previous row of the same (cik, measure, basis, period):
  raised / lowered / maintained (within 0.5%) / new.

Rows are deduplicated per (accession, measure, basis, period, low, high, point). ``available_at`` is the 8-K's EDGAR
acceptance (sec_filings rule); nothing is overwritten (each release is its own row set).
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from . import common as C

EXTRACTOR_VERSION = "guidance-rules-v1"
STAGE = "events"
TABLE = "guidance"
SCHEMA = "atx.alpha-panel.events.guidance/v1"

# ---------------------------------------------------------------------------------------------------------
# Lexicon
# ---------------------------------------------------------------------------------------------------------

_NUM = r"\d{1,3}(?:,\d{3})+(?:\.\d+)?|\d+(?:\.\d+)?|\.\d+"
_UNIT = r"billion|million|thousand|bn|mm|mn|b|m|k"
#: One dollar amount. ``pre`` catches a currency prefix glued to the sign (C$, A$, US$ ...), ``neg`` a
#: parenthesised loss.
_MONEY = (r"(?:(?P<pre@>\b[A-Z]{1,3})(?=\$))?(?P<nega@>\()?\s*\$\s*(?P<negb@>\()?\s*(?P<n@>" + _NUM +
          r")(?P<negc@>\s*\))?(?:\s*(?P<u@>" + _UNIT + r")\b)?")
_MONEY2 = (r"(?P<nega@>\()?\s*\$?\s*(?P<negb@>\()?\s*(?P<n@>" + _NUM + r")(?P<negc@>\s*\))?(?:\s*(?P<u@>"
           + _UNIT + r")\b)?")
_RANGE_SEP = r"\s*(?:-|to|through)\s*"
VALUE_RE = re.compile(
    r"(?P<between>between\s+)?" + _MONEY.replace("@", "1") + r"(?:(?(between)\s*(?:and|to|-)\s*|" + _RANGE_SEP + r")"
    + _MONEY2.replace("@", "2") + r")?(?!\s*%)(?![\d.]\d)",
    re.IGNORECASE,
)

EPS_KW = re.compile(
    r"\b(?:(?:fully\s+)?diluted\s+|basic\s+)?(?:net\s+)?(?:earnings|income|loss|earnings\s*\(loss\)|"
    r"income\s*\(loss\)|\(loss\)\s*earnings)\s+per\s+(?:(?:fully\s+)?diluted\s+|basic\s+|common\s+|ordinary\s+|"
    r"weighted\s+(?:average\s+)?)*share\b|\bEPS\b|\bearnings\s+(?:per|a)\s+share\b",
    re.IGNORECASE,
)
PER_SHARE_AFTER = re.compile(
    r"^\s*(?:\)|,)?\s*(?:in\s+)?(?:(?:adjusted|non-gaap|gaap|core|diluted)\s+)?(?:earnings\s+|eps\s+|net\s+income\s+)?"
    r"per\s+(?:(?:fully\s+)?diluted\s+|basic\s+|common\s+|ordinary\s+)*share|^\s*(?:in\s+)?(?:adjusted\s+|diluted\s+)*eps\b",
    re.IGNORECASE,
)
REV_KW = re.compile(
    r"\b(?P<mods>(?:(?:total|net|consolidated|gaap|non-gaap|adjusted|reported|operating|company|full[- ]year|"
    r"fiscal|quarterly|annual)\s+)*)(?P<kw>revenues?|net\s+sales|sales)\b",
    re.IGNORECASE,
)
#: Words directly before a revenue keyword that make it a segment / component (skipped) or a non-revenue measure.
REV_BLOCK_BEFORE = re.compile(
    r"(?:^|\W)(?:cost\s+of|costs\s+of|same[- ]store|comparable|comp|organic|product|products|service|services|subscription|"
    r"license|licensing|recurring|segment|hardware|software|cloud|platform|transaction|advertising|equipment|"
    r"segment's|international|domestic|u\.s\.|us|retail|wholesale|contract|interest|fee|fees|premium|premiums|"
    r"rental|lease|product\s+and\s+service|gross|days|per|of\s+the|pro\s+forma|digital|online|e-commerce|"
    r"consumer|commercial|merchandise|food|store|stores|brand|system[- ]wide|systemwide|franchise|revpar|"
    r"deferred|unearned|non-interest|noninterest|ex-fuel|excluding\s+fuel|tax|sales\s+and|and|higher|lower|"
    r"increased|decreased|incremental|additional|segment\s+\w+)\s*$",
    re.IGNORECASE,
)
SEG_OK = frozenset({
    "total", "net", "consolidated", "gaap", "adjusted", "company", "our", "its", "the", "and", "of", "for", "fiscal",
    "full", "full-year", "quarter", "year", "annual", "outlook", "guidance", "expects", "expected", "reported",
    "non-gaap", "operating", "raises", "raised", "reaffirms", "reaffirmed", "maintains", "maintained", "updates",
    "updated", "lowers", "lowered", "sees", "increases", "increased", "narrows", "narrowed", "provides", "initiates",
    "issues", "projects", "anticipates", "reiterates", "guides", "targets", "forecasts", "record", "quarterly",
    "first", "second", "third", "fourth", "fy", "estimated", "projected", "expect", "raise", "reaffirm", "maintain",
    "update", "lower", "increase", "narrow", "introduces", "confirms", "affirms", "boosts", "cuts", "reduces",
    "company's", "corporation", "inc", "includes", "range", "reported", "comparable", "current", "prior",
})
REV_BLOCK_AFTER = re.compile(r"^\s*(?:growth|per|and\s+marketing|&\s+marketing|tax|taxes|force|cycle|days|"
                             r"backlog|multiple|mix|volume|volumes|margin|price|prices|representation|"
                             r"representatives?|teams?|organization|agreements?|channels?|offices?|personnel|"
                             r"professionals|commissions?|pipeline|agents?|leaders?|leadership|process)\b",
                             re.IGNORECASE)
OTHER_KW = re.compile(
    r"\b(?:ebitda(?:re)?|ebit|ebitdar|operating\s+(?:income|profit|margin|loss|expenses?|cash\s+flow)|"
    r"net\s+income|net\s+loss|net\s+earnings|income\s+from\s+operations|pre-tax\s+income|pretax\s+income|"
    r"(?:free\s+)?cash\s+flows?|capital\s+(?:expenditures|spending|investment|investments|budget|plan)|capex|"
    r"expenses?|costs?|spending|interest|tax(?:es)?|tax\s+rate|depreciation|amortization|d&a|margins?|"
    r"gross\s+profit|ffo|affo|funds\s+from\s+operations|dividends?|distributions?|repurchases?|buybacks?|"
    r"share\s+repurchase|distributable\s+earnings|net\s+investment\s+income|nii|book\s+value|tangible\s+book|"
    r"noi|net\s+operating\s+income|backlog|bookings|billings|arr|annual\s+recurring\s+revenue|"
    r"remaining\s+performance\s+obligations|rpo|cash|debt|liquidity|borrowings|proceeds|investments?|"
    r"production|volume|volumes|earnings|income|profit|loss|charges?|impairment|synergies|savings|"
    r"contribution|headwind|tailwind|impact|benefit|revpar|same[- ]store|gmv|gross\s+merchandise|"
    r"premiums?|assets|deposits|loans|aum|dilution|shares|share\s+count|weighted\s+average|stock[- ]based)\b",
    re.IGNORECASE,
)
NON_EPS_PER_SHARE = re.compile(
    r"\b(?:ffo|affo|funds\s+from\s+operations|core\s+ffo|dividends?|distributions?|distributable\s+earnings|"
    r"net\s+investment\s+income|nii|book\s+value|nav|net\s+asset\s+value|cash\s+flow|free\s+cash\s+flow|"
    r"adjusted\s+funds|normalized\s+ffo|fad|cad|ebitda)\b[^$]{0,60}$",
    re.IGNORECASE,
)
#: Generic profit words that become EPS when the value is followed by "per share" ("earnings of $2.10 per share").
EPS_GENERIC = re.compile(r"(?:net\s+)?(?:earnings|income|loss|profit)|eps", re.IGNORECASE)
#: A base-year actual right before the value ("versus 2023 adjusted EPS of $3.84"): not guidance.
BASE_BEFORE = re.compile(r"\b(?:versus|vs\.?|compared\s+(?:to|with)|over|from|relative\s+to|off)\s+(?:the\s+)?"
                         r"(?:fiscal\s+|full[- ]year\s+|calendar\s+)?(?:(?:19|20)\d{2}|last\s+year|prior\s+year|"
                         r"the\s+prior\s+year)\b[^$]{0,70}\b(?:of|was|were|at)\s*$|"
                         r"\bbase\s+(?:year\s+)?(?:of|at)?\s*$", re.IGNORECASE)
#: Sentences that decompose or reconcile guidance into components: their amounts are not guidance.
COMPONENT_SENTENCE = re.compile(r"\bdifference\s+between\b|\breconcil\w*\b", re.IGNORECASE)
#: A change verb right before the measure keyword ("expected to reduce total revenues $70 to $100 million").
CHANGE_BEFORE_KW = re.compile(r"\b(?:reduc|decreas|increas|lower|impact|affect|add|boost|offset|dilut|contribut)\w*"
                              r"\s+(?:\S+\s+){0,5}$", re.IGNORECASE)
KW_VALUE_LINK = re.compile(r"\b(?:to|of|at|be|approximately|about|between|range|around|roughly|near|is|are)\b",
                           re.IGNORECASE)
#: A value followed by "of <something other than revenue>" belongs to that noun ("$5.3 million of severance").
OF_OTHER_AFTER = re.compile(r"^\s*of\s+(?!(?:total\s+|net\s+|consolidated\s+|gaap\s+|adjusted\s+)?(?:revenues?|net\s+"
                            r"sales|sales)\b)(?!approximately|about|around)[a-z]", re.IGNORECASE)
#: Results words that stop an outlook heading: a release title ("Reports Q4 Results; Provides 2020 Outlook").
HEADING_NOT_OUTLOOK = re.compile(r"\b(?:reports?|reported|announces?|results|posts?|delivers?|highlights|record)\b",
                                 re.IGNORECASE)
#: Sentences that only an outlook heading makes forward-looking must not describe the reported period.
OUTLOOK_ONLY_BLOCK = re.compile(r"\bin\s+the\s+quarter\b|\byear[- ]to[- ]date\b|\bfor\s+the\s+(?:first|second|third|"
                                r"fourth)\s+quarter\s+(?:was|were)\b", re.IGNORECASE)
PRIOR_GUIDANCE_BEFORE = re.compile(r"\b(?:previous|previously|prior|original|originally|earlier|initial)\b[^$.;]{0,70}"
                                   r"\b(?:guidance|outlook|range|expectations?|estimates?|forecast)\b[^$]{0,20}$",
                                   re.IGNORECASE)
VALUE_IS_CHANGE = re.compile(r"^\s*(?:\w+\s+)?(?:increase|decrease|reduction|decline|improvement|headwind|tailwind|"
                             r"impact|benefit|drag|uplift|contribution|year[- ]over[- ]year\s+(?:increase|decrease)|"
                             r"higher|lower|more|less|above|below)\b", re.IGNORECASE)
MAINTAIN_PRIOR = re.compile(r"\b(?:maintain|reaffirm|reiterat|confirm|affirm)\w*\s+(?:its|the|our|their)?\s*"
                            r"(?:prior|previous|previously\s+(?:issued|provided|announced))\b[^$]{0,60}$", re.IGNORECASE)
CHANGE_BETWEEN = re.compile(
    r"^\s*(?:by|of\s+up\s+to)\b|\bby\s+(?:approximately|about|roughly|around|nearly|over)?\s*$|"
    r"\b(?:increase|decrease|grow|decline|reduce|improve|rise|expand|contract)\w*\s+(?:by|of)\b|\bgrowth\s+of\b|"
    r"\bimpact\b|\bheadwind\b|\btailwind\b|\bbenefit\b|\bcontribution\b|\bincremental\b|\bchange\b|"
    r"\bup\s+\$|\bdown\s+\$|\bexclud|\bexcept\b|\bother\s+than\b",
    re.IGNORECASE,
)
FORWARD_CUE = re.compile(
    r"\b(?:expect(?:s|ed|ing|ation|ations)?|anticipat(?:e|es|ed|ing)|guidance|guide[sd]?|outlook|"
    r"forecast(?:s|ed|ing)?|project(?:s|ed|ing|ion|ions)?|targets?|targeting|sees|reaffirm\w*|reiterat\w*|"
    r"rais(?:e|es|ed|ing)\s+(?:its|the|our|full|fiscal|\d)|narrow(?:s|ed|ing)|updat(?:e|es|ed|ing)\s+(?:its|the|our|"
    r"full|fiscal|\d)|estimat(?:e|es|ed)\s+(?:its|that|the|our|full|fiscal|\d)|now\s+(?:sees|expects)|"
    r"(?:is|are)\s+(?:now\s+)?(?:expected|projected|forecast|anticipated)|to\s+be\s+in\s+the\s+range)\b",
    re.IGNORECASE,
)
HISTORICAL = re.compile(
    r"\b(?:exceed(?:ed|ing|s)?|beat|beating|surpass\w*|achiev\w*|came\s+in|coming\s+in|in\s+line\s+with|"
    r"within\s+(?:our|its|the|their)\s+(?:\w+\s+){0,3}(?:guidance|outlook|range|expectations?)|"
    r"(?:above|below|at)\s+(?:the\s+)?(?:high|top|upper|low|lower|bottom)[- ]end\s+of\s+(?:our|its|the|their)\s+"
    r"(?:\w+\s+){0,3}(?:guidance|outlook|range)|ahead\s+of|versus\s+(?:our|its)\s+(?:guidance|outlook)|"
    r"was|were|reported|delivered|generated|totaled|totalled|included|"
    r"(?:increased|decreased|grew|rose|fell|declined|improved)\s+(?:by\s+)?\d+|"
    r"compared\s+(?:to|with)\s+(?:\$|the\s+prior[- ]year|last\s+year|a\s+(?:loss|profit))|"
    r"prior[- ]year\s+(?:quarter|period)|year[- ]ago|year[- ]to[- ]date|resulting\s+in|resulted\s+in)\b",
    re.IGNORECASE,
)
#: Right after a value: the value was a result measured against guidance ("$2.74 ... achieved the high end").
HIST_AFTER = re.compile(
    r"^[^.;]{0,60}?\b(?:achiev\w*|exceed\w*|beat|beating|surpass\w*|came\s+in|was\s+(?:above|below|within|in\s+line|"
    r"at\s+the)|were\s+(?:above|below|within)|in\s+line\s+with|(?:above|below|within)\s+(?:the\s+)?(?:high|top|upper|"
    r"low|lower)?[- ]?(?:end\s+of\s+)?(?:our|its|the|their)\s+(?:\w+\s+){0,3}(?:guidance|outlook|range))",
    re.IGNORECASE,
)
PRIOR_BEFORE = re.compile(
    r"(?:(?:compared|comparable)\s+(?:to|with)|versus|vs\.?|from|previously|prior|previous|originally|original|"
    r"up\s+from|down\s+from|replac\w*|earlier)\s+(?:(?:the|our|its|a|an|their)\s+)?"
    r"(?:(?:company's|prior|previous|previously|original|originally|earlier|last|most\s+recent|february|may|august|"
    r"november|provided|issued|announced|stated|communicated|given)\s+)*"
    r"(?:(?:guidance|outlook|range|expectations?|estimate|forecast)\s+)?(?:(?:range\s+)?(?:of|for|at|was|range))?\s*"
    r"(?:(?:approximately|about|around|a\s+range\s+of)\s+)?$",
    re.IGNORECASE,
)
FROM_TO = re.compile(r"\bfrom\s+(?:a\s+range\s+of\s+|the\s+range\s+of\s+|approximately\s+)?$", re.IGNORECASE)
RANGE_FROM = re.compile(r"\b(?:rang(?:e|es|ing)|range\s+of)\s+from\s+$", re.IGNORECASE)
FLOOR_BEFORE = re.compile(r"\b(?:at\s+least|more\s+than|greater\s+than|in\s+excess\s+of|exceed|over|above|"
                          r"no\s+less\s+than|minimum\s+of)\s*$", re.IGNORECASE)
CEIL_BEFORE = re.compile(r"\b(?:up\s+to|less\s+than|no\s+more\s+than|below|under|maximum\s+of|not\s+to\s+exceed)"
                         r"\s*$", re.IGNORECASE)
QUALIFIER_BEFORE = re.compile(r"\b(?P<q>high|top|upper|low|lower|bottom)[- ]end\b|\b(?P<m>midpoint)\b", re.IGNORECASE)
ADJ_BASIS = re.compile(r"\b(?:non-?gaap|adjusted|core|operating|ongoing|comparable|excluding|ex-items|pro\s+forma|"
                       r"cash\s+eps|normalized|underlying|recurring|economic|before\s+special|management)\b",
                       re.IGNORECASE)
GAAP_BASIS = re.compile(r"(?<!non-)(?<!non)\bgaap\b|\breported\b", re.IGNORECASE)
DIRECTION = (
    ("raised", re.compile(r"\b(?:rais(?:e|es|ed|ing)|increas(?:e|es|ed|ing)\s+(?:its|the|our|their)|boost\w*|"
                          r"upgrad\w*|up\s+from|revis\w*\s+upward\w*|increas(?:e|es|ed|ing)\s+(?:\S+\s+){0,5}?"
                          r"(?:guidance|outlook|range|forecast|end\s+of))\b", re.IGNORECASE)),
    ("lowered", re.compile(r"\b(?:lower(?:s|ed|ing)\s+(?:its|the|our|their)|lowers|lowered|reduc(?:e|es|ed|ing)\s+"
                           r"(?:its|the|our|their)|cut(?:s|ting)?\s+(?:its|the|our|their)|revis\w*\s+down\w*|"
                           r"downgrad\w*|down\s+from|trim\w*|(?:decreas|reduc)(?:e|es|ed|ing)\s+(?:\S+\s+){0,5}?"
                           r"(?:guidance|outlook|range|forecast|end\s+of))\b", re.IGNORECASE)),
    ("narrowed", re.compile(r"\bnarrow\w*\b|\btighten\w*\b", re.IGNORECASE)),
    ("maintained", re.compile(r"\b(?:reaffirm\w*|reiterat\w*|maintain\w*|confirm\w*|unchanged|affirm\w*|"
                              r"continues?\s+to\s+expect|remains?\s+(?:unchanged|on\s+track))\b", re.IGNORECASE)),
    ("initiated", re.compile(r"\b(?:initiat\w*|introduc\w*|establish\w*|issu(?:es|ed|ing)|provid(?:es|ed|ing))\b",
                             re.IGNORECASE)),
    ("updated", re.compile(r"\bupdat\w*\b|\brevis\w*\b", re.IGNORECASE)),
)

_QWORD = {"first": 1, "1st": 1, "second": 2, "2nd": 2, "third": 3, "3rd": 3, "fourth": 4, "4th": 4}
PERIOD_RE = re.compile(
    r"(?P<q1>first|second|third|fourth|1st|2nd|3rd|4th)(?:[- ]fiscal)?[- ]quarter(?:\s+(?:of\s+)?(?:(?:fiscal|fy)\s*(?:year\s+)?)?"
    r"(?P<y1>(?:19|20)\d{2}|'\d{2}))?"
    r"|\b(?:fiscal\s+|fy\s*)?(?P<y2>(?:19|20)\d{2})\s+(?P<q2>first|second|third|fourth)[- ]quarter"
    r"|\bQ(?P<q3>[1-4])(?:\s*(?:of\s+)?(?:FY|F|fiscal\s+)?\s*'?(?P<y3>(?:19|20)\d{2}|\d{2}))?(?![\d.])"
    r"|\b(?:FY|F)\s*'?(?P<y6>(?:19|20)\d{2}|\d{2})\s+Q(?P<q6>[1-4])\b"
    r"|\b(?P<y8>(?:19|20)\d{2})\s*Q(?P<q8>[1-4])\b"
    r"|\b(?:full[- ]year|fiscal[- ]year|fiscal|fy|calendar[- ]year|calendar|year)\s*'?(?P<y4>(?:19|20)\d{2}|\d{2})\b"
    r"(?!\s*(?:first|second|third|fourth)[- ]quarter)"
    r"|\b(?P<y5>(?:19|20)\d{2})\s+(?:full[- ]year|fiscal[- ]year|annual|outlook|guidance|financial\s+outlook|"
    r"financial\s+guidance|earnings\s+guidance|business\s+outlook|expectations|ongoing\s+earnings|"
    r"(?:adjusted\s+|non-gaap\s+|gaap\s+|core\s+)?(?:diluted\s+)?(?:eps|earnings)|revenues?|net\s+sales|sales|"
    r"targets?|forecast)\b"
    r"|\bfor\s+(?:the\s+(?:full\s+)?(?:fiscal\s+)?year\s+)?(?P<y7>(?:19|20)\d{2})\b"
    r"|\b(?P<fy0>full[- ]year|fiscal[- ]year|full\s+fiscal\s+year|the\s+year|annual|year[- ]end)\b"
    r"|\b(?P<rq>current|next|coming|upcoming|following)\s+(?:fiscal\s+)?quarter\b"
    r"|\b(?P<hq>(?:first|second)\s+half)\b",
    re.IGNORECASE,
)
SENT_SPLIT = re.compile(r"(?<=[a-z0-9%)\"'])\.\s+(?=[A-Z(\"'$])|(?<=[.!?])\s+(?=[A-Z][a-z])|\s{2,}")
PLUS_MINUS = re.compile(r"^\s*,?\s*(?:\+/-|\+/\s*-|±|plus\s+or\s+minus)\s*$", re.IGNORECASE)
PLUS_MINUS_PCT = re.compile(r"^\s*,?\s*(?:\+/-|\+/\s*-|±|plus\s+or\s+minus)\s*(\d{1,2}(?:\.\d+)?)\s*(?:%|percent)",
                            re.IGNORECASE)
ABBREV_END = re.compile(r"\b(?:inc|corp|co|ltd|no|approx|u\.s|vs|jan|feb|mar|apr|jun|jul|aug|sep|sept|oct|nov|dec|"
                        r"st|mr|ms|dr|e\.g|i\.e)$", re.IGNORECASE)
HEADING_CUE = re.compile(r"\b(?:outlook|guidance|expectations|forecast|financial\s+targets?)(?:\b|\d)",
                         re.IGNORECASE)
SAFE_HARBOR = re.compile(r"^(?:forward[- ]looking\s+statements?|safe\s+harbor|cautionary\s+(?:note|statement))",
                         re.IGNORECASE)
TABLE_UNIT = re.compile(r"\((?:\$\s*)?in\s+(?P<u>millions|billions|thousands)|\$\s*in\s+(?P<u2>millions|billions|"
                        r"thousands)|\((?P<u3>millions|billions|thousands)", re.IGNORECASE)

SCALE = {"billion": 1e9, "bn": 1e9, "b": 1e9, "million": 1e6, "mm": 1e6, "mn": 1e6, "m": 1e6, "thousand": 1e3,
         "k": 1e3, "millions": 1e6, "billions": 1e9, "thousands": 1e3}


# ---------------------------------------------------------------------------------------------------------
# Period arithmetic
# ---------------------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class ReportedPeriod:
    """The release's reported fiscal period: its end date, the issuer's fiscal-year end, and (when the release
    names it, e.g. "third quarter of fiscal 2025") the company's own (fiscal year label, quarter)."""

    period_end: dt.date | None
    fye_month: int = 12
    fye_day: int = 31
    label: tuple[int, int] | None = None

    def fiscal_quarter(self) -> tuple[int, int] | None:
        """(fiscal year label, quarter 1-4): the company's stated label, else from ``period_end`` with the label =
        calendar year of the fiscal year's end."""
        if self.label is not None:
            return self.label
        if self.period_end is None:
            return None
        pe = self.period_end
        # months from the fiscal-year start (the month after the FYE month) to the period end month
        start_month = self.fye_month % 12 + 1
        months = (pe.month - start_month) % 12 + 1
        # 52/53-week years end within a week of the month end: a period ending in the first 7 days belongs to the
        # prior month
        if pe.day <= 7:
            months = (months - 2) % 12 + 1
            m_adj = (pe.month - 2) % 12 + 1
        else:
            m_adj = pe.month
        q = min(4, max(1, (months + 2) // 3))
        fy = pe.year + (1 if m_adj > self.fye_month else 0)
        if pe.day <= 7 and pe.month == 1 and m_adj == 12:
            fy = pe.year - 1 + (1 if 12 > self.fye_month else 0)
        return fy, q


def period_end_estimate(fy: int, q: int | None, fye_month: int, fye_day: int = 31) -> dt.date:
    """Approximate end of fiscal ``fy`` (label = calendar year of its end) or its quarter ``q``."""
    end_month = fye_month
    if q is not None:
        end_month = (fye_month - 3 * (4 - q) - 1) % 12 + 1
    year = fy if end_month <= fye_month else fy - 1
    day = 28 if end_month == 2 else 30 if end_month in (4, 6, 9, 11) else 31
    return dt.date(year, end_month, min(day, fye_day if end_month == fye_month else day))


def _month_end(y: int, m: int) -> dt.date:
    nxt = dt.date(y + (m == 12), m % 12 + 1, 1)
    return nxt - dt.timedelta(days=1)


def guided_period_end(period_type: str, fy: int | None, q: int | None, rep: ReportedPeriod) -> dt.date | None:
    """Estimated end date of a guided period: the reported period end moved by the quarters between the reported
    and the guided (label, quarter) (both in the company's own label convention), month-end rounded; without a
    reported period, :func:`period_end_estimate` from the fiscal-year end."""
    if fy is None:
        return None
    rq = rep.fiscal_quarter()
    if rq is not None and rep.period_end is not None:
        n = (fy - rq[0]) * 4 + ((q or 4) - rq[1])
        pe = rep.period_end if rep.period_end.day > 7 else rep.period_end - dt.timedelta(days=8)
        months = pe.year * 12 + (pe.month - 1) + 3 * n
        return _month_end(months // 12, months % 12 + 1)
    return period_end_estimate(fy, q, rep.fye_month, rep.fye_day)


def _year(y: str | None, anchor: int) -> int | None:
    if not y:
        return None
    y = y.strip("'")
    v = int(y)
    if v < 100:
        v += 2000
    if not 1990 <= v <= anchor + 6:
        return None
    return v


@dataclass
class PeriodMention:
    start: int
    end: int
    kind: str  # FY, Q, REL_Q, FY0, H
    fy: int | None = None
    q: int | None = None


def find_periods(text: str, anchor_year: int) -> list[PeriodMention]:
    out = []
    for m in PERIOD_RE.finditer(text):
        g = m.groupdict()
        if g["q1"] or g["q2"] or g["q3"] or g["q6"] or g["q8"]:
            qw = (g["q1"] or g["q2"] or "").lower()
            q = _QWORD.get(qw) if qw else int(g["q3"] or g["q6"] or g["q8"])
            y = _year(g["y1"] or g["y2"] or g["y3"] or g["y6"] or g["y8"], anchor_year)
            out.append(PeriodMention(m.start(), m.end(), "Q", y, q))
        elif g["y4"] or g["y5"] or g["y7"]:
            y = _year(g["y4"] or g["y5"] or g["y7"], anchor_year)
            if y is not None:
                out.append(PeriodMention(m.start(), m.end(), "FY", y, None))
        elif g["fy0"]:
            out.append(PeriodMention(m.start(), m.end(), "FY0"))
        elif g["rq"]:
            rel = g["rq"].lower()
            out.append(PeriodMention(m.start(), m.end(), "REL_Q", None, 0 if rel == "current" else 1))
        elif g["hq"]:
            out.append(PeriodMention(m.start(), m.end(), "H"))
    return out


def resolve_period(p: PeriodMention, rep: ReportedPeriod, anchor_year: int
                   ) -> tuple[str, int | None, int | None, str] | None:
    """(period_type, fiscal_year, fiscal_quarter, period_basis) or None when unusable (half-years)."""
    rq = rep.fiscal_quarter()
    if p.kind == "H":
        return None
    if p.kind == "FY":
        return "FY", p.fy, None, "stated"
    if p.kind == "Q" and p.fy is not None:
        return "Q", p.fy, p.q, "stated"
    if rq is None:
        return None
    rfy, rqn = rq
    if p.kind == "FY0":
        return "FY", (rfy + 1 if rqn == 4 else rfy), None, "inferred"
    if p.kind == "Q":  # year-less quarter: the reported one itself, else its next occurrence
        q = p.q or 1
        return "Q", (rfy if q >= rqn else rfy + 1), q, "inferred"
    if p.kind == "REL_Q":
        q = rqn + (p.q or 0) + (1 if p.q == 0 else 0)  # "current quarter" in a release = the quarter after reported
        fy = rfy + (q - 1) // 4
        return "Q", fy, (q - 1) % 4 + 1, "inferred"
    return None


MAX_YEARS_AHEAD = 2
REPORTED_CUE = re.compile(r"\b(?:results?|report(?:s|ed)?|ended|announc\w*|earnings|delivers?|posts?)\b", re.IGNORECASE)


def is_forward(period_type: str, fy: int | None, q: int | None, rep: ReportedPeriod) -> bool:
    """True when the guided (fiscal year, quarter) is after the reported one, at most two fiscal years ahead.

    Compared in the company's label space when the release names its period; otherwise the end-year label is
    computed from the fiscal-year end, and for a January/February year end a label one lower is also accepted
    (retail convention: fiscal 2024 ends in early 2025)."""
    if fy is None:
        return False
    rq = rep.fiscal_quarter()
    if rq is None:
        return True
    rfy, rqn = rq
    slack = 1 if (rep.label is None and rep.fye_month <= 2) else 0
    if fy > rfy + MAX_YEARS_AHEAD or fy < rfy - slack:
        return False
    if period_type == "FY":
        return fy + slack > rfy or (fy + slack == rfy and rqn < 4)
    return (fy + slack, q or 0) > (rfy, rqn)


def detect_reported_label(lines: list[str], rep: ReportedPeriod, anchor_year: int) -> tuple[int, int] | None:
    """The company's own (fiscal year label, quarter) of the reported period, from the release's first lines:
    the first quarter mention with a year ("third quarter of fiscal 2025", "Q3 FY25"), or in a fourth-quarter
    release "full year / fiscal YYYY". None when absent or implausible."""
    computed = ReportedPeriod(rep.period_end, rep.fye_month, rep.fye_day).fiscal_quarter()
    head = " ".join(x for x in lines[:30] if REPORTED_CUE.search(x) and not HEADING_CUE.search(x)
                    and not re.search(r"\b(?:expect\w*|anticipat\w*)\b", x, re.IGNORECASE))[:4000]
    for p in find_periods(head, anchor_year):
        if p.kind == "Q" and p.fy is not None and p.q is not None:
            if computed is None or abs(p.fy - computed[0]) <= 1:
                return p.fy, p.q
    if computed is not None and computed[1] == 4:
        for p in find_periods(head, anchor_year):
            if p.kind == "FY" and p.fy is not None and abs(p.fy - computed[0]) <= 1:
                return p.fy, 4
    return None


# ---------------------------------------------------------------------------------------------------------
# Values
# ---------------------------------------------------------------------------------------------------------


@dataclass
class Value:
    start: int
    end: int
    low: float | None
    high: float | None
    point: float | None
    kind: str  # range / point / floor / ceiling
    scale_word: str | None
    raw: str
    prior: bool = False
    from_to_new: bool = False  # the second half of "from $A to $B" (a change): B is the new point
    from_to_prior: float | None = None
    qualifier: str | None = None


def _num(s: str | None) -> float | None:
    if s is None:
        return None
    try:
        return float(s.replace(",", ""))
    except ValueError:
        return None


def find_values(text: str) -> list[Value]:
    out = []
    for m in VALUE_RE.finditer(text):
        pre = m.group("pre1") or ""
        if pre and pre.upper() not in ("US", "U"):
            continue  # C$, A$, NT$, HK$ ...
        a = _num(m.group("n1"))
        b = _num(m.group("n2"))
        if a is None:
            continue
        neg_a = bool((m.group("nega1") or m.group("negb1")) and m.group("negc1"))
        neg_b = bool((m.group("nega2") or m.group("negb2")) and m.group("negc2")) if b is not None else False
        u1 = (m.group("u1") or "").lower() or None
        u2 = (m.group("u2") or "").lower() or None if b is not None else None
        if b is not None and u1 and u2 and SCALE.get(u1) != SCALE.get(u2):
            # "$900 million to $1.1 billion": scale each side
            a_v, b_v = a * SCALE[u1], b * SCALE[u2]
            unit = "absolute"
        else:
            a_v, b_v = a, b
            unit = (u2 or u1) if b is not None else u1
        if neg_a:
            a_v = -a_v
        if neg_b and b_v is not None:
            b_v = -b_v
        end = m.end()
        start = m.start()
        while start < end and text[start].isspace():
            start += 1
        raw = text[start:end]
        if b_v is not None:
            lo, hi = (a_v, b_v) if a_v <= b_v else (b_v, a_v)
            out.append(Value(start, end, lo, hi, (lo + hi) / 2, "range", unit, raw))
        else:
            out.append(Value(start, end, None, None, a_v, "point", unit, raw))
    # "$10.25 billion, plus or minus $500 million" / "$4.02 +/- $0.20": one range around the point
    merged: list[Value] = []
    for v in out:
        prev = merged[-1] if merged else None
        if (prev is not None and prev.kind == "point" and v.kind == "point" and prev.point is not None
                and v.point is not None and PLUS_MINUS.match(text[prev.end:v.start])):
            if prev.scale_word != v.scale_word and v.scale_word is not None:
                p = _apply_scale(prev.point, prev.scale_word) or 0.0
                d = _apply_scale(v.point, v.scale_word) or 0.0
                sw = "absolute"
            else:
                p, d, sw = prev.point, v.point, prev.scale_word or v.scale_word
            merged[-1] = Value(prev.start, v.end, p - d, p + d, p, "range", sw, text[prev.start:v.end])
            continue
        merged.append(v)
    return merged


def _apply_scale(v: float | None, word: str | None) -> float | None:
    if v is None:
        return None
    if word in (None, "absolute"):
        return v
    return v * SCALE.get(word, 1.0)


# ---------------------------------------------------------------------------------------------------------
# Sentence extraction
# ---------------------------------------------------------------------------------------------------------


def split_sentences(line: str) -> list[tuple[int, str]]:
    """(offset, sentence) pieces of one line; avoids splitting after common abbreviations."""
    out = []
    start = 0
    for m in SENT_SPLIT.finditer(line):
        piece = line[start:m.start() + 1]
        if ABBREV_END.search(piece.rstrip(".;").strip()):
            continue
        if piece.strip():
            out.append((start, piece))
        start = m.end()
    if line[start:].strip():
        out.append((start, line[start:]))
    return out


@dataclass
class Keyword:
    start: int
    end: int
    kind: str  # EPS / REVENUE / OTHER
    text: str


def find_keywords(s: str) -> list[Keyword]:
    kws: list[Keyword] = []
    for m in EPS_KW.finditer(s):
        kws.append(Keyword(m.start(), m.end(), "EPS", m.group(0)))
    for m in REV_KW.finditer(s):
        before = s[max(0, m.start() - 40):m.start()]
        after = s[m.end():m.end() + 30]
        mods = (m.group("mods") or "").strip()
        if (not mods and REV_BLOCK_BEFORE.search(before)) or REV_BLOCK_AFTER.search(after):
            kws.append(Keyword(m.start(), m.end(), "OTHER", m.group(0)))
            continue
        # a capitalised segment name mid-sentence right before ("expects Cloud revenue", "Corporate & Other Net
        # Sales") blocks it; sentence-initial words and headline verbs do not
        seg = re.search(r"\S\s+([A-Z][A-Za-z&'-]+)\s+$", before)
        if seg and seg.group(1).lower().rstrip("'s") not in SEG_OK:
            kws.append(Keyword(m.start(), m.end(), "OTHER", m.group(0)))
            continue
        kws.append(Keyword(m.start(), m.end(), "REVENUE", m.group(0)))
    taken = [(k.start, k.end) for k in kws]
    for m in OTHER_KW.finditer(s):
        if any(a <= m.start() < b or a < m.end() <= b for a, b in taken):
            continue
        kws.append(Keyword(m.start(), m.end(), "OTHER", m.group(0)))
    kws.sort(key=lambda k: k.start)
    return kws


@dataclass
class GuidanceRow:
    measure: str
    basis: str
    period_type: str
    fiscal_year: int | None
    fiscal_quarter: int | None
    period_basis: str
    low: float | None
    high: float | None
    point: float | None
    value_type: str
    qualifier: str | None
    prior_low: float | None
    prior_high: float | None
    text_direction: str | None
    unit: str
    evidence: str
    source_kind: str  # sentence / table
    extras: dict[str, Any] = field(default_factory=dict)


def _basis(context: str, kw: str) -> str:
    """ADJUSTED / GAAP / UNSPECIFIED: the keyword's own qualifier, else the basis cue nearest the value."""
    if ADJ_BASIS.search(kw):
        return "ADJUSTED"
    if GAAP_BASIS.search(kw):
        return "GAAP"
    cues = [(m.start(), "ADJUSTED") for m in ADJ_BASIS.finditer(context)]
    cues += [(m.start(), "GAAP") for m in GAAP_BASIS.finditer(context)]
    return max(cues)[1] if cues else "UNSPECIFIED"


def _direction(text: str) -> str | None:
    for name, rx in DIRECTION:
        if rx.search(text):
            return name
    return None


def _nearest_period(periods: list[PeriodMention], pos: int, lo: int, hi: int) -> PeriodMention | None:
    before = [p for p in periods if lo <= p.start < pos]
    if before:
        return before[-1]
    after = [p for p in periods if pos <= p.start < hi]
    return after[0] if after else None


def extract_from_sentence(s: str, rep: ReportedPeriod, anchor_year: int, heading_period: PeriodMention | None,
                          line_prefix_period: PeriodMention | None, in_outlook: bool = False) -> list[GuidanceRow]:
    """Guidance rows of one sentence; ``in_outlook`` (under an outlook / guidance heading) stands in for a cue."""
    cue_in_sentence = FORWARD_CUE.search(s) is not None
    if "$" not in s or not (in_outlook or cue_in_sentence) or COMPONENT_SENTENCE.search(s):
        return []
    outlook_only = not cue_in_sentence
    if outlook_only and (HISTORICAL.search(s) or OUTLOOK_ONLY_BLOCK.search(s)):
        return []  # a results line under (or near) an outlook heading
    values = find_values(s)
    if not values:
        return []
    kws = find_keywords(s)
    periods = find_periods(s, anchor_year)
    rows: list[GuidanceRow] = []
    # mark prior values: "compared to prior guidance of $A", "(previously $A)", "from $A to $B" changes
    for i, v in enumerate(values):
        before = s[max(0, v.start - 90):v.start]
        if RANGE_FROM.search(before):
            continue
        if FROM_TO.search(before) and v.kind == "point":
            nxt = values[i + 1] if i + 1 < len(values) else None
            gap = s[v.end:nxt.start] if nxt else ""
            if nxt is not None and re.fullmatch(r"\s*(?:to|,)\s*(?:a\s+range\s+of\s+|approximately\s+)?", gap,
                                                re.IGNORECASE):
                v.prior = True
                nxt.from_to_new = True
                nxt.from_to_prior = v.point
                continue
        if v.kind == "range" and FROM_TO.search(before):
            nxt = values[i + 1] if i + 1 < len(values) else None
            gap = s[v.end:nxt.start] if nxt else ""
            if nxt is not None and re.fullmatch(r"\s*,?\s*to\s+(?:a\s+range\s+of\s+|a\s+new\s+range\s+of\s+)?",
                                                gap, re.IGNORECASE):
                v.prior = True
                nxt.from_to_new = True
                continue
        if MAINTAIN_PRIOR.search(before):
            pass  # "maintaining its prior revenue guidance of $11.65 billion" is current guidance
        elif (PRIOR_BEFORE.search(before[-60:]) and i > 0) or PRIOR_GUIDANCE_BEFORE.search(before):
            v.prior = True
        elif BASE_BEFORE.search(before):
            v.kind = "base"
    # a "from $A to $B" point pair shows up as one range match when both are bare amounts: split it
    for v in values:
        before = s[max(0, v.start - 60):v.start]
        if v.prior:
            continue
        if v.kind == "range" and re.search(r"\bfrom\s+$", before, re.IGNORECASE) and not RANGE_FROM.search(before) \
                and _direction(s[max(0, v.start - 200):v.start]) in ("raised", "lowered", "updated", "narrowed"):
            v.kind = "change"
    # "... its 2020 and 2021 adjusted EPS outlooks to $5.45 to $5.75 and $5.80 to $6.10, respectively"
    resp: dict[int, PeriodMention] = {}
    live = [i for i, v in enumerate(values) if not v.prior and v.kind not in ("change", "base")]
    if live and re.search(r"\brespectively\b", s, re.IGNORECASE):
        lead_p = [p for p in periods if p.end <= values[live[0]].start and p.kind in ("FY", "Q") and p.fy is not None]
        if len(live) >= 2 and len(lead_p) == len(live):
            resp = dict(zip(live, lead_p))
    for i, v in enumerate(values):
        if v.prior or v.kind in ("change", "base"):
            continue
        clause_start = max(s.rfind(";", 0, v.start), 0)
        # measure: nearest keyword before the value in the clause (no other non-prior value in between)
        prev_vals = [w for w in values[:i] if not w.prior]
        lo = max(clause_start, prev_vals[-1].end if prev_vals else 0)
        cand = [k for k in kws if k.end <= v.start and k.start >= max(0, v.start - 160)]
        after = s[v.end:v.end + 60]
        per_share = PER_SHARE_AFTER.search(after) is not None
        kw: Keyword | None = cand[-1] if cand else None
        measure = None
        if per_share:
            if kw is not None and kw.kind == "OTHER" and NON_EPS_PER_SHARE.search(s[max(0, kw.start - 5):v.start]):
                continue
            if NON_EPS_PER_SHARE.search(s[max(0, v.start - 80):v.start]):
                continue
            if kw is not None and kw.kind == "OTHER" and not EPS_GENERIC.fullmatch(kw.text.strip()):
                continue  # "FX impact of $0.45 per share", "charges of $0.10 per share"
            measure = "EPS"
            if kw is None or kw.kind != "EPS":
                eps_kws = [k for k in cand if k.kind == "EPS"]
                kw = eps_kws[-1] if eps_kws else kw
        elif kw is not None and kw.kind in ("EPS", "REVENUE") and kw.start >= lo - 5:
            measure = kw.kind
        if measure is None:
            continue
        kw_start = kw.start if kw is not None else v.start
        between = s[kw.end:v.start] if kw is not None and kw.end <= v.start else ""
        if measure == "REVENUE" and (CHANGE_BETWEEN.search(between) or len(between) > 110):
            continue
        if measure == "REVENUE" and (OF_OTHER_AFTER.match(s[v.end:v.end + 40]) or (
                CHANGE_BEFORE_KW.search(s[max(0, kw_start - 50):kw_start]) and not KW_VALUE_LINK.search(between))):
            continue
        if measure == "EPS" and not per_share and (CHANGE_BETWEEN.search(between) or len(between) > 110):
            continue
        if measure == "EPS" and per_share and kw is not None and kw.kind == "EPS" and CHANGE_BETWEEN.search(between):
            continue
        # scale and plausibility
        if measure == "EPS":
            if v.scale_word not in (None,):
                continue
            vals = [x for x in (v.low, v.high, v.point) if x is not None]
            if any(abs(x) >= 1000 for x in vals):
                continue
            loss = re.search(r"\b(?:net\s+)?loss\s+per\b|\bloss\s+of\s*$|\(loss\)\s+per", s[max(0, kw_start - 5):v.end],
                             re.IGNORECASE)
            low, high, point = v.low, v.high, v.point
            if loss and all(x >= 0 for x in vals) and not re.search(r"\bincome\s+per\b|\bearnings\s+per\b",
                                                                   s[max(0, kw_start - 5):v.start], re.IGNORECASE):
                low, high, point = (-v.high if v.high is not None else None, -v.low if v.low is not None else None,
                                    -v.point if v.point is not None else None)
            unit = "USD_PER_SHARE"
        else:
            if v.scale_word is None:
                vals = [x for x in (v.low, v.high, v.point) if x is not None]
                if not vals or min(abs(x) for x in vals) < 100_000:
                    continue
            low, high, point = (_apply_scale(v.low, v.scale_word), _apply_scale(v.high, v.scale_word),
                                _apply_scale(v.point, v.scale_word))
            if max(abs(x) for x in (low, high, point) if x is not None) < 1e6:
                continue  # below $1 million: a segment remainder or a unit error, not company revenue
            unit = "USD"
        # forward cue in the sentence up to the value (or right after it), and no historical marker in the clause
        head = s[:v.start]
        if not in_outlook and not FORWARD_CUE.search(s[:v.end]) and not FORWARD_CUE.search(s[v.end:v.end + 80]):
            continue
        hist_zone = s[max(clause_start, kw_start - 80):v.start]
        if HIST_AFTER.search(s[v.end:v.end + 90]):
            continue
        if HISTORICAL.search(hist_zone) and not re.search(
                r"\b(?:expect\w*|anticipat\w*|now\s+sees|forecast\w*|project\w*|reaffirm\w*|rais\w*|updat\w*)\b",
                hist_zone, re.IGNORECASE):
            continue
        if VALUE_IS_CHANGE.match(s[v.end:v.end + 30]):
            continue  # "a $5 million increase in R&D"
        # period: one right after the value ("$3.45 to $3.70 for 2025", "... in the second quarter") wins, then the
        # nearest before it (an explicit year over a bare "full year"), then after it
        after_p = [p for p in periods if v.end <= p.start <= v.end + 30
                   and re.fullmatch(r"[\s,]*(?:per\s+(?:diluted\s+)?share\s*)?(?:(?:for|in|during)\s+(?:the\s+)?"
                                    r"(?:full\s+year\s+|fiscal\s+(?:year\s+)?)?)?", s[v.end:p.start], re.IGNORECASE)
                   and (re.match(r"(?:for|in|during)\b", s[p.start:p.end], re.IGNORECASE)
                        or re.search(r"\b(?:for|in|during)\s+(?:the\s+)?(?:full\s+year\s+|fiscal\s+(?:year\s+)?)?$",
                                     s[v.end:p.start], re.IGNORECASE))]
        per = after_p[0] if after_p else _nearest_period(periods, v.start, 0, v.end + 60)
        lead = [p for p in periods if p.start < 40 and p.start < v.start
                and (re.fullmatch(r"\W*(?:for|in|during)\s+(?:the\s+)?(?:full\s+year\s+|fiscal\s+(?:year\s+)?)?",
                                  s[:p.start], re.IGNORECASE)
                     or (re.fullmatch(r"\W*", s[:p.start]) and re.match(r"(?:for|in|during)\b", s[p.start:p.end],
                                                                         re.IGNORECASE)))
                and re.match(r"\s*(?:,|and\b)", s[p.end:p.end + 5])]
        if lead and not after_p and lead[0].kind in ("FY", "Q") and lead[0].fy is not None:
            per = lead[0]  # "For 2025, ... translates to $3.45 to $3.70": the sentence's lead period governs
        if per is not None and per.kind == "FY0" and not after_p:
            explicit = [p for p in periods if p.start < v.start and p.kind in ("FY", "Q") and p.fy is not None]
            if explicit:
                per = explicit[-1]
        if i in resp:
            per = resp[i]
        pr = resolve_period(per, rep, anchor_year) if per is not None else None
        if pr is not None and outlook_only and pr[3] == "inferred":
            continue  # "for the year" without a cue of its own: the reported year as often as the next
        if pr is None and per is None:
            pm = line_prefix_period or heading_period
            pr = resolve_period(pm, rep, anchor_year) if pm is not None else None
            if pr is not None:
                pr = (pr[0], pr[1], pr[2], "heading" if pm is heading_period else pr[3])
        if pr is None:
            continue
        ptype, fy, fq, pbasis = pr
        if not is_forward(ptype, fy, fq, rep):
            continue
        # value type qualifiers
        vt = v.kind
        pct = PLUS_MINUS_PCT.match(s[v.end:v.end + 40])
        if vt == "point" and pct and point is not None:
            d = abs(point) * float(pct.group(1)) / 100.0
            vt, low, high = "range", point - d, point + d  # "$337 million, plus or minus 3 percent"
        pre = s[max(0, v.start - 30):v.start]
        if vt == "point" and FLOOR_BEFORE.search(pre):
            vt, low, high, point = "floor", point, None, None
        elif vt == "point" and CEIL_BEFORE.search(pre):
            vt, low, high, point = "ceiling", None, point, None
        qual = None
        qm = QUALIFIER_BEFORE.search(s[max(clause_start, v.start - 70):v.start])
        if qm:
            qual = "high_end" if qm.group("q") and qm.group("q").lower() in ("high", "top", "upper") else (
                "low_end" if qm.group("q") else "midpoint")
        prior_low = prior_high = None
        if v.from_to_new and v.from_to_prior is not None:
            prior_low = prior_high = v.from_to_prior
        else:
            nxt = values[i + 1] if i + 1 < len(values) else None
            if nxt is not None and nxt.prior and nxt.start - v.end < 90:
                scale = nxt.scale_word or v.scale_word
                if measure == "EPS":
                    prior_low, prior_high = (nxt.low, nxt.high) if nxt.kind == "range" else (nxt.point, nxt.point)
                else:
                    prior_low, prior_high = ((_apply_scale(nxt.low, scale), _apply_scale(nxt.high, scale))
                                             if nxt.kind == "range" else
                                             (_apply_scale(nxt.point, scale), _apply_scale(nxt.point, scale)))
            prev = values[i - 1] if i > 0 else None
            if prior_low is None and prev is not None and prev.prior and v.from_to_new:
                scale = prev.scale_word or v.scale_word
                if measure == "EPS":
                    prior_low, prior_high = (prev.low, prev.high) if prev.kind == "range" else (prev.point, prev.point)
                else:
                    prior_low, prior_high = ((_apply_scale(prev.low, scale), _apply_scale(prev.high, scale))
                                             if prev.kind == "range" else
                                             (_apply_scale(prev.point, scale), _apply_scale(prev.point, scale)))
        kw_text = kw.text if kw is not None else ""
        rows.append(GuidanceRow(
            measure=measure, basis=_basis(s[max(clause_start, kw_start - 60):v.start], kw_text),
            period_type=ptype, fiscal_year=fy, fiscal_quarter=fq, period_basis=pbasis,
            low=low, high=high, point=point, value_type=vt, qualifier=qual, prior_low=prior_low,
            prior_high=prior_high, text_direction=_direction(head[max(0, kw_start - 110):]),
            unit=unit, evidence=s.strip()[:700], source_kind="sentence",
        ))
    return rows


# ---------------------------------------------------------------------------------------------------------
# Tables
# ---------------------------------------------------------------------------------------------------------

TABLE_MEASURE = re.compile(
    r"^(?P<basis>(?:gaap|non-gaap|adjusted|core|reported)\s+)?(?:(?:total|net|consolidated)\s+)?"
    r"(?P<m>revenues?|net\s+sales|sales|(?:(?:gaap|non-gaap|adjusted|core|diluted)\s+)*(?:diluted\s+)?"
    r"(?:eps|earnings\s+per\s+(?:diluted\s+)?share|net\s+income\s+per\s+(?:diluted\s+)?share|"
    r"(?:net\s+)?(?:income|earnings)\s*\(loss\)\s+per\s+(?:diluted\s+)?share|"
    r"net\s+(?:loss|income)\s+per\s+(?:diluted\s+)?share))"
    r"(?P<tail>\s*(?:\((?:gaap|non-gaap|adjusted|diluted|\d|[a-z])\))*\s*(?:\d|[a-z]|\*)?)$",
    re.IGNORECASE,
)
TABLE_CUE = re.compile(r"\b(?:expect\w*|estimat\w*|project\w*|forecast\w*|targets?|range|low|high|midpoint)\b",
                       re.IGNORECASE)
PRIOR_COL = re.compile(r"\b(?:prior|previous|previously|original|actual|actuals|last\s+year|prior\s+year|results?|"
                       r"reported|a\s+year\s+ago)\b", re.IGNORECASE)
_JOIN_TOKEN = re.compile(r"(?:-|to|\)|%|million|billion|thousand|\+/-|±|plus\s+or\s+minus)", re.IGNORECASE)


def _cells(line: str) -> list[str]:
    return [c.strip() for c in line.split("|")]


def _merge_row_cells(cells: list[str]) -> list[str]:
    """Collapse split money cells ('$', '1.20', '-', '$', '1.25', 'million', '+/-') into value strings per column."""
    toks = [c for c in cells if c != ""]
    out: list[str] = []
    for t in toks:
        if out and (out[-1].endswith(("$", "-", "to", "(", "+/-", "±")) or _JOIN_TOKEN.fullmatch(t)
                    or (out[-1] in ("$", "($") and re.match(r"[\d.(]", t))):
            out[-1] = out[-1] + (" " if not out[-1].endswith(("$", "(")) else "") + t
        else:
            out.append(t)
    return out


def _table_rows(lines: list[str], idx: int, rep: ReportedPeriod, anchor_year: int, scale_word: str | None
                ) -> list[GuidanceRow]:
    """Guidance rows from the table starting at ``lines[idx]`` (a header line with period labels).

    A column labelled prior / previous / original / actual is a prior column; when two columns carry the same
    period, the rightmost non-prior one is current and the others are its prior values."""
    header = _merge_row_cells(_cells(lines[idx]))
    for near in lines[max(0, idx - 3):idx + 4]:
        tu = TABLE_UNIT.search(near)
        if tu:
            scale_word = (tu.group("u") or tu.group("u2") or tu.group("u3")).lower()
            break
    periods: list[tuple[str, int | None, int | None, str] | None] = []
    prior_of: dict[int, int] = {}  # prior column -> current column
    cue_near = any(HEADING_CUE.search(x) or TABLE_CUE.search(x) for x in lines[max(0, idx - 3):idx + 1])
    for h in header:
        ps = find_periods(h, anchor_year)
        pr = resolve_period(ps[0], rep, anchor_year) if ps else None
        if pr is not None and pr[3] == "inferred" and not cue_near:
            pr = None  # a year-less "Q2" column of a results table is not a guidance column
        periods.append(pr)
    if not any(periods):
        return []
    is_prior = [bool(p is not None and PRIOR_COL.search(h)) for h, p in zip(header, periods)]
    for i, p in enumerate(periods):
        if p is None:
            continue
        same = [j for j, q in enumerate(periods) if q is not None and q[:3] == p[:3]]
        current = [j for j in same if not is_prior[j]]
        cur = current[-1] if current else None
        if cur is not None and i != cur:
            prior_of[i] = cur
    if all(p is None or i in prior_of for i, p in enumerate(periods)):
        return []
    out: list[GuidanceRow] = []
    for j in range(idx + 1, min(len(lines), idx + 25)):
        line = lines[j]
        if "|" not in line:
            break  # the table ended
        cells = _merge_row_cells(_cells(line))
        if not cells:
            continue
        if _cells(line)[0] == "" and find_periods(line, anchor_year):
            break  # a new header row: another table
        label = re.sub(r"[(\[]?\d[)\]]?$", "", cells[0]).strip(" :*")
        m = TABLE_MEASURE.match(label)
        if not m:
            continue
        mtxt = m.group("m").lower()
        measure = "REVENUE" if re.match(r"revenues?|net\s+sales|sales", mtxt) else "EPS"
        vals_cells = cells[1:]
        # align from the right: header labels usually cover the value columns
        if len(vals_cells) > len(periods):
            continue
        offset = len(periods) - len(vals_cells)
        parsed: dict[int, tuple[float | None, float | None, float | None, str]] = {}
        for k, cell in enumerate(vals_cells):
            col = offset + k
            if periods[col] is None or "%" in cell or re.search(r"[A-Za-z]{4,}", cell.replace("million", "")
                                                                  .replace("billion", "").replace("plus or minus", "")):
                continue
            vs = find_values(cell if "$" in cell else "$" + cell.strip())
            if len(vs) != 1:
                continue
            v = vs[0]
            if measure == "EPS":
                vals = [x for x in (v.low, v.high, v.point) if x is not None]
                if (v.scale_word and v.scale_word != "absolute") or any(abs(x) >= 1000 for x in vals):
                    continue
                parsed[col] = (v.low, v.high, v.point, v.kind)
            else:
                sw = v.scale_word or scale_word
                if sw is None:
                    vals = [x for x in (v.low, v.high, v.point) if x is not None]
                    if not vals or min(abs(x) for x in vals) < 100_000:
                        continue
                parsed[col] = (_apply_scale(v.low, sw), _apply_scale(v.high, sw), _apply_scale(v.point, sw), v.kind)
        for col, (low, high, point, kind) in parsed.items():
            if col in prior_of:
                continue
            pr = periods[col]
            assert pr is not None
            ptype, fy, fq, _pb = pr
            if not is_forward(ptype, fy, fq, rep):
                continue
            prior = next((parsed[p] for p, c in prior_of.items() if c == col and p in parsed), None)
            out.append(GuidanceRow(
                measure=measure, basis=_basis("", " ".join((label, header[col], header[0] if col else ""))),
                period_type=ptype, fiscal_year=fy,
                fiscal_quarter=fq, period_basis="table", low=low, high=high, point=point, value_type=kind,
                qualifier=None, prior_low=(prior[0] if prior and prior[3] == "range" else prior[2] if prior else None),
                prior_high=(prior[1] if prior and prior[3] == "range" else prior[2] if prior else None),
                text_direction=None, unit="USD_PER_SHARE" if measure == "EPS" else "USD",
                evidence=(lines[idx][:250] + " || " + line[:350]), source_kind="table",
            ))
    return out


# ---------------------------------------------------------------------------------------------------------
# Document
# ---------------------------------------------------------------------------------------------------------


def extract_guidance(text: str, rep: ReportedPeriod, anchor_year: int) -> list[GuidanceRow]:
    """All guidance rows of one release text (see the module doc); deduplicated."""
    return extract_release(text, rep, anchor_year)[0]


def extract_release(text: str, rep: ReportedPeriod, anchor_year: int) -> tuple[list[GuidanceRow], ReportedPeriod]:
    """(guidance rows, the reported period with the release's own label when it names one)."""
    lines = text.split("\n")
    if rep.label is None:
        label = detect_reported_label(lines, rep, anchor_year)
        if label is not None:
            rep = ReportedPeriod(rep.period_end, rep.fye_month, rep.fye_day, label)
    rows: list[GuidanceRow] = []
    heading_period: PeriodMention | None = None
    heading_line = -100
    outlook_until = -1
    table_scale: str | None = None
    for i, line in enumerate(lines):
        if SAFE_HARBOR.match(line.strip()):
            break
        stripped = line.strip(" |")
        tu = TABLE_UNIT.search(line)
        if tu:
            table_scale = (tu.group("u") or tu.group("u2") or tu.group("u3")).lower()
        short = len(stripped) <= 110 and "$" not in stripped and not stripped.endswith((".", ";", ","))
        if short and HEADING_CUE.search(stripped) and not HEADING_NOT_OUTLOOK.search(stripped):
            ps = find_periods(stripped, anchor_year)
            heading_period = ps[-1] if ps else PeriodMention(0, 0, "FY0") if re.search(
                r"\b(?:full[- ]year|annual|fiscal\s+year)\b", stripped, re.IGNORECASE) else None
            heading_line = i
            outlook_until = i + 40
            if "|" not in line:
                continue
        elif short and "|" not in line and len(stripped) <= 80 and not stripped.endswith(":") and (
                stripped.isupper() or re.fullmatch(r"(?:[A-Z0-9][\w&'/-]*\s*){1,8}", stripped)):
            heading_period = None  # a new, non-outlook section heading ends the outlook scope
            heading_line = -100
        if i - heading_line > 30:
            heading_period = None
        if "|" in line:
            if i <= outlook_until and find_periods(line, anchor_year) and not TABLE_MEASURE.match(
                    _cells(line)[0].strip(" :*") if _cells(line) else ""):
                rows.extend(_table_rows(lines, i, rep, anchor_year, table_scale))
            # a table row may also be a one-cell sentence (bulleted outlook in a table)
            cells = [c for c in _cells(line) if c]
            if len(cells) == 1 or (len(cells) == 2 and len(cells[0]) <= 3):
                line = cells[-1]
            else:
                continue
        prefix = re.match(r"^(?P<p>[^:]{3,60}):\s", line)
        prefix_period = None
        if prefix:
            ps = find_periods(prefix.group("p"), anchor_year)
            prefix_period = ps[-1] if ps else None
        in_outlook = heading_line >= 0 and i - heading_line <= 30
        for _off, sent in split_sentences(line):
            rows.extend(extract_from_sentence(sent, rep, anchor_year, heading_period, prefix_period, in_outlook))
    seen: dict[tuple[Any, ...], GuidanceRow] = {}
    out = []
    for r in rows:
        key = (r.measure, r.basis, r.period_type, r.fiscal_year, r.fiscal_quarter,
               None if r.low is None else round(r.low, 4), None if r.high is None else round(r.high, 4),
               None if r.point is None else round(r.point, 4))
        first = seen.get(key)
        if first is not None:  # the same guidance repeated (headline and body): keep the first, fill its gaps
            if first.prior_low is None and first.prior_high is None and (r.prior_low is not None
                                                                         or r.prior_high is not None):
                first.prior_low, first.prior_high = r.prior_low, r.prior_high
            if first.text_direction is None and r.text_direction is not None:
                first.text_direction = r.text_direction
            continue
        seen[key] = r
        out.append(r)
    specific = {k[:1] + k[2:] for k in seen if k[1] != "UNSPECIFIED"}
    out = [r for r in out if not (r.basis == "UNSPECIFIED" and _key_nobasis(r) in specific)]
    return out, rep


def _key_nobasis(r: GuidanceRow) -> tuple[Any, ...]:
    return (r.measure, r.period_type, r.fiscal_year, r.fiscal_quarter,
            None if r.low is None else round(r.low, 4), None if r.high is None else round(r.high, 4),
            None if r.point is None else round(r.point, 4))


# ---------------------------------------------------------------------------------------------------------
# Revisions (pure)
# ---------------------------------------------------------------------------------------------------------

REVISION_TOLERANCE = 0.005


def _level(low: float | None, high: float | None, point: float | None) -> float | None:
    if point is not None:
        return point
    return low if low is not None else high


def revise(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Set ``revision`` / ``revision_basis`` / ``prev_*`` on one CIK's rows (sorted by available_at, accession).

    The reference is the latest earlier row of the same (measure, basis, period_type, fiscal_year, fiscal_quarter)
    from another accession (``previous_release``), else the prior range stated in the text (``text_prior``).
    Midpoints differing by more than 0.5% are raised / lowered, else maintained; no reference is ``new``."""
    last: dict[tuple[Any, ...], dict[str, Any]] = {}
    for r in sorted(rows, key=lambda x: (x["available_at"] or dt.datetime.max, x["accession"])):
        key = (r["measure"], r["basis"], r["period_type"], r["fiscal_year"], r["fiscal_quarter"])
        cur = _level(r["low"], r["high"], r["point"])
        prev = last.get(key)
        ref, basis = None, "none"
        if prev is not None and prev["accession"] != r["accession"]:
            ref, basis = _level(prev["low"], prev["high"], prev["point"]), "previous_release"
            r["prev_accession"], r["prev_point"] = prev["accession"], ref
        elif r.get("prior_low") is not None or r.get("prior_high") is not None:
            pl, ph = r.get("prior_low"), r.get("prior_high")
            ref = (pl + ph) / 2 if pl is not None and ph is not None else (pl if pl is not None else ph)
            basis = "text_prior"
            r["prev_accession"], r["prev_point"] = None, ref
        else:
            r["prev_accession"], r["prev_point"] = None, None
        if ref is None or cur is None:
            r["revision"] = "new"
        else:
            scale = max(abs(ref), 1e-9)
            d = (cur - ref) / scale
            r["revision"] = "raised" if d > REVISION_TOLERANCE else "lowered" if d < -REVISION_TOLERANCE else "maintained"
        r["revision_basis"] = basis
        last[key] = r
    return rows


# ---------------------------------------------------------------------------------------------------------
# Stage build
# ---------------------------------------------------------------------------------------------------------

OUT_COLUMNS = [
    ("cik", "BIGINT"), ("accession", "VARCHAR"), ("form", "VARCHAR"), ("items", "VARCHAR"),
    ("source_doc", "VARCHAR"), ("doc_source", "VARCHAR"), ("doc_sha256", "VARCHAR"),
    ("filing_date", "DATE"), ("event_date", "DATE"), ("available_at", "TIMESTAMP"), ("acceptance_clock", "VARCHAR"),
    ("vintage_risk", "VARCHAR"), ("release_period_end", "DATE"), ("reported_fy", "INTEGER"),
    ("reported_fq", "INTEGER"), ("reported_label_basis", "VARCHAR"), ("measure", "VARCHAR"), ("basis", "VARCHAR"),
    ("period_type", "VARCHAR"), ("fiscal_year", "INTEGER"), ("fiscal_quarter", "INTEGER"), ("period_basis", "VARCHAR"),
    ("period_end", "DATE"), ("value_type", "VARCHAR"), ("low", "DOUBLE"), ("high", "DOUBLE"), ("point", "DOUBLE"),
    ("mid", "DOUBLE"), ("qualifier", "VARCHAR"),
    ("unit", "VARCHAR"), ("prior_low", "DOUBLE"), ("prior_high", "DOUBLE"), ("text_direction", "VARCHAR"),
    ("revision", "VARCHAR"), ("revision_basis", "VARCHAR"), ("prev_accession", "VARCHAR"), ("prev_point", "DOUBLE"),
    ("source_kind", "VARCHAR"), ("evidence", "VARCHAR"), ("extractor_version", "VARCHAR"),
]


def _arrow_schema() -> Any:
    import pyarrow as pa

    m = {"BIGINT": pa.int64(), "VARCHAR": pa.string(), "DATE": pa.date32(), "TIMESTAMP": pa.timestamp("us"),
         "INTEGER": pa.int32(), "DOUBLE": pa.float64()}
    return pa.schema([(n, m[t]) for n, t in OUT_COLUMNS])


def build_doc_list(con: Any, dest: Path, start: dt.date) -> dict[str, Any]:
    """Every release document to parse: landed v2 EX-99 objects of 2.02 8-Ks, plus fetched EX-99 hits of 2.02 /
    7.01 8-Ks; with the filing clock, the reported fiscal period end and the issuer's fiscal-year end."""
    from . import events_common as EC
    from . import events_sources as ES

    cat = ES.catalog_path().as_posix()
    f = EC.sec_stage_path("filings.parquet").as_posix()
    prof = EC.sec_stage_path("issuer_profile.parquet").as_posix()
    forms = "'10-Q', '10-K', '10-QT', '10-KT', '20-F', '40-F'"
    # landed v2 exhibits are 2.02 releases; fetched ones are FTS guidance hits on EX-99 of 2.02 / 7.01 8-Ks
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _docs1 AS
        SELECT cik, accession, form, items, doc_source, source_doc, doc_sha256, url, filing_date, event_date,
               available_at, acceptance_clock, vintage_risk
        FROM read_parquet('{cat}')
        WHERE form IN ('8-K', '8-K/A') AND filing_date >= DATE '{start}'
          AND (doc_source = 'landed_v2' AND items LIKE '%2.02%'
               OR doc_source = 'sec_events' AND file_type LIKE 'EX-99%' AND list_contains(qids, 'guidance')
                  AND (items LIKE '%2.02%' OR items LIKE '%7.01%'))
        QUALIFY row_number() OVER (PARTITION BY accession, doc_sha256 ORDER BY doc_source) = 1""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _per AS
        SELECT cik, report_date FROM read_parquet('{f}') WHERE form IN ({forms}) AND report_date IS NOT NULL
        GROUP BY 1, 2""")
    n = C.copy_to_parquet(con, f"""
        SELECT d.*, p.report_date AS release_period_end, pr.fiscal_year_end
        FROM _docs1 d
        ASOF LEFT JOIN _per p ON d.cik = p.cik AND d.filing_date >= p.report_date
        LEFT JOIN read_parquet('{prof}') pr ON pr.cik = d.cik
        ORDER BY d.cik, d.available_at, d.accession""", dest)
    by_src = dict(con.execute(f"SELECT doc_source, count(*) FROM read_parquet('{dest.as_posix()}') GROUP BY 1").fetchall())
    return {"documents": n, "by_source": by_src}


def parse_documents(doc_list: Path, dest: Path, limit: int | None = None) -> dict[str, Any]:
    """Pure-Python pass (no DuckDB): text + :func:`extract_release` per document, revisions per CIK."""
    import pyarrow as pa
    import pyarrow.parquet as pq

    from . import events_sources as ES

    store = ES.open_store()
    schema = _arrow_schema()
    tmp = dest.with_name(dest.name + ".partial")
    writer = pq.ParquetWriter(tmp, schema, compression="zstd")
    stats = {"documents": 0, "docs_with_rows": 0, "rows": 0, "unreadable": 0, "label_from_text": 0}
    cur_cik = None
    buf: list[dict[str, Any]] = []
    pending: list[dict[str, Any]] = []

    acc_seen: set[tuple[Any, ...]] = set()

    def flush_cik() -> None:
        if buf:
            pending.extend(revise(buf))
            buf.clear()
        acc_seen.clear()
        if len(pending) >= 20_000:
            writer.write_table(pa.Table.from_pylist(pending, schema=schema))
            pending.clear()

    pf = pq.ParquetFile(doc_list)
    for batch in pf.iter_batches(batch_size=2000):
        for d in batch.to_pylist():
            if limit is not None and stats["documents"] >= limit:
                break
            if d["cik"] != cur_cik:
                flush_cik()
                cur_cik = d["cik"]
            stats["documents"] += 1
            blob = ES.read_landed(d["doc_sha256"]) if d["doc_source"] == "landed_v2" else ES.read_record(store, d["url"])
            if not blob:
                stats["unreadable"] += 1
                continue
            fye = d.get("fiscal_year_end") or ""
            fm, fdy = (int(fye[:2]), int(fye[2:])) if len(fye) == 4 and fye.isdigit() else (12, 31)
            rep = ReportedPeriod(d["release_period_end"], fm, fdy)
            anchor = (d["filing_date"] or dt.date.today()).year
            try:
                rows, rep_used = extract_release(ES.document_text(blob, d["source_doc"] or ""), rep, anchor)
            except Exception as exc:  # noqa: BLE001 - one bad document must not stop the stage; counted
                stats.setdefault("errors", []).append([d["accession"], type(exc).__name__, str(exc)[:120]])
                continue
            if rep_used.label is not None:
                stats["label_from_text"] += 1
            rq = rep_used.fiscal_quarter()
            if rows:
                stats["docs_with_rows"] += 1
            for r in rows:
                dkey = (d["accession"], r.measure, r.basis, r.period_type, r.fiscal_year, r.fiscal_quarter,
                        r.low, r.high, r.point)
                if dkey in acc_seen:  # the same guidance in a second exhibit of the same filing
                    continue
                acc_seen.add(dkey)
                stats["rows"] += 1
                buf.append({
                    "cik": d["cik"], "accession": d["accession"], "form": d["form"], "items": d["items"],
                    "source_doc": d["source_doc"], "doc_source": d["doc_source"], "doc_sha256": d["doc_sha256"],
                    "filing_date": d["filing_date"], "event_date": d["event_date"], "available_at": d["available_at"],
                    "acceptance_clock": d["acceptance_clock"], "vintage_risk": d["vintage_risk"],
                    "release_period_end": d["release_period_end"], "reported_fy": rq[0] if rq else None,
                    "reported_fq": rq[1] if rq else None,
                    "reported_label_basis": "release_text" if rep_used.label is not None else "fiscal_year_end",
                    "measure": r.measure, "basis": r.basis, "period_type": r.period_type,
                    "fiscal_year": r.fiscal_year, "fiscal_quarter": r.fiscal_quarter, "period_basis": r.period_basis,
                    "period_end": guided_period_end(r.period_type, r.fiscal_year, r.fiscal_quarter, rep_used),
                    "value_type": r.value_type, "low": r.low, "high": r.high, "point": r.point,
                    "mid": r.point if r.point is not None else (r.low if r.value_type == "floor" else r.high),
                    "qualifier": r.qualifier, "unit": r.unit, "prior_low": r.prior_low, "prior_high": r.prior_high,
                    "text_direction": r.text_direction, "source_kind": r.source_kind, "evidence": r.evidence,
                    "extractor_version": EXTRACTOR_VERSION,
                })
        if limit is not None and stats["documents"] >= limit:
            break
    flush_cik()
    if pending:
        writer.write_table(pa.Table.from_pylist(pending, schema=schema))
    writer.close()
    os.replace(tmp, dest)
    if "errors" in stats:
        stats["n_errors"] = len(stats["errors"])
        stats["errors"] = stats["errors"][:20]
    return stats


def _tmp_dir() -> Path:
    tmp = C.build_root() / "_tmp" / "events"
    tmp.mkdir(parents=True, exist_ok=True)
    return tmp


def phase_doclist(start: dt.date) -> dict[str, Any]:
    receipt: dict[str, Any] = {}
    con = C.connect(memory="300MB", threads=2)
    with C.timed(receipt, "doc_list"):
        receipt["doc_list"] = build_doc_list(con, _tmp_dir() / "guidance_docs.parquet", start)
    con.close()
    C.write_json_atomic(_tmp_dir() / "guidance_doclist_receipt.json", receipt)
    return receipt


def phase_parse(limit: int | None = None) -> dict[str, Any]:
    """No DuckDB: runs unguarded under ruling C-1 (peak recorded)."""
    receipt: dict[str, Any] = {}
    with C.timed(receipt, "parse"):
        receipt["parse"] = parse_documents(_tmp_dir() / "guidance_docs.parquet", _tmp_dir() / "guidance_rows.parquet",
                                           limit)
    receipt["peak_memory"] = peak_memory()
    C.write_json_atomic(_tmp_dir() / "guidance_parse_receipt.json", receipt)
    return receipt


def peak_memory() -> dict[str, Any]:
    from .events_sources import peak_memory as _pm

    return _pm()


def build(start: dt.date = dt.date(2019, 1, 1), limit: int | None = None, phase: str = "all") -> dict[str, Any]:
    from . import events_common as EC

    tmp = _tmp_dir()
    receipt: dict[str, Any] = {}
    if phase in ("all", "doclist"):
        receipt.update(phase_doclist(start))
    if phase in ("all", "parse"):
        receipt.update(phase_parse(limit))
    if phase not in ("all", "publish"):
        return receipt
    for name in ("guidance_doclist_receipt.json", "guidance_parse_receipt.json"):
        if (tmp / name).exists() and phase == "publish":
            receipt.update(C.read_json(tmp / name))
    con = C.connect(memory="300MB", threads=2)
    con.execute(f"CREATE TABLE g AS SELECT * FROM read_parquet('{(tmp / 'guidance_rows.parquet').as_posix()}')")
    EC.attach_security(con, "g", "g2", date_col="filing_date")
    out = C.stage_dir(EC.STAGE) / f"{TABLE}.parquet"
    cols = ", ".join(n for n, _ in OUT_COLUMNS[:1]) + ", security_id, link_tier, link_basis, is_member_issuer, " + \
        ", ".join(n for n, _ in OUT_COLUMNS[1:])
    receipt["rows"] = C.copy_to_parquet(con, f"SELECT {cols} FROM g2 ORDER BY cik, available_at, accession, measure",
                                        out, row_group_size=32768)
    o = out.as_posix()
    receipt["per_year"] = {str(y): {"rows": int(n), "releases": int(a), "ciks": int(c), "member_ciks": int(mc),
                                    "eps": int(e), "revenue": int(rv)}
                           for y, n, a, c, mc, e, rv in con.execute(f"""
        SELECT year(filing_date), count(*), count(DISTINCT accession), count(DISTINCT cik),
               count(DISTINCT cik) FILTER (WHERE is_member_issuer), count(*) FILTER (WHERE measure = 'EPS'),
               count(*) FILTER (WHERE measure = 'REVENUE')
        FROM read_parquet('{o}') GROUP BY 1 ORDER BY 1""").fetchall()}
    receipt["by_revision"] = dict(con.execute(f"SELECT revision, count(*) FROM read_parquet('{o}') GROUP BY 1").fetchall())
    receipt["by_link_tier"] = dict(con.execute(f"SELECT link_tier, count(*) FROM read_parquet('{o}') GROUP BY 1").fetchall())
    con.close()
    EC.publish_table(TABLE, ("guidance", "events_common", "events_sources", "common"), {
        "schema": SCHEMA, "rule": __doc__, "extractor_version": EXTRACTOR_VERSION, **receipt,
        "sources": {"landed_v2": str(C.PACKAGE_ROOT / "data" / "raw" / "sec-earnings-release"),
                    "sec_events": str(C.PACKAGE_ROOT / "data" / "raw" / "sec_events")}})
    return receipt


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--start", type=dt.date.fromisoformat, default=dt.date(2019, 1, 1))
    ap.add_argument("--limit", type=int, default=None, help="parse at most N documents (smoke runs)")
    ap.add_argument("--phase", choices=("all", "doclist", "parse", "publish"), default="all",
                    help="doclist and publish open DuckDB (guarded); parse is pure Python (ruling C-1)")
    args = ap.parse_args(argv)
    print(json.dumps(build(args.start, args.limit, args.phase), default=str, indent=1)[:6000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
