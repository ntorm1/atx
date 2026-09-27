"""Monthly evaluation harness (task R3b): honest evidence for cross-sectional predictors.

For every feature x variant x basis x horizon (1/3/6/12 months) cell this module
measures, on month-end formations:

* rank IC per formation (Spearman) and its mean;
* equal- and value-weighted decile and quintile long-short spreads, with monotonicity;
* Fama-MacBeth slopes, univariate and with size (log market cap), value
  (``book_to_market``) and momentum (``momentum_12_1``) controls;
* an IC decay curve (cumulative horizons, and the marginal month-k return) and an
  availability-lag sensitivity (feature of the previous formation, +1 month);
* turnover (rank autocorrelation, one-way top/bottom decile turnover) and net spreads
  at 10/25/50 bp per unit traded;
* size-bucket (micro/small/large by point-in-time NYSE 20th/50th percentile
  breakpoints; market-cap terciles, reported apart, where no point-in-time venue
  exists), subperiod and frozen train/validation/holdout slices, and decile spreads on
  point-in-time NYSE breakpoints next to the all-name deciles;
* label attrition (missing/invalid/unsupported labels, observed/policy terminals) and
  the R3a per-horizon cause classification;
* a per-formation series of every cell (IC, spreads, FM slopes, names, coverage);
* family-wide Benjamini-Hochberg q-values, Holm p-values, the Harvey-Liu-Zhu hurdle
  and the deflated Sharpe ratio of every cell.

Results and a sealed, reproducible run manifest live in the research store (RX6).

Node 4.1 reported economics
--------------------------
Schema 3 appends diagnostics while retaining every original cell/series field and qualification rule.
JKP terciles/deciles rank all names using non-microcap signal breakpoints (verified ME >= NYSE p20);
EW, VW and capped-VW (NYSE p80) each require five valid weighted names per leg. Missing NYSE evidence
has no substituted breakpoint. Raw economics can be provided in context or loaded, one feature at a
time, from the explicit auxiliary inventory; absent inputs remain NULL with their basis/coverage.

Trading diagnostics form unit-notional long and short sleeves on formation weights. Turnover is half
the L1 change from drifted holdings, summed over the legs; h-month sleeves compare formation m to m-h.
The prior label's end must equal the rebalance formation; a fixed-session window ending before/after
that formation cannot supply exact drift, and turnover/cost are unavailable for that row.
Costs charge the FULL L1 change times stock half-spread (AR, then CS), including exits. Break-even bps
are mean gross / mean summed one-sided turnover, on matched formations. Flat-bp diagnostics use that
same one-sided convention; the legacy flat-bp columns remain unchanged. Capacity is aggregate daily
dollars at 1% ADV, requiring complete ADV within each decile, not a claim of feasible book AUM.

Additional FM controls use raw log ME, log BE/ME, momentum, profitability and investment; a focal
control is explicitly omitted, any missing remaining role makes the whole regression unavailable.
K-month slopes are trailing averages with all K observations present; the NW lag rule stays the
brief's max(h-1, floor(4*(T/100)^(2/9))). Signal-age IC always uses the one-month target label, not an
overlapping cumulative label. Publication splits omit the publication year (the date is unknown).
MDE is a labeled normal approximation at two-sided alpha .05/power .8. EB uses a zero-centered
normal prior and a cross-replication method-of-moments variance, fitted after pooling the full run.

Factor models and P3 announcement-session contributions must be explicitly supplied. External
calendar-month factor files are labeled proxies for 21-session labels; their significance is not
claimable. Beta-neutral return statistics are OFF unless the caller supplies the matching wave's
registration, made before any label read, containing each exact source-variant/horizon configuration.
They must never be enabled inside w1's frozen 432-cell grid simply by calling them reported metrics.

Ranked universe
---------------
Every statistic ranks one universe per formation: the eligible R2a cohort rows with
``cohort_reason='valid'`` on the issuer's primary line (``primary_line``), so an
issuer counts once and non-common, overlapping or secondary rows never enter. Per the
controller ruling on R2b I1, an identity-free price-line feature also ranks every
eligible line without an owner link as its own name (R2b ``owner_basis='unlinked_line'``;
cells say ``universe_scope``), so the survivor-conditioned owner link of the
reconstructed basis does not drop the delisted tail from momentum/volatility/reversal;
fundamentals and size stay linked-only. The engine drops any other value and counts
the drops per formation (``dropped_not_valid`` / ``dropped_not_primary``); the store
adapter refuses them. Coverage is measured against the feature's own ranked universe.

Size and venue
--------------
* Size is the R2a panel's ``market_cap`` only where its ``size_status`` is the panel's
  verified status (``size_policy.verified_status``: DEI share counts). Unverified
  (vendor/archive share) caps are NULL here: they never weight a portfolio, set or fall
  into a size bucket, or enter the Fama-MacBeth size control.
* The venue is point in time: the strict ``us_listed_v1`` membership row visible at the
  formation cutoff (listing evidence as of the formation). The cohort's own exchange
  code (a current-directory backcast on the reconstructed basis) is never used.
* Size buckets (``slice_kind='size_bucket'``, ``venue_basis='nyse_pit'``): NYSE 20th/50th
  percentile breakpoints of the ranked universe's point-in-time NYSE names with a
  verified cap, at formations with at least ``nyse_min_names`` of them. Other formations
  are bucketed by verified-cap terciles of the ranked universe and reported separately
  (``slice_kind='size_tercile'``, ``venue_basis='cap_terciles'``), so an NYSE-bucket
  statistic never mixes in another breakpoint rule. NYSE-breakpoint deciles use the
  point-in-time NYSE names of the ranked universe. Cells and the per-formation series
  carry ``venue_basis``; formations without point-in-time NYSE breakpoints are a counted
  run blocker.

Inference policy (controller ruling on the R3a review)
------------------------------------------------------
* Every mean (IC, spread, net spread, FM slope) is tested by
  :func:`atx_db.research.stats.mean_inference` in formation units: monthly-sampled
  h-month labels overlap, so ``horizon_periods = h``. Significance, HLZ and BH use the
  EWC fixed-b Student-t test (``robust_p_value`` / ``z_equivalent``); the Newey-West t
  is reported as a secondary column only (it over-rejects under overlap at T ~ 160).
* Intervals: the EWC t interval and the studentized circular block bootstrap (block
  >= h, fixed seed). The bootstrap drops missing formations (documented in stats).
* Per-formation statistics are vectorized over all formations at once, with their
  definitions pinned by tests to the existing implementations: rank IC and rank
  autocorrelation to ``signal_eval`` (``_rank_corr``: Pearson of average ranks, NaN below
  3 pairs or for a constant side), deciles/quintiles to ``compute_quantile_spread``
  (``rank(method='first')`` with security tie-break, then ``pd.qcut``) and per-date OLS
  slopes to :func:`stats.fama_macbeth`. Calling those per formation costs 4 s and 220 MB
  (IC, 2,000 names x 160 months x 4 horizons) and 0.5 s per FM call, which a ~2,000
  feature-variant x 2 basis run cannot afford. Portfolios are formed on every name with
  a feature value at the formation (FQ2 rule); returns average over names with a valid
  label, and unlabeled names are counted as attrition, never dropped before ranking.
* The family is anchored to the catalog, not to what was produced: by default the
  catalog snapshot R2b stored with the feature versions (sign, class, hypothesis family
  as built; a later catalog edit cannot change a finished evaluation, and a snapshot
  that differs from the committed catalog is a blocker). The expected family is every
  research-eligible catalog feature x its expected variants x the default horizons on
  every testable basis. A cell that is ``tested`` enters BH and
  Holm with its primary-sample IC robust p-value; an expected cell that is
  ``insufficient_formations``, ``no_values``, ``not_produced`` (R2b wrote no such
  feature/variant) or ``excluded_by_subset`` (the run's features/variants/horizons
  left it out) enters as untestable, i.e. p = 1 (conservative: omissions and subset
  runs can never shrink m). ``untestable_strict`` cells (a data-less basis) are
  reported outside the family. A run is ``family_complete`` only when nothing expected
  was excluded or missing; subset runs carry the blocker ``partial_family_subset`` and
  must not be qualified (R4); a run that did not supply both bases is not complete
  either. Without a catalog (pure-engine callers) the family is the produced cells.
  Significance is the IC's; decile spreads, Fama-MacBeth slopes and Sharpe-based
  statistics are supporting evidence. The probabilistic and deflated Sharpe ratios of
  a cell use its equal-weighted decile long-short series with ``horizon_periods = h``
  (``floor(n/h)`` independent returns); the DSR uses ``n_trials`` = the whole family
  and the cross-trial variance of the Sharpe ratios of tested cells at the same horizon
  (the same period units). Rank-equivalent variants (``signed_raw``/``zscore``/
  ``rank_normal`` give identical IC and deciles) are near-duplicate members: BH is
  unaffected, Holm and ``n_trials`` are conservative. Two-sided hypotheses
  (``expected_sign`` 0) are tested like the others (the EWC p-value is two-sided);
  their values are unoriented, so the sign of the IC is the finding.
* Selection sample. With a frozen split (RX7) the primary statistics, the family and
  every derived slice (subperiods, size buckets, decay, quantiles) use only the
  selection sample: train + validation formations whose label window ends before the
  holdout starts. Holdout statistics appear only as ``slice_kind='split'`` rows. Without
  a split the primary sample is every formation and the run carries a blocker. A
  subperiod that overlaps the holdout is truncated to its selection formations (its
  ``formations`` count says how many remain; it may be empty).
* Reproducibility scope: the code digest covers this module, ``stats.py``, ``labels.py``,
  ``features.py`` and ``factor_returns.py``; panel/store/catalog inputs are covered by their manifests. Byte
  identity relies on batched LAPACK solves, so ``verify`` belongs on the same host/BLAS.

Policy v4 additions (tier-1 v2 node 1.10; off unless the spec enables them)
--------------------------------------------------------------------------
* Investable co-primary slice (``investable_min_price`` / ``investable_nyse_percentile``):
  ``slice_kind='investable'``: the rank IC re-ranked among names with a price (context
  ``price``) at or above the minimum and a verified cap at or above the NYSE percentile
  breakpoint -- supplied with the calendar (``nyse_me_p<k>``, e.g. the Fama-French NYSE ME
  breakpoints: ``venue_basis='nyse_breakpoints'``), else from point-in-time NYSE names
  (``nyse_pit``); formations with neither have no investable names. Its spread is the
  value-weighted decile long-short formed within the investable names.
* Supplied NYSE 20/50 breakpoints (calendar ``nyse_me_p20`` and ``nyse_me_p50``) also set
  the size buckets of their formations (``slice_kind='size_bucket_nyse_bp'``).
* Jegadeesh-Titman calendar-time holdings (``jt_holding_months``, needs the 1-month labels):
  ``slice_kind='jt_calendar'``, ``slice_name='k<K>'``, ``horizon_months=1``: the month-t
  return averages the EW decile long-shorts formed at t-1 ... t-K
  (:func:`jegadeesh_titman_series`); months with all K cohorts inside the selection sample
  enter the EWC test (``horizon_periods = 1``). Reported, never gating.
* Population coverage (``population_coverage``): a feature whose catalog ``population`` is
  not ``all`` measures coverage against the ranked-universe members flagged
  ``population_<name>`` in the context (NaN when the flag is absent), not the ranked universe.
* Family scope: :func:`family_scope` marks the gating family (primary cells of a wave's
  gating hypotheses); :func:`gating_bh_q` computes its Benjamini-Hochberg q-values with the
  earlier waves' gating hypotheses at p = 1. The R3b family columns stay the reported family.
* Holdout seal (``seal_holdout``, needs the split): every label whose expected end is on/after
  the holdout start, or unknown, is dropped (and its formation counts as not matured) before
  anything is computed, so no statistic -- cells, slices, series, lags, JT -- sees a
  holdout-period return. The count per horizon is ``labels_<h>m_sealed_formations`` in the
  prepared digests.
* Order and label binding: ``wave_registration_id`` (the trial-registry record sha of the
  wave's registration), ``created_at`` and ``label_sha256``: the label-matrix spec sha of the
  opened holdout's labels (``label_matrix.compute_label_sha``, what
  ``trial_registry.open_holdout`` records; ruling C-52). A basis read from the label matrix
  records that read in ``meta['label_read']`` (:func:`label_read_meta` of
  ``LabelMatrix.r3b_inputs``'s info). A run whose spec declares ``label_sha256`` refuses a basis
  whose recorded read names another set, or none, before it prepares that basis: no statistic
  or row of another label set exists. :func:`label_set_sha256` digests the prepared label arrays
  a run saw (every basis's ``labels_<h>m_sha256``): an audit digest, not the holdout identity.

Point-in-time guards
--------------------
* Formation authority is the R2a ``research_panel_calendar``: only ``formed`` months
  are evaluated, each at its decision date; a missing month-end stays a gap (its
  position is kept in every HAC sum).
* A label is used only when it is anchored exactly at its formation's entry session
  (the session after the decision date) and its window ends where the label calendar
  says; anything else raises :class:`LookaheadError`. Labels are the R3a monthly
  source selected by revision, then validated by the FQ2 validity fragment at the
  evaluation observation cutoff (``calculation_version`` forward_return_publication_v3).
* A feature value whose ``available_at`` is after its formation cutoff raises
  :class:`LookaheadError`.

R2b feature store (``atx_db.research.features``, query version research-feature-store-v1)
-----------------------------------------------------------------------------------------
:func:`load_feature_table` is the only function that reads R2b tables (with
``verify_panels`` the R2b validator, :func:`features.validate_feature_version`, re-checks
the version first). It reads, in the research store:

``research_feature_versions`` (one row per immutable version)
    ``feature_version``, ``status`` (``sealed``, or ``untestable_strict`` for a strict
    build over an empty cohort; ``building``/``failed`` versions are refused), ``basis``,
    ``panel_run_id`` and ``panel_sha256`` (the sealed R2a run; the seal must still hold),
    ``classification_basis`` (RX1 label), ``values_sha256``, ``query_version`` (one of
    :data:`FEATURE_QUERY_VERSIONS`), ``universe_rule`` (one of
    :data:`FEATURE_UNIVERSE_RULES`), ``catalog_sha256`` (compared with the committed
    catalog's file digest), ``blockers_json`` (carried into the run blockers) and
    ``spec_json.store_schema`` (>= :data:`MIN_FEATURE_STORE_SCHEMA`, i.e. built after R2e;
    checked even when the validators are skipped). A P5 composite version
    (``spec_json.kind='composite'``) is evaluated only in its holdout window's one recorded
    run (``research_composite_builds``, read-only): the run's split must be the window's,
    the registry must hold the version's build, and no other run may have used the window.
``research_feature_catalog`` (version x catalog row: the catalog snapshot)
    ``feature_id``, ``anomaly_class``, ``hypothesis_family``, ``expected_sign``,
    ``status``, ``status_reason``. Every row not ``blocked_admission`` is a hypothesis of
    the expected family (:func:`feature_version_catalog`); the status explains
    ``not_produced`` cells (input not in the panel, ...).
``research_feature_values`` (a long VIEW over the wide ``research_feature_matrix``)
    ``feature_version``, ``formation_date``, ``security_id``, ``feature_id``, ``variant``,
    ``value`` (sign-oriented by the catalog ``expected_sign``; unoriented for a two-sided
    row; NULL outside the domain and for standardized variants on a non-``formed``
    formation), ``expected_sign`` (+1/-1/0, equal to the R1a catalog) and
    ``available_at`` (latest input clock; must be <= the formation cutoff). A value
    exists only on an eligible R2a cohort row with ``cohort_reason='valid'`` on the
    issuer's ``primary_line`` whose panel row is valid, or, when the optional
    ``owner_basis`` column says ``unlinked_line``, on an eligible line without an owner
    link (identity-free price-line features); the adapter refuses any other valued row
    and a second valued line of one owner.
``research_feature_dates`` (formation x feature x variant)
    ``date_status`` (only ``formed`` formations are ranked: ``thin_cross_section``,
    ``thin_covariate_coverage``, ``degenerate_cross_section`` and ``empty_common_cohort``
    are not), ``eligible_members``, ``valid_names``, ``coverage_fraction``. The variants
    of a feature are its date rows' variants.

Market cap (value weights, size buckets, size control), the cohort flags and the venue
are read from the R2a panel of the version (``market_cap`` daily feature with verified
``size_status``, ``research_panel_cohort``) and the warehouse's strict membership, not
from R2b. Controls ``book_to_market`` and ``momentum_12_1`` are read from the same
feature version at ``control_variant`` (default ``rank_normal``); one query per
(feature, all variants).
"""

from __future__ import annotations

import datetime as dt
import hashlib
import itertools
import json
import math
import re
import tempfile
from collections.abc import Callable, Hashable, Iterable, Mapping, Sequence
from contextlib import AbstractContextManager, nullcontext, suppress
from dataclasses import dataclass, field, replace
from functools import lru_cache
from pathlib import Path
from typing import Any

import duckdb
import numpy as np
import pandas as pd

from ..universe_us_listed import DEFAULT_US_LISTED_UNIVERSE_ID, UNIVERSE_SOURCE_NAME
from . import stats
from .labels import (
    HORIZON_FORMATION_UNITS,
    LABEL_CALCULATION_VERSION,
    MONTHLY_LABEL_SOURCE,
    label_revision_order_sql,
    label_status_sql,
    monthly_label_diagnostics,
)
from .labels import _calendar_keys as _label_calendar_keys  # same package: the label calendar identity
from .panel import CALENDAR_FORMED, OWNER_LINK_FAILURES, validate_research_panel
from .store import ResearchStore

EVALUATION_VERSION = "research-monthly-evaluation-v2"
EVALUATION_SCHEMA_VERSION = 4
FEATURE_CONTRACT = "r2b-research-feature-store-v1"
#: R2b query versions whose tables this adapter reads (a new R2b version needs review here);
#: v2 is R2b fix round 1 (identity-free price-line features also rank unlinked lines).
FEATURE_QUERY_VERSIONS = ("research-feature-store-v1", "research-feature-store-v2")
#: R2e kept the query version at v2 and bumped the feature store schema instead (recorded as
#: ``spec_json.store_schema``); versions below it revive stale market values and leave survivor
#: conditioning unlabeled, so they are refused even when validators are skipped.
MIN_FEATURE_STORE_SCHEMA = 3
#: P5 composite feature versions carry ``kind='composite'`` in their spec (one holdout look each).
COMPOSITE_KIND = "composite"
FEATURE_VERSION_STATUSES = ("sealed", "untestable_strict")
FEATURE_VARIANTS = ("signed_raw", "winsor", "zscore", "rank_normal", "industry_neutral", "size_neutral")
#: Variants on a standardized scale (FM slopes comparable across features); the others are raw units.
STANDARDIZED_VARIANTS = frozenset({"zscore", "rank_normal", "industry_neutral", "size_neutral"})
UNIVERSE_RULE = "cohort_reason_valid_on_primary_line"
#: Controller ruling on R2b I1: identity-free price-line features also rank every eligible line
#: without an owner link as its own name (``owner_basis='unlinked_line'``); fundamentals and
#: size stay on linked primary lines.
UNIVERSE_RULE_WITH_UNLINKED = "cohort_reason_valid_on_primary_line_price_line_unlinked_lines"
FEATURE_UNIVERSE_RULES = (UNIVERSE_RULE, UNIVERSE_RULE_WITH_UNLINKED)
OWNER_BASIS_UNLINKED = "unlinked_line"
UNIVERSE_SCOPE_LINKED = "valid_primary_lines"
UNIVERSE_SCOPE_WITH_UNLINKED = "valid_primary_and_unlinked_lines"
#: Point-in-time venue: the strict universe's membership visible at the formation cutoff.
PIT_VENUE_UNIVERSE_ID = DEFAULT_US_LISTED_UNIVERSE_ID
PIT_VENUE_SOURCE = UNIVERSE_SOURCE_NAME
BASES = ("strict", "reconstructed")
BASIS_AVAILABLE = "available"
BASIS_UNTESTABLE = "untestable_strict"
DEFAULT_HORIZONS: tuple[int, ...] = (1, 3, 6, 12)
HORIZON_SESSIONS: dict[int, int] = {units: sessions for sessions, units in HORIZON_FORMATION_UNITS.items()}
COST_BPS: tuple[int, ...] = (10, 25, 50)
DEFAULT_SUBPERIODS: tuple[tuple[str, dt.date, dt.date], ...] = (
    ("sub_2013_2016", dt.date(2013, 1, 1), dt.date(2016, 12, 31)),
    ("sub_2017_2020", dt.date(2017, 1, 1), dt.date(2020, 12, 31)),
    ("sub_2021_2025", dt.date(2021, 1, 1), dt.date(2025, 12, 31)),
)
SPLIT_SEGMENTS = ("train", "validation", "holdout")
SIZE_BUCKETS = ("unknown", "micro", "small", "large")
NYSE_EXCHANGE_CODE = "XNYS"
#: ``venue_basis`` of a formation's size breakpoints: point-in-time NYSE 20/50, verified-cap
#: terciles (no point-in-time venue), none (fewer than 3 verified caps), no context rows.
VENUE_NYSE_PIT, VENUE_CAP_TERCILES = "nyse_pit", "cap_terciles"
#: Policy v4: NYSE market-cap breakpoints supplied with the calendar (``nyse_me_p20`` / ``nyse_me_p50``,
#: e.g. the Fama-French NYSE ME breakpoints) take precedence over point-in-time NYSE names.
VENUE_NYSE_BREAKPOINTS = "nyse_breakpoints"
VENUE_BASES = (VENUE_NYSE_PIT, VENUE_CAP_TERCILES, "none", "no_context", VENUE_NYSE_BREAKPOINTS)
#: Size-bucket slice kind per breakpoint basis (an NYSE-bucket slice never mixes in terciles).
SIZE_SLICE_KINDS = {VENUE_NYSE_PIT: "size_bucket", VENUE_CAP_TERCILES: "size_tercile"}
#: Size-bucket slice kind of formations bucketed on supplied NYSE breakpoints (emitted only when supplied).
SUPPLIED_SIZE_SLICE_KIND = "size_bucket_nyse_bp"
#: Policy v4 slices: the investable co-primary cell and the Jegadeesh-Titman calendar-time holdings.
INVESTABLE_SLICE_KIND, INVESTABLE_SLICE_NAME = "investable", "investable"
JT_SLICE_KIND = "jt_calendar"
#: Calendar column of a supplied NYSE market-cap percentile breakpoint (units of ``market_cap``).
SUPPLIED_BREAKPOINT_COLUMN = "nyse_me_p{percentile}"
#: Context column prefix of a catalog-population membership flag (``population_<name>``).
POPULATION_FLAG_PREFIX = "population_"
POPULATION_ALL = "all"
SIZE_FEATURES = frozenset({"market_cap"})
LABEL_BASIS = "adjusted_close_forward_return_with_observed_or_policy_terminal_stitch"
BH_ALPHA = 0.05
NOT_PRODUCED = "not_produced"
EXCLUDED_BY_SUBSET = "excluded_by_subset"
FAMILY_STATUSES = ("tested", "insufficient_formations", "no_values", NOT_PRODUCED, EXCLUDED_BY_SUBSET)
CELL_STATUSES = (*FAMILY_STATUSES, BASIS_UNTESTABLE)
#: Status-only rows are rebuilt by the family stage (never by the per-feature engine).
STATUS_ONLY = (NOT_PRODUCED, EXCLUDED_BY_SUBSET, BASIS_UNTESTABLE)
#: Label-window alignments that are normal operation, not a blocker.
BENIGN_ALIGNMENTS = frozenset({"aligned", "entry_after_label_cutoff"})

# Tier-1 v2 node 4.1: reported-only metrics (module docstring). None of them gates or enters a family.
#: JKP portfolios: terciles and deciles on non-microcap breakpoints (market cap at or above the NYSE 20th
#: percentile), equal, value and capped-value (cap winsorized at the NYSE 80th percentile) weights.
JKP_QUANTILES: tuple[int, ...] = (3, 10)
JKP_WEIGHTINGS: tuple[str, ...] = ("ew", "vw", "cvw")
JKP_NONMICRO_PERCENTILE, JKP_CAP_PERCENTILE = 20, 80
#: A JKP long-short needs at least this many names (with a valid label and a weight) in each leg.
JKP_MIN_LEG_NAMES = 5
#: Optional ``BasisInputs.context`` columns of the cost / capacity / beta-neutral metrics: full bid-ask
#: spread estimates as a fraction of price (Abdi-Ranaldo, the catalog's ``bidask_ar_21d``; Corwin-Schultz
#: ``bidask_cs_21d``, the fallback), average daily dollar volume (``dolvol_126d``) and the market beta
#: (``beta_ew_252d``). Absent columns make their metrics NULL with a labeled basis.
CONTEXT_SPREAD_AR, CONTEXT_SPREAD_CS = "spread_ar", "spread_cs"
CONTEXT_ADV, CONTEXT_BETA = "adv_usd", "beta"
AUX_CONTEXT_COLUMNS: tuple[str, ...] = (CONTEXT_SPREAD_AR, CONTEXT_SPREAD_CS, CONTEXT_ADV, CONTEXT_BETA)
HALF_SPREAD_NONE, HALF_SPREAD_AR, HALF_SPREAD_CS = 0, 1, 2
#: Capacity: the dollars a decile can trade per day at this fraction of each name's average daily volume.
CAPACITY_ADV_FRACTION = 0.01
#: Buy/hold-spread variant: enter the top (bottom) decile, hold while at or above decile 8 (at or below 3).
BUY_HOLD_ENTER, BUY_HOLD_STAY = 10, 8
#: Flag a long-short whose mean one-sided turnover per leg exceeds this share per month.
TURNOVER_FLAG_MONTHLY = 0.5
#: IC decay by signal age: IC_k = corr(x_{t-k}, r_{t+1}), k = 0 .. 11 (one-month labels).
SIGNAL_AGES: tuple[int, ...] = tuple(range(12))
#: Fama-MacBeth control set (JKP / HXZ convention): role -> candidate catalog features (the first one the basis
#: carries as a control serves); ``log_me`` is the engine's log verified market cap. Additional controls
#: use raw, unoriented values (BE/ME is logged); the legacy spec controls remain unchanged. A missing role
#: is recorded per cell (``fmj_controls_missing``), never silently dropped.
FM_JKP_CONTROLS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("log_me", ()),
    ("log_bm", ("book_to_market", "be_me")),
    ("ret_12_1", ("ret_12_1", "momentum_12_1")),
    ("gp_at", ("gross_profitability", "gp_at")),
    ("at_gr1", ("asset_growth", "at_gr1")),
)
#: Long-shorts spanned by the factor models and costed: the all-name EW decile L/S and the JKP capped-VW decile L/S.
SPAN_PORTFOLIOS: tuple[str, ...] = ("ew10", "cvw10")
#: The span models, in the column order of the cells (factor_returns.SPAN_MODELS keys).
SPAN_MODEL_NAMES: tuple[str, ...] = ("capm_atx", "ff6_atx", "capm_french", "ff6_french", "q5")
PUBLICATION_SLICE_KIND = "publication"
#: Earnings-announcement-day decomposition input (optional ``BasisInputs.event_returns``): per (formation,
#: security, horizon) the part of the label return earned on P3 announcement sessions [-1, +1].
EVENT_RETURN_COLUMNS = ("month_index", "security", "horizon_months", "ea_return")
#: Ex-post accounting only: current rebalance month m, return from entry[m-h] through entry[m].
#: The caller verifies the pinned source and registration before reading these returns; no feature uses them.
REBALANCE_RETURN_COLUMNS = ("month_index", "security", "horizon_months", "previous_entry_date", "entry_date",
                            "realized_return", "status")
#: OSAP (Chen-Zimmermann) SignalDoc acronym of a catalog feature whose construct matches closely enough for
#: the replication table (atx t vs the original paper's t); anything else is left out of the table.
OSAP_ACRONYMS: dict[str, str] = {
    "ret_12_1": "Mom12m", "ret_6_1": "Mom6m", "ret_12_7": "IntMom", "ret_36_13": "LRreversal",
    "seas_1_1an": "MomSeasonShort", "seas_2_5an": "MomSeason", "ret_1_0": "STreversal", "rvol_21d": "RealizedVol",
    "rmax1_21d": "MaxRet", "ivol_ew_21d": "IdioVol3F", "coskew_252d": "CoskewACX", "beta_bab_1260d": "BetaFP",
    "zero_trade_21d": "zerotrade1M", "zero_trade_252d": "zerotrade12M", "std_turn_126d": "std_turn",
    "std_dvol_126d": "VolSD", "ami_252d": "Illiquidity", "prc_log": "Price", "prc_highprc_252d": "High52",
    "me_line_log": "Size", "dolvol_126d": "DolVol", "price_delay_52w": "PriceDelayRsq",
}

LABEL_VALID, LABEL_INVALID, LABEL_UNSUPPORTED, LABEL_MISSING = 0, 1, 2, 3
TERMINAL_NONE, TERMINAL_OBSERVED, TERMINAL_POLICY = 0, 1, 2

_ID = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_VERSION = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$")
_CODE_FILES = (Path(__file__), Path(stats.__file__), *(Path(__file__).with_name(name) for name in
               ("labels.py", "features.py", "factor_returns.py")))
_NAN = float("nan")


class EvaluationInputError(ValueError):
    """Inputs violate the evaluation contract (calendar, grain, basis, split, R2b contract)."""


class LookaheadError(EvaluationInputError):
    """A label or feature value would let the evaluation see the future."""


# ---------------------------------------------------------------------------
# Frozen split (RX7) and run specification
# ---------------------------------------------------------------------------

def _canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _as_date(value: Any, label: str) -> dt.date:
    if isinstance(value, dt.datetime):
        raise EvaluationInputError(f"{label} must be a date, not a datetime")
    if isinstance(value, dt.date):
        return value
    try:
        return dt.date.fromisoformat(str(value))
    except ValueError as error:
        raise EvaluationInputError(f"{label} must be an ISO date: {value!r}") from error


@dataclass(frozen=True)
class FrozenSplit:
    """Train/validation/holdout segments by formation date, frozen by a content hash.

    ``segments`` are ``(name, first_formation_date, last_formation_date)`` in the order
    train, validation, holdout, non-overlapping and ascending. ``embargo_months`` drops the
    first months of validation and holdout from their own slices. A formation whose label
    window ends on/after the next segment's start is purged from its segment.
    """

    split_version: str
    segments: tuple[tuple[str, dt.date, dt.date], ...]
    embargo_months: int
    sha256: str

    def content(self) -> dict[str, Any]:
        return {"split_version": self.split_version,
                "segments": [{"name": name, "start": start.isoformat(), "end": end.isoformat()}
                             for name, start, end in self.segments],
                "embargo_months": self.embargo_months}

    @property
    def holdout_start(self) -> dt.date:
        return self.segments[-1][1]


def _split_parts(data: Mapping[str, Any]) -> tuple[str, tuple[tuple[str, dt.date, dt.date], ...], int]:
    if set(data) - {"split_version", "segments", "embargo_months", "sha256"}:
        raise EvaluationInputError("split has unknown keys")
    version = data.get("split_version")
    if not isinstance(version, str) or not _VERSION.fullmatch(version):
        raise EvaluationInputError("split_version must be a short identifier")
    raw = data.get("segments")
    if not isinstance(raw, Sequence) or len(raw) != len(SPLIT_SEGMENTS):
        raise EvaluationInputError(f"split needs exactly the segments {SPLIT_SEGMENTS}")
    segments = []
    for expected, item in zip(SPLIT_SEGMENTS, raw, strict=True):
        if not isinstance(item, Mapping) or item.get("name") != expected or set(item) != {"name", "start", "end"}:
            raise EvaluationInputError(f"split segments must be {SPLIT_SEGMENTS} in order with start/end")
        start, end = _as_date(item["start"], f"{expected}.start"), _as_date(item["end"], f"{expected}.end")
        if start > end:
            raise EvaluationInputError(f"split segment {expected} starts after it ends")
        segments.append((expected, start, end))
    for (name, _, end), (_, start, _) in itertools.pairwise(segments):
        if not end < start:
            raise EvaluationInputError(f"split segment {name} overlaps the next segment")
    embargo = data.get("embargo_months", 0)
    if isinstance(embargo, bool) or not isinstance(embargo, int) or not 0 <= embargo <= 24:
        raise EvaluationInputError("embargo_months must be an integer 0..24")
    return version, tuple(segments), embargo


def split_sha256(content: Mapping[str, Any]) -> str:
    """Content hash of a split: sha256 of the canonical JSON of version, segments, embargo."""
    version, segments, embargo = _split_parts({k: v for k, v in content.items() if k != "sha256"})
    return _sha(_canonical(FrozenSplit(version, segments, embargo, "").content()))


