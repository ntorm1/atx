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
* size-bucket (micro/small/large by NYSE 20th/50th percentile breakpoints when NYSE
  names are identifiable, else market-cap terciles), subperiod and frozen
  train/validation/holdout slices;
* label attrition (missing/invalid/unsupported labels, observed/policy terminals) and
  the R3a per-horizon cause classification;
* family-wide Benjamini-Hochberg q-values, Holm p-values, the Harvey-Liu-Zhu hurdle
  and the deflated Sharpe ratio of every cell.

Results and a sealed, reproducible run manifest live in the research store (RX6).

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
* The family is every cell whose status is ``tested`` or ``insufficient_formations``:
  a hypothesis that had data and could have been selected. ``untestable_strict`` and
  ``no_values`` cells have no data; they are reported, never counted. BH and Holm run
  over the primary-sample IC robust p-values (insufficient cells enter as untestable,
  i.e. p = 1). The deflated Sharpe ratio of a cell uses its equal-weighted decile
  long-short series, ``horizon_periods = h``, ``n_trials`` = the whole family and the
  cross-trial variance of the Sharpe ratios of tested cells at the same horizon (the
  same period units).
* Selection sample. With a frozen split (RX7) the primary statistics, the family and
  every derived slice (subperiods, size buckets, decay, quantiles) use only the
  selection sample: train + validation formations whose label window ends before the
  holdout starts. Holdout statistics appear only as ``slice_kind='split'`` rows. Without
  a split the primary sample is every formation and the run carries a blocker.

Point-in-time guards
--------------------
* Formation authority is the R2a ``research_panel_calendar``: only ``formed`` months
  are evaluated, each at its decision date; a missing month-end stays a gap (its
  position is kept in every HAC sum).
* A label is used only when it is anchored exactly at its formation's entry session
  (the session after the decision date) and its window ends where the label calendar
  says; anything else raises :class:`LookaheadError`. Labels are the R3a monthly
  source selected by revision, then validated by the FQ2 validity fragment at the
  evaluation observation cutoff (``calculation_version`` forward_return_publication_v2).
* A feature value whose ``available_at`` is after its formation cutoff raises
  :class:`LookaheadError`.

R2b feature-table contract (declared by R3b; R2b conforms or updates the adapter)
----------------------------------------------------------------------------------
R2b is not built yet. :func:`load_feature_table` is the only function that reads R2b
tables. It expects, in the research store:

``research_feature_versions`` (one row per immutable version)
    ``feature_version`` (sha256 hex of spec + code + input digests), ``status``
    (``sealed``; ``untestable_strict`` for a strict build over an empty cohort), ``basis``
    (``strict`` | ``reconstructed``), ``panel_run_id`` and ``panel_sha256`` (the sealed R2a
    run it was built from), ``classification_basis`` (e.g. a current-SIC backcast label,
    RX1) and ``values_sha256`` (R2b's own digest of its value rows).
``research_feature_values`` (long: formation x security x feature x variant)
    ``feature_version``, ``formation_date`` (the R2a formation date), ``security_id`` (the
    R2a price line), ``feature_id`` (R1a catalog id), ``variant`` (``signed_raw``,
    ``winsor``, ``zscore``, ``rank_normal``, ``industry_neutral``, ``size_neutral``; any
    lower-case id is accepted), ``value`` (sign-oriented by the catalog ``expected_sign``,
    so higher always means higher expected return; NULL when not standardized),
    ``expected_sign`` (+1/-1, constant per feature) and ``available_at`` (latest input
    clock; must be <= the formation cutoff). One row per (formation, security, feature,
    variant); multi-line issuers already collapsed to one line (R2a m1).
``research_feature_dates`` (formation x feature x variant)
    ``feature_version``, ``formation_date``, ``feature_id``, ``variant``, ``date_status``
    (``formed``; ``thin_cross_section`` when fewer than 200 valid names, which carries no
    standardized values), ``eligible_members``, ``valid_names``, ``coverage_fraction``.

Market cap (value weights, size buckets, size control) and the NYSE venue flag are read
from the R2a panel of the version (``market_cap`` daily feature, ``research_panel_cohort``
exchange code), not from R2b. Controls ``book_to_market`` and ``momentum_12_1`` are read
from the same feature version at ``control_variant`` (default ``rank_normal``). R3b's
access pattern is one query per (feature, all variants): R2b should cluster its value
table by ``feature_id`` (insertion order) so each query prunes row groups.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import itertools
import json
import math
import re
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass, field, replace
from functools import lru_cache
from pathlib import Path
from typing import Any

import duckdb
import numpy as np
import pandas as pd

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
from .panel import CALENDAR_FORMED, validate_research_panel
from .store import ResearchStore

EVALUATION_VERSION = "research-monthly-evaluation-v1"
EVALUATION_SCHEMA_VERSION = 1
FEATURE_CONTRACT = "r2b-feature-table-v1-declared-by-r3b"
FEATURE_VERSION_STATUSES = ("sealed", "untestable_strict")
FEATURE_VARIANTS = ("signed_raw", "winsor", "zscore", "rank_normal", "industry_neutral", "size_neutral")
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
SIZE_FEATURES = frozenset({"market_cap"})
LABEL_BASIS = "adjusted_close_forward_return_with_observed_or_policy_terminal_stitch"
BH_ALPHA = 0.05
FAMILY_STATUSES = ("tested", "insufficient_formations")
CELL_STATUSES = ("tested", "insufficient_formations", "no_values", BASIS_UNTESTABLE)

LABEL_VALID, LABEL_INVALID, LABEL_UNSUPPORTED, LABEL_MISSING = 0, 1, 2, 3
TERMINAL_NONE, TERMINAL_OBSERVED, TERMINAL_POLICY = 0, 1, 2

_ID = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_VERSION = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$")
_CODE_FILES = (Path(__file__), Path(stats.__file__), Path(__file__).with_name("labels.py"))
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
    return replace(spec, feature_versions=versions, horizons_months=horizons, marginal_months=marginal,
                   label_cutoff=cutoff, features=_ids(spec.features, "features"),
                   variants=_ids(spec.variants, "variants"), subperiods=tuple(subperiods),
                   control_features=controls or ())


def spec_payload(spec: EvaluationSpec) -> dict[str, Any]:
    """Canonical JSON-able form of a validated spec (``run_id`` excluded)."""
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
    )
    return validate_spec(spec)


# ---------------------------------------------------------------------------
# Canonical inputs (the pure engine never touches a database)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class FeatureData:
    """One feature, every variant, in canonical integer keys.

    ``values``: ``month_index``, ``security`` (int code), ``variant``, ``value`` and
    optionally ``available_at`` (checked against the formation cutoff). ``dates``
    (optional): ``month_index``, ``variant``, ``date_status`` and optionally
    ``coverage_fraction``; months whose status is not ``formed`` carry no values.
    """

    feature_id: str
    expected_sign: int
    values: pd.DataFrame
    dates: pd.DataFrame | None = None
    anomaly_class: str | None = None


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
    * ``context``: ``month_index``, ``security``, ``market_cap``, ``is_nyse`` and
      optionally ``valid_member`` (the cohort at the formation).
    * ``controls``: ``month_index``, ``security``, ``control``, ``value``.
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
    size_method: list[str]
    control_keys: dict[str, np.ndarray]
    control_values: dict[str, np.ndarray]
    samples: dict[int, _Samples]
    digests: dict[str, str]


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


def _size_buckets(keys: np.ndarray, cap: np.ndarray, nyse: np.ndarray, months: int, span: int,
                  spec: EvaluationSpec) -> tuple[np.ndarray, list[str]]:
    """Micro/small/large by NYSE 20th/50th percentiles, else cap terciles; 0 = unknown cap."""
    month = keys // span
    counts = np.bincount(month, minlength=months)[:months]
    starts = _starts(counts)
    buckets = np.zeros(len(keys), dtype=np.int8)
    methods: list[str] = []
    for m in range(months):
        begin, end = int(starts[m]), int(starts[m] + counts[m])
        caps = cap[begin:end]
        positive = np.isfinite(caps) & (caps > 0)
        nyse_caps = caps[positive & nyse[begin:end]]
        if len(nyse_caps) >= spec.nyse_min_names:
            low, high = np.percentile(nyse_caps, [20.0, 50.0])
            methods.append("nyse_20_50")
        elif positive.sum() >= 3:
            low, high = np.percentile(caps[positive], [100.0 / 3.0, 200.0 / 3.0])
            methods.append("cap_terciles")
        else:
            methods.append("none" if counts[m] else "no_context")
            continue
        segment = np.where(caps < low, 1, np.where(caps < high, 2, 3)).astype(np.int8)
        buckets[begin:end] = np.where(positive, segment, 0)
    return buckets, methods


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
        matured[h], expected_end[h] = mature, ends
        digests[f"labels_{h}m_sha256"] = _array_digest(keys, returns, status, terminal, mature, ends)

    del horizon_view
    context = inputs.context
    if len(context):
        ctx_keys, order = _keyed(context, months, span, "context")
        cap = pd.to_numeric(context["market_cap"], errors="coerce").to_numpy(dtype=float)[order]
        nyse = context["is_nyse"].fillna(False).to_numpy(dtype=bool)[order]
        member = (context["valid_member"].fillna(False).to_numpy(dtype=bool)[order] if "valid_member" in context
                  else np.ones(len(context), dtype=bool))
    else:
        ctx_keys, cap, nyse, member = np.zeros(0, np.int64), np.zeros(0), np.zeros(0, bool), np.zeros(0, bool)
    size, methods = _size_buckets(ctx_keys, cap, nyse, months, span, spec)
    digests["context_sha256"] = _array_digest(ctx_keys, cap, nyse, member)

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
    if inputs.release_frames:
        inputs.labels = inputs.context = inputs.controls = pd.DataFrame()
    return _Prepared(inputs.basis, months, span, month_start, formation, cutoff, formed, eligible,
                     label_keys, label_return, label_status, label_terminal, matured, expected_end,
                     ctx_keys, cap, size, member, methods, control_keys, control_values, samples, digests)


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
        return _Arrays(empty, empty, np.zeros(0), thin, coverage, 0)
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
    return _Arrays(month[keep], security[keep], value[keep], thin, coverage, len(part))