def freeze_split(split_version: str, segments: Sequence[tuple[str, Any, Any]], *,
                 embargo_months: int = 0) -> FrozenSplit:
    """Build a split and compute its hash (commit the result before any real-data run)."""
    content = {"split_version": split_version, "embargo_months": embargo_months,
               "segments": [{"name": n, "start": str(s), "end": str(e)} for n, s, e in segments]}
    version, parsed, embargo = _split_parts(content)
    return FrozenSplit(version, parsed, embargo, split_sha256(content))


def load_frozen_split(source: Path | str | Mapping[str, Any]) -> FrozenSplit:
    """Load a frozen split (a split JSON, or a policy JSON with a ``split`` object).

    The declared ``sha256`` must equal the content hash: an edited split is refused.
    """
    data: Any = source if isinstance(source, Mapping) else json.loads(Path(source).read_text(encoding="utf-8"))
    if isinstance(data, Mapping) and isinstance(data.get("split"), Mapping):
        data = data["split"]
    if not isinstance(data, Mapping):
        raise EvaluationInputError("split must be a JSON object")
    declared = data.get("sha256")
    version, segments, embargo = _split_parts(data)
    actual = _sha(_canonical(FrozenSplit(version, segments, embargo, "").content()))
    if declared != actual:
        raise EvaluationInputError(f"split is not frozen: declared sha256 {declared!r} != content sha256 {actual}")
    return FrozenSplit(version, segments, embargo, actual)


@dataclass(frozen=True)
class EvaluationSpec:
    """Everything that determines an evaluation's results (``run_id`` excepted)."""

    run_id: str
    feature_versions: tuple[str, ...] = ()
    label_cutoff: dt.datetime | None = None
    label_source: str = MONTHLY_LABEL_SOURCE
    horizons_months: tuple[int, ...] = DEFAULT_HORIZONS
    features: tuple[str, ...] | None = None
    variants: tuple[str, ...] | None = None
    split: FrozenSplit | None = None
    allow_unsplit: bool = False
    subperiods: tuple[tuple[str, dt.date, dt.date], ...] = DEFAULT_SUBPERIODS
    min_names: int = 200
    min_bucket_names: int = 50
    fm_min_obs: int = 200
    min_formations: int = 36
    nyse_min_names: int = 20
    bootstrap_resamples: int = 1999
    bootstrap_seed: int = stats.DEFAULT_BOOTSTRAP_SEED
    control_variant: str = "rank_normal"
    control_features: tuple[str, ...] = ("book_to_market", "momentum_12_1")
    marginal_months: tuple[int, ...] = (1, 3, 6, 12)
    label_diagnostics: bool = True
    verify_panels: bool = True
    #: sha256 of the frozen R4 policy file the split came from (recorded, not interpreted).
    policy_sha256: str | None = None
    #: Policy v4 (off by default; a spec that leaves them off has the pre-v4 payload and results):
    #: the investable slice (price >= this and market cap >= the NYSE ``investable_nyse_percentile``
    #: percentile), Jegadeesh-Titman calendar-time holdings (months; needs the 1-month labels) and
    #: coverage measured against the catalog population instead of the ranked universe.
    investable_min_price: float | None = None
    investable_nyse_percentile: int | None = None
    jt_holding_months: tuple[int, ...] = ()
    population_coverage: bool = False
    #: Policy v4 order and seal (off by default, absent from a pre-v4 payload): ``seal_holdout``
    #: drops every label whose expected end is on/after the split's holdout start (or unknown)
    #: before anything is computed, so no holdout-period return enters any statistic;
    #: ``wave_registration_id`` binds the run to its trial-registry registration (a record sha
    #: that exists only after the wave was registered); ``label_sha256`` declares the label-matrix
    #: set (spec sha) the run must see -- an opened holdout's labels; the engine refuses a basis
    #: whose ``meta['label_read']`` names another (ruling C-52); ``created_at`` is when the spec
    #: was built (naive-UTC ISO seconds).
    seal_holdout: bool = False
    wave_registration_id: str | None = None
    label_sha256: str | None = None
    created_at: str | None = None