def _cell_identity(prep: _Prepared, feature: FeatureData, variant: str, h: int, meta: Mapping[str, Any],
                   sample: str) -> dict[str, Any]:
    return {
        "basis": prep.basis, "feature_id": feature.feature_id, "variant": variant, "horizon_months": h,
        "horizon_sessions": HORIZON_SESSIONS[h], "sample": sample,
        "anomaly_class": feature.anomaly_class, "expected_sign": int(feature.expected_sign),
        "identity_basis": meta.get("identity_basis"), "universe_basis": meta.get("universe_basis"),
        "classification_basis": meta.get("classification_basis"),
        "availability_basis": meta.get("availability_basis"), "label_basis": LABEL_BASIS,
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


def _evaluate_variant(prep: _Prepared, spec: EvaluationSpec, feature: FeatureData, variant: str,
                      arrays: _Arrays, meta: Mapping[str, Any]) -> dict[str, list[dict[str, Any]]]:
    months, span = prep.months, prep.span
    month, security, value = arrays.month, arrays.security, arrays.value
    out: dict[str, list[dict[str, Any]]] = {"cells": [], "slices": [], "quantiles": [], "decay": []}
    sample_name = "full" if spec.split is None else "selection"
    counts = np.bincount(month, minlength=months)[:months]
    formation_ok = counts >= spec.min_names
    keys = month * span + security
    deciles = grouped_quantiles(month, value, security, months, 10, 1)
    quintiles = grouped_quantiles(month, value, security, months, 5, 1)
    context_index = _lookup(prep.context_keys, keys)
    cap = _take(prep.context_cap, context_index, _NAN)
    size = _take(prep.context_size, context_index, 0).astype(np.int8)

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
        del u_cap
        assert vw10 is not None and vw5 is not None
        spreads = {"ew10": ew10[:, 9] - ew10[:, 0], "vw10": vw10[:, 9] - vw10[:, 0],
                   "ew5": ew5[:, 4] - ew5[:, 0], "vw5": vw5[:, 4] - vw5[:, 0]}
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
        with np.errstate(invalid="ignore", divide="ignore"):
            coverage = used_counts[in_sample] / prep.eligible[in_sample]
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
            "mean_coverage": _nanmean(coverage) if n_formations else None,
            "mean_feature_coverage": _nanmean(arrays.coverage[primary]),
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
        out["cells"].append(row)
        out["decay"].append(_decay_row(prep, feature, variant, "availability_lag1", h, lag_inf,
                                       lag_inf.mean / ic_inf.mean if ic_inf.mean else _NAN))
        # Slices: frozen split segments and subperiods.
        for (kind, name), mask in samples.masks.items():
            slice_mask = mask & usable
            purged, embargoed = samples.notes.get(name, (0, 0)) if kind == "split" else (0, 0)
            out["slices"].append(_slice_row(
                prep, feature, variant, h, kind, name, _infer(ic, mask, h), _infer(spreads["ew10"], mask, h), "ew10",
                int(slice_mask.sum()), purged, embargoed,
                float(used_counts[slice_mask].mean()) if slice_mask.any() else None, None))
        # Size buckets on the primary sample.
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
            valid_months = in_sample & (bucket_n >= spec.min_bucket_names)
            out["slices"].append(_slice_row(
                prep, feature, variant, h, "size_bucket", bucket_name, _infer(bucket_ic, primary, h),
                _infer(bucket_ls, primary, h), "ew5", int(valid_months.sum()), 0, 0,
                float(bucket_n[valid_months].mean()) if valid_months.any() else None,
                _nanmean(share[in_sample])))

    first = cumulative[0][1].mean if cumulative else _NAN
    for h, inference in cumulative:
        out["decay"].append(_decay_row(prep, feature, variant, "cumulative", h, inference,
                                       inference.mean / first if first else _NAN))
    if 1 in spec.horizons_months:
        out["decay"].extend(_marginal_decay(prep, spec, feature, variant, month, keys, value))
    return out


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
               embargoed: int, mean_names: float | None, share: float | None) -> dict[str, Any]:
    return {"basis": prep.basis, "feature_id": feature.feature_id, "variant": variant, "horizon_months": h,
            "slice_kind": kind, "slice_name": name, "formations": formations, "purged": purged,
            "embargoed": embargoed, "mean_names": mean_names, "name_share": share,
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


def _evaluate_feature(prep: _Prepared, inputs: BasisInputs, spec: EvaluationSpec,
                      feature_id: str) -> tuple[dict[str, list[dict[str, Any]]], dict[str, Any]]:
    feature = inputs.load_feature(feature_id)
    if feature.feature_id != feature_id:
        raise EvaluationInputError(f"loader returned {feature.feature_id} for {feature_id}")
    if feature.expected_sign not in (-1, 1):
        raise EvaluationInputError(f"{feature_id}: expected_sign must be +1 or -1")
    variants = [v for v in inputs.variants_by_feature.get(feature_id, ())
                if spec.variants is None or v in spec.variants]
    out: dict[str, list[dict[str, Any]]] = {"cells": [], "slices": [], "quantiles": [], "decay": []}
    digest = hashlib.sha256(feature_id.encode("utf-8"))
    total = 0
    for variant in variants:
        arrays = _variant_arrays(prep, feature, variant)
        digest.update(variant.encode("utf-8"))
        digest.update(_array_digest(arrays.month, arrays.security, arrays.value, arrays.thin,
                                    arrays.coverage).encode("ascii"))
        total += arrays.rows
        for key, rows in _evaluate_variant(prep, spec, feature, variant, arrays, inputs.meta).items():
            out[key].extend(rows)
    return out, {"basis": prep.basis, "feature_id": feature_id, "status": "evaluated",
                 "expected_sign": int(feature.expected_sign), "variants": _canonical(variants),
                 "value_rows": total, "values_sha256": digest.hexdigest()}


def _untestable_cells(inputs: BasisInputs, spec: EvaluationSpec,
                      evaluated: Iterable[tuple[str, str]]) -> list[dict[str, Any]]:
    """Status-only cells of a data-less basis for every evaluated or declared (feature, variant)."""
    rows = []
    meta = inputs.meta
    declared = {(f, v) for f, variants in inputs.variants_by_feature.items() for v in variants
                if (spec.features is None or f in spec.features) and (spec.variants is None or v in spec.variants)}
    for feature_id, variant in sorted(set(evaluated) | declared):
        for h in spec.horizons_months:
            rows.append({"basis": inputs.basis, "feature_id": feature_id, "variant": variant,
                         "horizon_months": h, "horizon_sessions": HORIZON_SESSIONS[h],
                         "sample": "full" if spec.split is None else "selection", "status": inputs.status,
                         "identity_basis": meta.get("identity_basis"), "universe_basis": meta.get("universe_basis"),
                         "classification_basis": meta.get("classification_basis"),
                         "availability_basis": meta.get("availability_basis"), "label_basis": LABEL_BASIS,
                         "formations_usable": 0, "feature_rows": 0, "ic_hlz_pass": False})
    return rows


def _basis_attrition(prep: _Prepared, spec: EvaluationSpec) -> list[dict[str, Any]]:
    """Label accounting over the basis cohort (context members) at matured formations."""
    rows: list[dict[str, Any]] = []
    member_keys = prep.context_keys[prep.context_member]
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

FAMILY_COLUMNS = ("family_member", "bh_q", "holm_p", "bh_discovery", "hlz_pass", "dsr", "dsr_z",
                  "dsr_benchmark", "dsr_n_trials", "dsr_effective_n", "dsr_sharpe_variance", "family_best")


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
                       "dsr": None, "dsr_z": None, "dsr_benchmark": None, "dsr_n_trials": None,
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
    summary = {
        "family_cells": int(member.sum()), "n_trials": n_trials, "tested_cells": int((status == "tested").sum()),
        "bh_alpha": BH_ALPHA, "bh_discoveries": int(family["bh_discovery"].sum()),
        "holm_discoveries": int(sum(1 for v in holm.values() if v is not None and v <= BH_ALPHA)),
        "hlz_passes": int(family["hlz_pass"].sum()),
        "family_rule": "cells with status tested or insufficient_formations; insufficient enter BH/Holm as p=1",
        "significance": "EWC fixed-b robust p (stats.mean_inference); NW t secondary",
        "horizons": summary_horizons,
    }
    return family, summary


# ---------------------------------------------------------------------------
# Result tables (schema, typing, digest)
# ---------------------------------------------------------------------------

_D, _I, _B, _V = "DOUBLE", "INTEGER", "BOOLEAN", "VARCHAR"
_IDENTITY = (("basis", _V), ("feature_id", _V), ("variant", _V), ("horizon_months", _I))
CELL_COLUMNS: tuple[tuple[str, str], ...] = (
    *_IDENTITY, ("horizon_sessions", _I), ("status", _V), ("sample", _V), ("anomaly_class", _V),
    ("expected_sign", _I), ("identity_basis", _V), ("universe_basis", _V), ("classification_basis", _V),
    ("availability_basis", _V), ("label_basis", _V),
    ("formations_calendar", _I), ("formations_formed", _I), ("formations_with_values", _I),
    ("formations_thin", _I), ("formations_matured", _I), ("formations_usable", _I),
    ("feature_rows", "BIGINT"), ("labels_valid", "BIGINT"), ("labels_missing", "BIGINT"),
    ("labels_invalid", "BIGINT"), ("labels_unsupported", "BIGINT"), ("stitched_observed", "BIGINT"),
    ("stitched_policy", "BIGINT"), ("mean_names", _D), ("min_names_used", _I), ("mean_coverage", _D),
    ("mean_feature_coverage", _D),
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
    ("family_member", _B), ("bh_q", _D), ("holm_p", _D), ("bh_discovery", _B), ("hlz_pass", _B), ("dsr", _D),
    ("dsr_z", _D), ("dsr_benchmark", _D), ("dsr_n_trials", _I), ("dsr_effective_n", _I),
    ("dsr_sharpe_variance", _D), ("family_best", _B),
)
SLICE_COLUMNS: tuple[tuple[str, str], ...] = (
    *_IDENTITY, ("slice_kind", _V), ("slice_name", _V), ("formations", _I), ("purged", _I), ("embargoed", _I),
    ("mean_names", _D), ("name_share", _D), ("ic_mean", _D), ("ic_robust_t", _D), ("ic_robust_p", _D),
    ("ic_robust_df", _I), ("ic_z", _D), ("ic_nw_t", _D), ("ls_kind", _V), ("ls_mean", _D), ("ls_robust_t", _D),
    ("ls_robust_p", _D), ("ls_z", _D),
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
        finished_at TIMESTAMP
    )"""


def ensure_evaluation_schema(con: duckdb.DuckDBPyConnection) -> None:
    """Create the ``research_eval_*`` tables (schema version 1) if absent.

    Kept inside this module until the research-store owner registers it as a store
    migration; the version table refuses a newer, unknown schema.
    """
    con.execute("""
        CREATE TABLE IF NOT EXISTS research_eval_schema (
            version INTEGER PRIMARY KEY, name VARCHAR NOT NULL, applied_at TIMESTAMP NOT NULL)""")
    versions = {int(row[0]) for row in con.execute("SELECT version FROM research_eval_schema").fetchall()}
    if versions - {EVALUATION_SCHEMA_VERSION}:
        raise RuntimeError(f"research evaluation schema has unknown versions {sorted(versions)}; code is older")
    con.execute(_RUN_DDL)
    for table, columns, _ in RESULT_TABLES.values():
        body = ", ".join(f"{name} {kind}" for name, kind in columns)
        con.execute(f"CREATE TABLE IF NOT EXISTS {table} (run_id VARCHAR NOT NULL, {body})")
    if not versions:
        con.execute("INSERT INTO research_eval_schema VALUES (?, ?, ?)",
                    [EVALUATION_SCHEMA_VERSION, "monthly_evaluation", dt.datetime.now(dt.UTC).replace(tzinfo=None)])


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
        else:
            data[name] = pd.Series([None if pd.isna(v) else str(v) for v in series], dtype=object)
    return pd.DataFrame(data)


def _insert(con: duckdb.DuckDBPyConnection, key: str, run_id: str, frame: pd.DataFrame) -> None:
    if frame is None or not len(frame):
        return
    table, columns, _ = RESULT_TABLES[key]
    typed = _typed(frame, columns)
    typed.insert(0, "run_id", run_id)
    con.register("_ev_insert", typed)
    try:
        con.execute(f"INSERT INTO {table} SELECT * FROM _ev_insert")
    finally:
        con.unregister("_ev_insert")


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


def _basis_manifest(inputs: BasisInputs, prep: _Prepared | None, absent: Sequence[str]) -> dict[str, Any]:
    manifest = {"basis": inputs.basis, "status": inputs.status, "meta": dict(inputs.meta),
                "digests": dict(inputs.digests), "features_absent": list(absent),
                "features": len(inputs.variants_by_feature)}
    if prep is not None:
        manifest["prepared_digests"] = prep.digests
        methods: dict[str, int] = {}
        for method in prep.size_method:
            methods[method] = methods.get(method, 0) + 1
        manifest["size_breakpoint_methods"] = dict(sorted(methods.items()))
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


def _frame(rows: list[dict[str, Any]], key: str) -> pd.DataFrame:
    columns = [name for name, _ in RESULT_TABLES[key][1]]
    return pd.DataFrame(rows, columns=columns) if rows else pd.DataFrame(columns=columns)


def evaluate_bases(bases: Iterable[BasisInputs], spec: EvaluationSpec, *,
                   label_diagnostics: Mapping[str, Any] | None = None) -> EvaluationTables:
    """Evaluate every basis in memory and seal the result digest (no store needed)."""
    spec = validate_spec(spec)
    rows: dict[str, list[dict[str, Any]]] = {key: [] for key in RESULT_TABLES}
    manifests: dict[str, Any] = {}
    pairs: set[tuple[str, str]] = set()
    untestable: list[BasisInputs] = []
    for inputs in bases:
        if inputs.basis in manifests:
            raise EvaluationInputError(f"basis {inputs.basis} supplied twice")
        features, absent = _selected_features(inputs, spec)
        if inputs.status == BASIS_UNTESTABLE:
            manifests[inputs.basis] = _basis_manifest(inputs, None, absent)
            untestable.append(inputs)
            continue
        if inputs.status != BASIS_AVAILABLE:
            raise EvaluationInputError(f"basis {inputs.basis}: unknown status {inputs.status!r}")
        prep = _prepare(inputs, spec)
        manifests[inputs.basis] = _basis_manifest(inputs, prep, absent)
        rows["attrition"].extend(_basis_attrition(prep, spec))
        for feature_id in features:
            produced, input_row = _evaluate_feature(prep, inputs, spec, feature_id)
            for key, items in produced.items():
                rows[key].extend(items)
            rows["feature_inputs"].append(input_row)
            pairs.update((feature_id, variant) for variant in json.loads(input_row["variants"]))
    for inputs in untestable:
        rows["cells"].extend(_untestable_cells(inputs, spec, pairs))
    if label_diagnostics is not None:
        rows["attrition"].extend(_label_source_attrition(label_diagnostics))
    frames = {key: _frame(items, key) for key, items in rows.items()}
    family_summary: dict[str, Any] = {}
    if len(frames["cells"]):
        family, family_summary = compute_family(frames["cells"])
        cells = frames["cells"].drop(columns=list(FAMILY_COLUMNS))
        frames["cells"] = cells.merge(family, on=["basis", "feature_id", "variant", "horizon_months"],
                                      how="left", validate="one_to_one")
    con = duckdb.connect(config={"threads": 1, "memory_limit": "256MB"})
    try:
        ensure_evaluation_schema(con)
        for key, frame in frames.items():
            _insert(con, key, "memory", frame)
        digest, parts = results_digest(con, "memory")
    finally:
        con.close()
    return EvaluationTables(**frames, family=family_summary, bases=manifests, results_sha256=digest,
                            table_digests=parts)


# ---------------------------------------------------------------------------
# R2b adapter (the ONLY reader of R2b tables) and R2a/R3a readers
# ---------------------------------------------------------------------------

_FEATURE_CONTRACT_COLUMNS: dict[str, tuple[str, ...]] = {
    "research_feature_versions": ("feature_version", "status", "basis", "panel_run_id", "panel_sha256",
                                  "classification_basis", "values_sha256"),
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


def _research_columns(con: duckdb.DuckDBPyConnection, table: str) -> set[str]:
    catalog = con.execute("SELECT current_database()").fetchone()
    rows = con.execute("SELECT column_name FROM duckdb_columns() WHERE database_name=? AND schema_name='main' "
                       "AND table_name=?", [catalog[0] if catalog else None, table]).fetchall()
    return {row[0] for row in rows}


def load_feature_table(store: ResearchStore, feature_version: str) -> FeatureTable:
    """R2b adapter: read one feature version under the contract in the module docstring.

    ``load_values(feature_id, variants)`` returns numpy arrays ``month_index``,
    ``security``, ``variant_code`` (index into ``variants``), ``value`` (NaN for NULL),
    ``available_at_us`` (epoch microseconds, -1 for NULL) and ``expected_sign``. It
    needs the caller's temp tables ``_ev_months(formation_date, month_index)`` and
    ``_ev_securities(security_id, code)``; unmapped rows come back as -1 and are refused
    by the caller.
    """
    con = store.con
    for table, required in _FEATURE_CONTRACT_COLUMNS.items():
        missing = sorted(set(required) - _research_columns(con, table))
        if missing:
            raise EvaluationInputError(f"R2b feature contract: research store table {table} lacks {missing} "
                                       f"(contract {FEATURE_CONTRACT}, atx_db.research.evaluation docstring)")
    row = con.execute("""
        SELECT status, basis, panel_run_id, panel_sha256, classification_basis, values_sha256
        FROM research_feature_versions WHERE feature_version=?
    """, [feature_version]).fetchall()
    if len(row) != 1:
        raise EvaluationInputError(f"feature version {feature_version!r} is absent or duplicated")
    status, basis, panel_run_id, panel_sha, classification, values_sha = row[0]
    if status not in FEATURE_VERSION_STATUSES:
        raise EvaluationInputError(f"feature version {feature_version} is {status!r}; only {FEATURE_VERSION_STATUSES}"
                                   " versions are evaluated")
    if basis not in BASES:
        raise EvaluationInputError(f"feature version {feature_version} has unknown basis {basis!r}")
    dates = con.execute("""
        SELECT formation_date, feature_id, variant, date_status, eligible_members, valid_names, coverage_fraction
        FROM research_feature_dates WHERE feature_version=?
        ORDER BY feature_id, variant, formation_date
    """, [feature_version]).df()

    def load_values(feature_id: str, variants: Sequence[str]) -> dict[str, np.ndarray]:
        result = con.execute("""
            SELECT coalesce(m.month_index, -1) AS month_index, coalesce(s.code, -1) AS security,
                   list_position(?::VARCHAR[], v.variant) - 1 AS variant_code,
                   coalesce(v.value, 'NaN'::DOUBLE) AS value,
                   coalesce(epoch_us(v.available_at), -1) AS available_at_us,
                   coalesce(v.expected_sign, 0) AS expected_sign
            FROM research_feature_values v
            LEFT JOIN _ev_months m ON m.formation_date = v.formation_date
            LEFT JOIN _ev_securities s ON s.security_id = v.security_id
            WHERE v.feature_version = ? AND v.feature_id = ? AND list_contains(?::VARCHAR[], v.variant)
        """, [list(variants), feature_version, feature_id, list(variants)]).fetchnumpy()
        return {name: np.asarray(values) for name, values in result.items()}

    return FeatureTable(feature_version, str(status), str(basis), str(panel_run_id), str(panel_sha),
                        None if classification is None else str(classification),
                        None if values_sha is None else str(values_sha), dates, load_values)


def _panel_run(store: ResearchStore, table: FeatureTable, verify: bool) -> dict[str, Any]:
    con = store.con
    row = con.execute("""
        SELECT status, basis, identity_basis, universe_basis, fundamental_availability_basis,
               market_availability_basis, panel_sha256, blockers_json, diagnostic_json
        FROM research_panel_runs WHERE run_id=?
    """, [table.panel_run_id]).fetchone()
    if row is None:
        raise EvaluationInputError(f"feature version {table.feature_version}: panel run {table.panel_run_id} absent")
    status, basis, identity, universe, fundamental_clock, market_clock, panel_sha, blockers, diagnostic = row
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
               CASE WHEN a.decision_session IS NULL THEN 'decision_not_in_label_calendar'
                    WHEN a.label_entry IS NULL THEN 'no_label_entry_session'
                    WHEN a.label_entry <> a.entry_date THEN 'entry_calendar_mismatch'
                    ELSE 'aligned' END AS alignment,
               x.trade_date AS expected_end,
               a.label_entry = a.entry_date AND x.trade_date IS NOT NULL
                 AND x.trade_date::TIMESTAMP + INTERVAL 1 DAY + INTERVAL 12 HOUR <= ? AS matured
        FROM aligned a CROSS JOIN horizons h
        LEFT JOIN _ev_sessions x ON x.session_number = a.entry_session + h.horizon_days
    """, [horizons, [HORIZON_SESSIONS[h] for h in horizons], cutoff])
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


def open_basis_inputs(store: ResearchStore, feature_version: str, spec: EvaluationSpec) -> BasisInputs:
    """Assemble one basis from the store: R2b adapter + R2a panel context + R3a labels."""
    con = store.con
    table = load_feature_table(store, feature_version)
    panel = _panel_run(store, table, spec.verify_panels)
    meta = {"feature_version": feature_version, "feature_version_status": table.status,
            "classification_basis": table.classification_basis, "r2b_values_sha256": table.values_sha256,
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
                                               "month_index": formed["month_index"].to_numpy(np.int64)}),
              (("formation_date", "DATE"), ("month_index", "BIGINT")))
    securities = [row[0] for row in con.execute(
        "SELECT DISTINCT security_id FROM research_panel_cohort WHERE run_id=? ORDER BY security_id",
        [table.panel_run_id]).fetchall()]
    _register(con, "_ev_securities", pd.DataFrame({"security_id": pd.Series(securities, dtype=object),
                                                   "code": np.arange(len(securities), dtype=np.int64)}),
              (("security_id", "VARCHAR"), ("code", "BIGINT")))
    context = con.execute("""
        WITH cap AS (
            SELECT formation_date, security_id, max(raw_value) AS market_cap
            FROM research_panel_values
            WHERE run_id=? AND metric_code='market_cap' AND metric_window='daily' AND reason='valid'
              AND raw_value > 0 AND isfinite(raw_value)
            GROUP BY ALL
        )
        SELECT m.month_index, s.code AS security, max(cap.market_cap) AS market_cap,
               bool_or(coalesce(c.exchange_code = ?, false)) AS is_nyse,
               bool_or(c.cohort_reason = 'valid') AS valid_member
        FROM research_panel_cohort c
        JOIN _ev_months m ON m.formation_date = c.formation_date
        JOIN _ev_securities s ON s.security_id = c.security_id
        LEFT JOIN cap ON cap.formation_date = c.formation_date AND cap.security_id = c.security_id
        WHERE c.run_id=?
        GROUP BY ALL ORDER BY 1, 2
    """, [table.panel_run_id, NYSE_EXCHANGE_CODE, table.panel_run_id]).df()
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
    catalog_classes = _catalog_classes()

    def load_feature(feature_id: str) -> FeatureData:
        variants = variants_by_feature[feature_id]
        arrays = table.load_values(feature_id, variants)
        if (arrays["month_index"] < 0).any():
            raise EvaluationInputError(f"{feature_id}: values dated at a non-formed panel formation")
        if (arrays["security"] < 0).any():
            raise EvaluationInputError(f"{feature_id}: values for securities outside the panel cohort")
        signs = set(np.unique(arrays["expected_sign"]).tolist())
        if len(signs) != 1 or signs - {-1, 1}:
            raise EvaluationInputError(f"{feature_id}: expected_sign must be one of +1/-1, got {sorted(signs)}")
        available = arrays["available_at_us"].astype("int64")
        stamps = available.astype("datetime64[us]")
        stamps[available < 0] = np.datetime64("NaT", "us")
        values = pd.DataFrame({
            "month_index": arrays["month_index"], "security": arrays["security"],
            "variant": pd.Categorical.from_codes(arrays["variant_code"].astype("int64"), categories=list(variants)),
            "value": arrays["value"].astype(float), "available_at": stamps})
        dates = table.dates[table.dates["feature_id"] == feature_id]
        date_frame = pd.DataFrame({
            "month_index": [month_of.get(day, -1) for day in _days(dates["formation_date"]).tolist()],
            "variant": dates["variant"].astype(str).to_numpy(), "date_status": dates["date_status"].astype(str).to_numpy(),
            "coverage_fraction": pd.to_numeric(dates["coverage_fraction"], errors="coerce").to_numpy(dtype=float)})
        if (date_frame["month_index"] < 0).any():
            raise EvaluationInputError(f"{feature_id}: date rows at a non-formed panel formation")
        return FeatureData(feature_id, int(signs.pop()), values, date_frame, catalog_classes.get(feature_id))

    digests = {"securities_sha256": _sha(_canonical(securities)), "label": label_info,
               "catalog_sha256": _catalog_sha()}
    return BasisInputs(table.basis, BASIS_AVAILABLE, max(len(securities), 1), calendar, labels, maturity,
                       context, controls_frame, variants_by_feature, load_feature, meta, digests,
                       release_frames=True)