def _positive_int(value: Any, label: str, low: int, high: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise EvaluationInputError(f"{label} must be an integer {low}..{high}")
    return value


def _ids(values: Iterable[str] | None, label: str) -> tuple[str, ...] | None:
    if values is None:
        return None
    items = tuple(values)
    if not items or len(set(items)) != len(items) or any(not isinstance(v, str) or not _ID.fullmatch(v)
                                                         for v in items):
        raise EvaluationInputError(f"{label} must be unique lower-case identifiers")
    return tuple(sorted(items))


def validate_spec(spec: EvaluationSpec) -> EvaluationSpec:
    """Validate and normalize (sorted id tuples, naive-UTC cutoff)."""
    if not isinstance(spec.run_id, str) or not _ID.fullmatch(spec.run_id):
        raise EvaluationInputError("run_id must be a lower-case identifier")
    versions = tuple(spec.feature_versions)
    if len(set(versions)) != len(versions) or any(not isinstance(v, str) or not _VERSION.fullmatch(v)
                                                  for v in versions):
        raise EvaluationInputError("feature_versions must be unique version identifiers")
    horizons = tuple(sorted(set(spec.horizons_months)))
    if not horizons or len(horizons) != len(spec.horizons_months) or any(h not in HORIZON_SESSIONS for h in horizons):
        raise EvaluationInputError(f"horizons_months must be distinct values from {sorted(HORIZON_SESSIONS)}")
    marginal = tuple(sorted(set(spec.marginal_months)))
    if any(isinstance(k, bool) or not isinstance(k, int) or not 1 <= k <= 12 for k in marginal):
        raise EvaluationInputError("marginal_months must be integers 1..12")
    cutoff = spec.label_cutoff
    if cutoff is not None:
        if not isinstance(cutoff, dt.datetime):
            raise EvaluationInputError("label_cutoff must be a datetime")
        if cutoff.tzinfo is not None:
            if cutoff.utcoffset() != dt.timedelta(0):
                raise EvaluationInputError("label_cutoff must be UTC")
            cutoff = cutoff.astimezone(dt.UTC).replace(tzinfo=None)
    if not isinstance(spec.label_source, str) or not 0 < len(spec.label_source) <= 200:
        raise EvaluationInputError("label_source must be a bounded source name")
    if spec.split is not None:
        if not isinstance(spec.split, FrozenSplit):
            raise EvaluationInputError("split must be a FrozenSplit")
        load_frozen_split({**spec.split.content(), "sha256": spec.split.sha256})
    subperiods = []
    for item in spec.subperiods:
        name, start, end = item
        if not isinstance(name, str) or not _ID.fullmatch(name) or not start <= end:
            raise EvaluationInputError("subperiods must be (identifier, start, end) with start <= end")
        subperiods.append((name, _as_date(start, name), _as_date(end, name)))
    if len({name for name, _, _ in subperiods}) != len(subperiods):
        raise EvaluationInputError("subperiod names must be unique")
    _positive_int(spec.min_names, "min_names", 3, 100_000)
    _positive_int(spec.min_bucket_names, "min_bucket_names", 5, 100_000)
    _positive_int(spec.fm_min_obs, "fm_min_obs", 3, 100_000)
    _positive_int(spec.min_formations, "min_formations", 2, 10_000)
    _positive_int(spec.nyse_min_names, "nyse_min_names", 1, 100_000)
    _positive_int(spec.bootstrap_resamples, "bootstrap_resamples", 0, 100_000)
    _positive_int(spec.bootstrap_seed, "bootstrap_seed", 0, 2**63 - 1)
    if not isinstance(spec.control_variant, str) or not _ID.fullmatch(spec.control_variant):
        raise EvaluationInputError("control_variant must be an identifier")
    if spec.policy_sha256 is not None and not re.fullmatch(r"[0-9a-f]{64}", str(spec.policy_sha256)):
        raise EvaluationInputError("policy_sha256 must be a sha256 hex digest")
    controls = _ids(spec.control_features, "control_features") if spec.control_features else ()
    price, percentile = spec.investable_min_price, spec.investable_nyse_percentile
    if (price is None) != (percentile is None):
        raise EvaluationInputError("investable_min_price and investable_nyse_percentile come together")
    if price is not None:
        if isinstance(price, bool) or not isinstance(price, (int, float)) or not 0.0 <= float(price) <= 10_000.0:
            raise EvaluationInputError("investable_min_price must be a non-negative price")
        price = float(price)
        _positive_int(percentile, "investable_nyse_percentile", 1, 99)
    holding = tuple(sorted(set(spec.jt_holding_months)))
    if len(holding) != len(spec.jt_holding_months) or any(isinstance(k, bool) or not isinstance(k, int)
                                                          or not 1 <= k <= 12 for k in holding):
        raise EvaluationInputError("jt_holding_months must be distinct integers 1..12")
    if holding and 1 not in horizons:
        raise EvaluationInputError("Jegadeesh-Titman holdings need the 1-month labels (horizons_months must hold 1)")
    if not isinstance(spec.population_coverage, bool):
        raise EvaluationInputError("population_coverage must be a boolean")
    if not isinstance(spec.seal_holdout, bool):
        raise EvaluationInputError("seal_holdout must be a boolean")
    if spec.seal_holdout and spec.split is None:
        raise EvaluationInputError("seal_holdout needs a frozen split (it seals the split's holdout)")
    for name in ("wave_registration_id", "label_sha256"):
        value = getattr(spec, name)
        if value is not None and not re.fullmatch(r"[0-9a-f]{64}", str(value)):
            raise EvaluationInputError(f"{name} must be a sha256 hex digest")
    if spec.seal_holdout and spec.label_sha256 is not None:
        raise EvaluationInputError("a sealed run reads no holdout label: label_sha256 declares an opened label set")
    created = spec.created_at
    if created is not None:
        try:
            stamp = dt.datetime.fromisoformat(str(created))
        except ValueError as error:
            raise EvaluationInputError("created_at must be an ISO datetime") from error
        if stamp.tzinfo is not None:
            stamp = stamp.astimezone(dt.UTC).replace(tzinfo=None)
        created = stamp.isoformat(timespec="seconds")
    return replace(spec, feature_versions=versions, horizons_months=horizons, marginal_months=marginal,
                   label_cutoff=cutoff, features=_ids(spec.features, "features"),
                   variants=_ids(spec.variants, "variants"), subperiods=tuple(subperiods),
                   control_features=controls or (), investable_min_price=price, jt_holding_months=holding,
                   created_at=created)


def spec_payload(spec: EvaluationSpec) -> dict[str, Any]:
    """Canonical JSON-able form of a validated spec (``run_id`` excluded).

    The policy v4 options appear only when set, so a pre-v4 spec keeps its payload and hash.
    """
    payload = _spec_payload_base(spec)
    if spec.investable_min_price is not None:
        payload["investable_min_price"] = spec.investable_min_price
        payload["investable_nyse_percentile"] = spec.investable_nyse_percentile
    if spec.jt_holding_months:
        payload["jt_holding_months"] = list(spec.jt_holding_months)
    if spec.population_coverage:
        payload["population_coverage"] = True
    if spec.seal_holdout:
        payload["seal_holdout"] = True
    for name in ("wave_registration_id", "label_sha256", "created_at"):
        if getattr(spec, name) is not None:
            payload[name] = getattr(spec, name)
    return payload


def _spec_payload_base(spec: EvaluationSpec) -> dict[str, Any]:
    return {
        "evaluation_version": EVALUATION_VERSION,
        "feature_contract": FEATURE_CONTRACT,
        "feature_versions": list(spec.feature_versions),
        "label_cutoff": None if spec.label_cutoff is None else spec.label_cutoff.isoformat(),
        "label_source": spec.label_source,
        "label_calculation_version": LABEL_CALCULATION_VERSION,
        "horizons_months": list(spec.horizons_months),
        "features": None if spec.features is None else list(spec.features),
        "variants": None if spec.variants is None else list(spec.variants),
        "split": None if spec.split is None else {**spec.split.content(), "sha256": spec.split.sha256},
        "allow_unsplit": spec.allow_unsplit,
        "subperiods": [[name, start.isoformat(), end.isoformat()] for name, start, end in spec.subperiods],
        "cost_bps": list(COST_BPS),
        "min_names": spec.min_names, "min_bucket_names": spec.min_bucket_names,
        "fm_min_obs": spec.fm_min_obs, "min_formations": spec.min_formations,
        "nyse_min_names": spec.nyse_min_names,
        "bootstrap_resamples": spec.bootstrap_resamples, "bootstrap_seed": spec.bootstrap_seed,
        "control_variant": spec.control_variant, "control_features": list(spec.control_features),
        "marginal_months": list(spec.marginal_months),
        "label_diagnostics": spec.label_diagnostics, "verify_panels": spec.verify_panels,
        "policy_sha256": spec.policy_sha256,
    }


def spec_from_payload(payload: Mapping[str, Any], run_id: str) -> EvaluationSpec:
    """Inverse of :func:`spec_payload` (used to reproduce a sealed run)."""
    if payload.get("evaluation_version") != EVALUATION_VERSION:
        raise EvaluationInputError("unsupported evaluation version")
    if payload.get("label_calculation_version") != LABEL_CALCULATION_VERSION:
        raise EvaluationInputError("the run used another label calculation version")
    cutoff = payload["label_cutoff"]
    spec = EvaluationSpec(
        run_id=run_id,
        feature_versions=tuple(payload["feature_versions"]),
        label_cutoff=None if cutoff is None else dt.datetime.fromisoformat(cutoff),
        label_source=payload["label_source"],
        horizons_months=tuple(payload["horizons_months"]),
        features=None if payload["features"] is None else tuple(payload["features"]),
        variants=None if payload["variants"] is None else tuple(payload["variants"]),
        split=None if payload["split"] is None else load_frozen_split(payload["split"]),
        allow_unsplit=payload["allow_unsplit"],
        subperiods=tuple((n, dt.date.fromisoformat(s), dt.date.fromisoformat(e)) for n, s, e in payload["subperiods"]),
        min_names=payload["min_names"], min_bucket_names=payload["min_bucket_names"],
        fm_min_obs=payload["fm_min_obs"], min_formations=payload["min_formations"],
        nyse_min_names=payload["nyse_min_names"],
        bootstrap_resamples=payload["bootstrap_resamples"], bootstrap_seed=payload["bootstrap_seed"],
        control_variant=payload["control_variant"], control_features=tuple(payload["control_features"]),
        marginal_months=tuple(payload["marginal_months"]),
        label_diagnostics=payload["label_diagnostics"], verify_panels=payload["verify_panels"],
        policy_sha256=payload["policy_sha256"],
        investable_min_price=payload.get("investable_min_price"),
        investable_nyse_percentile=payload.get("investable_nyse_percentile"),
        jt_holding_months=tuple(payload.get("jt_holding_months") or ()),
        population_coverage=bool(payload.get("population_coverage", False)),
        seal_holdout=bool(payload.get("seal_holdout", False)),
        wave_registration_id=payload.get("wave_registration_id"),
        label_sha256=payload.get("label_sha256"),
        created_at=payload.get("created_at"),
    )
    return validate_spec(spec)


# ---------------------------------------------------------------------------
# Canonical inputs (the pure engine never touches a database)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class FeatureData:
    """One feature, every variant, in canonical integer keys.

    ``values``: ``month_index``, ``security`` (int code), ``variant``, ``value`` and
    optionally ``available_at`` (checked against the formation cutoff) and
    ``unlinked_line`` (R2b ``owner_basis='unlinked_line'``: an eligible line without an
    owner link, ranked as its own name by an identity-free price-line feature). ``dates``
    (optional): ``month_index``, ``variant``, ``date_status`` and optionally
    ``coverage_fraction``; months whose status is not ``formed`` carry no values.
    """

    feature_id: str
    expected_sign: int
    values: pd.DataFrame
    dates: pd.DataFrame | None = None
    anomaly_class: str | None = None
    hypothesis_family: str | None = None
    #: CB2 catalog ``population`` (coverage denominator under ``EvaluationSpec.population_coverage``).
    population: str | None = None
    #: Catalog ``evidence_class`` and ``publication_year`` (node 4.1: EB shrinkage of replications and the
    #: pre/post publication split; reported only).
    evidence_class: str | None = None
    publication_year: int | None = None


@dataclass(frozen=True)
class CatalogFeature:
    """One pre-registered hypothesis of the expected family (an R1a catalog row)."""

    feature_id: str
    expected_sign: int  # +1, -1, or 0 (two-sided)
    anomaly_class: str
    variants: tuple[str, ...] = FEATURE_VARIANTS
    #: R1b grouping of near-duplicate / same-construct hypotheses (carried to every cell).
    hypothesis_family: str | None = None
    #: CB2 ``population`` (not part of the family digest; read only under population coverage).
    population: str | None = None
    #: CB2 ``evidence_class`` / ``publication_year`` (not part of the family digest; reported metrics only).
    evidence_class: str | None = None
    publication_year: int | None = None


def expected_variants(anomaly_class: str) -> tuple[str, ...]:
    """Variants expected of a catalog feature: all six, except ``size_neutral`` for the size
    class (a residual on its own log size is identically zero)."""
    return tuple(v for v in FEATURE_VARIANTS if not (v == "size_neutral" and anomaly_class == "size"))


def catalog_features(entries: Iterable[Any] | None = None) -> tuple[CatalogFeature, ...]:
    """The expected family from R1a catalog rows: research-eligible rows x their variants.

    ``entries`` default to the committed catalog, validated (``load_anomaly_catalog``).
    Store runs take the family from the feature version's own catalog snapshot instead
    (:func:`feature_version_catalog`), so a later catalog edit cannot change a finished
    evaluation.
    """
    from .catalog import load_anomaly_catalog

    rows = load_anomaly_catalog() if entries is None else entries
    features = []
    for entry in rows:
        if not entry.is_research_eligible:
            continue
        features.append(CatalogFeature(entry.feature_id, int(entry.expected_sign), entry.anomaly_class,
                                       expected_variants(entry.anomaly_class),
                                       getattr(entry, "hypothesis_family", None),
                                       getattr(entry, "population", None) or None,
                                       getattr(entry, "evidence_class", None) or None,
                                       _optional_year(getattr(entry, "publication_year", None))))
    return tuple(sorted(features, key=lambda item: item.feature_id))


def _optional_year(value: Any) -> int | None:
    if value is None or isinstance(value, bool):
        return None
    try:
        year = int(value)
    except (TypeError, ValueError, OverflowError):
        return None
    return year if 1800 <= year <= 2100 else None


def _catalog_map(catalog: Iterable[CatalogFeature] | None) -> dict[str, CatalogFeature] | None:
    if catalog is None:
        return None
    mapping: dict[str, CatalogFeature] = {}
    for item in catalog:
        if not isinstance(item, CatalogFeature) or not _ID.fullmatch(item.feature_id) or item.feature_id in mapping:
            raise EvaluationInputError("catalog must hold unique CatalogFeature rows")
        if item.expected_sign not in (-1, 0, 1) or not item.variants or len(set(item.variants)) != len(item.variants):
            raise EvaluationInputError(f"catalog row {item.feature_id}: bad expected_sign or variants")
        mapping[item.feature_id] = item
    return mapping


def _catalog_sha(catalog: Mapping[str, CatalogFeature] | None) -> str | None:
    if catalog is None:
        return None
    return _sha(_canonical([[f.feature_id, f.expected_sign, f.anomaly_class, list(f.variants), f.hypothesis_family]
                            for _, f in sorted(catalog.items())]))


def _resolve_catalog(store: ResearchStore, spec: EvaluationSpec, catalog: Iterable[CatalogFeature] | None
                     ) -> tuple[dict[str, CatalogFeature] | None, dict[str, Any]]:
    """The expected family of a store run and its provenance.

    An injected ``catalog`` wins (fixtures, explicit families). Otherwise the family is
    the catalog snapshot R2b stored with the feature versions (sign, class and
    hypothesis family as they were when the features were built); every version of the
    run must carry the same snapshot. The committed catalog's file digest is recorded
    so a snapshot that differs from today's catalog is flagged, never silently mixed.
    """
    from .catalog import anomaly_catalog_sha256

    committed = anomaly_catalog_sha256()
    if catalog is not None:
        return _catalog_map(catalog), {"catalog_source": "injected", "committed_catalog_sha256": committed}
    snapshots = {version: feature_version_catalog(store, version) for version in spec.feature_versions}
    digests = {(sha, _catalog_sha(_catalog_map(features))) for sha, features in snapshots.values()}
    if len(digests) != 1:
        raise EvaluationInputError("the feature versions were built from different catalog snapshots "
                                   f"({sorted(str(d) for d, _ in digests)}): one run tests one pre-registered family")
    feature_catalog_sha, features = next(iter(snapshots.values()))
    if not features:
        raise EvaluationInputError("the feature versions carry no research-eligible catalog snapshot rows")
    return _catalog_map(features), {"catalog_source": "feature_version_snapshot",
                                    "feature_catalog_sha256": feature_catalog_sha,
                                    "committed_catalog_sha256": committed}


@dataclass
class BasisInputs:
    """Canonical inputs of one basis.

    * ``calendar``: one row per calendar month, ``month_index`` 0..M-1 consecutive,
      ``month_start``, ``formation_date``, ``cutoff``, ``entry_date``, ``status``
      (``formed`` rows are evaluated) and optionally ``eligible_members``.
    * ``labels``: ``month_index``, ``security``, ``horizon_months``, ``forward_return``,
      ``status`` (0 valid, 1 invalid, 2 unsupported basis), ``terminal`` (0 none,
      1 observed, 2 policy stitch) and ``anchor_date`` (the label's as-of session).
    * ``maturity``: ``month_index``, ``horizon_months``, ``expected_end``, ``matured``.
    * ``context``: ``month_index``, ``security``, ``market_cap`` (verified size only, NaN
      otherwise) and optionally ``valid_member`` (eligible with ``cohort_reason`` valid),
      ``primary_line`` (the issuer's primary line), ``unlinked_member`` (eligible, no owner
      link), ``venue_pit`` (a point-in-time venue observation exists) and ``is_nyse_pit``
      (that venue is NYSE). Absent flags mean valid / primary / linked / no point-in-time
      venue. Valid primary rows are ranked; unlinked members only for values flagged
      ``unlinked_line``.
    * ``controls``: ``month_index``, ``security``, ``control``, ``value``.

    Node 4.1 (reported-only metrics; every input optional, its metrics NULL with a labeled basis when absent):
    ``calendar`` may carry ``nyse_me_p80`` (the capped-VW winsorization point; ``nyse_me_p20`` is the JKP
    non-microcap breakpoint); ``context`` may carry :data:`AUX_CONTEXT_COLUMNS`; ``factors`` maps a
    :data:`SPAN_MODEL_NAMES` model to its monthly factor rows (``month_index`` plus the model's columns,
    ``factor_returns.SPAN_ROW_RULE``); ``event_returns`` holds :data:`EVENT_RETURN_COLUMNS`.
    ``rebalance_returns`` holds :data:`REBALANCE_RETURN_COLUMNS`, pinned by ``rebalance_returns_sha256``;
    current month m must span entry[m-h] to entry[m]. These realized values are accounting inputs only.
    Without them turnover/cost/net stay NULL with zero drift-priced coverage. Context AR/CS half-spreads
    are formation-time estimates, not observed execution spreads. The reader must verify the source pin,
    registration and holdout seal before loading either kind of realized return.
    """

    basis: str
    status: str
    security_count: int
    calendar: pd.DataFrame
    labels: pd.DataFrame
    maturity: pd.DataFrame
    context: pd.DataFrame
    controls: pd.DataFrame
    variants_by_feature: Mapping[str, tuple[str, ...]]
    load_feature: Callable[[str], FeatureData]
    meta: Mapping[str, Any] = field(default_factory=dict)
    digests: Mapping[str, Any] = field(default_factory=dict)
    #: Drop the label/context/control frames once indexed (store-built inputs own them).
    release_frames: bool = False
    #: Node 4.1 span models (``factor_returns.benchmark_span_factors`` / ``atx_span_factors``).
    factors: Mapping[str, pd.DataFrame] = field(default_factory=dict)
    #: Node 4.1 earnings-announcement-day returns (P3 sessions); None = unavailable.
    event_returns: pd.DataFrame | None = None
    #: OSAP acronym -> original-paper t; explicit snapshot adapter, never an implicit network read.
    original_paper_t: Mapping[str, float] = field(default_factory=dict)
    #: Optional full loader inventory for economics; does not add any evaluated/trial cells.
    reported_variants_by_feature: Mapping[str, tuple[str, ...]] = field(default_factory=dict)
    #: Explicit WaveRegistration returned BEFORE reading labels. Only registered
    #: (feature, '<source_variant>_beta_neutral', h) configurations enable beta-return statistics.
    #: Default None: disabled for w1's frozen 432 cells (C-82).
    beta_neutral_registration: Any | None = None
    #: Separately pinned exact entry-to-entry realized returns for trading accounting. No label fallback.
    #: Target weights and AR/CS spread estimates remain fixed at the current formation cutoff.
    rebalance_returns: pd.DataFrame | None = None
    rebalance_returns_sha256: str | None = None


def empty_basis(basis: str, *, status: str = BASIS_UNTESTABLE, meta: Mapping[str, Any] | None = None,
                variants_by_feature: Mapping[str, tuple[str, ...]] | None = None) -> BasisInputs:
    """A basis with no data (``untestable_strict``): its cells are reported, not tested."""
    def _absent(feature_id: str) -> FeatureData:
        raise EvaluationInputError(f"basis {basis} has no feature data ({feature_id})")

    return BasisInputs(basis, status, 0, pd.DataFrame(), pd.DataFrame(), pd.DataFrame(), pd.DataFrame(),
                       pd.DataFrame(), dict(variants_by_feature or {}), _absent, dict(meta or {}))


# ---------------------------------------------------------------------------
# Vectorized cross-sectional primitives (definitions pinned to signal_eval by tests)
# ---------------------------------------------------------------------------

def _starts(counts: np.ndarray) -> np.ndarray:
    return np.cumsum(counts) - counts


def _group_order(group: np.ndarray, values: np.ndarray, counts: np.ndarray,
                 tiebreak: np.ndarray | None = None) -> np.ndarray:
    """Stable order by (group, value[, tiebreak]); a per-group argsort when groups are contiguous."""
    if len(group) > 1 and np.any(group[1:] < group[:-1]):
        return np.lexsort((values, group) if tiebreak is None else (tiebreak, values, group))
    order = np.empty(len(values), dtype=np.int64)
    starts = _starts(counts)
    for g in np.flatnonzero(counts):
        begin, end = int(starts[g]), int(starts[g] + counts[g])
        local = (np.argsort(values[begin:end], kind="stable") if tiebreak is None
                 else np.lexsort((tiebreak[begin:end], values[begin:end])))
        order[begin:end] = local + begin
    return order


def grouped_average_ranks(group: np.ndarray, values: np.ndarray, n_groups: int) -> np.ndarray:
    """1-based average ranks of ``values`` within each group (ties share the mean rank)."""
    group = np.asarray(group)
    values = np.asarray(values, dtype=float)
    size = len(values)
    ranks = np.empty(size)
    if not size:
        return ranks
    counts = np.bincount(group, minlength=n_groups)
    starts = _starts(counts)
    order = _group_order(group, values, counts)
    sorted_group = group[order]
    new_run = np.empty(size, dtype=bool)
    new_run[0] = True
    sorted_values = values[order]
    np.not_equal(sorted_values[1:], sorted_values[:-1], out=new_run[1:])
    del sorted_values
    new_run[1:] |= sorted_group[1:] != sorted_group[:-1]
    run_start = np.flatnonzero(new_run)
    del new_run
    first = run_start - starts[sorted_group[run_start]] + 1
    del sorted_group
    length = np.diff(np.append(run_start, size))
    ranks[order] = np.repeat(first + (length - 1) / 2.0, length)
    return ranks


def grouped_rank_correlation(group: np.ndarray, x: np.ndarray, y: np.ndarray,
                             n_groups: int) -> tuple[np.ndarray, np.ndarray]:
    """Per-group Spearman correlation (Pearson of average ranks) and pair counts.

    Pairs with a non-finite member are dropped. NaN below 3 pairs or when either side
    is constant: the definition of ``signal_eval._rank_corr``.
    """
    group = np.asarray(group)
    x = np.asarray(x, dtype=float)
    y = np.asarray(y, dtype=float)
    ok = np.isfinite(x) & np.isfinite(y)
    if not ok.all():
        group, x, y = group[ok], x[ok], y[ok]
    del ok
    counts = np.bincount(group, minlength=n_groups)[:n_groups]
    rho = np.full(n_groups, _NAN)
    if not len(group):
        return rho, counts
    center = (counts[group] + 1.0) / 2.0
    cx = grouped_average_ranks(group, x, n_groups)
    cx -= center
    cy = grouped_average_ranks(group, y, n_groups)
    cy -= center
    del center
    sxx = np.bincount(group, weights=cx * cx, minlength=n_groups)
    syy = np.bincount(group, weights=cy * cy, minlength=n_groups)
    cx *= cy
    del cy
    sxy = np.bincount(group, weights=cx, minlength=n_groups)
    good = (counts >= 3) & (sxx > 0) & (syy > 0)
    rho[good] = sxy[good] / np.sqrt(sxx[good] * syy[good])
    return rho, counts


@lru_cache(maxsize=8192)
def _qcut_lut(n: int, q: int) -> np.ndarray:
    """Bucket (1..q) of first-ranks 1..n: exactly ``pd.qcut(ranks, q, labels=False) + 1``."""
    labels = pd.qcut(np.arange(1, n + 1, dtype=float), q, labels=False)
    lut = (np.asarray(labels, dtype=np.int64) + 1).astype(np.int16)
    lut.setflags(write=False)
    return lut


def grouped_quantiles(group: np.ndarray, values: np.ndarray, tiebreak: np.ndarray, n_groups: int,
                      q: int, min_names: int) -> np.ndarray:
    """Quantile 1..q within each group (0 when the group has fewer than ``max(q, min_names)``).

    Ranks are ``rank(method='first')`` after ordering ties by ``tiebreak`` (security),
    then ``pd.qcut``: the ``signal_eval.compute_quantile_spread`` rule.
    """
    group = np.asarray(group, dtype=np.int64)
    counts = np.bincount(group, minlength=n_groups)
    order = _group_order(group, np.asarray(values, dtype=float), counts, np.asarray(tiebreak))
    starts = _starts(counts)
    first = np.arange(len(order)) - starts[group[order]] + 1
    labels = np.zeros(len(order), dtype=np.int16)
    for g in np.flatnonzero(counts >= max(q, min_names)):
        begin, n = int(starts[g]), int(counts[g])
        labels[begin:begin + n] = _qcut_lut(n, q)[first[begin:begin + n] - 1]
    out = np.empty(len(order), dtype=np.int16)
    out[order] = labels
    return out


def breakpoint_quantiles(group: np.ndarray, values: np.ndarray, reference: np.ndarray, n_groups: int, q: int,
                         min_reference: int) -> np.ndarray:
    """Quantile 1..q of every row by breakpoints from the ``reference`` rows of its group.

    The Fama-French NYSE rule: the breakpoints are the k/q percentiles (numpy linear
    interpolation) of the reference (NYSE) values; a value equal to a breakpoint goes to
    the lower quantile. 0 when the group has fewer than ``max(q, min_reference)``
    reference rows. ``group`` must be sorted (rows of a group contiguous).
    """
    group = np.asarray(group, dtype=np.int64)
    values = np.asarray(values, dtype=float)
    reference = np.asarray(reference, dtype=bool)
    out = np.zeros(len(values), dtype=np.int16)
    if not len(values):
        return out
    if np.any(group[1:] < group[:-1]):
        raise ValueError("breakpoint_quantiles needs rows sorted by group")
    counts = np.bincount(group, minlength=n_groups)[:n_groups]
    references = np.bincount(group[reference], minlength=n_groups)[:n_groups]
    starts = _starts(counts)
    percentiles = np.arange(1, q) * (100.0 / q)
    for g in np.flatnonzero(references >= max(q, min_reference)):
        begin, end = int(starts[g]), int(starts[g] + counts[g])
        chunk = values[begin:end]
        breaks = np.percentile(chunk[reference[begin:end]], percentiles)
        out[begin:end] = np.searchsorted(breaks, chunk, side="left") + 1
    return out


def _bucket_means(group: np.ndarray, bucket: np.ndarray, returns: np.ndarray, n_groups: int, q: int,
                  weight: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray | None, np.ndarray]:
    sel = bucket > 0
    index = group[sel].astype(np.int64) * q + (bucket[sel].astype(np.int64) - 1)
    size = n_groups * q
    count = np.bincount(index, minlength=size).reshape(n_groups, q)
    total = np.bincount(index, weights=returns[sel], minlength=size).reshape(n_groups, q)
    with np.errstate(invalid="ignore", divide="ignore"):
        equal = total / count
        value = None
        if weight is not None:
            w = weight[sel]
            ok = np.isfinite(w) & (w > 0)
            wsum = np.bincount(index[ok], weights=w[ok], minlength=size).reshape(n_groups, q)
            wret = np.bincount(index[ok], weights=w[ok] * returns[sel][ok], minlength=size).reshape(n_groups, q)
            value = wret / wsum
    return equal, value, count


def _lookup(sorted_keys: np.ndarray, query: np.ndarray) -> np.ndarray:
    if not len(sorted_keys):
        return np.full(len(query), -1, dtype=np.int64)
    position = np.searchsorted(sorted_keys, query)
    clipped = np.minimum(position, len(sorted_keys) - 1)
    return np.where(sorted_keys[clipped] == query, clipped, -1)


def _take(values: np.ndarray, index: np.ndarray, fill: Any) -> np.ndarray:
    if not len(values):
        return np.full(len(index), fill)
    return np.where(index >= 0, values[np.maximum(index, 0)], fill)


def traded_fraction(member_month: np.ndarray, member_security: np.ndarray, span: int, months: int,
                    formation_ok: np.ndarray, lag: int) -> np.ndarray:
    """Sum of absolute weight changes of an equal-weighted leg rebalanced from month m-lag to m.

    With ``a`` old names, ``b`` new names and ``k`` kept: ``k|1/b-1/a| + (a-k)/a + (b-k)/b``
    (2 x one-way turnover). NaN unless both formations are valid and non-empty.
    """
    member_month = np.asarray(member_month, dtype=np.int64)
    member_security = np.asarray(member_security, dtype=np.int64)
    counts = np.bincount(member_month, minlength=months)[:months].astype(float)
    keys = np.sort(member_month * span + member_security)
    previous = (member_month - lag) * span + member_security
    kept = (member_month >= lag) & np.isin(previous, keys)
    overlap = np.bincount(member_month[kept], minlength=months)[:months].astype(float)
    old = np.full(months, _NAN)
    valid = np.zeros(months, dtype=bool)
    if lag < months:
        old[lag:] = counts[:months - lag]
        valid[lag:] = formation_ok[lag:] & formation_ok[:months - lag]
    valid &= (old > 0) & (counts > 0)
    traded = np.full(months, _NAN)
    k, a, b = overlap[valid], old[valid], counts[valid]
    traded[valid] = k * np.abs(1.0 / b - 1.0 / a) + (a - k) / a + (b - k) / b
    return traded


# ---------------------------------------------------------------------------
# Preparation of one basis (numpy indexes, guards, samples)
# ---------------------------------------------------------------------------

def _days(values: Any) -> np.ndarray:
    series = values if isinstance(values, pd.Series) else pd.Series(list(values), dtype=object)
    return np.array(pd.to_datetime(series).to_numpy(dtype="datetime64[D]"), copy=True)


def _stamps(values: Any) -> np.ndarray:
    series = values if isinstance(values, pd.Series) else pd.Series(list(values), dtype=object)
    return np.array(pd.to_datetime(series).to_numpy(dtype="datetime64[us]"), copy=True)


def _add_months(day: dt.date, months: int) -> dt.date:
    total = day.month - 1 + months
    return dt.date(day.year + total // 12, total % 12 + 1, 1)


def _array_digest(*arrays: np.ndarray) -> str:
    digest = hashlib.sha256()
    for array in arrays:
        data = np.ascontiguousarray(array)
        digest.update(str(data.dtype).encode("ascii"))
        digest.update(data.tobytes())
    return digest.hexdigest()


@dataclass
class _Samples:
    primary: np.ndarray
    masks: dict[tuple[str, str], np.ndarray]
    notes: dict[str, tuple[int, int]]
    selection_dates: np.ndarray
    holdout_start: np.datetime64 | None


@dataclass
class _Prepared:
    basis: str
    months: int
    span: int
    month_start: np.ndarray
    formation: np.ndarray
    cutoff: np.ndarray
    formed: np.ndarray
    eligible: np.ndarray
    label_keys: dict[int, np.ndarray]
    label_return: dict[int, np.ndarray]
    label_status: dict[int, np.ndarray]
    label_terminal: dict[int, np.ndarray]
    matured: dict[int, np.ndarray]
    expected_end: dict[int, np.ndarray]
    context_keys: np.ndarray
    context_cap: np.ndarray
    context_size: np.ndarray
    context_member: np.ndarray
    context_universe: np.ndarray
    context_unlinked: np.ndarray
    context_nyse: np.ndarray
    venue_basis: list[str]
    venue_pit_share: np.ndarray
    nyse_pit_names: np.ndarray
    universe_names: np.ndarray
    unlinked_names: np.ndarray
    segment: np.ndarray
    control_keys: dict[str, np.ndarray]
    control_values: dict[str, np.ndarray]
    samples: dict[int, _Samples]
    digests: dict[str, str]
    #: Policy v4 inputs (None/empty unless the spec enables them or the calendar supplies them).
    context_investable: np.ndarray | None = None   # per context row
    investable_basis: list[str] | None = None      # per formation: nyse_breakpoints / nyse_pit / none
    population_flags: dict[str, np.ndarray] = field(default_factory=dict)  # per context row
    supplied_breakpoints: bool = False
    #: Node 4.1 inputs (reported-only metrics). Per formation: the JKP non-microcap breakpoint (NYSE p20)
    #: and the capped-VW winsorization point (NYSE p80), NaN where no basis exists, and their basis.
    jkp_p20: np.ndarray | None = None
    jkp_p80: np.ndarray | None = None
    jkp_p20_basis: list[str] = field(default_factory=list)
    jkp_p80_basis: list[str] = field(default_factory=list)
    #: Per context row (NaN / 0 where absent): half-spread (AR, else CS), its source, ADV dollars, beta.
    context_half_spread: np.ndarray | None = None
    context_spread_source: np.ndarray | None = None
    context_adv: np.ndarray | None = None
    context_beta: np.ndarray | None = None
    aux_present: frozenset[str] = frozenset()
    #: Span model -> (factor names, months x k rows incl. the risk-free column of an excess market).
    factor_models: dict[str, tuple[tuple[str, ...], np.ndarray]] = field(default_factory=dict)
    factor_windows: dict[str, str] = field(default_factory=dict)
    #: Horizon -> (sorted keys, announcement-window returns).
    event_keys: dict[int, np.ndarray] = field(default_factory=dict)
    event_values: dict[int, np.ndarray] = field(default_factory=dict)
    jkp_controls: dict[str, np.ndarray] = field(default_factory=dict)
    original_paper_t: dict[str, float] = field(default_factory=dict)
    beta_neutral_cells: frozenset[tuple[str, str, int]] = frozenset()
    rebalance_keys: dict[int, np.ndarray] = field(default_factory=dict)
    rebalance_values: dict[int, np.ndarray] = field(default_factory=dict)


def _ints(values: np.ndarray, name: str) -> np.ndarray:
    values = np.asarray(values)
    if values.dtype.kind in "iu":
        return values.astype(np.int64)
    numbers = pd.to_numeric(pd.Series(values), errors="raise").to_numpy(dtype=float)
    if len(numbers) and (not np.all(np.isfinite(numbers)) or np.any(numbers != np.round(numbers))):
        raise EvaluationInputError(f"{name} must hold integers")
    return numbers.astype(np.int64)


def _int_column(frame: pd.DataFrame, name: str) -> np.ndarray:
    return _ints(frame[name].to_numpy(), name)


def _keyed(frame: pd.DataFrame, months: int, span: int, label: str) -> tuple[np.ndarray, np.ndarray]:
    return _keys_from(_int_column(frame, "month_index"), _int_column(frame, "security"), months, span, label)


def _keys_from(month: np.ndarray, security: np.ndarray, months: int, span: int,
               label: str) -> tuple[np.ndarray, np.ndarray]:
    """Sorted unique ``month * span + security`` keys and the sorting order."""
    if len(month) and (month.min() < 0 or month.max() >= months):
        raise EvaluationInputError(f"{label}: month_index outside the calendar")
    if len(security) and (security.min() < 0 or security.max() >= span):
        raise EvaluationInputError(f"{label}: security code outside 0..security_count-1")
    keys = month * span + security
    order = np.argsort(keys, kind="stable")
    keys = keys[order]
    if len(keys) > 1 and np.any(keys[1:] == keys[:-1]):
        raise EvaluationInputError(f"{label}: duplicate (formation, security) rows")
    return keys, order


@dataclass(frozen=True)
class _SizeBuckets:
    buckets: np.ndarray          # per context row: 0 unknown (no verified cap), 1 micro, 2 small, 3 large
    venue_basis: list[str]       # per formation: VENUE_BASES
    venue_pit_share: np.ndarray  # per formation: share of universe rows with a point-in-time venue
    nyse_pit_names: np.ndarray   # per formation: universe rows on the point-in-time NYSE with a verified cap


def _size_buckets(keys: np.ndarray, cap: np.ndarray, months: int, span: int, spec: EvaluationSpec, *,
                  universe: np.ndarray | None = None, venue_pit: np.ndarray | None = None,
                  nyse_pit: np.ndarray | None = None,
                  supplied: tuple[np.ndarray, np.ndarray] | None = None) -> _SizeBuckets:
    """Micro/small/large per formation (0 = unknown: no verified cap).

    ``keys`` must be sorted. Breakpoints come only from ranked-universe rows with a
    positive (verified) cap: the 20th and 50th percentiles of those on the point-in-time
    NYSE (``venue_pit & nyse_pit``) when at least ``nyse_min_names`` exist
    (``nyse_pit``), else their cap terciles (``cap_terciles``), else none. A backcast or
    current venue is never used. ``supplied`` (policy v4: per-formation NYSE 20th/50th
    percentile breakpoints given with the calendar) wins where both are finite
    (``nyse_breakpoints``).
    """
    size = len(keys)
    universe = np.ones(size, dtype=bool) if universe is None else universe
    venue_pit = np.zeros(size, dtype=bool) if venue_pit is None else venue_pit
    nyse_pit = np.zeros(size, dtype=bool) if nyse_pit is None else nyse_pit
    month = keys // span
    counts = np.bincount(month, minlength=months)[:months]
    starts = _starts(counts)
    buckets = np.zeros(size, dtype=np.int8)
    pit_share = np.full(months, _NAN)
    nyse_names = np.zeros(months, dtype=np.int64)
    methods: list[str] = []
    for m in range(months):
        begin, end = int(starts[m]), int(starts[m] + counts[m])
        if end == begin:
            methods.append("no_context")
            continue
        caps = cap[begin:end]
        members = universe[begin:end]
        known = np.isfinite(caps) & (caps > 0)
        positive = known & members
        if members.any():
            pit_share[m] = float(venue_pit[begin:end][members].mean())
        reference = positive & venue_pit[begin:end] & nyse_pit[begin:end]
        nyse_names[m] = int(reference.sum())
        if supplied is not None and np.isfinite(supplied[0][m]) and np.isfinite(supplied[1][m]):
            method = VENUE_NYSE_BREAKPOINTS
            low, high = float(supplied[0][m]), float(supplied[1][m])
        elif nyse_names[m] >= spec.nyse_min_names:
            method = VENUE_NYSE_PIT
            low, high = np.percentile(caps[reference], [20.0, 50.0])
        elif positive.sum() >= 3:
            method = VENUE_CAP_TERCILES
            low, high = np.percentile(caps[positive], [100.0 / 3.0, 200.0 / 3.0])
        else:
            methods.append("none")
            continue
        methods.append(method)
        segment = np.where(caps < low, 1, np.where(caps < high, 2, 3)).astype(np.int8)
        buckets[begin:end] = np.where(known, segment, 0)
    return _SizeBuckets(buckets, methods, pit_share, nyse_names)


def _method_counts(methods: Sequence[str], mask: np.ndarray) -> str | None:
    """``method:count`` pairs over the masked formations, e.g. ``cap_terciles:40,nyse_pit:120``."""
    counts: dict[str, int] = {}
    for method, chosen in zip(methods, mask, strict=True):
        if chosen:
            counts[method] = counts.get(method, 0) + 1
    return ",".join(f"{name}:{count}" for name, count in sorted(counts.items())) or None


def _samples(formed: np.ndarray, formation: np.ndarray, expected_end: np.ndarray,
             spec: EvaluationSpec) -> _Samples:
    masks: dict[tuple[str, str], np.ndarray] = {}
    notes: dict[str, tuple[int, int]] = {}
    split = spec.split
    if split is None:
        primary = formed.copy()
        selection_dates = formed.copy()
        holdout_start = None
    else:
        masks[("split", "full")] = formed.copy()
        segments = split.segments
        for i, (name, start, end) in enumerate(segments):
            inside = formed & (formation >= np.datetime64(start)) & (formation <= np.datetime64(end))
            purged = np.zeros(len(formed), dtype=bool)
            embargoed = np.zeros(len(formed), dtype=bool)
            if i + 1 < len(segments):
                purged = inside & (expected_end >= np.datetime64(segments[i + 1][1]))
            if i > 0 and split.embargo_months:
                embargoed = inside & ~purged & (formation < np.datetime64(_add_months(start, split.embargo_months)))
            masks[("split", name)] = inside & ~purged & ~embargoed
            notes[name] = (int(purged.sum()), int(embargoed.sum()))
        holdout_start = np.datetime64(split.holdout_start)
        selection_dates = formed & (formation >= np.datetime64(segments[0][1])) & \
            (formation <= np.datetime64(segments[-2][2]))
        primary = selection_dates & ~(expected_end >= holdout_start)
        masks[("split", "selection")] = primary.copy()
        notes["selection"] = (int((selection_dates & ~primary).sum()), 0)
    for name, start, end in spec.subperiods:
        masks[("subperiod", name)] = primary & (formation >= np.datetime64(start)) & \
            (formation <= np.datetime64(end))
    return _Samples(primary, masks, notes, selection_dates, holdout_start)


def _prepare(inputs: BasisInputs, spec: EvaluationSpec) -> _Prepared:
    calendar = inputs.calendar.sort_values("month_index", kind="stable").reset_index(drop=True)
    months = len(calendar)
    if months == 0:
        raise EvaluationInputError(f"basis {inputs.basis}: empty formation calendar")
    if not np.array_equal(_int_column(calendar, "month_index"), np.arange(months)):
        raise EvaluationInputError("calendar month_index must be 0..M-1 without gaps")
    span = int(inputs.security_count)
    if span < 1:
        raise EvaluationInputError(f"basis {inputs.basis}: security_count must be positive")
    formed = (calendar["status"] == CALENDAR_FORMED).to_numpy(dtype=bool)
    month_start = _days(calendar["month_start"])
    formation = _days(calendar["formation_date"])
    entry = _days(calendar["entry_date"])
    cutoff = _stamps(calendar["cutoff"])
    formation[~formed] = np.datetime64("NaT", "D")
    entry[~formed] = np.datetime64("NaT", "D")
    cutoff[~formed] = np.datetime64("NaT", "us")
    if np.isnat(formation[formed]).any() or np.isnat(entry[formed]).any() or np.isnat(cutoff[formed]).any():
        raise EvaluationInputError("a formed month lacks its formation date, entry session or cutoff")
    if np.any(entry[formed] <= formation[formed]):
        raise EvaluationInputError("an entry session is not after its decision date")
    eligible = (pd.to_numeric(calendar["eligible_members"], errors="coerce").to_numpy(dtype=float)
                if "eligible_members" in calendar else np.full(months, _NAN))
    digests = {"calendar_sha256": _array_digest(month_start, formation, entry, cutoff, formed)}

    label_keys: dict[int, np.ndarray] = {}
    label_return: dict[int, np.ndarray] = {}
    label_status: dict[int, np.ndarray] = {}
    label_terminal: dict[int, np.ndarray] = {}
    matured: dict[int, np.ndarray] = {}
    expected_end: dict[int, np.ndarray] = {}
    labels, maturity = inputs.labels, inputs.maturity
    # Gather each horizon's rows from column views: never materialize every column at once.
    horizon_view = labels["horizon_months"].to_numpy() if len(labels) else np.zeros(0, np.int64)
    for h in spec.horizons_months:
        chosen = np.flatnonzero(horizon_view == h)
        if len(chosen):
            keys, order = _keys_from(_ints(labels["month_index"].to_numpy()[chosen], "month_index"),
                                     _ints(labels["security"].to_numpy()[chosen], "security"),
                                     months, span, f"labels h={h}")
            chosen = chosen[order]
            month = keys // span
            if not formed[month].all():
                raise EvaluationInputError(f"labels h={h} exist at a month that is not a formed formation")
            anchor = _days(labels["anchor_date"].iloc[chosen])
            wrong = anchor != entry[month]
            if wrong.any():
                early = int((anchor[wrong] <= formation[month[wrong]]).sum())
                example = int(np.flatnonzero(wrong)[0])
                raise LookaheadError(
                    f"{int(wrong.sum())} labels at horizon {h}m are not anchored at their formation's entry "
                    f"session ({early} on/before the decision date: look-ahead); e.g. month {int(month[example])} "
                    f"anchor {anchor[example]} vs entry {entry[month[example]]}")
            status = _ints(labels["status"].to_numpy()[chosen], "status").astype(np.int8)
            if not np.isin(status, (LABEL_VALID, LABEL_INVALID, LABEL_UNSUPPORTED)).all():
                raise EvaluationInputError("label status codes must be 0/1/2")
            terminal = (_ints(labels["terminal"].to_numpy()[chosen], "terminal").astype(np.int8)
                        if "terminal" in labels else np.zeros(len(chosen), np.int8))
            returns = pd.to_numeric(labels["forward_return"].iloc[chosen], errors="coerce").to_numpy(dtype=float)
        else:
            keys = np.zeros(0, np.int64)
            status = terminal = np.zeros(0, np.int8)
            returns = np.zeros(0)
        label_keys[h], label_return[h], label_status[h], label_terminal[h] = keys, returns, status, terminal
        mature = np.zeros(months, dtype=bool)
        ends = np.full(months, np.datetime64("NaT", "D"), dtype="datetime64[D]")
        rows = maturity[maturity["horizon_months"] == h] if len(maturity) else maturity
        if len(rows):
            index = _int_column(rows, "month_index")
            if len(index) != len(set(index.tolist())) or index.min() < 0 or index.max() >= months:
                raise EvaluationInputError(f"maturity h={h}: bad or duplicate month_index")
            mature[index] = rows["matured"].to_numpy(dtype=bool)
            ends[index] = _days(rows["expected_end"])
        mature &= formed
        if spec.seal_holdout and spec.split is not None:
            # Policy v4 seal: a label whose window ends on/after the holdout start (or whose end is
            # unknown) is dropped before any statistic; it counts as not matured.
            sealed = formed & ~(ends < np.datetime64(spec.split.holdout_start))
            if len(keys):
                keep = ~sealed[keys // span]
                keys, returns, status, terminal = keys[keep], returns[keep], status[keep], terminal[keep]
                label_keys[h], label_return[h], label_status[h], label_terminal[h] = keys, returns, status, terminal
            mature &= ~sealed
            digests[f"labels_{h}m_sealed_formations"] = str(int(sealed.sum()))
        matured[h], expected_end[h] = mature, ends
        digests[f"labels_{h}m_sha256"] = _array_digest(keys, returns, status, terminal, mature, ends)

    del horizon_view, labels
    if inputs.release_frames:
        inputs.labels = pd.DataFrame()
    context = inputs.context
    if len(context):
        ctx_keys, order = _keyed(context, months, span, "context")
        cap = pd.to_numeric(context["market_cap"], errors="coerce").to_numpy(dtype=float)[order]

        def flag(name: str, default: bool) -> np.ndarray:
            if name not in context:
                return np.full(len(context), default, dtype=bool)
            return context[name].fillna(False).to_numpy(dtype=bool)[order]

        member, primary = flag("valid_member", True), flag("primary_line", True)
        unlinked = flag("unlinked_member", False)
        venue_pit, nyse_pit = flag("venue_pit", False), flag("is_nyse_pit", False)
        price = (pd.to_numeric(context["price"], errors="coerce").to_numpy(dtype=float)[order]
                 if "price" in context else np.full(len(context), _NAN))
        population_flags = {str(name)[len(POPULATION_FLAG_PREFIX):]: flag(str(name), False)
                            for name in context.columns
                            if spec.population_coverage and str(name).startswith(POPULATION_FLAG_PREFIX)}
    else:
        ctx_keys, cap, price = np.zeros(0, np.int64), np.zeros(0), np.zeros(0)
        member = primary = unlinked = venue_pit = nyse_pit = np.zeros(0, bool)
        population_flags = {}
    universe = member & primary
    unlinked &= ~universe
    supplied = _supplied_breakpoints(calendar, (20, 50))
    sized = _size_buckets(ctx_keys, cap, months, span, spec, universe=universe, venue_pit=venue_pit,
                          nyse_pit=nyse_pit, supplied=supplied)
    universe_names = np.bincount(ctx_keys[universe] // span, minlength=months)[:months] if len(ctx_keys) \
        else np.zeros(months, np.int64)
    unlinked_names = np.bincount(ctx_keys[unlinked] // span, minlength=months)[:months] if len(ctx_keys) \
        else np.zeros(months, np.int64)
    digests["context_sha256"] = _array_digest(ctx_keys, cap, member, primary, unlinked, venue_pit, nyse_pit)
    if supplied is not None:
        digests["supplied_breakpoints_sha256"] = _array_digest(*supplied)
    investable = investable_basis = None
    if spec.investable_min_price is not None:
        investable, investable_basis = _investable(
            ctx_keys, cap, price, months, span, spec, universe=universe, venue_pit=venue_pit, nyse_pit=nyse_pit,
            supplied=_supplied_breakpoints(calendar, (int(spec.investable_nyse_percentile or 0),)))
        digests["investable_sha256"] = _array_digest(price, investable)
    for name, flags in sorted(population_flags.items()):
        digests[f"population_{name}_sha256"] = _array_digest(flags)
    segment = np.full(months, "full" if spec.split is None else "outside", dtype=object)
    if spec.split is not None:
        for name, start, end in spec.split.segments:
            segment[(formation >= np.datetime64(start)) & (formation <= np.datetime64(end))] = name

    control_keys: dict[str, np.ndarray] = {}
    control_values: dict[str, np.ndarray] = {}
    controls = inputs.controls
    for name in spec.control_features:
        part = controls[controls["control"] == name] if len(controls) else controls
        if not len(part):
            continue
        keys, order = _keyed(part, months, span, f"control {name}")
        values = pd.to_numeric(part["value"], errors="coerce").to_numpy(dtype=float)[order]
        control_keys[name], control_values[name] = keys, values
        digests[f"control_{name}_sha256"] = _array_digest(keys, values)

    samples = {h: _samples(formed, formation, expected_end[h], spec) for h in spec.horizons_months}
    prepared = _Prepared(inputs.basis, months, span, month_start, formation, cutoff, formed, eligible,
                     label_keys, label_return, label_status, label_terminal, matured, expected_end,
                     ctx_keys, cap, sized.buckets, member, universe, unlinked, venue_pit & nyse_pit,
                     sized.venue_basis, sized.venue_pit_share, sized.nyse_pit_names, universe_names, unlinked_names,
                     segment, control_keys, control_values, samples, digests,
                     context_investable=investable, investable_basis=investable_basis,
                     population_flags=population_flags, supplied_breakpoints=supplied is not None)
    context = pd.DataFrame()  # release the frame captured by flag(), which is no longer called
    del controls
    _prepare_reported(prepared, inputs, calendar, spec)
    if inputs.release_frames:
        inputs.labels = inputs.context = inputs.controls = pd.DataFrame()
        inputs.event_returns = None
        inputs.rebalance_returns = None
    return prepared


def _prepare_reported(prep: _Prepared, inputs: BasisInputs, calendar: pd.DataFrame,
                      spec: EvaluationSpec) -> None:
    """Index optional economics separately: these inputs never change the legacy/gating arrays."""
    from .factor_returns import SPAN_MODELS, WINDOW_EXACT

    registration = inputs.beta_neutral_registration
    if registration is not None:
        from .trial_registry import WaveRegistration

        if (not isinstance(registration, WaveRegistration)
                or registration.registration_id != spec.wave_registration_id
                or spec.created_at is None or spec.created_at < registration.registered_at):
            raise EvaluationInputError("beta-neutral returns require a matching preregistered wave/spec before labels")
        prep.beta_neutral_cells = frozenset(registration.cells)
        prep.digests["beta_neutral_registration_sha256"] = registration.registration_id
    context = inputs.context
    order = _keyed(context, prep.months, prep.span, "context")[1] if len(context) else np.zeros(0, int)
    aux = {}
    candidates = {CONTEXT_SPREAD_AR: ("bidask_ar_21d",), CONTEXT_SPREAD_CS: ("bidask_cs_21d",),
                  CONTEXT_ADV: ("dolvol_126d",), CONTEXT_BETA: ("beta_ew_252d",),
                  "log_bm": ("book_to_market", "be_me"), "ret_12_1": ("ret_12_1", "momentum_12_1"),
                  "gp_at": ("gp_at", "gross_profitability"), "at_gr1": ("at_gr1", "asset_growth")}
    for role, names in candidates.items():
        values = np.full(len(prep.context_keys), _NAN)
        if role in context:
            values = pd.to_numeric(context[role], errors="coerce").to_numpy(float)[order]
        else:
            for name in names:
                inventory = inputs.reported_variants_by_feature or inputs.variants_by_feature
                if "signed_raw" not in inventory.get(name, ()):
                    continue
                feature = inputs.load_feature(name)
                arrays = _variant_arrays(prep, feature, "signed_raw")
                keys = arrays.month * prep.span + arrays.security
                raw = arrays.value / (feature.expected_sign or 1)
                if role == "log_bm":
                    raw = np.log(np.where(raw > 0, raw, np.nan))
                values = _take(raw, _lookup(keys, prep.context_keys), _NAN)
                del feature, arrays, keys, raw
                break
        aux[role] = values
        prep.digests[f"reported_{role}_sha256"] = _array_digest(values)
    ar, cs = aux[CONTEXT_SPREAD_AR], aux[CONTEXT_SPREAD_CS]
    ar_ok, cs_ok = np.isfinite(ar) & (ar >= 0), np.isfinite(cs) & (cs >= 0)
    prep.context_half_spread = np.where(ar_ok, ar / 2, np.where(cs_ok, cs / 2, np.nan))
    prep.context_spread_source = np.where(ar_ok, HALF_SPREAD_AR, np.where(cs_ok, HALF_SPREAD_CS, HALF_SPREAD_NONE))
    prep.context_adv, prep.context_beta = aux[CONTEXT_ADV], aux[CONTEXT_BETA]
    prep.aux_present = frozenset(name for name, values in aux.items() if np.isfinite(values).any())
    prep.jkp_controls = {name: aux[name] for name in ("log_bm", "ret_12_1", "gp_at", "at_gr1")}
    del context
    if inputs.release_frames:
        inputs.context = pd.DataFrame()
    group = prep.context_keys // prep.span
    for percentile in (20, 80):
        supplied = _supplied_breakpoints(calendar, (percentile,))
        values = np.full(prep.months, _NAN) if supplied is None else supplied[0].copy()
        basis = ["nyse_breakpoints" if np.isfinite(v) and v > 0 else "unavailable" for v in values]
        eligible = prep.context_universe & prep.context_nyse & np.isfinite(prep.context_cap) & (prep.context_cap > 0)
        for m in range(prep.months):
            if basis[m] != "unavailable":
                continue
            cap = prep.context_cap[eligible & (group == m)]
            values[m] = np.percentile(cap, percentile) if len(cap) >= spec.nyse_min_names else _NAN
            if np.isfinite(values[m]):
                basis[m] = "nyse_pit"
        setattr(prep, f"jkp_p{percentile}", values)
        setattr(prep, f"jkp_p{percentile}_basis", basis)
        prep.digests[f"jkp_p{percentile}_sha256"] = _array_digest(values)
    for model, frame in sorted(inputs.factors.items()):
        if model not in SPAN_MODELS or not set(SPAN_MODELS[model]) <= set(frame):
            raise EvaluationInputError(f"invalid reported factor model {model}")
        index = _int_column(frame, "month_index")
        if len(set(index)) != len(index) or (index < 0).any() or (index >= prep.months).any():
            raise EvaluationInputError(f"factor model {model}: bad/duplicate month_index")
        names = tuple(str(c) for c in frame if c != "month_index")
        values = np.full((prep.months, len(names)), _NAN)
        values[index] = frame[list(names)].apply(pd.to_numeric, errors="coerce").to_numpy(float)
        prep.factor_models[model] = names, values
        prep.factor_windows[model] = str(frame.attrs.get("monthly_window_basis", WINDOW_EXACT))
        prep.digests[f"span_{model}_sha256"] = _sha(_array_digest(values) + _canonical(names)
                                                   + prep.factor_windows[model])
    if inputs.event_returns is not None:
        events = inputs.event_returns
        if not set(EVENT_RETURN_COLUMNS) <= set(events):
            raise EvaluationInputError("event_returns lacks its documented columns")
        for h in spec.horizons_months:
            part = events[events.horizon_months == h]
            keys, ordering = _keyed(part, prep.months, prep.span, "event_returns")
            values = pd.to_numeric(part.ea_return, errors="coerce").to_numpy(float)[ordering]
            keep = prep.matured[h][keys // prep.span]
            prep.event_keys[h], prep.event_values[h] = keys[keep], values[keep]
            prep.digests[f"events_{h}_sha256"] = _array_digest(keys[keep], values[keep])
    prep.original_paper_t = {str(k): float(v) for k, v in inputs.original_paper_t.items() if np.isfinite(v)}
    prep.digests["original_paper_t_sha256"] = _sha(_canonical(prep.original_paper_t))
    if inputs.rebalance_returns is not None:
        frame = inputs.rebalance_returns
        pin = inputs.rebalance_returns_sha256
        if not isinstance(pin, str) or not re.fullmatch(r"[a-f0-9]{64}", pin):
            raise EvaluationInputError("rebalance_returns requires its independently verified source SHA256")
        if not set(REBALANCE_RETURN_COLUMNS) <= set(frame):
            raise EvaluationInputError("rebalance_returns lacks its documented columns")
        entry = _days(calendar["entry_date"])
        prep.digests["rebalance_source_sha256"] = pin
        for h in spec.horizons_months:
            part = frame[frame.horizon_months == h]
            keys, ordering = _keyed(part, prep.months, prep.span, "rebalance_returns")
            current = keys // prep.span
            start, end = _days(part.previous_entry_date)[ordering], _days(part.entry_date)[ordering]
            if (current < h).any() or (start != entry[current - h]).any() or (end != entry[current]).any():
                raise EvaluationInputError("rebalance returns must span exactly old entry to current entry")
            if (~prep.formed[current] | ~prep.formed[current - h]).any() or (start >= end).any():
                raise EvaluationInputError("rebalance returns require two ordered formed entries")
            keep = np.ones(len(keys), bool)
            if spec.seal_holdout and spec.split is not None:
                keep &= end < np.datetime64(spec.split.holdout_start)
            # Drop sealed rows before numeric extraction/digest; the reader must also enforce the seal.
            selected = ordering[keep]
            values = pd.to_numeric(part.realized_return.iloc[selected], errors="coerce").to_numpy(float)
            status = _ints(part.status.to_numpy()[selected], "rebalance status")
            if not np.isin(status, (LABEL_VALID, LABEL_INVALID, LABEL_UNSUPPORTED)).all():
                raise EvaluationInputError("rebalance return status codes must be 0/1/2")
            values = np.where((status == LABEL_VALID) & np.isfinite(values) & (values >= -1), values, np.nan)
            prep.rebalance_keys[h], prep.rebalance_values[h] = keys[keep], values
            prep.digests[f"rebalance_{h}_sha256"] = _array_digest(keys[keep], start[keep], end[keep], values)


def _supplied_breakpoints(calendar: pd.DataFrame, percentiles: Sequence[int]) -> tuple[np.ndarray, ...] | None:
    """Per-formation supplied NYSE market-cap breakpoints (calendar ``nyse_me_p<k>``), or None if absent."""
    columns = [SUPPLIED_BREAKPOINT_COLUMN.format(percentile=k) for k in percentiles]
    if not columns or any(column not in calendar for column in columns):
        return None
    arrays = tuple(pd.to_numeric(calendar[column], errors="coerce").to_numpy(dtype=float) for column in columns)
    if any(np.any(np.isfinite(a) & (a <= 0)) for a in arrays):
        raise EvaluationInputError("supplied NYSE breakpoints must be positive market caps")
    return arrays


def _investable(keys: np.ndarray, cap: np.ndarray, price: np.ndarray, months: int, span: int,
                spec: EvaluationSpec, *, universe: np.ndarray, venue_pit: np.ndarray, nyse_pit: np.ndarray,
                supplied: tuple[np.ndarray, ...] | None) -> tuple[np.ndarray, list[str]]:
    """Policy v4 investable flag per context row, and the breakpoint basis per formation.

    Investable = a positive (verified) cap at or above the NYSE ``investable_nyse_percentile``
    percentile and a price at or above ``investable_min_price``. The breakpoint is the supplied
    one (``nyse_breakpoints``) where finite, else the percentile of the ranked universe's
    point-in-time NYSE names with a verified cap when at least ``nyse_min_names`` exist
    (``nyse_pit``), else the formation has no investable names (``none``). ``keys`` sorted.
    """
    month = keys // span
    counts = np.bincount(month, minlength=months)[:months]
    starts = _starts(counts)
    flags = np.zeros(len(keys), dtype=bool)
    methods: list[str] = []
    percentile = float(spec.investable_nyse_percentile or 0)
    minimum = float(spec.investable_min_price or 0.0)
    for m in range(months):
        begin, end = int(starts[m]), int(starts[m] + counts[m])
        if end == begin:
            methods.append("no_context")
            continue
        caps = cap[begin:end]
        known = np.isfinite(caps) & (caps > 0)
        if supplied is not None and np.isfinite(supplied[0][m]):
            method, breakpoint = VENUE_NYSE_BREAKPOINTS, float(supplied[0][m])
        else:
            reference = known & universe[begin:end] & venue_pit[begin:end] & nyse_pit[begin:end]
            if int(reference.sum()) < spec.nyse_min_names:
                methods.append("none")
                continue
            method, breakpoint = VENUE_NYSE_PIT, float(np.percentile(caps[reference], percentile))
        methods.append(method)
        prices = price[begin:end]
        flags[begin:end] = known & (caps >= breakpoint) & np.isfinite(prices) & (prices >= minimum)
    return flags, methods


# ---------------------------------------------------------------------------
# Statistics per series
# ---------------------------------------------------------------------------

def _infer(series: np.ndarray, mask: np.ndarray, h: int) -> stats.MeanInference:
    return stats.mean_inference(np.where(mask, series, _NAN), horizon_periods=h)


def _num(value: Any) -> float | None:
    if value is None:
        return None
    number = float(value)
    return number if math.isfinite(number) else None


def _short(prefix: str, inference: stats.MeanInference) -> dict[str, Any]:
    return {f"{prefix}_mean": _num(inference.mean), f"{prefix}_robust_t": _num(inference.robust_t),
            f"{prefix}_robust_p": _num(inference.robust_p_value), f"{prefix}_z": _num(inference.z_equivalent)}


def _bootstrap(series: np.ndarray, mask: np.ndarray, h: int, spec: EvaluationSpec) -> stats.BootstrapInterval | None:
    values = series[mask & np.isfinite(series)]
    if len(values) < 2 or spec.bootstrap_resamples < 1:
        return None
    return stats.circular_block_bootstrap_ci(values, horizon_periods=h, n_resamples=spec.bootstrap_resamples,
                                             seed=spec.bootstrap_seed)


def _spearman_vector(x: np.ndarray, y: np.ndarray) -> float | None:
    if len(x) < 3 or not np.all(np.isfinite(y)):
        return None
    rho, _ = grouped_rank_correlation(np.zeros(len(x), np.int64), x, y, 1)
    return _num(rho[0])


def _nanmean(values: np.ndarray) -> float | None:
    finite = values[np.isfinite(values)]
    return float(finite.mean()) if len(finite) else None


# ---------------------------------------------------------------------------
# One feature variant
# ---------------------------------------------------------------------------

@dataclass
class _Arrays:
    month: np.ndarray
    security: np.ndarray
    value: np.ndarray
    thin: np.ndarray
    coverage: np.ndarray
    rows: int
    dropped_not_valid: np.ndarray
    dropped_not_primary: np.ndarray
    #: per formation: ranked values on unlinked lines (their own names)
    unlinked_values: np.ndarray


def _has_unlinked_lines(feature: FeatureData) -> bool:
    """True for an identity-free price-line feature that ranks unlinked lines as their own names."""
    values = feature.values
    return "unlinked_line" in values and bool(np.asarray(values["unlinked_line"].fillna(False), dtype=bool).any())


def _variant_arrays(prep: _Prepared, feature: FeatureData, variant: str) -> _Arrays:
    frame = feature.values
    part = frame[frame["variant"] == variant] if len(frame) else frame
    months, span = prep.months, prep.span
    thin = np.zeros(months, dtype=bool)
    coverage = np.full(months, _NAN)
    if feature.dates is not None and len(feature.dates):
        dates = feature.dates[feature.dates["variant"] == variant]
        if len(dates):
            index = _int_column(dates, "month_index")
            if index.min() < 0 or index.max() >= months:
                raise EvaluationInputError(f"{feature.feature_id}/{variant}: date rows outside the calendar")
            thin[index] = (dates["date_status"] != "formed").to_numpy(dtype=bool)
            if "coverage_fraction" in dates:
                coverage[index] = pd.to_numeric(dates["coverage_fraction"], errors="coerce").to_numpy(dtype=float)
    if not len(part):
        empty = np.zeros(0, np.int64)
        return _Arrays(empty, empty, np.zeros(0), thin, coverage, 0, np.zeros(months, np.int64),
                       np.zeros(months, np.int64), np.zeros(months, np.int64))
    keys, order = _keyed(part, months, span, f"{feature.feature_id}/{variant}")
    month = keys // span
    security = keys % span
    if not prep.formed[month].all():
        raise EvaluationInputError(f"{feature.feature_id}/{variant}: values at a month that is not a formed formation")
    value = pd.to_numeric(part["value"], errors="coerce").to_numpy(dtype=float)[order]
    if np.isinf(value).any():
        raise EvaluationInputError(f"{feature.feature_id}/{variant}: infinite feature values")
    if "available_at" in part:
        available = _stamps(part["available_at"])[order]
        unknown = np.isnat(available) & np.isfinite(value)
        if unknown.any():
            raise LookaheadError(f"{feature.feature_id}/{variant}: {int(unknown.sum())} values without a clock")
        late = available > prep.cutoff[month]
        if late.any():
            raise LookaheadError(f"{feature.feature_id}/{variant}: {int(late.sum())} values available after "
                                 f"their formation cutoff")
    keep = np.isfinite(value) & ~thin[month]
    # The ranked universe: valid cohort rows on the issuer's primary line (one line per issuer),
    # plus, for values R2b flags as unlinked lines, eligible lines without an owner link.
    context_index = _lookup(prep.context_keys, keys)
    valid = _take(prep.context_member, context_index, False).astype(bool)
    universe = _take(prep.context_universe, context_index, False).astype(bool)
    flagged = (part["unlinked_line"].fillna(False).to_numpy(dtype=bool)[order] if "unlinked_line" in part
               else np.zeros(len(keys), dtype=bool))
    linked_ok = universe & ~flagged
    unlinked_ok = flagged & _take(prep.context_unlinked, context_index, False).astype(bool)
    not_primary = keep & ~flagged & valid & ~universe
    not_valid = keep & ~linked_ok & ~unlinked_ok & ~not_primary
    dropped_not_valid = np.bincount(month[not_valid], minlength=months)[:months]
    dropped_not_primary = np.bincount(month[not_primary], minlength=months)[:months]
    keep &= linked_ok | unlinked_ok
    unlinked_values = np.bincount(month[keep & unlinked_ok], minlength=months)[:months]
    return _Arrays(month[keep], security[keep], value[keep], thin, coverage, len(part), dropped_not_valid,
                   dropped_not_primary, unlinked_values)


def _cell_identity(prep: _Prepared, feature: FeatureData, variant: str, h: int, meta: Mapping[str, Any],
                   sample: str) -> dict[str, Any]:
    return {
        "basis": prep.basis, "feature_id": feature.feature_id, "variant": variant, "horizon_months": h,
        "horizon_sessions": HORIZON_SESSIONS[h], "sample": sample,
        "anomaly_class": feature.anomaly_class, "hypothesis_family": feature.hypothesis_family,
        "expected_sign": int(feature.expected_sign),
        "identity_basis": meta.get("identity_basis"), "universe_basis": meta.get("universe_basis"),
        "classification_basis": meta.get("classification_basis"),
        "availability_basis": meta.get("availability_basis"), "label_basis": LABEL_BASIS,
        "universe_rule": UNIVERSE_RULE, "fm_standardized": variant in STANDARDIZED_VARIANTS,
    }


FM_ESTIMATED, FM_INSUFFICIENT, FM_RANK_DEFICIENT = 0, 1, 2
_FM_RANK_TOLERANCE = 1e-12


def cross_sectional_ols(group: np.ndarray, y: np.ndarray, regressors: Sequence[np.ndarray], n_groups: int,
                        *, min_obs: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Per-group OLS of ``y`` on ``[1, regressors]``: slopes (n_groups x k), status, row counts.

    Vectorized form of the per-date regressions of :func:`stats.fama_macbeth` (pinned
    to it by tests): rows with a non-finite value are dropped per group, a group needs
    ``max(min_obs, k + 3)`` rows (``k`` regressors plus the intercept, plus two) and a
    full-rank design. Slopes solve the group-centered normal equations (centering
    removes the intercept column and its conditioning cost); a group whose centered
    Gram matrix has ``min eigenvalue <= 1e-12 x max eigenvalue`` is rank deficient.
    """
    group = np.asarray(group, dtype=np.int64)
    y = np.asarray(y, dtype=float)
    k = len(regressors)
    design = np.column_stack([np.asarray(column, dtype=float) for column in regressors]) if k else \
        np.zeros((len(y), 0))
    ok = np.isfinite(y) & np.isfinite(design).all(axis=1)
    if not ok.all():
        group, y, design = group[ok], y[ok], design[ok]
    counts = np.bincount(group, minlength=n_groups)[:n_groups]
    slopes = np.full((n_groups, k), _NAN)
    status = np.full(n_groups, FM_INSUFFICIENT, dtype=np.int8)
    enough = counts >= max(int(min_obs), k + 3)
    if not enough.any() or not k:
        return slopes, status, counts
    with np.errstate(invalid="ignore", divide="ignore"):
        denominator = np.maximum(counts, 1).astype(float)
        design -= (np.column_stack([np.bincount(group, weights=design[:, i], minlength=n_groups)
                                    for i in range(k)]) / denominator[:, None])[group]
        y = y - (np.bincount(group, weights=y, minlength=n_groups) / denominator)[group]
    gram = np.empty((n_groups, k, k))
    moment = np.empty((n_groups, k))
    for i in range(k):
        moment[:, i] = np.bincount(group, weights=design[:, i] * y, minlength=n_groups)
        for j in range(i, k):
            gram[:, i, j] = gram[:, j, i] = np.bincount(group, weights=design[:, i] * design[:, j],
                                                        minlength=n_groups)
    chosen = np.flatnonzero(enough)
    eigen = np.linalg.eigvalsh(gram[chosen])
    full = eigen[:, 0] > _FM_RANK_TOLERANCE * np.maximum(eigen[:, -1], 0.0)
    status[chosen[~full]] = FM_RANK_DEFICIENT
    solved = chosen[full]
    if len(solved):
        slopes[solved] = np.linalg.solve(gram[solved], moment[solved][:, :, None])[:, :, 0]
        status[solved] = FM_ESTIMATED
    return slopes, status, counts


def _fama_macbeth(month: np.ndarray, y: np.ndarray, columns: Mapping[str, np.ndarray], months: int,
                  spec: EvaluationSpec) -> np.ndarray:
    """Per-formation slope of the first column (``x``); NaN where not estimated."""
    slopes, _, _ = cross_sectional_ols(month, y, list(columns.values()), months, min_obs=spec.fm_min_obs)
    return slopes[:, 0] if slopes.shape[1] else np.full(months, _NAN)


def _reported_inference(prefix: str, values: np.ndarray, mask: np.ndarray, h: int) -> dict[str, Any]:
    inf = _infer(values, mask, h)
    return {f"{prefix}_mean": _num(inf.mean), f"{prefix}_nw_t": _num(inf.nw_t),
            f"{prefix}_n": inf.n_obs}


def _weighted_fm(month: np.ndarray, y: np.ndarray, regressors: Sequence[np.ndarray], weight: np.ndarray,
                 months: int, min_obs: int) -> np.ndarray:
    """WLS per formation with positive ME weights; standardized design avoids size-unit conditioning."""
    out = np.full(months, _NAN)
    counts = np.bincount(month, minlength=months)
    starts = _starts(counts)
    for m in np.flatnonzero(counts >= min_obs):
        rows = slice(starts[m], starts[m] + counts[m])
        design = np.column_stack([column[rows] for column in regressors])
        w, response = weight[rows], y[rows]
        ok = np.isfinite(w) & (w > 0) & np.isfinite(response) & np.isfinite(design).all(axis=1)
        if ok.sum() < max(min_obs, design.shape[1] + 3):
            continue
        design, response, w = design[ok], response[ok], w[ok]
        w = w / w.sum()
        centered = design - np.sum(w[:, None] * design, axis=0)
        scales = np.sqrt(np.sum(w[:, None] * centered ** 2, axis=0))
        if (scales <= 1e-12).any():
            continue
        root = np.sqrt(w)
        fit, _, rank, _ = np.linalg.lstsq(centered / scales * root[:, None],
                                          (response - np.dot(w, response)) * root, rcond=1e-12)
        if rank == design.shape[1]:
            out[m] = fit[0] / scales[0]
    return out


def _hold_spread_legs(month: np.ndarray, security: np.ndarray, quantile: np.ndarray, months: int, h: int = 1
                      ) -> np.ndarray:
    """Independent h-month sleeves; a missing name exits at that sleeve's rebalance.

    Long enters 10/stays >=8, short enters 1/stays <=3. Offset m%h inherits only the state from m-h.
    """
    if h < 1:
        raise ValueError("sleeve horizon must be positive")
    out = np.zeros(len(month), np.int8)
    counts = np.bincount(month, minlength=months)
    starts = _starts(counts)
    prior_by_offset: list[dict[int, int]] = [{} for _ in range(h)]
    for m in range(months):
        prior = prior_by_offset[m % h]
        now = {}
        for i in range(int(starts[m]), int(starts[m] + counts[m])):
            q, s = int(quantile[i]), int(security[i])
            side = 1 if q == 10 or (prior.get(s) == 1 and q >= 8) else \
                -1 if q == 1 or (prior.get(s) == -1 and 0 < q <= 3) else 0
            out[i] = side
            if side:
                now[s] = side
        prior_by_offset[m % h] = now
    return out


def _portfolio_path(prep: _Prepared, month: np.ndarray, security: np.ndarray, legs: np.ndarray,
                     weight: np.ndarray, h: int, use: np.ndarray, returns: np.ndarray) -> dict[str, np.ndarray]:
    """Fixed formation weights, exact entry-to-entry drift and estimated AR/CS trading costs.

    Each leg has unit notional. Turnover is half the L1 weight change; execution cost uses the full L1
    change times each stock's half-spread, including exits. No initial build is counted. A missing prior
    accounting return or formation spread makes that formation unavailable, rather than zero. Each h-month
    formation is compared with m-h (a staggered h-month sleeve), avoiding double-counted overlapping P&L.
    Gross/EA require every held name's current label; no future-availability renormalization. A fixed-session
    label is never substituted for the separately pinned realized entry-to-entry accounting return.
    """
    result = {name: np.full(prep.months, _NAN) for name in ("gross", "turnover", "cost", "net", "ea")}
    result.update({name: np.zeros(prep.months, np.int64) for name in
                   ("held", "priced", "drift_held", "drift_priced")})
    counts = np.bincount(month, minlength=prep.months)
    starts = _starts(counts)
    holdings: list[dict[int, dict[int, float]]] = []
    event = _take(prep.event_values.get(h, np.zeros(0)),
                  _lookup(prep.event_keys.get(h, np.zeros(0, np.int64)), month * prep.span + security), _NAN)
    for m in range(prep.months):
        rows = np.arange(starts[m], starts[m] + counts[m], dtype=int)
        sides, gross, ea = {}, [], []
        for side in (1, -1):
            selected = rows[(legs[rows] == side) & np.isfinite(weight[rows]) & (weight[rows] > 0)]
            total_weight = float(weight[selected].sum())
            sides[side] = ({int(security[i]): float(weight[i] / total_weight) for i in selected}
                           if len(selected) >= JKP_MIN_LEG_NAMES else {})
            held = selected if sides[side] else selected[:0]
            valid = held[use[held] & np.isfinite(returns[held])]
            result["held"][m] += len(held)
            result["priced"][m] += len(valid)
            complete = len(held) >= JKP_MIN_LEG_NAMES and len(valid) == len(held)
            gross.append(float(np.average(returns[held], weights=weight[held])) if complete else _NAN)
            ea.append(float(np.average(event[held], weights=weight[held]))
                      if complete and np.isfinite(event[held]).all() else _NAN)
        holdings.append(sides)
        result["gross"][m], result["ea"][m] = gross[0] - gross[1], ea[0] - ea[1]
        if m < h or not all(sides.values()) or not all(holdings[m - h].values()):
            continue
        result["drift_held"][m] = sum(len(leg) for leg in holdings[m - h].values())
        turn, cost, complete_drift, complete_cost = 0.0, 0.0, True, True
        for side in (1, -1):
            old = holdings[m - h][side]
            ids = np.array(sorted(old), dtype=np.int64)
            index = _lookup(prep.rebalance_keys.get(h, np.zeros(0, np.int64)), m * prep.span + ids)
            old_return = _take(prep.rebalance_values.get(h, np.zeros(0)), index, _NAN)
            result["drift_priced"][m] += int(np.isfinite(old_return).sum())
            if not np.isfinite(old_return).all():
                complete_drift = False
                continue
            drift = np.array([old[int(s)] for s in ids]) * (1 + old_return)
            if drift.sum() <= 0:
                complete_drift = False
                continue
            before = dict(zip(ids.tolist(), (drift / drift.sum()).tolist(), strict=True))
            union = np.array(sorted(set(before) | set(sides[side])), np.int64)
            trades = np.array([abs(sides[side].get(int(s), 0.0) - before.get(int(s), 0.0)) for s in union])
            active = trades > 1e-12
            spreads = _take(prep.context_half_spread, _lookup(prep.context_keys, m * prep.span + union), _NAN)
            turn += float(trades.sum()) / 2
            if not np.isfinite(spreads[active]).all():
                complete_cost = False
            else:
                cost += float(np.dot(trades[active], spreads[active]))
        if complete_drift:
            result["turnover"][m] = turn
            if complete_cost:
                result["cost"][m] = cost
                result["net"][m] = result["gross"][m] - cost
    return result


def _reported_metrics(prep: _Prepared, spec: EvaluationSpec, feature: FeatureData, variant: str,
                       arrays: _Arrays, h: int, use: np.ndarray, returns: np.ndarray,
                       legacy_ic: np.ndarray, out: dict[str, list[Any]]) -> tuple[dict[str, Any], dict[str, np.ndarray]]:
    """Node 4.1 diagnostics. No returned value is used by the testing-family or qualification path."""
    from .factor_returns import span_cell
    from .features import beta_neutralize

    month, security, value = arrays.month, arrays.security, arrays.value
    months, mask = prep.months, prep.samples[h].primary
    keys = month * prep.span + security
    ctx = _lookup(prep.context_keys, keys)
    cap = _take(prep.context_cap, ctx, _NAN)
    capped = np.minimum(cap, prep.jkp_p80[month])
    reference = np.isfinite(cap) & (cap > 0) & (cap >= prep.jkp_p20[month])
    reported: dict[str, Any] = {"evidence_class": feature.evidence_class, "publication_year": feature.publication_year,
                               "reported_metrics_version": "tier1-v2-4.1-v2",
                               "jkp_breakpoint_basis": _method_counts(prep.jkp_p20_basis, mask),
                               "jkp_cap_basis": _method_counts(prep.jkp_p80_basis, mask),
                               "cost_basis": ("exact_entry_drift_ar_else_cs_decision_spread_estimate"
                                              if h in prep.rebalance_keys and np.isfinite(prep.context_half_spread).any()
                                              else "unavailable"),
                               "capacity_basis": ("sum_1pct_ADV_complete_decile"
                                                  if CONTEXT_ADV in prep.aux_present else "unavailable"),
                               "ea_basis": "p3_session_window_m1_p1" if h in prep.event_keys else "unavailable",
                               "beta_neutral_basis": "not_preregistered"}
    series: dict[str, np.ndarray] = {}
    buckets = {}
    for q in JKP_QUANTILES:
        bucket = breakpoint_quantiles(month, value, reference, months, q, spec.nyse_min_names)
        buckets[q] = bucket
        for weighting, w in (("ew", np.ones(len(value))), ("vw", cap), ("cvw", capped)):
            valid = use & np.isfinite(w) & (w > 0)
            _, means, names = _bucket_means(month[valid], bucket[valid], returns[valid], months, q, w[valid])
            means[names < JKP_MIN_LEG_NAMES] = _NAN
            name = f"jkp_{weighting}{q}"
            series[name] = means[:, -1] - means[:, 0]
            reported.update(_reported_inference(name, series[name], mask, h))
            _quantile_rows(out["quantiles"], prep, feature, variant, h, mask,
                           ((f"jkp_{weighting}", q, means, names),))
    all_deciles = grouped_quantiles(month, value, security, months, 10, 1)
    equal_weight = np.broadcast_to(np.array(1.0), value.shape)
    portfolios = {"ew10": (np.where(all_deciles == 10, 1, np.where(all_deciles == 1, -1, 0)).astype(np.int8), equal_weight),
                  "cvw10": (np.where(buckets[10] == 10, 1, np.where(buckets[10] == 1, -1, 0)).astype(np.int8), capped),
                  "buy_hold": (_hold_spread_legs(month, security, all_deciles, months, h), equal_weight)}
    for name, (legs, weight) in portfolios.items():
        path = _portfolio_path(prep, month, security, legs, weight, h, use, returns)
        for metric in ("gross", "turnover", "cost", "net"):
            field = f"trade_{name}_{metric}"
            series[field] = path[metric]
            reported.update(_reported_inference(field, path[metric], mask, h))
        for metric in ("held", "priced", "drift_held", "drift_priced"):
            field = f"trade_{name}_{metric}"
            series[field] = path[metric]
            reported[f"{field}_n"] = int(path[metric][mask].sum())
        matched = mask & np.isfinite(path["gross"]) & np.isfinite(path["turnover"])
        mean_turn = _nanmean(path["turnover"][matched])
        mean_gross = _nanmean(path["gross"][matched])
        reported[f"trade_{name}_break_even_bps"] = (10_000 * mean_gross / mean_turn
                                                     if mean_turn is not None and mean_turn > 0 else None)
        reported[f"trade_{name}_break_even_basis"] = "gross_per_summed_one_sided_leg_turnover"
        for bp in COST_BPS:
            reported[f"trade_{name}_net{bp}_mean"] = _nanmean((path["gross"] - bp / 10000 * path["turnover"])[mask])
        if h == 1:
            reported[f"trade_{name}_high_turnover"] = mean_turn / 2 > TURNOVER_FLAG_MONTHLY if mean_turn is not None else None
        paired = mask & np.isfinite(path["ea"]) & np.isfinite(path["gross"])
        denom = _nanmean(path["gross"][paired])
        reported[f"trade_{name}_ea_share"] = _nanmean(path["ea"][paired]) / denom if denom else None
        reported[f"trade_{name}_ea_n"] = int(paired.sum())
        if name in SPAN_PORTFOLIOS:
            for model in SPAN_MODEL_NAMES:
                prefix = f"span_{name}_{model}"
                reported[f"{prefix}_basis"] = "unavailable"
                if model in prep.factor_models:
                    names, values = prep.factor_models[model]
                    span = span_cell(np.where(mask, path["gross"], np.nan),
                                     {n: values[:, i] for i, n in enumerate(names)}, horizon_periods=h, model=model,
                                     monthly_window_basis=prep.factor_windows[model])
                    reported.update({f"{prefix}_{k}": v for k, v in span.items()})
                    reported[f"{prefix}_basis"] = "supplied_model"
    del portfolios, path, legs, weight, all_deciles
    adv = _take(prep.context_adv, ctx, _NAN)
    for q in range(1, 11):
        selected = buckets[10] == q
        count = np.bincount(month[selected], minlength=months)
        valid = selected & np.isfinite(adv) & (adv > 0)
        covered = np.bincount(month[valid], minlength=months)
        capacity = np.bincount(month[valid], weights=adv[valid] * CAPACITY_ADV_FRACTION, minlength=months)
        capacity = np.where((count >= JKP_MIN_LEG_NAMES) & (covered == count), capacity, np.nan)
        reported[f"capacity_decile{q}_usd"] = _nanmean(capacity[mask])
    # A wide per-feature panel must not retain portfolio temporaries alongside full FM regressors.
    del adv, buckets, bucket, capped, reference, w, valid, selected
    # Full JKP/HXZ controls: a focal control is explicitly omitted to prevent self-regression.
    controls = {"log_me": np.log(np.where(cap > 0, cap, np.nan)),
                **{k: _take(v, ctx, _NAN) for k, v in prep.jkp_controls.items()}}
    self_roles = {role for role, candidates in FM_JKP_CONTROLS if feature.feature_id in candidates}
    if feature.feature_id in SIZE_FEATURES:
        self_roles.add("log_me")
    for role in self_roles:
        controls.pop(role, None)
    missing = [name for name, values in controls.items() if not np.isfinite(values).any()]
    reported["fmj_controls_missing"] = ",".join(missing) or None
    reported["fmj_controls"] = ",".join(controls)
    reported["fmj_self_controls_omitted"] = ",".join(sorted(self_roles)) or None
    regressors = [] if missing else [value[use], *(v[use] for v in controls.values())]
    for weighting in ("ols", "wls"):
        slopes = np.full(months, _NAN)
        if not missing:
            slopes = _weighted_fm(month[use], returns[use], regressors,
                                  cap[use] if weighting == "wls" else np.ones(int(use.sum())), months, spec.fm_min_obs)
        series[f"fmj_{weighting}"] = slopes
        reported.update(_reported_inference(f"fmj_{weighting}", slopes, mask, h))
        for k in (3, 6, 12):
            # HXZ trailing K-month slope averages. Mask BEFORE smoothing to prevent split/holdout leakage.
            averaged = pd.Series(np.where(mask, slopes, np.nan)).rolling(k, min_periods=k).mean().to_numpy()
            reported.update(_reported_inference(f"fmj_{weighting}_k{k}", averaged, mask, h))
    if (feature.feature_id, f"{variant}_beta_neutral", h) in prep.beta_neutral_cells:
        beta = _take(prep.context_beta, ctx, _NAN)
        neutral, _, _ = beta_neutralize(month, value, beta, months, min_names=spec.min_names)
        valid = use & np.isfinite(neutral)
        neutral_ic, neutral_n = grouped_rank_correlation(month[valid], neutral[valid], returns[valid], months)
        neutral_ic[neutral_n < spec.min_names] = _NAN
        neutral_bucket = grouped_quantiles(month[np.isfinite(neutral)], neutral[np.isfinite(neutral)],
                                           security[np.isfinite(neutral)], months, 10, spec.min_names)
        neutral_use = use[np.isfinite(neutral)]
        neutral_ew, _, neutral_count = _bucket_means(month[valid], neutral_bucket[neutral_use],
                                                    returns[valid], months, 10)
        neutral_ew[neutral_count < JKP_MIN_LEG_NAMES] = _NAN
        neutral_ls = neutral_ew[:, -1] - neutral_ew[:, 0]
        reported.update(_reported_inference("beta_neutral_ic", neutral_ic, mask, h))
        reported.update(_reported_inference("beta_neutral_ls", neutral_ls, mask, h))
        reported["beta_neutral_basis"] = "preregistered_ols_beta_ew_252d" if CONTEXT_BETA in prep.aux_present else "unavailable"
        series["beta_neutral_ic"] = neutral_ic
    if feature.publication_year is not None:
        # Only the year is known: exclude that entire year rather than invent a publication date.
        year = pd.DatetimeIndex(prep.formation).year.to_numpy()
        for name, selected in (("pre", year < feature.publication_year), ("post", year > feature.publication_year)):
            selected &= mask
            reported.update(_reported_inference(f"publication_{name}_ic", legacy_ic, selected, h))
            out["slices"].append(_slice_row(prep, feature, variant, h, PUBLICATION_SLICE_KIND, name,
                                             _infer(legacy_ic, selected, h), _infer(series["trade_ew10_gross"], selected, h),
                                             "trade_ew10", int((selected & np.isfinite(legacy_ic)).sum()),
                                             0, 0, None, None, None, None))
    inf = _infer(legacy_ic, mask, h)
    reported["ic_nw_se"] = _num(inf.nw_standard_error)
    reported["ic_mde_80pct"] = _num((1.959963984540054 + 0.8416212335729143) * inf.nw_standard_error)
    reported["ic_mde_basis"] = "normal_approx_two_sided_alpha05_power80_NW_SE_reported_only"
    acronym = OSAP_ACRONYMS.get(feature.feature_id)
    reported["osap_acronym"] = acronym
    reported["osap_original_t"] = prep.original_paper_t.get(acronym)
    reported["osap_atx_t"] = _num(_infer(series["trade_ew10_gross"], mask, h).nw_t)
    reported["osap_basis"] = "original_paper_vs_atx_NW_nonidentical_samples" if acronym in prep.original_paper_t else "unavailable"
    return reported, series


def _signal_age_metrics(prep: _Prepared, spec: EvaluationSpec, feature: FeatureData, variant: str,
                         arrays: _Arrays, out: dict[str, list[Any]]) -> dict[str, Any]:
    """Always one-month future labels, indexed by target formation; never h-month cumulative IC."""
    reported: dict[str, Any] = {"ic_half_life_months": None, "ic_half_life_basis": "one_month_labels_unavailable"}
    if 1 not in prep.label_keys:
        return reported
    month, keys = arrays.month, arrays.month * prep.span + arrays.security
    curve = []
    for age in SIGNAL_AGES:
        target = month + age
        chosen = target < prep.months
        target = target[chosen]
        index = _lookup(prep.label_keys[1], keys[chosen] + age * prep.span)
        y = _take(prep.label_return[1], index, _NAN)
        valid = (prep.matured[1][target] & (_take(prep.label_status[1], index, LABEL_MISSING) == LABEL_VALID)
                 & np.isfinite(y))
        ic, n = grouped_rank_correlation(target[valid], arrays.value[chosen][valid], y[valid], prep.months)
        ic[n < spec.min_names] = _NAN
        inf = _infer(ic, prep.samples[1].primary, 1)
        curve.append(inf.mean)
        reported[f"ic_age{age}_mean"] = _num(inf.mean)
        out["decay"].append(_decay_row(prep, feature, variant, "signal_age", age, inf,
                                       inf.mean / curve[0] if curve[0] else _NAN))
    reported["ic_half_life_basis"] = "not_estimable"
    if np.isfinite(curve[0]) and curve[0] != 0:
        normalized = np.array(curve) / curve[0]
        reported["ic_half_life_basis"] = "right_censored_after_11_months"
        for k in range(1, len(normalized)):
            if np.isfinite(normalized[k - 1:k + 1]).all() and normalized[k - 1] > .5 >= normalized[k]:
                reported["ic_half_life_months"] = k - 1 + (normalized[k - 1] - .5) / (normalized[k - 1] - normalized[k])
                reported["ic_half_life_basis"] = "first_half_crossing_linear_interpolation"
                break
    return reported


def _evaluate_variant(prep: _Prepared, spec: EvaluationSpec, feature: FeatureData, variant: str,
                      arrays: _Arrays, meta: Mapping[str, Any], unlinked_scope: bool = False
                      ) -> dict[str, list[dict[str, Any]]]:
    months, span = prep.months, prep.span
    month, security, value = arrays.month, arrays.security, arrays.value
    out: dict[str, list[Any]] = {"cells": [], "slices": [], "quantiles": [], "decay": [], "series": []}
    sample_name = "full" if spec.split is None else "selection"
    counts = np.bincount(month, minlength=months)[:months]
    formation_ok = counts >= spec.min_names
    # Coverage denominator: the feature's ranked universe (valid primary lines, plus eligible
    # unlinked lines for an identity-free price-line feature).
    universe_names = prep.universe_names + (prep.unlinked_names if unlinked_scope else 0)
    with np.errstate(invalid="ignore", divide="ignore"):
        value_coverage = counts / np.where(universe_names > 0, universe_names, np.nan)
    keys = month * span + security
    deciles = grouped_quantiles(month, value, security, months, 10, 1)
    quintiles = grouped_quantiles(month, value, security, months, 5, 1)
    context_index = _lookup(prep.context_keys, keys)
    cap = _take(prep.context_cap, context_index, _NAN)
    size = _take(prep.context_size, context_index, 0).astype(np.int8)
    nyse_deciles = breakpoint_quantiles(month, value, _take(prep.context_nyse, context_index, False).astype(bool),
                                        months, 10, spec.nyse_min_names)
    investable = (None if prep.context_investable is None
                  else _take(prep.context_investable, context_index, False).astype(bool))
    if spec.population_coverage and feature.population not in (None, "", POPULATION_ALL):
        # Policy v4: coverage of the catalog population (members of the ranked universe flagged
        # population_<name>); without the flag the coverage is unknown (NaN), never the universe's.
        value_coverage = _population_coverage(prep, feature.population, month, context_index)
    del context_index
    formed_index = np.flatnonzero(prep.formed)
    size_kinds = dict(SIZE_SLICE_KINDS)
    if prep.supplied_breakpoints:
        size_kinds[VENUE_NYSE_BREAKPOINTS] = SUPPLIED_SIZE_SLICE_KIND
    venue_masks = {venue: np.array([basis == venue for basis in prep.venue_basis], dtype=bool)
                   for venue in size_kinds}

    # Turnover and rank autocorrelation (horizon independent).
    top = (deciles == 10) & formation_ok[month]
    bottom = (deciles == 1) & formation_ok[month]
    lags = sorted({1, *spec.horizons_months})
    traded = {lag: (traded_fraction(month[top], security[top], span, months, formation_ok, lag),
                    traded_fraction(month[bottom], security[bottom], span, months, formation_ok, lag))
              for lag in lags}
    previous = np.where(month >= 1, _lookup(keys, keys - span), -1)
    pair = previous >= 0
    pair &= formation_ok[month] & formation_ok[np.maximum(month - 1, 0)]
    autocorr, _ = grouped_rank_correlation(month[pair], value[pair], value[previous[pair]], months)

    controls: dict[str, np.ndarray] = {}
    for name in spec.control_features:
        if name != feature.feature_id and name in prep.control_keys:
            controls[name] = _take(prep.control_values[name], _lookup(prep.control_keys[name], keys), _NAN)
    log_cap = np.full(len(cap), _NAN)
    positive_cap = np.isfinite(cap) & (cap > 0)
    log_cap[positive_cap] = np.log(cap[positive_cap])
    use_size_control = feature.feature_id not in SIZE_FEATURES
    age_metrics = _signal_age_metrics(prep, spec, feature, variant, arrays, out)

    cumulative: list[tuple[int, stats.MeanInference]] = []
    for h in spec.horizons_months:
        samples = prep.samples[h]
        primary = samples.primary
        index = _lookup(prep.label_keys[h], keys)
        status = _take(prep.label_status[h], index, LABEL_MISSING).astype(np.int8)
        returns = _take(prep.label_return[h], index, _NAN)
        terminal = _take(prep.label_terminal[h], index, TERMINAL_NONE).astype(np.int8)
        del index
        matured_row = prep.matured[h][month]
        valid = matured_row & (status == LABEL_VALID) & np.isfinite(returns)
        used_counts = np.bincount(month[valid], minlength=months)[:months]
        usable = used_counts >= spec.min_names
        use = valid & usable[month]
        del valid
        u_month, u_value, u_return = month[use], value[use], returns[use]
        ic, _ = grouped_rank_correlation(u_month, u_value, u_return, months)
        ic[~usable] = _NAN
        u_cap = cap[use]
        ew10, vw10, count10 = _bucket_means(u_month, deciles[use], u_return, months, 10, u_cap)
        ew5, vw5, count5 = _bucket_means(u_month, quintiles[use], u_return, months, 5, u_cap)
        nyse_ew10, nyse_vw10, _ = _bucket_means(u_month, nyse_deciles[use], u_return, months, 10, u_cap)
        del u_cap
        assert vw10 is not None and vw5 is not None and nyse_vw10 is not None
        spreads = {"ew10": ew10[:, 9] - ew10[:, 0], "vw10": vw10[:, 9] - vw10[:, 0],
                   "ew5": ew5[:, 4] - ew5[:, 0], "vw5": vw5[:, 4] - vw5[:, 0],
                   "nyse_ew10": nyse_ew10[:, 9] - nyse_ew10[:, 0], "nyse_vw10": nyse_vw10[:, 9] - nyse_vw10[:, 0]}
        del nyse_ew10, nyse_vw10
        for series in spreads.values():
            series[~usable] = _NAN
        fm = _fama_macbeth(u_month, u_return, {"x": u_value}, months, spec)
        control_columns = {"x": u_value}
        if use_size_control:
            control_columns["size"] = log_cap[use]
        control_columns.update({name: column[use] for name, column in controls.items()})
        control_names = [name for name in control_columns if name != "x"]
        fmc = (_fama_macbeth(u_month, u_return, control_columns, months, spec) if control_names
               else np.full(months, _NAN))
        del control_columns, u_month, u_value, u_return
        traded_top, traded_bottom = traded[h]
        net = {bp: spreads["ew10"] - bp / 10_000.0 * (traded_top + traded_bottom) for bp in COST_BPS}
        # Availability-lag sensitivity: the previous formation's value against this formation's label.
        later = month + 1 < months
        lag_index = np.where(later, _lookup(prep.label_keys[h], keys + span), -1)
        target = np.minimum(month + 1, months - 1)
        lag_valid = (later & (lag_index >= 0) & prep.matured[h][target]
                     & (_take(prep.label_status[h], lag_index, LABEL_MISSING) == LABEL_VALID))
        lag_returns = _take(prep.label_return[h], lag_index, _NAN)[lag_valid]
        del lag_index, later
        lag_ic, lag_n = grouped_rank_correlation(target[lag_valid], value[lag_valid], lag_returns, months)
        del lag_returns, lag_valid, target
        lag_ic[lag_n < spec.min_names] = _NAN

        ic_inf = _infer(ic, primary, h)
        cumulative.append((h, ic_inf))
        boot = _bootstrap(ic, primary, h, spec)
        ls_inf = {name: _infer(series, primary, h) for name, series in spreads.items()}
        ls_boot = _bootstrap(spreads["ew10"], primary, h, spec)
        fm_inf, fmc_inf = _infer(fm, primary, h), _infer(fmc, primary, h)
        net_inf = {bp: _infer(series, primary, h) for bp, series in net.items()}
        lag_inf = _infer(lag_ic, primary, h)
        in_sample = primary & usable
        ic_values = ic[in_sample & np.isfinite(ic)]
        ic_sd = float(ic_values.std(ddof=1)) if len(ic_values) > 1 else _NAN
        ls_values = spreads["ew10"][in_sample & np.isfinite(spreads["ew10"])]
        sharpe = skew = kurt = _NAN
        if len(ls_values) >= 3 and float(np.var(ls_values)) > 0:
            sharpe, skew, kurt, _ = stats.sharpe_moments(ls_values)
        counted = matured_row & primary[month]
        status_primary = status[counted]
        used_primary = use & primary[month]
        coverage = value_coverage[primary]
        n_formations = int(in_sample.sum())
        testable = ic_inf.n_obs >= spec.min_formations and ic_inf.robust_df >= 1 and \
            math.isfinite(ic_inf.robust_p_value)
        if not ((counts > 0) & primary).any():
            cell_status = "no_values"
        else:
            cell_status = "tested" if testable else "insufficient_formations"
        row = _cell_identity(prep, feature, variant, h, meta, sample_name)
        row.update({
            "status": cell_status,
            "status_reason": {"tested": None, "no_values": "no_values_in_primary_sample"}.get(
                cell_status, f"ic_formations_{ic_inf.n_obs}_below_{spec.min_formations}_or_no_ewc_df"),
            "formations_calendar": months, "formations_formed": int(prep.formed.sum()),
            "formations_with_values": int(((counts > 0) & primary).sum()),
            "formations_thin": int((arrays.thin & primary).sum()),
            "formations_matured": int((prep.matured[h] & primary).sum()),
            "formations_usable": n_formations,
            "feature_rows": int(counted.sum()),
            "labels_valid": int(((status_primary == LABEL_VALID)
                                 & np.isfinite(returns[counted])).sum()),
            "labels_missing": int((status_primary == LABEL_MISSING).sum()),
            "labels_invalid": int((status_primary == LABEL_INVALID).sum()),
            "labels_unsupported": int((status_primary == LABEL_UNSUPPORTED).sum()),
            "stitched_observed": int((terminal[used_primary] == TERMINAL_OBSERVED).sum()),
            "stitched_policy": int((terminal[used_primary] == TERMINAL_POLICY).sum()),
            "mean_names": float(used_counts[in_sample].mean()) if n_formations else None,
            "min_names_used": int(used_counts[in_sample].min()) if n_formations else None,
            "universe_scope": UNIVERSE_SCOPE_WITH_UNLINKED if unlinked_scope else UNIVERSE_SCOPE_LINKED,
            "values_unlinked_lines": int(arrays.unlinked_values[primary].sum()),
            "mean_universe_names": _nanmean(universe_names[primary].astype(float)),
            "mean_coverage": _nanmean(coverage),
            "min_coverage": float(np.nanmin(coverage)) if np.isfinite(coverage).any() else None,
            "mean_feature_coverage": _nanmean(arrays.coverage[primary]),
            "values_dropped_not_valid": int(arrays.dropped_not_valid[primary].sum()),
            "values_dropped_not_primary": int(arrays.dropped_not_primary[primary].sum()),
            "ic_mean": _num(ic_inf.mean), "ic_sd": _num(ic_sd),
            "ic_ir": _num(ic_inf.mean / ic_sd) if ic_sd and math.isfinite(ic_sd) and ic_sd > 0 else None,
            "ic_positive_share": float((ic_values > 0).mean()) if len(ic_values) else None,
            "ic_n": ic_inf.n_obs, "ic_robust_t": _num(ic_inf.robust_t), "ic_robust_p": _num(ic_inf.robust_p_value),
            "ic_robust_df": ic_inf.robust_df, "ic_z": _num(ic_inf.z_equivalent),
            "ic_ci_low": _num(ic_inf.robust_ci95_low), "ic_ci_high": _num(ic_inf.robust_ci95_high),
            "ic_nw_t": _num(ic_inf.nw_t), "ic_nw_p": _num(ic_inf.nw_p_value), "ic_nw_lags": ic_inf.nw_lags,
            "ic_boot_low": None if boot is None else _num(boot.low),
            "ic_boot_high": None if boot is None else _num(boot.high),
            "ic_boot_pct_low": None if boot is None else _num(boot.percentile_low),
            "ic_boot_pct_high": None if boot is None else _num(boot.percentile_high),
            "ic_hlz_pass": bool(ic_inf.hlz_pass),
            **_short("ls_ew10", ls_inf["ew10"]),
            "ls_ew10_nw_t": _num(ls_inf["ew10"].nw_t),
            "ls_ew10_hit_rate": float((ls_values > 0).mean()) if len(ls_values) else None,
            "ls_ew10_boot_low": None if ls_boot is None else _num(ls_boot.low),
            "ls_ew10_boot_high": None if ls_boot is None else _num(ls_boot.high),
            "ls_ew10_n": ls_inf["ew10"].n_obs,
            **_short("ls_vw10", ls_inf["vw10"]), **_short("ls_ew5", ls_inf["ew5"]), **_short("ls_vw5", ls_inf["vw5"]),
            **_short("ls_nyse_ew10", ls_inf["nyse_ew10"]), **_short("ls_nyse_vw10", ls_inf["nyse_vw10"]),
            "ls_nyse_n": ls_inf["nyse_ew10"].n_obs,
            "venue_basis": _method_counts(prep.venue_basis, primary),
            "venue_pit_share": _nanmean(prep.venue_pit_share[primary]),
            "fm_slope": _num(fm_inf.mean), "fm_robust_t": _num(fm_inf.robust_t),
            "fm_robust_p": _num(fm_inf.robust_p_value), "fm_z": _num(fm_inf.z_equivalent),
            "fm_nw_t": _num(fm_inf.nw_t), "fm_n": fm_inf.n_obs,
            "fmc_slope": _num(fmc_inf.mean), "fmc_robust_t": _num(fmc_inf.robust_t),
            "fmc_robust_p": _num(fmc_inf.robust_p_value), "fmc_z": _num(fmc_inf.z_equivalent),
            "fmc_nw_t": _num(fmc_inf.nw_t), "fmc_n": fmc_inf.n_obs,
            "fmc_controls": ",".join(control_names) if control_names else None,
            "rank_autocorr_1m": _nanmean(autocorr[primary]),
            "top_turnover_1m": _half(traded[1][0], primary), "bottom_turnover_1m": _half(traded[1][1], primary),
            "top_turnover_h": _half(traded_top, primary), "bottom_turnover_h": _half(traded_bottom, primary),
            "net10_mean": _num(net_inf[10].mean), "net10_z": _num(net_inf[10].z_equivalent),
            "net25_mean": _num(net_inf[25].mean), "net25_robust_t": _num(net_inf[25].robust_t),
            "net25_robust_p": _num(net_inf[25].robust_p_value), "net25_z": _num(net_inf[25].z_equivalent),
            "net50_mean": _num(net_inf[50].mean), "net50_z": _num(net_inf[50].z_equivalent),
            "ic_lag1_mean": _num(lag_inf.mean), "ic_lag1_robust_p": _num(lag_inf.robust_p_value),
            "ic_lag1_z": _num(lag_inf.z_equivalent), "ic_lag1_n": lag_inf.n_obs,
            "sharpe": _num(sharpe), "sharpe_skew": _num(skew), "sharpe_kurt": _num(kurt),
            "sharpe_n": len(ls_values) if math.isfinite(sharpe) else None,
        })
        row["mono_ew10"], row["mono_vw10"], row["mono_ew5"], row["mono_vw5"] = _quantile_rows(
            out["quantiles"], prep, feature, variant, h, in_sample,
            (("ew", 10, ew10, count10), ("vw", 10, vw10, count10), ("ew", 5, ew5, count5), ("vw", 5, vw5, count5)))
        reported, reported_series = _reported_metrics(prep, spec, feature, variant, arrays, h, use, returns, ic, out)
        row.update(reported)
        row.update(age_metrics)
        if h != 1 and out["cells"] and out["cells"][0]["horizon_months"] == 1:
            row.update({name: value for name, value in out["cells"][0].items() if name.endswith("_high_turnover")})
        out["cells"].append(row)
        out["decay"].append(_decay_row(prep, feature, variant, "availability_lag1", h, lag_inf,
                                       lag_inf.mean / ic_inf.mean if ic_inf.mean else _NAN))
        chosen_months = formed_index
        out["series"].append(pd.DataFrame({
            "basis": prep.basis, "feature_id": feature.feature_id, "variant": variant, "horizon_months": h,
            "formation_date": prep.formation[chosen_months], "segment": prep.segment[chosen_months],
            "in_selection": primary[chosen_months], "matured": prep.matured[h][chosen_months],
            "usable": usable[chosen_months], "universe_names": universe_names[chosen_months],
            "n_values": counts[chosen_months], "n_unlinked": arrays.unlinked_values[chosen_months],
            "n_used": used_counts[chosen_months],
            "coverage": value_coverage[chosen_months],
            "dropped_not_valid": arrays.dropped_not_valid[chosen_months],
            "dropped_not_primary": arrays.dropped_not_primary[chosen_months],
            "venue_basis": [prep.venue_basis[i] for i in chosen_months],
            "nyse_pit_names": prep.nyse_pit_names[chosen_months],
            "ic": ic[chosen_months], "ls_ew10": spreads["ew10"][chosen_months],
            "ls_vw10": spreads["vw10"][chosen_months], "ls_ew5": spreads["ew5"][chosen_months],
            "ls_vw5": spreads["vw5"][chosen_months], "ls_nyse_ew10": spreads["nyse_ew10"][chosen_months],
            "ls_nyse_vw10": spreads["nyse_vw10"][chosen_months], "fm_slope": fm[chosen_months],
            "fmc_slope": fmc[chosen_months], "net25_ew10": net[25][chosen_months],
            "ic_lag1": lag_ic[chosen_months],
            **{name: values[chosen_months] for name, values in reported_series.items()}}))
        # Slices: frozen split segments and subperiods.
        for (kind, name), mask in samples.masks.items():
            slice_mask = mask & usable
            purged, embargoed = samples.notes.get(name, (0, 0)) if kind == "split" else (0, 0)
            out["slices"].append(_slice_row(
                prep, feature, variant, h, kind, name, _infer(ic, mask, h), _infer(spreads["ew10"], mask, h), "ew10",
                int(slice_mask.sum()), purged, embargoed,
                float(used_counts[slice_mask].mean()) if slice_mask.any() else None, None, None, None))
        # Size buckets on the primary sample, one slice kind per breakpoint basis: point-in-time
        # NYSE buckets never mix in formations bucketed by cap terciles.
        for bucket, bucket_name in enumerate(SIZE_BUCKETS):
            in_bucket = size == bucket
            chosen = use & in_bucket
            bucket_ic, bucket_n = grouped_rank_correlation(month[chosen], value[chosen], returns[chosen], months)
            bucket_ic[bucket_n < spec.min_bucket_names] = _NAN
            members = in_bucket
            bucket_q = grouped_quantiles(month[members], value[members], security[members], months, 5,
                                         spec.min_bucket_names)
            labeled = use[members]
            bucket_ew, _, _ = _bucket_means(month[members][labeled], bucket_q[labeled], returns[members][labeled],
                                            months, 5)
            bucket_ls = bucket_ew[:, 4] - bucket_ew[:, 0]
            bucket_ls[bucket_n < spec.min_bucket_names] = _NAN
            with np.errstate(invalid="ignore", divide="ignore"):
                share = bucket_n / used_counts
            for venue, kind in size_kinds.items():
                sample = primary & venue_masks[venue]
                valid_months = in_sample & venue_masks[venue] & (bucket_n >= spec.min_bucket_names)
                out["slices"].append(_slice_row(
                    prep, feature, variant, h, kind, bucket_name, _infer(bucket_ic, sample, h),
                    _infer(bucket_ls, sample, h), "ew5", int(valid_months.sum()), 0, 0,
                    float(bucket_n[valid_months].mean()) if valid_months.any() else None,
                    _nanmean(share[in_sample & venue_masks[venue]]), venue,
                    _nanmean(prep.venue_pit_share[sample])))
        if investable is not None:
            out["slices"].append(_investable_slice(prep, spec, feature, variant, h, month, security, value,
                                                   returns, cap, use, investable, used_counts, primary, in_sample))

    first = cumulative[0][1].mean if cumulative else _NAN
    for h, inference in cumulative:
        out["decay"].append(_decay_row(prep, feature, variant, "cumulative", h, inference,
                                       inference.mean / first if first else _NAN))
    if 1 in spec.horizons_months:
        out["decay"].extend(_marginal_decay(prep, spec, feature, variant, month, keys, value))
        legs = np.where(top, 1, np.where(bottom, -1, 0)).astype(np.int8)
        for holding in spec.jt_holding_months:
            series, cohorts, names = jegadeesh_titman_series(
                month, security, legs, span, months, prep.label_keys[1], prep.label_return[1],
                prep.label_status[1], prep.matured[1], holding)
            mask = prep.samples[1].primary & (cohorts == holding)
            used = mask & np.isfinite(series)
            out["slices"].append(_slice_row(
                prep, feature, variant, 1, JT_SLICE_KIND, f"k{holding}", _infer(np.full(months, _NAN), mask, 1),
                _infer(series, mask, 1), "jt_ew10", int(used.sum()), 0, 0, _nanmean(names[used]), None, None,
                None))
    return out


def _population_coverage(prep: _Prepared, population: str, month: np.ndarray,
                         context_index: np.ndarray) -> np.ndarray:
    """Per formation: valued names in the catalog population / population members of the ranked universe.

    Members are ranked-universe context rows flagged ``population_<name>``; NaN everywhere when
    the inputs carry no such flag (coverage unknown, never measured against another denominator).
    """
    flags = prep.population_flags.get(population)
    if flags is None or not len(prep.context_keys):
        return np.full(prep.months, _NAN)
    members = np.bincount(prep.context_keys[flags & prep.context_universe] // prep.span,
                          minlength=prep.months)[:prep.months]
    valued = np.bincount(month[_take(flags, context_index, False).astype(bool)], minlength=prep.months)[:prep.months]
    with np.errstate(invalid="ignore", divide="ignore"):
        return valued / np.where(members > 0, members, np.nan)


def _investable_slice(prep: _Prepared, spec: EvaluationSpec, feature: FeatureData, variant: str, h: int,
                      month: np.ndarray, security: np.ndarray, value: np.ndarray, returns: np.ndarray,
                      cap: np.ndarray, use: np.ndarray, investable: np.ndarray, used_counts: np.ndarray,
                      primary: np.ndarray, in_sample: np.ndarray) -> dict[str, Any]:
    """Policy v4 investable co-primary slice: rank IC re-ranked among investable names, and the
    value-weighted decile long-short formed within them (``min_bucket_names`` per formation)."""
    months = prep.months
    chosen = use & investable
    ic, n = grouped_rank_correlation(month[chosen], value[chosen], returns[chosen], months)
    thin = n < spec.min_bucket_names
    ic[thin] = _NAN
    deciles = grouped_quantiles(month[investable], value[investable], security[investable], months, 10,
                                spec.min_bucket_names)
    labeled = use[investable]
    _, weighted, _ = _bucket_means(month[investable][labeled], deciles[labeled], returns[investable][labeled],
                                   months, 10, cap[investable][labeled])
    assert weighted is not None
    spread = weighted[:, 9] - weighted[:, 0]
    spread[thin] = _NAN
    valid_months = in_sample & ~thin
    with np.errstate(invalid="ignore", divide="ignore"):
        share = n / used_counts
    return _slice_row(prep, feature, variant, h, INVESTABLE_SLICE_KIND, INVESTABLE_SLICE_NAME,
                      _infer(ic, primary, h), _infer(spread, primary, h), "vw10", int(valid_months.sum()), 0, 0,
                      float(n[valid_months].mean()) if valid_months.any() else None, _nanmean(share[in_sample]),
                      _method_counts(prep.investable_basis or [], primary), None)


def jegadeesh_titman_series(month: np.ndarray, security: np.ndarray, legs: np.ndarray, span: int, months: int,
                            label_keys: np.ndarray, label_return: np.ndarray, label_status: np.ndarray,
                            matured: np.ndarray, holding: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Calendar-time returns of an equal-weighted long-short held ``holding`` months (Jegadeesh-Titman 1993).

    ``legs`` marks each value row (``month``, ``security``: one cohort per formation) +1 long,
    -1 short, 0 neither. ``label_*`` / ``matured`` are the one-month labels (keys sorted
    ``month * span + security``). Cohort ``s`` earns in its k-th holding month (k = 1..K) the
    one-month label return of formation ``s + k - 1`` of its names, equally weighted over the
    names with a valid matured label (a name that stops trading leaves the leg; nothing is
    imputed). The return of label period ``j`` averages the cohorts formed at ``j, j-1, ...,
    j-K+1``: calendar month ``t = j + 1`` averages the cohorts formed at ``t-1 ... t-K``.

    Returns per label period ``j``: the calendar-time return (NaN without a cohort), the number
    of cohorts averaged and the names (long + short, summed over cohorts) that earned it.
    """
    if holding < 1:
        raise EvaluationInputError("holding must be a positive number of months")
    month = np.asarray(month, dtype=np.int64)
    security = np.asarray(security, dtype=np.int64)
    legs = np.asarray(legs)
    cohort_returns = np.full((months, holding), _NAN)
    cohort_names = np.zeros((months, holding), dtype=np.int64)
    for k in range(holding):
        target = month + k
        inside = target < months
        index = np.where(inside, _lookup(label_keys, target * span + security), -1)
        returns = _take(label_return, index, _NAN)
        valid = (inside & (index >= 0) & matured[np.minimum(target, months - 1)]
                 & (_take(label_status, index, LABEL_MISSING) == LABEL_VALID) & np.isfinite(returns))
        means = []
        for side in (1, -1):
            rows = valid & (legs == side)
            count = np.bincount(month[rows], minlength=months)[:months]
            total = np.bincount(month[rows], weights=returns[rows], minlength=months)[:months]
            with np.errstate(invalid="ignore", divide="ignore"):
                means.append(np.where(count > 0, total / np.maximum(count, 1), _NAN))
            cohort_names[:, k] += count
        cohort_returns[:, k] = means[0] - means[1]
    series = np.full(months, _NAN)
    cohorts = np.zeros(months, dtype=np.int64)
    names = np.zeros(months, dtype=np.int64)
    for j in range(months):
        values = [cohort_returns[j - k, k] for k in range(holding) if j - k >= 0]
        counts = [cohort_names[j - k, k] for k in range(holding) if j - k >= 0]
        finite = [(v, c) for v, c in zip(values, counts, strict=True) if math.isfinite(v)]
        if finite:
            series[j] = sum(v for v, _ in finite) / len(finite)
            cohorts[j] = len(finite)
            names[j] = sum(c for _, c in finite)
    return series, cohorts, names


def _half(traded: np.ndarray, mask: np.ndarray) -> float | None:
    mean = _nanmean(traded[mask])
    return None if mean is None else mean / 2.0


def _quantile_rows(sink: list[dict[str, Any]], prep: _Prepared, feature: FeatureData, variant: str, h: int,
                   in_sample: np.ndarray, sets: Sequence[tuple[str, int, np.ndarray | None, np.ndarray]]
                   ) -> list[float | None]:
    monotonicity: list[float | None] = []
    for weighting, q, means, count in sets:
        assert means is not None
        chosen = means[in_sample]
        names = count[in_sample]
        series = np.full(q, _NAN)
        for b in range(q):
            finite = np.isfinite(chosen[:, b])
            series[b] = chosen[finite, b].mean() if finite.any() else _NAN
            sink.append({"basis": prep.basis, "feature_id": feature.feature_id, "variant": variant,
                         "horizon_months": h, "weighting": weighting, "n_quantiles": q, "quantile": b + 1,
                         "mean_return": _num(series[b]), "formations": int(finite.sum()),
                         "mean_names": float(names[finite, b].mean()) if finite.any() else None})
        monotonicity.append(_spearman_vector(np.arange(1, q + 1, dtype=float), series))
    return monotonicity


def _decay_row(prep: _Prepared, feature: FeatureData, variant: str, kind: str, h: int,
               inference: stats.MeanInference, ratio: float) -> dict[str, Any]:
    return {"basis": prep.basis, "feature_id": feature.feature_id, "variant": variant, "kind": kind,
            "horizon_months": h, "ic_mean": _num(inference.mean), "n": inference.n_obs,
            "robust_t": _num(inference.robust_t), "robust_p": _num(inference.robust_p_value),
            "z": _num(inference.z_equivalent), "ratio_to_first": _num(ratio)}


def _slice_row(prep: _Prepared, feature: FeatureData, variant: str, h: int, kind: str, name: str,
               ic: stats.MeanInference, ls: stats.MeanInference, ls_kind: str, formations: int, purged: int,
               embargoed: int, mean_names: float | None, share: float | None, venue_basis: str | None,
               venue_pit_share: float | None) -> dict[str, Any]:
    return {"basis": prep.basis, "feature_id": feature.feature_id, "variant": variant, "horizon_months": h,
            "slice_kind": kind, "slice_name": name, "formations": formations, "purged": purged,
            "embargoed": embargoed, "mean_names": mean_names, "name_share": share,
            "venue_basis": venue_basis, "venue_pit_share": venue_pit_share,
            "ic_mean": _num(ic.mean), "ic_robust_t": _num(ic.robust_t), "ic_robust_p": _num(ic.robust_p_value),
            "ic_robust_df": ic.robust_df, "ic_z": _num(ic.z_equivalent), "ic_nw_t": _num(ic.nw_t),
            "ls_kind": ls_kind, "ls_mean": _num(ls.mean), "ls_robust_t": _num(ls.robust_t),
            "ls_robust_p": _num(ls.robust_p_value), "ls_z": _num(ls.z_equivalent)}


def _marginal_decay(prep: _Prepared, spec: EvaluationSpec, feature: FeatureData, variant: str,
                    month: np.ndarray, keys: np.ndarray, value: np.ndarray) -> list[dict[str, Any]]:
    """IC of the formation-m value with the one-month return of month m+k (k-1 formations later)."""
    months, span = prep.months, prep.span
    samples = prep.samples[1]
    rows: list[dict[str, Any]] = []
    first = _NAN
    for k in spec.marginal_months:
        shift = k - 1
        later = month + shift < months
        target = np.minimum(month + shift, months - 1)
        index = np.where(later, _lookup(prep.label_keys[1], keys + shift * span), -1)
        valid = (later & (index >= 0) & prep.matured[1][target]
                 & (_take(prep.label_status[1], index, LABEL_MISSING) == LABEL_VALID))
        returns = _take(prep.label_return[1], index, _NAN)
        ic, n = grouped_rank_correlation(month[valid], value[valid], returns[valid], months)
        ic[n < spec.min_names] = _NAN
        mask = samples.selection_dates.copy()
        if samples.holdout_start is not None:
            label_end = np.full(months, np.datetime64("NaT", "D"), dtype="datetime64[D]")
            if shift < months:
                label_end[:months - shift] = prep.expected_end[1][shift:]
            mask &= label_end < samples.holdout_start
        inference = _infer(ic, mask, 1)
        if k == spec.marginal_months[0]:
            first = inference.mean
        rows.append(_decay_row(prep, feature, variant, "marginal_month", k, inference,
                               inference.mean / first if first else _NAN))
    return rows


def _evaluate_feature(prep: _Prepared, inputs: BasisInputs, spec: EvaluationSpec, feature_id: str,
                      catalog: Mapping[str, CatalogFeature] | None = None
                      ) -> tuple[dict[str, list[Any]], dict[str, Any]]:
    feature = inputs.load_feature(feature_id)
    if feature.feature_id != feature_id:
        raise EvaluationInputError(f"loader returned {feature.feature_id} for {feature_id}")
    if feature.expected_sign not in (-1, 0, 1):
        raise EvaluationInputError(f"{feature_id}: expected_sign must be +1, -1 or 0 (two-sided)")
    entry = None if catalog is None else catalog.get(feature_id)
    if entry is not None:
        if feature.expected_sign != entry.expected_sign:
            raise EvaluationInputError(f"{feature_id}: expected_sign {feature.expected_sign} differs from the R1a "
                                       f"catalog ({entry.expected_sign})")
        feature = replace(feature, anomaly_class=feature.anomaly_class or entry.anomaly_class,
                          hypothesis_family=feature.hypothesis_family or entry.hypothesis_family,
                          population=feature.population or entry.population,
                          evidence_class=feature.evidence_class or entry.evidence_class,
                          publication_year=feature.publication_year or entry.publication_year)
    variants = [v for v in inputs.variants_by_feature.get(feature_id, ())
                if spec.variants is None or v in spec.variants]
    out: dict[str, list[Any]] = {"cells": [], "slices": [], "quantiles": [], "decay": [], "series": []}
    digest = hashlib.sha256(feature_id.encode("utf-8"))
    total = 0
    unlinked_scope = _has_unlinked_lines(feature)
    for variant in variants:
        arrays = _variant_arrays(prep, feature, variant)
        digest.update(variant.encode("utf-8"))
        digest.update(_array_digest(arrays.month, arrays.security, arrays.value, arrays.thin,
                                    arrays.coverage).encode("ascii"))
        total += arrays.rows
        for key, rows in _evaluate_variant(prep, spec, feature, variant, arrays, inputs.meta,
                                           unlinked_scope).items():
            out[key].extend(rows)
    return out, {"basis": prep.basis, "feature_id": feature_id, "status": "evaluated",
                 "expected_sign": int(feature.expected_sign), "variants": _canonical(variants),
                 "value_rows": total, "values_sha256": digest.hexdigest()}


@dataclass(frozen=True)
class _BasisInfo:
    status: str
    meta: Mapping[str, Any]
    declared: frozenset[tuple[str, str]]


def _subset_excluded(spec: EvaluationSpec, feature_id: str, variant: str, h: int | None = None) -> bool:
    return ((spec.features is not None and feature_id not in spec.features)
            or (spec.variants is not None and variant not in spec.variants)
            or (h is not None and h not in spec.horizons_months))


def _is_subset(spec: EvaluationSpec) -> bool:
    return spec.features is not None or spec.variants is not None or spec.horizons_months != DEFAULT_HORIZONS


def _status_reason(info: _BasisInfo, feature_id: str, status: str) -> str:
    """Why a status-only cell has no statistics (R2b's catalog status for an unproduced feature)."""
    if status == BASIS_UNTESTABLE:
        return "basis_untestable"
    if status == EXCLUDED_BY_SUBSET:
        return "run_subset"
    listed = info.meta.get("feature_catalog_status")
    if not isinstance(listed, Mapping):
        return "not_supplied"
    if feature_id not in listed:
        return "r2b:not_listed"
    return f"r2b:{listed[feature_id]}" if not str(listed[feature_id]).startswith("built") else "r2b:variant_not_built"


def _status_row(basis: str, info: _BasisInfo, spec: EvaluationSpec, feature_id: str, variant: str, h: int,
                status: str, catalog: Mapping[str, CatalogFeature] | None) -> dict[str, Any]:
    """A cell with a status and no statistics (untestable basis, not produced, excluded by subset)."""
    meta = info.meta
    entry = None if catalog is None else catalog.get(feature_id)
    return {"basis": basis, "feature_id": feature_id, "variant": variant, "horizon_months": h,
            "horizon_sessions": HORIZON_SESSIONS[h], "sample": "full" if spec.split is None else "selection",
            "status": status, "status_reason": _status_reason(info, feature_id, status),
            "anomaly_class": None if entry is None else entry.anomaly_class,
            "hypothesis_family": None if entry is None else entry.hypothesis_family,
            "expected_sign": None if entry is None else entry.expected_sign,
            "identity_basis": meta.get("identity_basis"), "universe_basis": meta.get("universe_basis"),
            "classification_basis": meta.get("classification_basis"),
            "availability_basis": meta.get("availability_basis"), "label_basis": LABEL_BASIS,
            "universe_rule": UNIVERSE_RULE, "fm_standardized": variant in STANDARDIZED_VARIANTS,
            "formations_usable": 0, "feature_rows": 0, "ic_hlz_pass": False}


def _status_cells(con: duckdb.DuckDBPyConnection, run_id: str, spec: EvaluationSpec,
                  info: Mapping[str, _BasisInfo], catalog: Mapping[str, CatalogFeature] | None
                  ) -> tuple[list[dict[str, Any]], dict[str, dict[str, int]]]:
    """Status rows that complete the expected family, and per-basis completeness counts.

    Expected cells: catalog feature x catalog variants x default horizons (or, without a
    catalog, the produced and declared cells). On a testable basis an expected cell the
    engine did not produce is ``excluded_by_subset`` when the run's subset left it out,
    else ``not_produced``; a data-less basis reports every expected, declared or produced
    (feature, variant) as ``untestable_strict``.
    """
    produced: dict[str, set[tuple[str, str, int]]] = {}
    for basis, feature_id, variant, h in con.execute(f"""
        SELECT basis, feature_id, variant, horizon_months FROM research_eval_cells
        WHERE run_id=? AND status NOT IN ({', '.join('?' * len(STATUS_ONLY))})
    """, [run_id, *STATUS_ONLY]).fetchall():
        produced.setdefault(str(basis), set()).add((str(feature_id), str(variant), int(h)))
    produced_pairs = {(f, v) for cells in produced.values() for f, v, _ in cells}
    expected = sorted((f, v) for f, entry in (catalog or {}).items() for v in entry.variants)
    horizons = DEFAULT_HORIZONS if catalog is not None else spec.horizons_months
    rows: list[dict[str, Any]] = []
    counts: dict[str, dict[str, int]] = {}
    for basis, item in sorted(info.items()):
        done = produced.get(basis, set())
        if item.status == BASIS_UNTESTABLE:
            declared = {(f, v) for f, v in item.declared if not _subset_excluded(spec, f, v)}
            for feature_id, variant in sorted(set(expected) | declared | produced_pairs):
                for h in sorted(set(horizons) | set(spec.horizons_months)):
                    rows.append(_status_row(basis, item, spec, feature_id, variant, h, BASIS_UNTESTABLE, catalog))
            continue
        missing = excluded = 0
        for feature_id, variant in expected:
            for h in horizons:
                if (feature_id, variant, h) in done:
                    continue
                status = EXCLUDED_BY_SUBSET if _subset_excluded(spec, feature_id, variant, h) else NOT_PRODUCED
                missing += status == NOT_PRODUCED
                excluded += status == EXCLUDED_BY_SUBSET
                rows.append(_status_row(basis, item, spec, feature_id, variant, h, status, catalog))
        outside = len({f for f, _, _ in done if catalog is not None and f not in catalog})
        counts[basis] = {"expected_cells": len(expected) * len(horizons), "produced_cells": len(done),
                         "not_produced": missing, "excluded_by_subset": excluded,
                         "features_outside_catalog": outside}
    return rows, counts


def _finish_family(con: duckdb.DuckDBPyConnection, run_id: str, spec: EvaluationSpec, info: Mapping[str, _BasisInfo],
                   catalog: Mapping[str, CatalogFeature] | None,
                   transaction: Callable[[], AbstractContextManager[Any]], *,
                   catalog_info: Mapping[str, Any] | None = None) -> dict[str, Any]:
    """Rebuild the status rows, then compute and store the family columns of every cell."""
    provenance = dict(catalog_info or {"catalog_source": None if catalog is None else "injected"})
    rows, counts = _status_cells(con, run_id, spec, info, catalog)
    with transaction():
        con.execute(f"DELETE FROM research_eval_cells WHERE run_id=? AND status IN "
                    f"({', '.join('?' * len(STATUS_ONLY))})", [run_id, *STATUS_ONLY])
        _insert(con, "cells", run_id, _frame(rows, "cells"))
    del rows
    cells = con.execute("""
        SELECT basis, feature_id, variant, horizon_months, status, ic_robust_p, ic_hlz_pass,
               sharpe, sharpe_skew, sharpe_kurt, sharpe_n
        FROM research_eval_cells WHERE run_id=? ORDER BY basis, feature_id, variant, horizon_months
    """, [run_id]).df()
    if not len(cells):
        return {"family_complete": False, "catalog_sha256": _catalog_sha(catalog), **provenance, "bases": counts,
                "bases_supplied": sorted(info)}
    family, summary = compute_family(cells)
    with transaction():
        typed = _typed(family, [(name, kind) for name, kind in CELL_COLUMNS if name in family.columns])
        con.register("_ev_family", typed)
        try:
            assignments = ", ".join(f"{name}=f.{name}" for name in FAMILY_COLUMNS)
            con.execute(f"""
                UPDATE research_eval_cells AS c SET {assignments} FROM _ev_family f
                WHERE c.run_id=? AND c.basis=f.basis AND c.feature_id=f.feature_id
                  AND c.variant=f.variant AND c.horizon_months=f.horizon_months
            """, [run_id])
        finally:
            con.unregister("_ev_family")
    # Shrinkage is a cross-feature report, fitted after every feature is written; never in FAMILY_COLUMNS.
    eb_cells = con.execute("""SELECT basis, feature_id, variant, horizon_months, status, evidence_class,
                             ic_mean, ic_nw_se FROM research_eval_cells WHERE run_id=?""", [run_id]).df()
    eb = empirical_bayes_ic(eb_cells)
    with transaction():
        con.register("_ev_eb", _typed(eb, [(name, kind) for name, kind in CELL_COLUMNS if name in eb]))
        try:
            con.execute("""UPDATE research_eval_cells AS c
                SET ic_eb_mean=e.ic_eb_mean, ic_eb_weight=e.ic_eb_weight, ic_eb_tau2=e.ic_eb_tau2,
                    ic_eb_n=e.ic_eb_n, ic_eb_basis=e.ic_eb_basis FROM _ev_eb e
                WHERE c.run_id=? AND c.basis=e.basis AND c.feature_id=e.feature_id
                  AND c.variant=e.variant AND c.horizon_months=e.horizon_months""", [run_id])
        finally:
            con.unregister("_ev_eb")
    missing = sum(item["not_produced"] for item in counts.values())
    summary.update({
        "catalog_sha256": _catalog_sha(catalog),
        **provenance,
        "catalog_anchored": catalog is not None,
        "subset": None if not _is_subset(spec) else {
            "features": None if spec.features is None else list(spec.features),
            "variants": None if spec.variants is None else list(spec.variants),
            "horizons_months": list(spec.horizons_months)},
        "bases": counts,
        "bases_supplied": sorted(info),
        "family_complete": (catalog is not None and not _is_subset(spec) and missing == 0
                            and set(BASES) <= set(info)),
    })
    return summary


def empirical_bayes_ic(cells: pd.DataFrame) -> pd.DataFrame:
    """Reported normal-normal IC shrinkage toward zero across replications within basis/variant/horizon.

    JKP-style empirical Bayes method of moments: tau²=max(mean(IC²-SE²),0), posterior weight
    tau²/(tau²+SE²). Uses every estimable replication (no significance selection), at least 3 distinct
    features, and NW sampling variances. Variants/horizons are never pooled as independent observations.
    Run reducers must call this on the full wave; per-feature workers correctly report unavailable.
    """
    cells = cells.reset_index(drop=True)
    keys = ["basis", "feature_id", "variant", "horizon_months"]
    out = cells[keys].copy()
    for column in ("ic_eb_mean", "ic_eb_weight", "ic_eb_tau2"):
        out[column] = np.nan
    out["ic_eb_n"], out["ic_eb_basis"] = 0, "insufficient_replication_pool"
    if not {"evidence_class", "ic_mean", "ic_nw_se"} <= set(cells):
        return out
    means = pd.to_numeric(cells.ic_mean, errors="coerce").to_numpy(float)
    se = pd.to_numeric(cells.ic_nw_se, errors="coerce").to_numpy(float)
    valid = (cells.evidence_class.eq("replication").to_numpy() & np.isfinite(means) & np.isfinite(se) & (se > 0))
    out.loc[~cells.evidence_class.eq("replication"), "ic_eb_basis"] = "not_replication"
    for _, group in cells.reset_index(drop=True).groupby(["basis", "variant", "horizon_months"], sort=True):
        index = group.index.to_numpy()[valid[group.index.to_numpy()]]
        out.loc[index, "ic_eb_n"] = len(index)
        if len(index) < 3 or cells.iloc[index].feature_id.nunique() != len(index):
            continue
        tau2 = max(float(np.mean(means[index] ** 2 - se[index] ** 2)), 0.0)
        weight = tau2 / (tau2 + se[index] ** 2)
        out.loc[index, "ic_eb_mean"] = weight * means[index]
        out.loc[index, "ic_eb_weight"] = weight
        out.loc[index, "ic_eb_tau2"] = tau2
        out.loc[index, "ic_eb_basis"] = "normal_normal_zero_prior_moment_NW_variance"
    return out


def _basis_attrition(prep: _Prepared, spec: EvaluationSpec) -> list[dict[str, Any]]:
    """Label accounting over the ranked universe (valid primary cohort rows) at matured formations."""
    rows: list[dict[str, Any]] = []
    member_keys = prep.context_keys[prep.context_universe]
    for h in spec.horizons_months:
        matured = prep.matured[h]
        keys = prep.label_keys[h]
        at_matured = matured[keys // prep.span] if len(keys) else np.zeros(0, bool)
        status = prep.label_status[h][at_matured]
        terminal = prep.label_terminal[h][at_matured]
        valid = status == LABEL_VALID
        cohort = member_keys[matured[member_keys // prep.span]] if len(member_keys) else member_keys
        measures = {
            "formations_formed": int(prep.formed.sum()),
            "formations_matured": int(matured.sum()),
            "label_rows": int(at_matured.sum()),
            "label_valid": int(valid.sum()),
            "label_invalid": int((status == LABEL_INVALID).sum()),
            "label_unsupported": int((status == LABEL_UNSUPPORTED).sum()),
            "stitched_observed": int((valid & (terminal == TERMINAL_OBSERVED)).sum()),
            "stitched_policy": int((valid & (terminal == TERMINAL_POLICY)).sum()),
            "cohort_rows": len(cohort),
            "cohort_without_label": int((_lookup(keys, cohort) < 0).sum()) if len(cohort) else 0,
        }
        rows.extend({"scope": prep.basis, "horizon_months": h, "measure": name, "value": float(v)}
                    for name, v in measures.items())
    return rows


# ---------------------------------------------------------------------------
# Family-wide multiple testing and deflated Sharpe
# ---------------------------------------------------------------------------

FAMILY_COLUMNS = ("family_member", "bh_q", "holm_p", "bh_discovery", "hlz_pass", "psr", "dsr", "dsr_z",
                  "dsr_benchmark", "dsr_n_trials", "dsr_effective_n", "dsr_sharpe_variance", "family_best")
#: Policy v4 family scopes: the gating family is the primary cells of a wave's gating hypotheses;
#: every cell (gating or not) is in the reported family, whose statistics are the R3b family columns.
FAMILY_SCOPE_GATING, FAMILY_SCOPE_REPORTED = "gating", "reported"


def family_scope(cells: pd.DataFrame, *, gating_features: Iterable[str], primary_variant: str,
                 primary_horizon: int) -> pd.Series:
    """Policy v4 scope of every cell: ``gating`` for the primary cell (``primary_variant`` x
    ``primary_horizon``) of a gating hypothesis (registered, research-eligible, not reported-only),
    else ``reported``. Untestable-strict cells are reported (outside every family, as in R3b)."""
    gating = set(gating_features)
    chosen = (cells["feature_id"].astype(str).isin(gating) & (cells["variant"].astype(str) == primary_variant)
              & (pd.to_numeric(cells["horizon_months"]) == int(primary_horizon))
              & (cells["status"].astype(str) != BASIS_UNTESTABLE))
    return pd.Series(np.where(chosen.to_numpy(dtype=bool), FAMILY_SCOPE_GATING, FAMILY_SCOPE_REPORTED),
                     index=cells.index, name="family_scope")


def gating_bh_q[K: Hashable](p_values: Mapping[K, float | None], *,
                             prior_hypotheses: int = 0) -> dict[K, float | None]:
    """Benjamini-Hochberg q-values of a policy v4 gating family.

    ``p_values``: each gating hypothesis's evidence-class p-value (None = untested, counted at
    p = 1). ``prior_hypotheses`` gating hypotheses of earlier registered waves (trial registry,
    cumulative across waves) join the family at p = 1: a later wave never tests against a
    smaller family than the waves before it. Returns the q-values of ``p_values``' keys.
    """
    if isinstance(prior_hypotheses, bool) or not isinstance(prior_hypotheses, int) or prior_hypotheses < 0:
        raise EvaluationInputError("prior_hypotheses must be a non-negative integer")
    padded: dict[Any, float | None] = {("family", key): value for key, value in p_values.items()}
    padded.update({("prior_wave", i): None for i in range(prior_hypotheses)})
    q = stats.benjamini_hochberg(padded) if padded else {}
    return {key: q[("family", key)] for key in p_values}


def compute_family(cells: pd.DataFrame) -> tuple[pd.DataFrame, dict[str, Any]]:
    """Family statistics for every cell (keyed by basis, feature_id, variant, horizon_months)."""
    key_columns = ["basis", "feature_id", "variant", "horizon_months"]
    frame = cells.reset_index(drop=True)
    keys = [(str(b), str(f), str(v), int(h)) for b, f, v, h in frame[key_columns].itertuples(index=False, name=None)]
    status = frame["status"].astype(str).to_numpy()
    member = np.isin(status, FAMILY_STATUSES)
    p_values: dict[tuple[Any, ...], float | None] = {}
    for i in np.flatnonzero(member):
        p = _num(frame["ic_robust_p"].iloc[i]) if status[i] == "tested" else None
        p_values[keys[i]] = p
    bh = stats.benjamini_hochberg(p_values) if p_values else {}
    holm = stats.holm(p_values) if p_values else {}
    n_trials = len(p_values)
    sharpe = pd.to_numeric(frame["sharpe"], errors="coerce").to_numpy(dtype=float)
    sharpe_n = pd.to_numeric(frame["sharpe_n"], errors="coerce").to_numpy(dtype=float)
    tested = member & (status == "tested") & np.isfinite(sharpe) & (sharpe_n >= 3)
    horizons = frame["horizon_months"].to_numpy(dtype=np.int64)
    variances: dict[int, float] = {}
    best: dict[int, int] = {}
    summary_horizons: dict[str, Any] = {}
    for h in sorted(set(horizons[tested].tolist())):
        chosen = np.flatnonzero(tested & (horizons == h))
        values = sharpe[chosen]
        variances[h] = float(np.var(values, ddof=1)) if len(values) >= 2 else (0.0 if n_trials == 1 else _NAN)
        best[h] = int(chosen[int(np.argmax(values))])
    records = []
    for i, key in enumerate(keys):
        record: dict[str, Any] = dict(zip(key_columns, key, strict=True))
        record.update({"family_member": bool(member[i]), "bh_q": None, "holm_p": None, "bh_discovery": False,
                       "hlz_pass": bool(member[i] and status[i] == "tested" and bool(frame["ic_hlz_pass"].iloc[i])),
                       "psr": None, "dsr": None, "dsr_z": None, "dsr_benchmark": None, "dsr_n_trials": None,
                       "dsr_effective_n": None, "dsr_sharpe_variance": None, "family_best": False})
        if member[i]:
            q = bh.get(key)
            record["bh_q"], record["holm_p"] = q, holm.get(key)
            record["bh_discovery"] = q is not None and q <= BH_ALPHA
        if tested[i]:
            h = int(horizons[i])
            record["dsr_n_trials"], record["dsr_sharpe_variance"] = n_trials, _num(variances[h])
            record["dsr_effective_n"] = stats.effective_sample_size(int(sharpe_n[i]), h)
            record["family_best"] = best[h] == i
            with suppress(ValueError, ArithmeticError):  # PSR vs zero, floor(n/h) independent returns
                record["psr"] = _num(stats.probabilistic_sharpe_ratio(
                    float(sharpe[i]), 0.0, n_obs=int(sharpe_n[i]), skewness=float(frame["sharpe_skew"].iloc[i]),
                    kurtosis=float(frame["sharpe_kurt"].iloc[i]), horizon_periods=h))
            try:
                result = stats.deflated_sharpe_ratio(
                    float(sharpe[i]), n_obs=int(sharpe_n[i]),
                    skewness=float(frame["sharpe_skew"].iloc[i]), kurtosis=float(frame["sharpe_kurt"].iloc[i]),
                    n_trials=n_trials, sharpe_variance=variances[h], horizon_periods=h)
            except (ValueError, ArithmeticError):
                pass
            else:
                record.update({"dsr": _num(result.deflated_sharpe_ratio), "dsr_z": _num(result.z),
                               "dsr_benchmark": _num(result.benchmark_sharpe),
                               "dsr_effective_n": result.effective_n_obs})
        records.append(record)
    family = pd.DataFrame(records, columns=[*key_columns, *FAMILY_COLUMNS])
    for h, i in best.items():
        summary_horizons[str(h)] = {
            "tested": int((tested & (horizons == h)).sum()), "sharpe_variance": _num(variances[h]),
            "best": {"key": list(keys[i]), "sharpe": _num(sharpe[i]), "dsr": _num(family.loc[i, "dsr"])}}
    composition: dict[str, dict[str, int]] = {}
    for (basis, _, _, _), cell_status in zip(keys, status, strict=True):
        by_status = composition.setdefault(basis, {})
        by_status[str(cell_status)] = by_status.get(str(cell_status), 0) + 1
    summary = {
        "family_cells": int(member.sum()), "n_trials": n_trials, "tested_cells": int((status == "tested").sum()),
        "bh_alpha": BH_ALPHA, "bh_discoveries": int(family["bh_discovery"].sum()),
        "holm_discoveries": int(sum(1 for v in holm.values() if v is not None and v <= BH_ALPHA)),
        "hlz_passes": int(family["hlz_pass"].sum()),
        "family_rule": ("every expected cell on a testable basis; only tested cells carry their IC robust p, "
                        "insufficient_formations/no_values/not_produced/excluded_by_subset enter BH/Holm as p=1; "
                        "untestable_strict cells are reported outside the family"),
        "significance": ("IC mean, EWC fixed-b robust p / z (stats.mean_inference, horizon_periods=h); NW t "
                         "secondary; decile spreads, FM slopes, PSR and DSR (EW10 long-short, horizon_periods=h, "
                         "DSR n_trials = the whole family) are supporting evidence"),
        "composition": {basis: dict(sorted(items.items())) for basis, items in sorted(composition.items())},
        "horizons": summary_horizons,
    }
    return family, summary


# ---------------------------------------------------------------------------
# Result tables (schema, typing, digest)
# ---------------------------------------------------------------------------

_D, _I, _B, _V = "DOUBLE", "INTEGER", "BOOLEAN", "VARCHAR"
_IDENTITY = (("basis", _V), ("feature_id", _V), ("variant", _V), ("horizon_months", _I))
REPORTED_SERIES_NAMES = (
    *(f"jkp_{w}{q}" for q in JKP_QUANTILES for w in JKP_WEIGHTINGS),
    *(f"trade_{p}_{m}" for p in (*SPAN_PORTFOLIOS, "buy_hold") for m in ("gross", "turnover", "cost", "net")),
    "fmj_ols", "fmj_wls", "beta_neutral_ic",
)
REPORTED_COUNT_NAMES = tuple(f"trade_{p}_{m}" for p in (*SPAN_PORTFOLIOS, "buy_hold")
                             for m in ("held", "priced", "drift_held", "drift_priced"))
REPORTED_CELL_COLUMNS: tuple[tuple[str, str], ...] = (
    ("reported_metrics_version", _V), ("evidence_class", _V), ("publication_year", _I),
    *((name, _V) for name in ("jkp_breakpoint_basis", "jkp_cap_basis", "cost_basis", "capacity_basis", "ea_basis",
                              "beta_neutral_basis", "fmj_controls", "fmj_controls_missing", "fmj_self_controls_omitted")),
    *((f"{prefix}_{suffix}", kind) for prefix in (*REPORTED_SERIES_NAMES, "beta_neutral_ls",
        *(f"fmj_{w}_k{k}" for w in ("ols", "wls") for k in (3, 6, 12)),
        "publication_pre_ic", "publication_post_ic") for suffix, kind in (("mean", _D), ("nw_t", _D), ("n", _I))),
    *((f"trade_{p}_{suffix}", kind) for p in (*SPAN_PORTFOLIOS, "buy_hold") for suffix, kind in
      (("break_even_bps", _D), ("break_even_basis", _V), ("net10_mean", _D), ("net25_mean", _D), ("net50_mean", _D),
       ("high_turnover", _B), ("ea_share", _D), ("ea_n", _I))),
    *((f"span_{p}_{model}_{suffix}", kind) for p in SPAN_PORTFOLIOS for model in SPAN_MODEL_NAMES
      for suffix, kind in (("alpha", _D), ("alpha_nw_t", _D), ("alpha_robust_p", _D), ("r2", _D), ("n", _I),
                           ("window_basis", _V), ("claimable", _B), ("basis", _V))),
    *((f"capacity_decile{q}_usd", _D) for q in range(1, 11)),
    *((f"ic_age{k}_mean", _D) for k in SIGNAL_AGES),
    ("ic_half_life_months", _D), ("ic_half_life_basis", _V), ("ic_nw_se", _D),
    ("ic_mde_80pct", _D), ("ic_mde_basis", _V),
    ("osap_acronym", _V), ("osap_original_t", _D), ("osap_atx_t", _D), ("osap_basis", _V),
    ("ic_eb_mean", _D), ("ic_eb_weight", _D), ("ic_eb_tau2", _D), ("ic_eb_n", _I), ("ic_eb_basis", _V),
    *((f"{name}_n", "BIGINT") for name in REPORTED_COUNT_NAMES),
)
CELL_COLUMNS: tuple[tuple[str, str], ...] = (
    *_IDENTITY, ("horizon_sessions", _I), ("status", _V), ("status_reason", _V), ("sample", _V),
    ("anomaly_class", _V), ("hypothesis_family", _V),
    ("expected_sign", _I), ("identity_basis", _V), ("universe_basis", _V), ("classification_basis", _V),
    ("availability_basis", _V), ("label_basis", _V), ("universe_rule", _V), ("fm_standardized", _B),
    ("formations_calendar", _I), ("formations_formed", _I), ("formations_with_values", _I),
    ("formations_thin", _I), ("formations_matured", _I), ("formations_usable", _I),
    ("feature_rows", "BIGINT"), ("labels_valid", "BIGINT"), ("labels_missing", "BIGINT"),
    ("labels_invalid", "BIGINT"), ("labels_unsupported", "BIGINT"), ("stitched_observed", "BIGINT"),
    ("stitched_policy", "BIGINT"), ("mean_names", _D), ("min_names_used", _I), ("universe_scope", _V),
    ("values_unlinked_lines", "BIGINT"), ("mean_universe_names", _D),
    ("mean_coverage", _D), ("min_coverage", _D), ("mean_feature_coverage", _D),
    ("values_dropped_not_valid", "BIGINT"), ("values_dropped_not_primary", "BIGINT"),
    ("ic_mean", _D), ("ic_sd", _D), ("ic_ir", _D), ("ic_positive_share", _D), ("ic_n", _I),
    ("ic_robust_t", _D), ("ic_robust_p", _D), ("ic_robust_df", _I), ("ic_z", _D), ("ic_ci_low", _D),
    ("ic_ci_high", _D), ("ic_nw_t", _D), ("ic_nw_p", _D), ("ic_nw_lags", _I), ("ic_boot_low", _D),
    ("ic_boot_high", _D), ("ic_boot_pct_low", _D), ("ic_boot_pct_high", _D), ("ic_hlz_pass", _B),
    ("ls_ew10_mean", _D), ("ls_ew10_robust_t", _D), ("ls_ew10_robust_p", _D), ("ls_ew10_z", _D),
    ("ls_ew10_nw_t", _D), ("ls_ew10_hit_rate", _D), ("ls_ew10_boot_low", _D), ("ls_ew10_boot_high", _D),
    ("ls_ew10_n", _I),
    ("ls_vw10_mean", _D), ("ls_vw10_robust_t", _D), ("ls_vw10_robust_p", _D), ("ls_vw10_z", _D),
    ("ls_ew5_mean", _D), ("ls_ew5_robust_t", _D), ("ls_ew5_robust_p", _D), ("ls_ew5_z", _D),
    ("ls_vw5_mean", _D), ("ls_vw5_robust_t", _D), ("ls_vw5_robust_p", _D), ("ls_vw5_z", _D),
    ("ls_nyse_ew10_mean", _D), ("ls_nyse_ew10_robust_t", _D), ("ls_nyse_ew10_robust_p", _D), ("ls_nyse_ew10_z", _D),
    ("ls_nyse_vw10_mean", _D), ("ls_nyse_vw10_robust_t", _D), ("ls_nyse_vw10_robust_p", _D), ("ls_nyse_vw10_z", _D),
    ("ls_nyse_n", _I), ("venue_basis", _V), ("venue_pit_share", _D),
    ("mono_ew10", _D), ("mono_vw10", _D), ("mono_ew5", _D), ("mono_vw5", _D),
    ("fm_slope", _D), ("fm_robust_t", _D), ("fm_robust_p", _D), ("fm_z", _D), ("fm_nw_t", _D), ("fm_n", _I),
    ("fmc_slope", _D), ("fmc_robust_t", _D), ("fmc_robust_p", _D), ("fmc_z", _D), ("fmc_nw_t", _D),
    ("fmc_n", _I), ("fmc_controls", _V),
    ("rank_autocorr_1m", _D), ("top_turnover_1m", _D), ("bottom_turnover_1m", _D), ("top_turnover_h", _D),
    ("bottom_turnover_h", _D),
    ("net10_mean", _D), ("net10_z", _D), ("net25_mean", _D), ("net25_robust_t", _D), ("net25_robust_p", _D),
    ("net25_z", _D), ("net50_mean", _D), ("net50_z", _D),
    ("ic_lag1_mean", _D), ("ic_lag1_robust_p", _D), ("ic_lag1_z", _D), ("ic_lag1_n", _I),
    ("sharpe", _D), ("sharpe_skew", _D), ("sharpe_kurt", _D), ("sharpe_n", _I),
    ("family_member", _B), ("bh_q", _D), ("holm_p", _D), ("bh_discovery", _B), ("hlz_pass", _B), ("psr", _D),
    ("dsr", _D),
    ("dsr_z", _D), ("dsr_benchmark", _D), ("dsr_n_trials", _I), ("dsr_effective_n", _I),
    ("dsr_sharpe_variance", _D), ("family_best", _B),
    *REPORTED_CELL_COLUMNS,
)
SLICE_COLUMNS: tuple[tuple[str, str], ...] = (
    *_IDENTITY, ("slice_kind", _V), ("slice_name", _V), ("formations", _I), ("purged", _I), ("embargoed", _I),
    ("mean_names", _D), ("name_share", _D), ("ic_mean", _D), ("ic_robust_t", _D), ("ic_robust_p", _D),
    ("ic_robust_df", _I), ("ic_z", _D), ("ic_nw_t", _D), ("ls_kind", _V), ("ls_mean", _D), ("ls_robust_t", _D),
    ("ls_robust_p", _D), ("ls_z", _D), ("venue_basis", _V), ("venue_pit_share", _D),
)
_DATE = "DATE"
SERIES_COLUMNS: tuple[tuple[str, str], ...] = (
    *_IDENTITY, ("formation_date", _DATE), ("segment", _V), ("in_selection", _B), ("matured", _B), ("usable", _B),
    ("universe_names", _I), ("n_values", _I), ("n_unlinked", _I), ("n_used", _I), ("coverage", _D),
    ("dropped_not_valid", _I),
    ("dropped_not_primary", _I), ("venue_basis", _V), ("nyse_pit_names", _I), ("ic", _D), ("ls_ew10", _D),
    ("ls_vw10", _D),
    ("ls_ew5", _D), ("ls_vw5", _D), ("ls_nyse_ew10", _D), ("ls_nyse_vw10", _D), ("fm_slope", _D), ("fmc_slope", _D),
    ("net25_ew10", _D), ("ic_lag1", _D),
    *((name, _D) for name in REPORTED_SERIES_NAMES),
    *((name, _I) for name in REPORTED_COUNT_NAMES),
)
QUANTILE_COLUMNS: tuple[tuple[str, str], ...] = (
    *_IDENTITY, ("weighting", _V), ("n_quantiles", _I), ("quantile", _I), ("mean_return", _D),
    ("formations", _I), ("mean_names", _D),
)
DECAY_COLUMNS: tuple[tuple[str, str], ...] = (
    ("basis", _V), ("feature_id", _V), ("variant", _V), ("kind", _V), ("horizon_months", _I), ("ic_mean", _D),
    ("n", _I), ("robust_t", _D), ("robust_p", _D), ("z", _D), ("ratio_to_first", _D),
)
ATTRITION_COLUMNS: tuple[tuple[str, str], ...] = (
    ("scope", _V), ("horizon_months", _I), ("measure", _V), ("value", _D),
)
FEATURE_INPUT_COLUMNS: tuple[tuple[str, str], ...] = (
    ("basis", _V), ("feature_id", _V), ("status", _V), ("expected_sign", _I), ("variants", _V),
    ("value_rows", "BIGINT"), ("values_sha256", _V),
)
RESULT_TABLES: dict[str, tuple[str, tuple[tuple[str, str], ...], tuple[str, ...]]] = {
    "cells": ("research_eval_cells", CELL_COLUMNS, ("basis", "feature_id", "variant", "horizon_months")),
    "slices": ("research_eval_slices", SLICE_COLUMNS,
               ("basis", "feature_id", "variant", "horizon_months", "slice_kind", "slice_name")),
    "quantiles": ("research_eval_quantiles", QUANTILE_COLUMNS,
                  ("basis", "feature_id", "variant", "horizon_months", "weighting", "n_quantiles", "quantile")),
    "decay": ("research_eval_decay", DECAY_COLUMNS, ("basis", "feature_id", "variant", "kind", "horizon_months")),
    "attrition": ("research_eval_attrition", ATTRITION_COLUMNS, ("scope", "horizon_months", "measure")),
    "feature_inputs": ("research_eval_feature_inputs", FEATURE_INPUT_COLUMNS, ("basis", "feature_id")),
    "series": ("research_eval_series", SERIES_COLUMNS,
               ("basis", "feature_id", "variant", "horizon_months", "formation_date")),
}
_RUN_DDL = """
    CREATE TABLE IF NOT EXISTS research_eval_runs (
        run_id VARCHAR PRIMARY KEY,
        status VARCHAR NOT NULL,
        evaluation_version VARCHAR NOT NULL,
        spec_json VARCHAR NOT NULL,
        spec_sha256 VARCHAR NOT NULL,
        code_sha256 VARCHAR NOT NULL,
        inputs_json VARCHAR,
        inputs_sha256 VARCHAR,
        results_sha256 VARCHAR,
        family_json VARCHAR,
        blockers_json VARCHAR,
        diagnostic_json VARCHAR,
        created_at TIMESTAMP NOT NULL,
        finished_at TIMESTAMP,
        family_complete BOOLEAN
    )"""
_SCHEMA_VERSIONS = {1: "monthly_evaluation", 2: "catalog_family_universe_series", 3: "reported_economics",
                    4: "reported_fixed_book_coverage"}


def ensure_evaluation_schema(con: duckdb.DuckDBPyConnection) -> None:
    """Create or upgrade the ``research_eval_*`` tables to schema version 3.

    Version 2 adds the catalog-anchored family flag, status reasons, the ranked-universe
    and point-in-time venue columns, PSR and ``research_eval_series``; a version-1 store
    is upgraded by adding the
    new columns (writers name their columns, so the physical order does not matter).
    Kept inside this module until the research-store owner registers it as a store
    migration; the version table refuses a newer, unknown schema.
    Version 3 adds reported-only economics; every pre-existing column and gating rule is retained.
    Version 4 adds held/priced coverage for fixed-weight trading books and their realized drift inputs.
    """
    con.execute("""
        CREATE TABLE IF NOT EXISTS research_eval_schema (
            version INTEGER PRIMARY KEY, name VARCHAR NOT NULL, applied_at TIMESTAMP NOT NULL)""")
    versions = {int(row[0]) for row in con.execute("SELECT version FROM research_eval_schema").fetchall()}
    if versions - set(_SCHEMA_VERSIONS):
        raise RuntimeError(f"research evaluation schema has unknown versions {sorted(versions)}; code is older")
    if EVALUATION_SCHEMA_VERSION in versions:
        return
    con.execute(_RUN_DDL)
    for table, columns, _ in RESULT_TABLES.values():
        body = ", ".join(f"{name} {kind}" for name, kind in columns)
        con.execute(f"CREATE TABLE IF NOT EXISTS {table} (run_id VARCHAR NOT NULL, {body})")
    if versions:  # upgrade an older schema in place
        con.execute("ALTER TABLE research_eval_runs ADD COLUMN IF NOT EXISTS family_complete BOOLEAN")
        for table, columns, _ in RESULT_TABLES.values():
            for name, kind in columns:
                con.execute(f"ALTER TABLE {table} ADD COLUMN IF NOT EXISTS {name} {kind}")
    now = dt.datetime.now(dt.UTC).replace(tzinfo=None)
    for version, name in sorted(_SCHEMA_VERSIONS.items()):
        if version not in versions:
            con.execute("INSERT INTO research_eval_schema VALUES (?, ?, ?)", [version, name, now])


def _typed(frame: pd.DataFrame, columns: Sequence[tuple[str, str]]) -> pd.DataFrame:
    data: dict[str, Any] = {}
    length = len(frame)
    for name, kind in columns:
        series = frame[name] if name in frame else pd.Series([None] * length, dtype=object)
        series = series.reset_index(drop=True)
        if kind == _D:
            numbers = pd.to_numeric(series, errors="coerce").astype("float64")
            data[name] = numbers.where(np.isfinite(numbers))
        elif kind in (_I, "BIGINT"):
            numbers = pd.to_numeric(series, errors="coerce").astype("float64")
            data[name] = numbers.where(np.isfinite(numbers)).astype("Int64")
        elif kind == _B:
            data[name] = series.astype("boolean")
        elif kind == _DATE:
            data[name] = pd.to_datetime(series).astype("datetime64[us]")
        else:
            data[name] = pd.Series([None if pd.isna(v) else str(v) for v in series], dtype=object)
    return pd.DataFrame(data)


_INSERT_CHUNK_ROWS = 16_384


def _insert(con: duckdb.DuckDBPyConnection, key: str, run_id: str, frame: pd.DataFrame) -> None:
    """Typed insert in bounded chunks (typing a large frame at once costs ~1 kB per row)."""
    if frame is None or not len(frame):
        return
    table, columns, _ = RESULT_TABLES[key]
    names = ", ".join(name for name, _ in columns)
    select = ", ".join(f"CAST({name} AS {kind})" for name, kind in columns)
    for start in range(0, len(frame), _INSERT_CHUNK_ROWS):
        typed = _typed(frame.iloc[start:start + _INSERT_CHUNK_ROWS], columns)
        con.register("_ev_insert", typed)
        try:
            con.execute(f"INSERT INTO {table} (run_id, {names}) SELECT CAST(? AS VARCHAR), {select} "
                        f"FROM _ev_insert", [run_id])
        finally:
            con.unregister("_ev_insert")
        del typed


def _insert_frames(con: duckdb.DuckDBPyConnection, key: str, run_id: str, frames: list[pd.DataFrame]) -> None:
    """Insert many small frames, concatenated into chunks of about ``_INSERT_CHUNK_ROWS`` rows."""
    group: list[pd.DataFrame] = []
    rows = 0
    while frames:
        frame = frames.pop(0)
        group.append(frame)
        rows += len(frame)
        if rows >= _INSERT_CHUNK_ROWS or not frames:
            _insert(con, key, run_id, pd.concat(group, ignore_index=True) if len(group) > 1 else group[0])
            group, rows = [], 0


def _canon(value: Any) -> Any:
    if value is None or isinstance(value, (bool, str, int)):
        return value
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, (dt.date, dt.datetime)):
        return value.isoformat()
    return str(value)


def results_digest(con: duckdb.DuckDBPyConnection, run_id: str) -> tuple[str, dict[str, Any]]:
    """Digest of every result row of a run in key order (run_id excluded)."""
    parts: dict[str, Any] = {}
    for key, (table, columns, order) in RESULT_TABLES.items():
        digest = hashlib.sha256()
        rows = 0
        cursor = con.execute(f"SELECT {', '.join(n for n, _ in columns)} FROM {table} WHERE run_id=? "
                             f"ORDER BY {', '.join(order)}", [run_id])
        while batch := cursor.fetchmany(4096):
            for row in batch:
                digest.update(_canonical([_canon(v) for v in row]).encode("utf-8"))
                digest.update(b"\n")
                rows += 1
        parts[key] = {"rows": rows, "sha256": digest.hexdigest()}
    return _sha(_canonical(parts)), parts


# ---------------------------------------------------------------------------
# In-memory evaluation (pure engine driver)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class EvaluationTables:
    cells: pd.DataFrame
    slices: pd.DataFrame
    quantiles: pd.DataFrame
    decay: pd.DataFrame
    attrition: pd.DataFrame
    feature_inputs: pd.DataFrame
    series: pd.DataFrame
    family: dict[str, Any]
    bases: dict[str, Any]
    results_sha256: str
    table_digests: dict[str, Any]

    def frames(self) -> dict[str, pd.DataFrame]:
        return {key: getattr(self, key) for key in RESULT_TABLES}


def _selected_features(inputs: BasisInputs, spec: EvaluationSpec) -> tuple[list[str], list[str]]:
    available = sorted(inputs.variants_by_feature)
    if spec.features is None:
        return available, []
    return [f for f in available if f in spec.features], sorted(set(spec.features) - set(available))


def _basis_manifest(inputs: BasisInputs, prep: _Prepared | None, absent: Sequence[str],
                    catalog_sha: str | None) -> dict[str, Any]:
    manifest = {"basis": inputs.basis, "status": inputs.status, "meta": dict(inputs.meta),
                "digests": dict(inputs.digests), "features_absent": list(absent),
                "features": len(inputs.variants_by_feature), "catalog_sha256": catalog_sha,
                "universe_rule": UNIVERSE_RULE}
    if prep is not None:
        manifest["prepared_digests"] = prep.digests
        manifest["venue_basis_formations"] = _method_counts(prep.venue_basis, prep.formed)
        manifest["universe_member_formations"] = int(prep.universe_names.sum())
    return json.loads(_canonical(manifest))


def _label_source_attrition(diagnostics: Mapping[str, Any]) -> list[dict[str, Any]]:
    rows = []
    for record in diagnostics.get("horizons", ()):
        months = HORIZON_FORMATION_UNITS.get(int(record["horizon_days"]))
        if months is None:
            continue
        for name, value in sorted(record.items()):
            if name in ("horizon_days", "formation_units") or isinstance(value, str) or value is None:
                continue
            rows.append({"scope": "label_source", "horizon_months": months, "measure": name, "value": float(value)})
    for name, value in sorted(diagnostics.get("terminal_gaps", {}).items()):
        if value is not None:
            rows.append({"scope": "label_source", "horizon_months": 0, "measure": f"terminal_gaps_{name}",
                         "value": float(value)})
    return rows


def _frame(rows: Sequence[Any], key: str) -> pd.DataFrame:
    columns = [name for name, _ in RESULT_TABLES[key][1]]
    if rows and isinstance(rows[0], pd.DataFrame):
        return pd.concat(rows, ignore_index=True)
    return pd.DataFrame(list(rows), columns=columns) if rows else pd.DataFrame(columns=columns)


def _read_table(con: duckdb.DuckDBPyConnection, key: str, run_id: str) -> pd.DataFrame:
    table, columns, order = RESULT_TABLES[key]
    return con.execute(f"SELECT {', '.join(name for name, _ in columns)} FROM {table} WHERE run_id=? "
                       f"ORDER BY {', '.join(order)}", [run_id]).df()


def _evaluate_into(con: duckdb.DuckDBPyConnection | None, run_id: str, bases: Iterable[BasisInputs],
                   spec: EvaluationSpec, catalog: Mapping[str, CatalogFeature] | None, *,
                   transaction: Callable[[], AbstractContextManager[Any]],
                   done: frozenset[tuple[str, str]] = frozenset(), previous: Mapping[str, Any] | None = None,
                   on_manifest: Callable[[dict[str, Any]], None] | None = None,
                   sink: Callable[[str, pd.DataFrame], None] | None = None
                   ) -> tuple[dict[str, Any], dict[str, _BasisInfo]]:
    """Evaluate every basis feature by feature into ``con`` (one transaction per feature).

    ``done`` (resume) skips (basis, feature) pairs already written; ``previous`` refuses a
    resume whose basis input manifest changed. With ``sink`` the rows go to the sink
    instead (``con`` unused), so no database is open while the engine works.
    """
    manifests: dict[str, Any] = {}
    info: dict[str, _BasisInfo] = {}
    catalog_sha = _catalog_sha(catalog)

    def write(key: str, frame: pd.DataFrame) -> None:
        if sink is not None:
            sink(key, frame)
        elif con is not None:
            _insert(con, key, run_id, frame)

    def attrition_written(basis: str) -> bool:
        if sink is not None or con is None:
            return False
        row = con.execute("SELECT count(*) FROM research_eval_attrition WHERE run_id=? AND scope=?",
                          [run_id, basis]).fetchone()
        return bool(row and row[0])

    for inputs in bases:
        if inputs.basis in manifests:
            raise EvaluationInputError(f"basis {inputs.basis} supplied twice")
        if inputs.status not in (BASIS_AVAILABLE, BASIS_UNTESTABLE):
            raise EvaluationInputError(f"basis {inputs.basis}: unknown status {inputs.status!r}")
        if spec.label_sha256 is not None and inputs.status != BASIS_UNTESTABLE:
            # Policy v4 (ruling C-52): refused before the basis is prepared, so no statistic or row of
            # another label set is ever computed or written.
            read = basis_label_read(inputs.meta)
            seen = None if read is None else read.get("label_sha")
            if seen != spec.label_sha256:
                raise EvaluationInputError(
                    f"basis {inputs.basis}: its labels were read from label set {seen} (meta['{LABEL_READ_META}'], "
                    f"label_read_meta), not the declared label_sha256 {spec.label_sha256} (policy v4: the opened "
                    "holdout's label-matrix set); refused before any statistic")
        features, absent = _selected_features(inputs, spec)
        prep = None if inputs.status == BASIS_UNTESTABLE else _prepare(inputs, spec)
        manifests[inputs.basis] = _basis_manifest(inputs, prep, absent, catalog_sha)
        if previous is not None and previous.get(inputs.basis) not in (None, manifests[inputs.basis]):
            raise EvaluationInputError(f"resume refused: basis {inputs.basis} inputs changed")
        if on_manifest is not None:
            on_manifest(manifests)
        info[inputs.basis] = _BasisInfo(inputs.status, dict(inputs.meta), frozenset(
            (f, v) for f, variants in inputs.variants_by_feature.items() for v in variants))
        if prep is None:
            continue
        if not attrition_written(inputs.basis):
            with transaction():
                write("attrition", _frame(_basis_attrition(prep, spec), "attrition"))
        for feature_id in features:
            if (inputs.basis, feature_id) in done:
                continue
            produced, input_row = _evaluate_feature(prep, inputs, spec, feature_id, catalog)
            with transaction():
                for key, items in produced.items():
                    write(key, _frame(items, key))
                write("feature_inputs", _frame([input_row], "feature_inputs"))
            del produced
        del prep
    return manifests, info


_LABEL_DIGEST = re.compile(r"labels_\d+m_sha256")
#: ``BasisInputs.meta`` key of the basis's label-matrix read (:func:`label_read_meta`).
LABEL_READ_META = "label_read"


def label_read_meta(info: Mapping[str, Any]) -> dict[str, Any]:
    """The ``BasisInputs.meta['label_read']`` record of a label-matrix read (ruling C-52).

    ``info`` is the third value ``LabelMatrix.r3b_inputs`` returns. The record names the
    label-matrix spec sha the basis's labels came from (the identity ``trial_registry.open_holdout``
    records), whether they are provisional, the read window and the trial-registry opening that
    let a holdout read through (its ``wave``, ``sequence`` and ``record_sha``; None when the read
    stayed sealed). A v4 run binds to it: the engine refuses a basis whose record names another
    set than the spec's ``label_sha256``, and ``qualification.grade_wave_v4`` compares it with the
    wave's opening.
    """
    sha = info.get("label_sha")
    if not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha):
        raise EvaluationInputError("label_read_meta takes LabelMatrix.r3b_inputs info (with its label_sha)")
    opening = info.get("holdout_opening")
    return {"label_sha": sha, "provisional": info.get("provisional"), "holdout_start": info.get("holdout_start"),
            "eom_before": info.get("eom_before"),
            "holdout_opening": None if not opening else {key: opening.get(key)
                                                         for key in ("wave", "sequence", "record_sha")}}


def basis_label_read(meta: Mapping[str, Any] | None) -> dict[str, Any] | None:
    """A basis's recorded label-matrix read (``meta['label_read']``, :func:`label_read_meta`), or None."""
    read = (meta or {}).get(LABEL_READ_META)
    return dict(read) if isinstance(read, Mapping) else None


def label_set_sha256(bases: Mapping[str, Any]) -> str:
    """Digest of the label arrays a run saw: every basis's prepared per-horizon label digests.

    ``bases`` is ``EvaluationTables.bases`` (or the stored run manifest's bases). An audit digest
    (the v4 grade records it in its manifest), not the holdout identity: that is the label-matrix
    spec sha (ruling C-52, :func:`label_read_meta`).
    """
    parts: dict[str, dict[str, str]] = {}
    for basis, manifest in sorted(bases.items()):
        digests = (manifest or {}).get("prepared_digests") or {}
        labels = {key: str(value) for key, value in digests.items() if _LABEL_DIGEST.fullmatch(str(key))}
        if labels:
            parts[str(basis)] = labels
    if not parts:
        raise EvaluationInputError("no prepared label digests in the run's basis manifests")
    return _sha(_canonical(parts))


def evaluate_bases(bases: Iterable[BasisInputs], spec: EvaluationSpec, *,
                   label_diagnostics: Mapping[str, Any] | None = None,
                   catalog: Iterable[CatalogFeature] | None = None,
                   keep_frames: bool | Iterable[str] = True,
                   catalog_info: Mapping[str, Any] | None = None) -> EvaluationTables:
    """Evaluate every basis in memory and seal the result digest (no research store needed).

    ``catalog`` anchors the family to the expected cells (see the module docstring);
    without it the family is the produced cells (``catalog_info`` only labels the
    family summary's catalog provenance). ``keep_frames`` names the result tables
    to return (``True``: all; ``False``: none, e.g. a full-catalog recomputation that only
    needs the digest); tables not kept come back empty, the digest always covers all.
    """
    spec = validate_spec(spec)
    expected = _catalog_map(catalog)
    kept = set(RESULT_TABLES) if keep_frames is True else set() if keep_frames is False else set(keep_frames)
    if kept - set(RESULT_TABLES):
        raise EvaluationInputError(f"keep_frames names unknown tables {sorted(kept - set(RESULT_TABLES))}")
    config = {"threads": 1, "memory_limit": "256MB"}
    with tempfile.TemporaryDirectory(prefix="atx_r3b_eval_") as scratch:
        con: duckdb.DuckDBPyConnection | None = None
        try:
            if keep_frames is not False:
                # Small results: buffer them, and open the database only after the engine is done
                # (a database growing next to the engine's arrays would double the peak).
                buffered: dict[str, list[pd.DataFrame]] = {key: [] for key in RESULT_TABLES}
                manifests, info = _evaluate_into(None, "memory", bases, spec, expected, transaction=nullcontext,
                                                 sink=lambda key, frame: buffered[key].append(frame))
                con = duckdb.connect(":memory:", config=config)
                ensure_evaluation_schema(con)
                for key in RESULT_TABLES:
                    _insert_frames(con, key, "memory", buffered[key])
            else:
                # A full-catalog recomputation (~1.4M series rows): stream into a file-backed scratch DB.
                con = duckdb.connect(str(Path(scratch) / "evaluation.duckdb"),
                                     config={**config, "temp_directory": scratch})
                ensure_evaluation_schema(con)
                manifests, info = _evaluate_into(con, "memory", bases, spec, expected, transaction=nullcontext)
            family_summary = _finish_family(con, "memory", spec, info, expected, nullcontext,
                                            catalog_info=catalog_info)
            if label_diagnostics is not None:
                _insert(con, "attrition", "memory", _frame(_label_source_attrition(label_diagnostics), "attrition"))
            digest, parts = results_digest(con, "memory")
            frames = {key: _read_table(con, key, "memory") if key in kept else _frame([], key)
                      for key in RESULT_TABLES}
        finally:
            if con is not None:
                con.close()
    return EvaluationTables(**frames, family=family_summary, bases=manifests, results_sha256=digest,
                            table_digests=parts)


# ---------------------------------------------------------------------------
# R2b adapter (the ONLY reader of R2b tables) and R2a/R3a readers
# ---------------------------------------------------------------------------

_FEATURE_CONTRACT_COLUMNS: dict[str, tuple[str, ...]] = {
    "research_feature_versions": ("feature_version", "status", "basis", "panel_run_id", "panel_sha256",
                                  "classification_basis", "values_sha256", "query_version", "universe_rule",
                                  "catalog_sha256", "blockers_json", "spec_json"),
    "research_feature_catalog": ("feature_version", "feature_id", "anomaly_class", "hypothesis_family",
                                 "expected_sign", "status", "status_reason"),
    "research_feature_values": ("feature_version", "formation_date", "security_id", "feature_id", "variant",
                                "value", "expected_sign", "available_at"),
    "research_feature_dates": ("feature_version", "formation_date", "feature_id", "variant", "date_status",
                               "eligible_members", "valid_names", "coverage_fraction"),
}


@dataclass(frozen=True)
class FeatureTable:
    """What the R2b adapter returns for one feature version."""

    feature_version: str
    status: str
    basis: str
    panel_run_id: str
    panel_sha256: str
    classification_basis: str | None
    values_sha256: str | None
    dates: pd.DataFrame
    load_values: Callable[[str, Sequence[str]], dict[str, np.ndarray]]
    query_version: str | None = None
    catalog_sha256: str | None = None
    blockers: tuple[str, ...] = ()
    #: R2b's status of every catalog row it listed, ``status`` or ``status:reason``.
    catalog_status: Mapping[str, str] = field(default_factory=dict)
    #: A P5 composite version (spec ``kind='composite'``): its ``holdout_key`` and ``build_sha256``.
    composite: Mapping[str, Any] | None = None


def _research_columns(con: duckdb.DuckDBPyConnection, table: str) -> set[str]:
    catalog = con.execute("SELECT current_database()").fetchone()
    rows = con.execute("SELECT column_name FROM duckdb_columns() WHERE database_name=? AND schema_name='main' "
                       "AND table_name=?", [catalog[0] if catalog else None, table]).fetchall()
    return {row[0] for row in rows}


def _check_contract(con: duckdb.DuckDBPyConnection, tables: Iterable[str]) -> None:
    for table in tables:
        missing = sorted(set(_FEATURE_CONTRACT_COLUMNS[table]) - _research_columns(con, table))
        if missing:
            raise EvaluationInputError(f"R2b feature contract: research store table {table} lacks {missing} "
                                       f"(contract {FEATURE_CONTRACT}, atx_db.research.evaluation docstring)")


#: R2b catalog statuses of a row that is not a research hypothesis (blocked at admission).
_NOT_A_HYPOTHESIS = ("blocked_admission",)


def feature_version_catalog(store: ResearchStore, feature_version: str) -> tuple[str | None, tuple[CatalogFeature, ...]]:
    """The catalog snapshot R2b stored with a version: its catalog digest and the expected
    family (every row R2b did not block at admission, with its sign, class and hypothesis
    family as built, and :func:`expected_variants`)."""
    con = store.con
    _check_contract(con, ("research_feature_versions", "research_feature_catalog"))
    row = con.execute("SELECT catalog_sha256 FROM research_feature_versions WHERE feature_version=?",
                      [feature_version]).fetchall()
    if len(row) != 1:
        raise EvaluationInputError(f"feature version {feature_version!r} is absent or duplicated")
    features = []
    for feature_id, anomaly_class, family, sign in con.execute(f"""
        SELECT feature_id, anomaly_class, hypothesis_family, expected_sign FROM research_feature_catalog
        WHERE feature_version=? AND status NOT IN ({', '.join('?' * len(_NOT_A_HYPOTHESIS))}) ORDER BY feature_id
    """, [feature_version, *_NOT_A_HYPOTHESIS]).fetchall():
        if sign not in (-1, 0, 1):
            raise EvaluationInputError(f"feature version {feature_version}: catalog row {feature_id} has sign {sign!r}")
        features.append(CatalogFeature(str(feature_id), int(sign), str(anomaly_class),
                                       expected_variants(str(anomaly_class)), None if family is None else str(family)))
    return (None if row[0][0] is None else str(row[0][0])), tuple(features)


def load_feature_table(store: ResearchStore, feature_version: str) -> FeatureTable:
    """R2b adapter: read one feature version under the contract in the module docstring.

    The version must be ``sealed``/``untestable_strict``, of an accepted R2b query
    version and built under one of :data:`FEATURE_UNIVERSE_RULES`.
    ``load_values(feature_id, variants)`` returns numpy arrays ``month_index``,
    ``security``, ``variant_code`` (index into ``variants``), ``value`` (NaN for NULL),
    ``available_at_us`` (epoch microseconds, -1 for NULL), ``expected_sign`` and
    ``unlinked_line`` (``owner_basis='unlinked_line'``; all False when the view has no
    ``owner_basis``). It needs the caller's temp tables ``_ev_months(formation_date,
    month_index)`` and ``_ev_securities(security_id, code)``; unmapped rows come back as
    -1 and are refused by the caller. Before returning it asserts the universe clause of
    the contract against the version's R2a cohort: a linked valued row outside the
    cohort, not eligible with ``cohort_reason='valid'``, not on the ``primary_line``, or a
    second valued line of one owner at a formation, and an unlinked-line row whose cohort
    row is not an eligible line without an owner link, raise :class:`EvaluationInputError`.
    """
    con = store.con
    _check_contract(con, _FEATURE_CONTRACT_COLUMNS)
    owner = ("coalesce(v.owner_basis = '" + OWNER_BASIS_UNLINKED + "', false)"
             if "owner_basis" in _research_columns(con, "research_feature_values") else "false")
    row = con.execute("""
        SELECT status, basis, panel_run_id, panel_sha256, classification_basis, values_sha256, query_version,
               universe_rule, catalog_sha256, blockers_json, spec_json
        FROM research_feature_versions WHERE feature_version=?
    """, [feature_version]).fetchall()
    if len(row) != 1:
        raise EvaluationInputError(f"feature version {feature_version!r} is absent or duplicated")
    (status, basis, panel_run_id, panel_sha, classification, values_sha, query_version, universe_rule,
     catalog_sha, blockers_json, version_spec) = row[0]
    try:
        parsed_spec = json.loads(version_spec or "{}")
        store_schema = parsed_spec.get("store_schema")
    except (AttributeError, ValueError) as error:
        raise EvaluationInputError(f"feature version {feature_version}: unreadable spec_json") from error
    composite = None
    if parsed_spec.get("kind") == COMPOSITE_KIND:
        composite = {"holdout_key": parsed_spec.get("holdout_key"), "build_sha256": parsed_spec.get("build_sha256")}
        if not all(isinstance(value, str) and value for value in composite.values()):
            raise EvaluationInputError(f"composite feature version {feature_version}: its spec lacks the P5 "
                                       "holdout_key / build_sha256 that tie it to one holdout window")
    if isinstance(store_schema, bool) or not isinstance(store_schema, int) or store_schema < MIN_FEATURE_STORE_SCHEMA:
        raise EvaluationInputError(
            f"R2b feature contract: version {feature_version} was built under feature store schema "
            f"{store_schema if store_schema is not None else '<3 (none recorded)'}, before R2e (revived market "
            f"values, unlabeled survivor conditioning); rebuild it under schema >= {MIN_FEATURE_STORE_SCHEMA}")
    if status not in FEATURE_VERSION_STATUSES:
        raise EvaluationInputError(f"feature version {feature_version} is {status!r}; only {FEATURE_VERSION_STATUSES}"
                                   " versions are evaluated")
    if basis not in BASES:
        raise EvaluationInputError(f"feature version {feature_version} has unknown basis {basis!r}")
    if query_version not in FEATURE_QUERY_VERSIONS:
        raise EvaluationInputError(f"R2b feature contract: version {feature_version} has query version "
                                   f"{query_version!r}; this adapter reads {FEATURE_QUERY_VERSIONS}")
    if universe_rule not in FEATURE_UNIVERSE_RULES:
        raise EvaluationInputError(f"R2b feature contract: version {feature_version} ranks universe "
                                   f"{universe_rule!r}, the evaluation reads {FEATURE_UNIVERSE_RULES}")
    try:
        blockers = tuple(str(item) for item in json.loads(blockers_json or "[]"))
    except (TypeError, ValueError) as error:
        raise EvaluationInputError(f"feature version {feature_version}: unreadable blockers_json") from error
    catalog_status = {str(feature): str(state) if reason is None else f"{state}:{reason}"
                      for feature, state, reason in con.execute("""
                          SELECT feature_id, status, status_reason FROM research_feature_catalog
                          WHERE feature_version=? ORDER BY feature_id""", [feature_version]).fetchall()}
    dates = con.execute("""
        SELECT formation_date, feature_id, variant, date_status, eligible_members, valid_names, coverage_fraction
        FROM research_feature_dates WHERE feature_version=?
        ORDER BY feature_id, variant, formation_date
    """, [feature_version]).df()

    def load_values(feature_id: str, variants: Sequence[str]) -> dict[str, np.ndarray]:
        violations = con.execute(f"""
            WITH valued AS (
                SELECT v.formation_date, v.security_id, v.variant, {owner} AS unlinked_line,
                       c.security_id IS NOT NULL AS in_cohort, coalesce(c.eligible, false) AS eligible,
                       c.cohort_reason, coalesce(c.primary_line, false) AS primary_line, c.owner_cik
                FROM research_feature_values v
                LEFT JOIN research_panel_cohort c
                  ON c.run_id = ? AND c.formation_date = v.formation_date AND c.security_id = v.security_id
                WHERE v.feature_version = ? AND v.feature_id = ? AND list_contains(?::VARCHAR[], v.variant)
                  AND v.value IS NOT NULL
            ), lines AS (
                SELECT count(*) AS extra FROM (
                    SELECT formation_date, variant, owner_cik FROM valued
                    WHERE in_cohort AND NOT unlinked_line AND owner_cik IS NOT NULL
                    GROUP BY ALL HAVING count(DISTINCT security_id) > 1)
            )
            SELECT count(*) FILTER (WHERE NOT in_cohort),
                   count(*) FILTER (WHERE in_cohort AND NOT unlinked_line
                                    AND (cohort_reason IS DISTINCT FROM 'valid' OR NOT eligible)),
                   count(*) FILTER (WHERE in_cohort AND NOT unlinked_line AND cohort_reason = 'valid' AND eligible
                                    AND NOT primary_line),
                   (SELECT extra FROM lines),
                   count(*) FILTER (WHERE in_cohort AND unlinked_line
                                    AND NOT (eligible AND list_contains(?::VARCHAR[], cohort_reason)))
            FROM valued
        """, [panel_run_id, feature_version, feature_id, list(variants), list(OWNER_LINK_FAILURES)]).fetchone()
        if violations is not None and any(violations):
            outside, not_valid, not_primary, multi, unlinked = (int(v or 0) for v in violations)
            raise EvaluationInputError(
                f"R2b feature contract: {feature_id} has valued rows outside the ranked universe "
                f"(outside_cohort={outside}, not_valid={not_valid}, not_primary_line={not_primary}, "
                f"multi_line_owner_formations={multi}, unlinked_line_not_an_unlinked_member={unlinked}); value "
                f"must be NULL unless the R2a cohort row is eligible and valid on the primary line (or, for "
                f"owner_basis='{OWNER_BASIS_UNLINKED}', an eligible line without an owner link)")
        result = con.execute(f"""
            SELECT coalesce(m.month_index, -1) AS month_index, coalesce(s.code, -1) AS security,
                   list_position(?::VARCHAR[], v.variant) - 1 AS variant_code,
                   coalesce(v.value, 'NaN'::DOUBLE) AS value,
                   coalesce(epoch_us(v.available_at), -1) AS available_at_us,
                   coalesce(v.expected_sign, -9) AS expected_sign,
                   {owner} AS unlinked_line
            FROM research_feature_values v
            LEFT JOIN _ev_months m ON m.formation_date = v.formation_date
            LEFT JOIN _ev_securities s ON s.security_id = v.security_id
            WHERE v.feature_version = ? AND v.feature_id = ? AND list_contains(?::VARCHAR[], v.variant)
        """, [list(variants), feature_version, feature_id, list(variants)]).fetchnumpy()
        return {name: np.asarray(values) for name, values in result.items()}

    return FeatureTable(feature_version, str(status), str(basis), str(panel_run_id), str(panel_sha),
                        None if classification is None else str(classification),
                        None if values_sha is None else str(values_sha), dates, load_values,
                        str(query_version), None if catalog_sha is None else str(catalog_sha), blockers,
                        catalog_status, composite)


def _check_composite_window(store: ResearchStore, table: FeatureTable, spec: EvaluationSpec) -> None:
    """P5 allows one holdout evaluation per composite build and holdout window (read-only check).

    A composite version may be evaluated only by the run P5 records for its window
    (``research_composite_builds``): the run's frozen split must be the window's, the
    registry must hold this version's build for the window, and no other run may have
    evaluated the window (recorded ``holdout_run_id``) or these versions.
    """
    assert table.composite is not None
    con, version = store.con, table.feature_version
    key, build = table.composite["holdout_key"], table.composite["build_sha256"]
    split_sha = key.rsplit(":", 1)[0]
    if spec.split is None or spec.split.sha256 != split_sha:
        raise EvaluationInputError(f"composite feature version {version}: the run's split "
                                   f"{None if spec.split is None else spec.split.sha256} is not its holdout window "
                                   f"{key}; composites are evaluated only in their recorded holdout run (P5)")
    if not {"holdout_key", "build_sha256", "holdout_run_id"} <= _research_columns(con, "research_composite_builds"):
        raise EvaluationInputError(f"composite feature version {version}: this research store has no P5 composite "
                                   "registry (research_composite_builds); refusing an unregistered holdout look")
    row = con.execute("SELECT build_sha256, holdout_run_id FROM research_composite_builds WHERE holdout_key = ?",
                      [key]).fetchone()
    if row is None:
        raise EvaluationInputError(f"composite feature version {version}: no P5 registry entry for holdout window "
                                   f"{key}")
    if row[0] != build:
        raise EvaluationInputError(f"composite feature version {version}: built by {build}, but the registry holds "
                                   f"build {row[0]} for holdout window {key} (a replaced build is never evaluated)")
    if row[1] is not None and row[1] != spec.run_id:
        raise EvaluationInputError(f"composite feature version {version}: holdout window {key} was already evaluated "
                                   f"by run {row[1]}; one holdout evaluation per build and window")
    # Another run looked at the window if it completed, or wrote any feature rows on this basis
    # (a run refused before evaluating, e.g. by this check, never saw the holdout).
    others = [r[0] for r in con.execute("""
        SELECT r.run_id FROM research_eval_runs r
        WHERE r.run_id <> ? AND contains(r.spec_json, ?)
          AND (r.status = 'complete' OR EXISTS (SELECT 1 FROM research_eval_feature_inputs f
                                                WHERE f.run_id = r.run_id AND f.basis = ?))
        ORDER BY r.run_id
    """, [spec.run_id, f'"{version}"', table.basis]).fetchall()] \
        if _research_columns(con, "research_eval_feature_inputs") else []
    if others:
        raise EvaluationInputError(f"composite feature version {version}: already evaluated by run(s) {others}; "
                                   f"one holdout evaluation per build and window ({key})")


def _panel_run(store: ResearchStore, table: FeatureTable, verify: bool) -> dict[str, Any]:
    con = store.con
    row = con.execute("""
        SELECT status, basis, identity_basis, universe_basis, fundamental_availability_basis,
               market_availability_basis, panel_sha256, blockers_json, diagnostic_json, spec_json
        FROM research_panel_runs WHERE run_id=?
    """, [table.panel_run_id]).fetchone()
    if row is None:
        raise EvaluationInputError(f"feature version {table.feature_version}: panel run {table.panel_run_id} absent")
    status, basis, identity, universe, fundamental_clock, market_clock, panel_sha, blockers, diagnostic, spec = row
    try:
        policy = json.loads(spec or "{}").get("size_policy")
    except (AttributeError, ValueError) as error:
        raise EvaluationInputError(f"panel run {table.panel_run_id}: unreadable spec_json") from error
    if not isinstance(policy, Mapping) or not isinstance(policy.get("verified_status"), str):
        raise EvaluationInputError(f"panel run {table.panel_run_id} has no size_policy.verified_status (R2a query "
                                   "version v3): the evaluation uses verified size only")
    if status not in ("complete", "untestable_strict"):
        raise EvaluationInputError(f"panel run {table.panel_run_id} is {status!r}, not sealed")
    if panel_sha != table.panel_sha256:
        raise EvaluationInputError(f"feature version {table.feature_version} was built from panel digest "
                                   f"{table.panel_sha256}, the panel run now seals {panel_sha}")
    if basis != table.basis:
        raise EvaluationInputError(f"feature version basis {table.basis} != panel basis {basis}")
    if verify:
        validate_research_panel(store, table.panel_run_id)
    return {"panel_run_id": table.panel_run_id, "panel_status": status, "panel_sha256": panel_sha,
            "identity_basis": identity, "universe_basis": universe,
            "size_verified_status": str(policy["verified_status"]),
            "availability_basis": f"fundamentals:{fundamental_clock};market:{market_clock}",
            "panel_blockers": json.loads(blockers) if blockers else [],
            "panel_diagnostic": json.loads(diagnostic) if diagnostic else None}


def _register(con: duckdb.DuckDBPyConnection, name: str, frame: pd.DataFrame,
              columns: Sequence[tuple[str, str]]) -> None:
    con.execute(f"CREATE OR REPLACE TEMP TABLE {name} ({', '.join(f'{c} {t}' for c, t in columns)})")
    if len(frame):
        con.register("_ev_stage", frame)
        try:
            select = ", ".join(f"CAST({c} AS {t})" for c, t in columns)
            con.execute(f"INSERT INTO {name} SELECT {select} FROM _ev_stage")
        finally:
            con.unregister("_ev_stage")


def load_label_inputs(store: ResearchStore, calendar: pd.DataFrame, spec: EvaluationSpec
                      ) -> tuple[pd.DataFrame, pd.DataFrame, dict[str, Any]]:
    """Monthly labels (R3a contract) for the staged ``_ev_securities`` at every formed month.

    Windows come from the label calendar (``trading_calendar``, sessions up to the
    cutoff): the entry is the session after the decision date and must equal the panel
    entry (else ``entry_calendar_mismatch``); the expected end is ``entry + h``
    sessions; a window is matured when that end + 1 day 12:00 is <= the cutoff.
    Revisions are selected before validity; status comes from the FQ2 fragment.
    """
    if spec.label_cutoff is None:
        raise EvaluationInputError("label_cutoff (observation vintage) is required to read labels")
    con = store.con
    cutoff = spec.label_cutoff
    calendar_id, calendar_source = _label_calendar_keys()
    formed = calendar[calendar["status"] == CALENDAR_FORMED]
    _register(con, "_ev_formed", pd.DataFrame({
        "month_index": formed["month_index"].astype("int64").to_numpy(),
        "formation_date": _days(formed["formation_date"]),
        "entry_date": _days(formed["entry_date"])}),
        (("month_index", "BIGINT"), ("formation_date", "DATE"), ("entry_date", "DATE")))
    horizons = list(spec.horizons_months)
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_sessions AS
        SELECT trade_date, row_number() OVER (ORDER BY trade_date) AS session_number
        FROM (SELECT DISTINCT trade_date FROM trading_calendar
              WHERE calendar_id=? AND source=? AND is_open AND trade_date <= CAST(? AS DATE))
    """, [calendar_id, calendar_source, cutoff])
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_windows AS
        WITH aligned AS (
            SELECT f.month_index, f.formation_date, f.entry_date, d.session_number AS decision_session,
                   e.trade_date AS label_entry, e.session_number AS entry_session
            FROM _ev_formed f
            LEFT JOIN _ev_sessions d ON d.trade_date = f.formation_date
            LEFT JOIN _ev_sessions e ON e.session_number = d.session_number + 1
        ), horizons AS (
            SELECT unnest(?::INTEGER[]) AS horizon_months, unnest(?::INTEGER[]) AS horizon_days
        )
        SELECT a.month_index, a.entry_date, h.horizon_months, h.horizon_days,
               CASE WHEN a.entry_date > CAST(? AS DATE) THEN 'entry_after_label_cutoff'
                    WHEN a.decision_session IS NULL THEN 'decision_not_in_label_calendar'
                    WHEN a.label_entry IS NULL THEN 'no_label_entry_session'
                    WHEN a.label_entry <> a.entry_date THEN 'entry_calendar_mismatch'
                    ELSE 'aligned' END AS alignment,
               x.trade_date AS expected_end,
               a.label_entry = a.entry_date AND x.trade_date IS NOT NULL
                 AND x.trade_date::TIMESTAMP + INTERVAL 1 DAY + INTERVAL 12 HOUR <= ? AS matured
        FROM aligned a CROSS JOIN horizons h
        LEFT JOIN _ev_sessions x ON x.session_number = a.entry_session + h.horizon_days
    """, [horizons, [HORIZON_SESSIONS[h] for h in horizons], cutoff, cutoff])
    maturity = con.execute("""
        SELECT month_index, horizon_months, expected_end, coalesce(matured, false) AS matured, alignment
        FROM _ev_windows ORDER BY horizon_months, month_index
    """).df()
    status_sql = label_status_sql(label="l", calculation_version="?", expected_end="w.expected_end",
                                  entry="w.entry_date", cutoff="?")
    result = con.execute(f"""
        WITH keys AS (SELECT DISTINCT entry_date, horizon_days FROM _ev_windows WHERE matured),
        revisions AS (
            SELECT l.*, row_number() OVER (
                PARTITION BY l.security_id, l.as_of_date, l.horizon_days
                ORDER BY {label_revision_order_sql("l")}) AS revision
            FROM forward_returns_survivorship_safe l
            JOIN keys k ON k.entry_date = l.as_of_date AND k.horizon_days = l.horizon_days
            JOIN _ev_securities s ON s.security_id = l.security_id
            WHERE l.source = ? AND l.available_at <= ?
        ), judged AS (
            SELECT w.month_index, s.code AS security, w.horizon_months, l.forward_return,
                   {status_sql} AS label_status,
                   CASE WHEN l.is_stitched AND l.terminal_return_source = 'observed' THEN 1
                        WHEN l.is_stitched AND l.terminal_return_source = 'policy' THEN 2 ELSE 0 END AS terminal,
                   l.as_of_date AS anchor_date
            FROM revisions l
            JOIN _ev_windows w ON w.entry_date = l.as_of_date AND w.horizon_days = l.horizon_days AND w.matured
            JOIN _ev_securities s ON s.security_id = l.security_id
            WHERE l.revision = 1
        )
        SELECT month_index, security, horizon_months, forward_return,
               CASE label_status WHEN 'valid' THEN 0 WHEN 'invalid' THEN 1 ELSE 2 END AS status,
               terminal, anchor_date
        FROM judged ORDER BY horizon_months, month_index, security
    """, [spec.label_source, cutoff, LABEL_CALCULATION_VERSION, cutoff]).df()
    alignment = maturity.drop_duplicates("month_index")["alignment"].value_counts().sort_index()
    info = {"label_source": spec.label_source, "label_cutoff": cutoff.isoformat(),
            "label_calculation_version": LABEL_CALCULATION_VERSION,
            "calendar_id": calendar_id, "calendar_source": calendar_source,
            "alignment": {str(k): int(v) for k, v in alignment.items()},
            "label_rows": len(result)}
    return result, maturity.drop(columns=["alignment"]), info


def _stage_pit_venue(con: duckdb.DuckDBPyConnection) -> bool:
    """``_ev_pit_venue(month_index, security, exchange_code)``: the venue as of each formation.

    Listing evidence as of the formation: the strict ``us_listed_v1`` membership interval
    covering the formation date, visible at its cutoff (the R2a cohort's own as-of rule);
    a security with several visible rows has no point-in-time venue. False when the
    warehouse has no membership table (every venue is then the cohort's).
    """
    present = con.execute("SELECT count(*) FROM duckdb_tables() WHERE table_name='universe_us_listed_membership'"
                          ).fetchone()
    if not (present and present[0]):
        con.execute("CREATE OR REPLACE TEMP TABLE _ev_pit_venue (month_index BIGINT, security BIGINT, "
                    "exchange_code VARCHAR)")
        return False
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ev_pit_venue AS
        SELECT m.month_index, s.code AS security, max(u.exchange_code) AS exchange_code
        FROM universe_us_listed_membership u
        JOIN _ev_securities s ON s.security_id = u.security_id
        JOIN _ev_months m
          ON u.valid_from <= m.formation_date AND (u.valid_to IS NULL OR u.valid_to >= m.formation_date)
         AND u.available_at <= m.cutoff AND u.as_of_date <= m.formation_date
        WHERE u.universe_id = ? AND u.source = ? AND u.exchange_code IS NOT NULL AND u.exchange_code <> 'UNKNOWN'
        GROUP BY ALL HAVING count(*) = 1
    """, [PIT_VENUE_UNIVERSE_ID, PIT_VENUE_SOURCE])
    return True


def open_basis_inputs(store: ResearchStore, feature_version: str, spec: EvaluationSpec,
                      catalog: Mapping[str, CatalogFeature] | None = None) -> BasisInputs:
    """Assemble one basis from the store: R2b adapter + R2a panel context + R3a labels.

    With ``spec.verify_panels`` the R2a panel validator and the R2b version validator
    (digests, universe, clocks, domain, formed-date rules) run before anything is read.
    """
    con = store.con
    table = load_feature_table(store, feature_version)
    if table.composite is not None:  # always, whatever verify_panels says (P5: one holdout look)
        _check_composite_window(store, table, spec)
    panel = _panel_run(store, table, spec.verify_panels)
    if spec.verify_panels:
        from .features import FeatureStoreError, validate_feature_version

        try:
            validate_feature_version(store, feature_version, verify_panel=False)  # the panel was just validated
        except FeatureStoreError as error:
            raise EvaluationInputError(f"R2b feature version {feature_version} fails its validator: {error}") \
                from error
    meta = {"feature_version": feature_version, "feature_version_status": table.status,
            "classification_basis": table.classification_basis, "r2b_values_sha256": table.values_sha256,
            "feature_query_version": table.query_version, "feature_catalog_sha256": table.catalog_sha256,
            "feature_blockers": list(table.blockers), "feature_catalog_status": dict(table.catalog_status),
            "composite_window": None if table.composite is None else dict(table.composite),
            **{k: v for k, v in panel.items() if k != "panel_diagnostic"}}
    variants_by_feature: dict[str, tuple[str, ...]] = {}
    for (feature_id, variant), _ in table.dates.groupby(["feature_id", "variant"], sort=True):
        variants_by_feature.setdefault(str(feature_id), ())
        variants_by_feature[str(feature_id)] += (str(variant),)
    if table.status == BASIS_UNTESTABLE or panel["panel_status"] == "untestable_strict":
        return empty_basis(table.basis, meta=meta, variants_by_feature=variants_by_feature)
    calendar = con.execute("""
        SELECT month_start, formation_date, cutoff, entry_date, status, eligible_members
        FROM research_panel_calendar WHERE run_id=? ORDER BY month_start
    """, [table.panel_run_id]).df()
    calendar.insert(0, "month_index", np.arange(len(calendar), dtype=np.int64))
    formed = calendar[calendar["status"] == CALENDAR_FORMED]
    _register(con, "_ev_months", pd.DataFrame({"formation_date": _days(formed["formation_date"]),
                                               "month_index": formed["month_index"].to_numpy(np.int64),
                                               "cutoff": _stamps(formed["cutoff"])}),
              (("formation_date", "DATE"), ("month_index", "BIGINT"), ("cutoff", "TIMESTAMP")))
    securities = [row[0] for row in con.execute(
        "SELECT DISTINCT security_id FROM research_panel_cohort WHERE run_id=? ORDER BY security_id",
        [table.panel_run_id]).fetchall()]
    _register(con, "_ev_securities", pd.DataFrame({"security_id": pd.Series(securities, dtype=object),
                                                   "code": np.arange(len(securities), dtype=np.int64)}),
              (("security_id", "VARCHAR"), ("code", "BIGINT")))
    pit_table = _stage_pit_venue(con)
    verified = panel["size_verified_status"]
    # Size: the panel's market cap only where its share count is verified (R2a size_policy).
    # Venue: point-in-time listing evidence only; the cohort's (backcast) exchange code is not read.
    context = con.execute("""
        WITH cap AS (
            SELECT formation_date, security_id, max(raw_value) AS market_cap
            FROM research_panel_values
            WHERE run_id=? AND metric_code='market_cap' AND metric_window='daily' AND reason='valid'
              AND size_status = ? AND raw_value > 0 AND isfinite(raw_value)
            GROUP BY ALL
        )
        SELECT m.month_index, s.code AS security, max(cap.market_cap) AS market_cap,
               bool_or(coalesce(c.eligible, false) AND c.cohort_reason = 'valid') AS valid_member,
               bool_or(coalesce(c.primary_line, false)) AS primary_line,
               bool_or(coalesce(c.eligible, false) AND list_contains(?::VARCHAR[], c.cohort_reason))
                   AS unlinked_member,
               bool_or(p.security IS NOT NULL) AS venue_pit,
               bool_or(coalesce(p.exchange_code = ?, false)) AS is_nyse_pit
        FROM research_panel_cohort c
        JOIN _ev_months m ON m.formation_date = c.formation_date
        JOIN _ev_securities s ON s.security_id = c.security_id
        LEFT JOIN cap ON cap.formation_date = c.formation_date AND cap.security_id = c.security_id
        LEFT JOIN _ev_pit_venue p ON p.month_index = m.month_index AND p.security = s.code
        WHERE c.run_id=?
        GROUP BY ALL ORDER BY 1, 2
    """, [table.panel_run_id, verified, list(OWNER_LINK_FAILURES), NYSE_EXCHANGE_CODE, table.panel_run_id]).df()
    venue_rows = int(context["venue_pit"].sum()) if len(context) else 0
    universe = context["valid_member"].fillna(False) & context["primary_line"].fillna(False) if len(context) \
        else pd.Series(dtype=bool)
    size_rows = con.execute("""
        SELECT count(*) FILTER (WHERE size_status = ?), count(*) FILTER (WHERE size_status IS DISTINCT FROM ?)
        FROM research_panel_values
        WHERE run_id=? AND metric_code='market_cap' AND metric_window='daily' AND reason='valid'
          AND raw_value > 0 AND isfinite(raw_value)
    """, [verified, verified, table.panel_run_id]).fetchone()
    size_info = {"verified_status": verified, "verified_cap_rows": int(size_rows[0]) if size_rows else 0,
                 "unverified_cap_rows_excluded": int(size_rows[1]) if size_rows else 0,
                 "universe_rows": int(universe.sum()),
                 "universe_rows_with_verified_cap": int((universe & context["market_cap"].notna()).sum())
                 if len(context) else 0}
    labels, maturity, label_info = load_label_inputs(store, calendar, spec)
    controls = []
    for name in spec.control_features:
        if spec.control_variant not in variants_by_feature.get(name, ()):
            continue
        arrays = table.load_values(name, [spec.control_variant])
        if (arrays["month_index"] < 0).any() or (arrays["security"] < 0).any():
            raise EvaluationInputError(f"control {name}: rows outside the panel calendar or cohort")
        controls.append(pd.DataFrame({"month_index": arrays["month_index"], "security": arrays["security"],
                                      "control": name, "value": arrays["value"]}))
    controls_frame = (pd.concat(controls, ignore_index=True) if controls
                      else pd.DataFrame(columns=["month_index", "security", "control", "value"]))
    month_of = dict(zip(_days(formed["formation_date"]).tolist(), formed["month_index"].tolist(), strict=True))

    def load_feature(feature_id: str) -> FeatureData:
        variants = variants_by_feature[feature_id]
        arrays = table.load_values(feature_id, variants)
        if (arrays["month_index"] < 0).any():
            raise EvaluationInputError(f"{feature_id}: values dated at a non-formed panel formation")
        if (arrays["security"] < 0).any():
            raise EvaluationInputError(f"{feature_id}: values for securities outside the panel cohort")
        signs = set(np.unique(arrays["expected_sign"]).tolist())
        if len(signs) != 1 or signs - {-1, 0, 1}:
            raise EvaluationInputError(f"{feature_id}: expected_sign must be one of +1/-1/0, got {sorted(signs)}")
        available = arrays["available_at_us"].astype("int64")
        stamps = available.astype("datetime64[us]")
        stamps[available < 0] = np.datetime64("NaT", "us")
        values = pd.DataFrame({
            "month_index": arrays["month_index"], "security": arrays["security"],
            "variant": pd.Categorical.from_codes(arrays["variant_code"].astype("int64"), categories=list(variants)),
            "value": arrays["value"].astype(float), "available_at": stamps,
            "unlinked_line": np.asarray(arrays["unlinked_line"], dtype=bool)})
        dates = table.dates[table.dates["feature_id"] == feature_id]
        date_frame = pd.DataFrame({
            "month_index": [month_of.get(day, -1) for day in _days(dates["formation_date"]).tolist()],
            "variant": dates["variant"].astype(str).to_numpy(), "date_status": dates["date_status"].astype(str).to_numpy(),
            "coverage_fraction": pd.to_numeric(dates["coverage_fraction"], errors="coerce").to_numpy(dtype=float)})
        if (date_frame["month_index"] < 0).any():
            raise EvaluationInputError(f"{feature_id}: date rows at a non-formed panel formation")
        entry = None if catalog is None else catalog.get(feature_id)
        return FeatureData(feature_id, int(signs.pop()), values, date_frame,
                           None if entry is None else entry.anomaly_class)

    digests = {"securities_sha256": _sha(_canonical(securities)), "label": label_info, "size": size_info,
               "venue": {"pit_membership_table": pit_table, "pit_venue_rows": venue_rows,
                         "pit_universe_id": PIT_VENUE_UNIVERSE_ID, "cohort_rows": len(context)}}
    return BasisInputs(table.basis, BASIS_AVAILABLE, max(len(securities), 1), calendar, labels, maturity,
                       context, controls_frame, variants_by_feature, load_feature, meta, digests,
                       release_frames=True)


# ---------------------------------------------------------------------------
# Persisted runs
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class EvaluationRunResult:
    run_id: str
    status: str
    results_sha256: str
    inputs_sha256: str
    cells: int
    family: dict[str, Any]
    blockers: tuple[str, ...]
    #: True only for a catalog-anchored run with no subset, no missing expected cell and both
    #: bases supplied (R4 must refuse to qualify from any other run).
    family_complete: bool = False


def _code_sha() -> str:
    digest = hashlib.sha256()
    for path in _CODE_FILES:
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes().replace(b"\r\n", b"\n"))  # checkout line endings do not matter
    return digest.hexdigest()


def _blockers(spec: EvaluationSpec, manifests: Mapping[str, Any], family: Mapping[str, Any]) -> list[str]:
    blockers = ["research_only_not_release_eligible"]
    if spec.split is None:
        blockers.append("no_frozen_split_selection_uses_all_formations")
    if _is_subset(spec):
        blockers.append("partial_family_subset")
    if not family.get("catalog_anchored"):
        blockers.append("family_not_catalog_anchored")
    if family.get("catalog_source") == "injected":
        blockers.append("family_catalog_injected_not_the_feature_version_snapshot")
    committed = family.get("committed_catalog_sha256")
    if "strict" not in manifests:
        blockers.append("strict_basis_not_supplied")
    counts = family.get("bases", {})
    for basis, manifest in sorted(manifests.items()):
        if manifest["status"] == BASIS_UNTESTABLE:
            blockers.append(f"{basis}_basis_untestable")
        if basis == "reconstructed":
            blockers.append("reconstructed_identity_universe_and_availability_not_certifiable")
        for blocker in manifest["meta"].get("panel_blockers", []):
            blockers.append(f"{basis}_panel:{blocker}")
        for blocker in manifest["meta"].get("feature_blockers", []):
            blockers.append(f"{basis}_features:{blocker}")
        built_from = manifest["meta"].get("feature_catalog_sha256")
        if committed is not None and built_from is not None and built_from != committed:
            blockers.append(f"{basis}_feature_catalog_differs_from_committed_catalog")
        alignment = manifest["digests"].get("label", {}).get("alignment", {}) if manifest["digests"] else {}
        for status, count in sorted(alignment.items()):
            if status not in BENIGN_ALIGNMENTS and count:
                blockers.append(f"{basis}_{status}:{count}")
        if manifest["features_absent"]:
            blockers.append(f"{basis}_features_absent:{len(manifest['features_absent'])}")
        venues = {name: int(count) for name, count in (
            item.split(":") for item in (manifest.get("venue_basis_formations") or "").split(",") if item)}
        without_pit = sum(count for name, count in venues.items() if name != VENUE_NYSE_PIT)
        if without_pit:
            blockers.append(f"{basis}_size_buckets_without_pit_nyse_venue:{without_pit}")
        basis_counts = counts.get(basis, {})
        if basis_counts.get("not_produced"):
            blockers.append(f"{basis}_catalog_cells_not_produced:{basis_counts['not_produced']}")
        if basis_counts.get("features_outside_catalog"):
            blockers.append(f"{basis}_features_outside_catalog:{basis_counts['features_outside_catalog']}")
    if not spec.label_diagnostics:
        blockers.append("label_source_attrition_not_measured")
    return blockers


def _open_bases(store: ResearchStore, spec: EvaluationSpec,
                catalog: Mapping[str, CatalogFeature] | None) -> Iterable[BasisInputs]:
    for version in spec.feature_versions:
        yield open_basis_inputs(store, version, spec, catalog)


def _diagnostics(store: ResearchStore, spec: EvaluationSpec) -> dict[str, Any] | None:
    if not spec.label_diagnostics:
        return None
    result: dict[str, Any] = monthly_label_diagnostics(
        store,  # type: ignore[arg-type]  # ResearchStore duck-types DuckDBStore.con
        source=spec.label_source, horizons=[HORIZON_SESSIONS[h] for h in spec.horizons_months],
        cutoff=spec.label_cutoff)
    return result


def run_evaluation(store: ResearchStore, spec: EvaluationSpec, *, resume: bool = False,
                   catalog: Iterable[CatalogFeature] | None = None) -> EvaluationRunResult:
    """Evaluate the spec's feature versions and persist a sealed run in the research store.

    One transaction per (basis, feature); ``resume=True`` continues a ``building`` or
    ``failed`` run with an identical spec and code, skipping features already written.
    RX7: a frozen split is required unless ``allow_unsplit`` (recorded as a blocker).
    The family is anchored to ``catalog`` (default: the R1a catalog's research-eligible
    rows, :func:`catalog_features`); ``family_complete`` is stored on the run row.
    """
    spec = validate_spec(spec)
    if not spec.feature_versions:
        raise EvaluationInputError("run_evaluation needs at least one feature version")
    if spec.split is None and not spec.allow_unsplit:
        raise EvaluationInputError("RX7: a frozen split (hash committed before real data) is required; "
                                   "pass allow_unsplit only for an explicitly exploratory run")
    if spec.label_cutoff is None:
        raise EvaluationInputError("label_cutoff (observation vintage) is required")
    con = store.con
    with store.transaction():
        ensure_evaluation_schema(con)
    spec_json = _canonical(spec_payload(spec))
    spec_sha, code_sha = _sha(spec_json), _code_sha()
    now = dt.datetime.now(dt.UTC).replace(tzinfo=None)
    existing = con.execute("SELECT status, spec_sha256, code_sha256, inputs_json FROM research_eval_runs "
                           "WHERE run_id=?", [spec.run_id]).fetchone()
    stored_inputs: dict[str, Any] = {}
    with store.transaction():
        if existing is not None:
            if not resume:
                raise EvaluationInputError(f"evaluation run_id already exists: {spec.run_id}")
            if existing[0] not in ("building", "failed"):
                raise EvaluationInputError(f"evaluation run {spec.run_id} is sealed ({existing[0]})")
            if (existing[1], existing[2]) != (spec_sha, code_sha):
                raise EvaluationInputError("resume refused: spec or code changed")
            stored_inputs = json.loads(existing[3]) if existing[3] else {}
            con.execute("UPDATE research_eval_runs SET status='building' WHERE run_id=?", [spec.run_id])
        else:
            con.execute("""
                INSERT INTO research_eval_runs (run_id, status, evaluation_version, spec_json, spec_sha256,
                                                code_sha256, created_at)
                VALUES (?, 'building', ?, ?, ?, ?, ?)
            """, [spec.run_id, EVALUATION_VERSION, spec_json, spec_sha, code_sha, now])
    try:
        expected, catalog_info = _resolve_catalog(store, spec, catalog)
        done = frozenset((row[0], row[1]) for row in con.execute(
            "SELECT basis, feature_id FROM research_eval_feature_inputs WHERE run_id=?", [spec.run_id]).fetchall())

        def record(manifests: dict[str, Any]) -> None:
            with store.transaction():
                con.execute("UPDATE research_eval_runs SET inputs_json=? WHERE run_id=?",
                            [_canonical({"bases": manifests}), spec.run_id])

        manifests, info = _evaluate_into(con, spec.run_id, _open_bases(store, spec, expected), spec, expected,
                                         transaction=store.transaction, done=done,
                                         previous=stored_inputs.get("bases", {}), on_manifest=record)
        family_summary = _finish_family(con, spec.run_id, spec, info, expected, store.transaction,
                                        catalog_info=catalog_info)
        with store.transaction():
            con.execute("DELETE FROM research_eval_attrition WHERE run_id=? AND scope='label_source'", [spec.run_id])
        diagnostics = _diagnostics(store, spec)
        with store.transaction():
            if diagnostics is not None:
                _insert(con, "attrition", spec.run_id, _frame(_label_source_attrition(diagnostics), "attrition"))
        results_sha, parts = results_digest(con, spec.run_id)
        inputs_json = _canonical({"bases": manifests, "feature_inputs": parts["feature_inputs"],
                                  "label_source": spec.label_source, "code_sha256": code_sha})
        blockers = _blockers(spec, manifests, family_summary)
        complete = bool(family_summary.get("family_complete"))
        with store.transaction():
            con.execute("""
                UPDATE research_eval_runs SET status='complete', inputs_json=?, inputs_sha256=?, results_sha256=?,
                    family_json=?, blockers_json=?, diagnostic_json=?, finished_at=?, family_complete=?
                WHERE run_id=? AND status='building'
            """, [inputs_json, _sha(inputs_json), results_sha, _canonical(family_summary), _canonical(blockers),
                  _canonical({"tables": parts, "label_diagnostics": diagnostics}),
                  dt.datetime.now(dt.UTC).replace(tzinfo=None), complete, spec.run_id])
        return EvaluationRunResult(spec.run_id, "complete", results_sha, _sha(inputs_json),
                                   int(parts["cells"]["rows"]), family_summary, tuple(blockers), complete)
    except Exception as exc:
        with store.transaction():
            con.execute("UPDATE research_eval_runs SET status='failed', diagnostic_json=? "
                        "WHERE run_id=? AND status='building'",
                        [_canonical({"error": type(exc).__name__, "message": str(exc)}), spec.run_id])
        raise


def verify_evaluation_run(store: ResearchStore, run_id: str, *,
                          catalog: Iterable[CatalogFeature] | None = None) -> dict[str, Any]:
    """Re-derive a sealed run from its manifest and compare digests.

    ``stored_rows_match``: the persisted rows still hash to the sealed digest (no
    tampering). ``reproduced``: recomputing from the manifest's spec over the current
    inputs (and the same catalog) gives byte-identical results and identical input digests.
    """
    con = store.con
    row = con.execute("SELECT status, spec_json, results_sha256, inputs_json FROM research_eval_runs WHERE run_id=?",
                      [run_id]).fetchone()
    if row is None or row[0] != "complete":
        raise EvaluationInputError(f"evaluation run {run_id} is absent or not sealed")
    spec = spec_from_payload(json.loads(row[1]), run_id)
    stored_sha, _ = results_digest(con, run_id)
    expected, catalog_info = _resolve_catalog(store, spec, catalog)
    tables = evaluate_bases(_open_bases(store, spec, expected), spec, label_diagnostics=_diagnostics(store, spec),
                            catalog=None if expected is None else expected.values(), keep_frames=False,
                            catalog_info=catalog_info)
    stored_bases = json.loads(row[3]).get("bases", {}) if row[3] else {}
    return {"run_id": run_id, "sealed_results_sha256": row[2], "stored_rows_sha256": stored_sha,
            "recomputed_results_sha256": tables.results_sha256,
            "stored_rows_match": stored_sha == row[2],
            "inputs_match": stored_bases == tables.bases,
            "reproduced": stored_sha == row[2] == tables.results_sha256 and stored_bases == tables.bases}


__all__ = [
    "BASES",
    "COST_BPS",
    "DEFAULT_HORIZONS",
    "DEFAULT_SUBPERIODS",
    "EVALUATION_VERSION",
    "FAMILY_SCOPE_GATING",
    "FAMILY_SCOPE_REPORTED",
    "FEATURE_CONTRACT",
    "FEATURE_QUERY_VERSIONS",
    "FEATURE_UNIVERSE_RULES",
    "FEATURE_VARIANTS",
    "HORIZON_SESSIONS",
    "INVESTABLE_SLICE_KIND",
    "INVESTABLE_SLICE_NAME",
    "JT_SLICE_KIND",
    "LABEL_READ_META",
    "MIN_FEATURE_STORE_SCHEMA",
    "OWNER_BASIS_UNLINKED",
    "POPULATION_ALL",
    "POPULATION_FLAG_PREFIX",
    "SIZE_BUCKETS",
    "SIZE_SLICE_KINDS",
    "SUPPLIED_BREAKPOINT_COLUMN",
    "SUPPLIED_SIZE_SLICE_KIND",
    "UNIVERSE_RULE",
    "VENUE_BASES",
    "VENUE_NYSE_BREAKPOINTS",
    "BasisInputs",
    "CatalogFeature",
    "EvaluationInputError",
    "EvaluationRunResult",
    "EvaluationSpec",
    "EvaluationTables",
    "FeatureData",
    "FeatureTable",
    "FrozenSplit",
    "LookaheadError",
    "basis_label_read",
    "breakpoint_quantiles",
    "catalog_features",
    "compute_family",
    "empirical_bayes_ic",
    "empty_basis",
    "ensure_evaluation_schema",
    "evaluate_bases",
    "expected_variants",
    "family_scope",
    "feature_version_catalog",
    "freeze_split",
    "gating_bh_q",
    "grouped_average_ranks",
    "grouped_quantiles",
    "grouped_rank_correlation",
    "jegadeesh_titman_series",
    "label_read_meta",
    "label_set_sha256",
    "load_feature_table",
    "load_frozen_split",
    "load_label_inputs",
    "open_basis_inputs",
    "results_digest",
    "run_evaluation",
    "spec_from_payload",
    "spec_payload",
    "split_sha256",
    "traded_fraction",
    "validate_spec",
    "verify_evaluation_run",
]