def _catalog_classes() -> dict[str, str]:
    from .catalog import read_anomaly_catalog

    return {entry.feature_id: entry.anomaly_class for entry in read_anomaly_catalog()}


def _catalog_sha() -> str:
    from .catalog import anomaly_catalog_sha256

    return anomaly_catalog_sha256()


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


def _code_sha() -> str:
    digest = hashlib.sha256()
    for path in _CODE_FILES:
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes())
    return digest.hexdigest()


def _blockers(spec: EvaluationSpec, manifests: Mapping[str, Any]) -> list[str]:
    blockers = ["research_only_not_release_eligible"]
    if spec.split is None:
        blockers.append("no_frozen_split_selection_uses_all_formations")
    if "strict" not in manifests:
        blockers.append("strict_basis_not_supplied")
    for basis, manifest in sorted(manifests.items()):
        if manifest["status"] == BASIS_UNTESTABLE:
            blockers.append(f"{basis}_basis_untestable")
        if basis == "reconstructed":
            blockers.append("reconstructed_identity_universe_and_availability_not_certifiable")
        for blocker in manifest["meta"].get("panel_blockers", []):
            blockers.append(f"{basis}_panel:{blocker}")
        alignment = manifest["digests"].get("label", {}).get("alignment", {}) if manifest["digests"] else {}
        for status, count in sorted(alignment.items()):
            if status != "aligned" and count:
                blockers.append(f"{basis}_{status}:{count}")
        if manifest["features_absent"]:
            blockers.append(f"{basis}_features_absent:{len(manifest['features_absent'])}")
    if FEATURE_CONTRACT.endswith("declared-by-r3b"):
        blockers.append("feature_table_contract_declared_by_r3b_pending_r2b")
    if not spec.label_diagnostics:
        blockers.append("label_source_attrition_not_measured")
    return blockers


def _open_bases(store: ResearchStore, spec: EvaluationSpec) -> Iterable[BasisInputs]:
    for version in spec.feature_versions:
        yield open_basis_inputs(store, version, spec)


def _diagnostics(store: ResearchStore, spec: EvaluationSpec) -> dict[str, Any] | None:
    if not spec.label_diagnostics:
        return None
    result: dict[str, Any] = monthly_label_diagnostics(
        store,  # type: ignore[arg-type]  # ResearchStore duck-types DuckDBStore.con
        source=spec.label_source, horizons=[HORIZON_SESSIONS[h] for h in spec.horizons_months],
        cutoff=spec.label_cutoff)
    return result


def run_evaluation(store: ResearchStore, spec: EvaluationSpec, *, resume: bool = False) -> EvaluationRunResult:
    """Evaluate the spec's feature versions and persist a sealed run in the research store.

    One transaction per (basis, feature); ``resume=True`` continues a ``building`` or
    ``failed`` run with an identical spec and code, skipping features already written.
    RX7: a frozen split is required unless ``allow_unsplit`` (recorded as a blocker).
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
        manifests: dict[str, Any] = {}
        untestable: list[BasisInputs] = []
        done = {(row[0], row[1]) for row in con.execute(
            "SELECT basis, feature_id FROM research_eval_feature_inputs WHERE run_id=?", [spec.run_id]).fetchall()}
        for inputs in _open_bases(store, spec):
            if inputs.basis in manifests:
                raise EvaluationInputError(f"basis {inputs.basis} supplied twice")
            features, absent = _selected_features(inputs, spec)
            prep = None if inputs.status == BASIS_UNTESTABLE else _prepare(inputs, spec)
            manifests[inputs.basis] = _basis_manifest(inputs, prep, absent)
            previous = stored_inputs.get("bases", {}).get(inputs.basis)
            if previous is not None and previous != manifests[inputs.basis]:
                raise EvaluationInputError(f"resume refused: basis {inputs.basis} inputs changed")
            with store.transaction():
                con.execute("UPDATE research_eval_runs SET inputs_json=? WHERE run_id=?",
                            [_canonical({"bases": manifests}), spec.run_id])
            if prep is None:
                untestable.append(inputs)
                continue
            scope_written = con.execute("SELECT count(*) FROM research_eval_attrition WHERE run_id=? AND scope=?",
                                        [spec.run_id, inputs.basis]).fetchone()
            if not (scope_written and scope_written[0]):
                with store.transaction():
                    _insert(con, "attrition", spec.run_id, _frame(_basis_attrition(prep, spec), "attrition"))
            for feature_id in features:
                if (inputs.basis, feature_id) in done:
                    continue
                produced, input_row = _evaluate_feature(prep, inputs, spec, feature_id)
                with store.transaction():
                    for key, items in produced.items():
                        _insert(con, key, spec.run_id, _frame(items, key))
                    _insert(con, "feature_inputs", spec.run_id, _frame([input_row], "feature_inputs"))
        pairs = {(row[0], row[1]) for row in con.execute(
            "SELECT DISTINCT feature_id, variant FROM research_eval_cells WHERE run_id=? AND status<>?",
            [spec.run_id, BASIS_UNTESTABLE]).fetchall()}
        with store.transaction():
            for inputs in untestable:
                con.execute("DELETE FROM research_eval_cells WHERE run_id=? AND basis=?", [spec.run_id, inputs.basis])
                _insert(con, "cells", spec.run_id, _frame(_untestable_cells(inputs, spec, pairs), "cells"))
        cells = con.execute("""
            SELECT basis, feature_id, variant, horizon_months, status, ic_robust_p, ic_hlz_pass,
                   sharpe, sharpe_skew, sharpe_kurt, sharpe_n
            FROM research_eval_cells WHERE run_id=? ORDER BY basis, feature_id, variant, horizon_months
        """, [spec.run_id]).df()
        family, family_summary = compute_family(cells) if len(cells) else (pd.DataFrame(), {})
        with store.transaction():
            if len(family):
                typed = _typed(family, [(name, kind) for name, kind in CELL_COLUMNS
                                        if name in family.columns])
                con.register("_ev_family", typed)
                try:
                    assignments = ", ".join(f"{name}=f.{name}" for name in FAMILY_COLUMNS)
                    con.execute(f"""
                        UPDATE research_eval_cells AS c SET {assignments} FROM _ev_family f
                        WHERE c.run_id=? AND c.basis=f.basis AND c.feature_id=f.feature_id
                          AND c.variant=f.variant AND c.horizon_months=f.horizon_months
                    """, [spec.run_id])
                finally:
                    con.unregister("_ev_family")
            con.execute("DELETE FROM research_eval_attrition WHERE run_id=? AND scope='label_source'", [spec.run_id])
        diagnostics = _diagnostics(store, spec)
        with store.transaction():
            if diagnostics is not None:
                _insert(con, "attrition", spec.run_id, _frame(_label_source_attrition(diagnostics), "attrition"))
        results_sha, parts = results_digest(con, spec.run_id)
        inputs_json = _canonical({"bases": manifests, "feature_inputs": parts["feature_inputs"],
                                  "label_source": spec.label_source, "code_sha256": code_sha})
        blockers = _blockers(spec, manifests)
        with store.transaction():
            con.execute("""
                UPDATE research_eval_runs SET status='complete', inputs_json=?, inputs_sha256=?, results_sha256=?,
                    family_json=?, blockers_json=?, diagnostic_json=?, finished_at=?
                WHERE run_id=? AND status='building'
            """, [inputs_json, _sha(inputs_json), results_sha, _canonical(family_summary), _canonical(blockers),
                  _canonical({"tables": parts, "label_diagnostics": diagnostics}),
                  dt.datetime.now(dt.UTC).replace(tzinfo=None), spec.run_id])
        return EvaluationRunResult(spec.run_id, "complete", results_sha, _sha(inputs_json),
                                   int(parts["cells"]["rows"]), family_summary, tuple(blockers))
    except Exception as exc:
        with store.transaction():
            con.execute("UPDATE research_eval_runs SET status='failed', diagnostic_json=? "
                        "WHERE run_id=? AND status='building'",
                        [_canonical({"error": type(exc).__name__, "message": str(exc)}), spec.run_id])
        raise


def verify_evaluation_run(store: ResearchStore, run_id: str) -> dict[str, Any]:
    """Re-derive a sealed run from its manifest and compare digests.

    ``stored_rows_match``: the persisted rows still hash to the sealed digest (no
    tampering). ``reproduced``: recomputing from the manifest's spec over the current
    inputs gives byte-identical results and identical input digests.
    """
    con = store.con
    row = con.execute("SELECT status, spec_json, results_sha256, inputs_json FROM research_eval_runs WHERE run_id=?",
                      [run_id]).fetchone()
    if row is None or row[0] != "complete":
        raise EvaluationInputError(f"evaluation run {run_id} is absent or not sealed")
    spec = spec_from_payload(json.loads(row[1]), run_id)
    stored_sha, _ = results_digest(con, run_id)
    tables = evaluate_bases(_open_bases(store, spec), spec, label_diagnostics=_diagnostics(store, spec))
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
    "FEATURE_CONTRACT",
    "FEATURE_VARIANTS",
    "HORIZON_SESSIONS",
    "SIZE_BUCKETS",
    "BasisInputs",
    "EvaluationInputError",
    "EvaluationRunResult",
    "EvaluationSpec",
    "EvaluationTables",
    "FeatureData",
    "FeatureTable",
    "FrozenSplit",
    "LookaheadError",
    "compute_family",
    "empty_basis",
    "ensure_evaluation_schema",
    "evaluate_bases",
    "freeze_split",
    "grouped_average_ranks",
    "grouped_quantiles",
    "grouped_rank_correlation",
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
