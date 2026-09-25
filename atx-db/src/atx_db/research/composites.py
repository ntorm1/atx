"""Build-forward v1 (task P5): redundancy, spanning and composites over the R4 ledger.

Input is a sealed R4 qualification ledger (``research_qualification_*``), the sealed R3b
run it graded, the R2b feature version of the composite basis and P4 monthly factor
returns. Everything here is research-only and lives in the research store.

Redundancy annotation (never alters an R4 status)
-------------------------------------------------
Over the qualified signals (``qualified_strict`` / ``qualified_reconstructed``):

* **Rank correlation.** Per fit formation, the Spearman correlation of two signals'
  sign-oriented values (the policy's ``constituent_variant``) over the names valued on
  both (at least ``min_names_per_formation``), averaged over the fit formations (at
  least ``min_formations``). Signed: two oriented signals that disagree are not
  duplicates.
* **Clusters.** Complete-linkage agglomeration at the declared
  ``rank_correlation_threshold`` (every pair inside a cluster correlates at or above
  it); deterministic tie-breaks. The representative is the member with the largest
  ``|ic_z|`` in the ledger (then the feature id).
* **Spanning.** The signal's R3b equal-weighted decile long-short series at 1 month
  (``research_eval_series.ls_ew10``, selection formations only, oriented by the realized
  direction of a two-sided hypothesis) regressed on the P4 monthly factors with
  :func:`atx_db.research.factor_returns.span_test` (EWC fixed-b robust alpha). The
  series and the P4 monthly rows share the formation date and the 21-session window.
* ``redundancy_status``: ``duplicate_of:<representative>`` for a non-representative
  cluster member; else ``spanned_by_known_factors`` when the oriented alpha z is below
  ``alpha_min_z``; else ``novel``. A signal whose spanning regression is untestable
  (too few periods) is not called spanned; its ``span_status`` says why and the build
  carries a blocker.

Composites
----------
Constituents are the qualified signals (a duplicate whose representative sits in
another anomaly class is left out, so one construct is not counted in two classes).
Per formation every constituent's oriented value is standardized to a cross-sectional
z. A **class composite** is the weighted mean of its members' z over the names with at
least ``min_weight_coverage`` of the class weight valued, re-standardized to a z; the
**across-class composite** combines the class composites the same way. Two weightings:
``equal`` and ``ic_shrunk`` = ``(1 - s) * max(IC, 0) / sum max(IC, 0) + s / k`` with
the declared shrinkage ``s`` (equal weights when no IC is positive), where IC is the
mean monthly rank IC of the member at ``fit_horizon_months`` over the fit sample.
Within-class composites are emitted for classes with at least
``min_class_constituents`` members, across-class composites when at least
``min_across_inputs`` classes exist. Ids: ``cmp_<class>_eq`` / ``cmp_<class>_icw`` and
``cmp_classes_eq`` / ``cmp_classes_icw``.

Fit sample (RX7): the formed formations of the frozen split's train and validation
segments whose label window at the fit horizon ends before the holdout starts. The fit
(:func:`fit_weights`) raises :class:`CompositeLeakError` on any row dated in, or whose
label window reaches into, the holdout; the correlations and the spanning regression
use the same selection sample. Holdout values are computed (a composite is defined on
every formation) but never enter a fit.

Evaluation and storage
----------------------
Composite values become an R2b-shaped feature version (store schema 3, query version
of R2b, ``anomaly_class='composite'``, the constituent ids and weights in each catalog
row's ``inputs_json``): every variant is produced by R2b's own
:func:`atx_db.research.features.standardize_frame` under the constituent version's
standardization policy, and the version passes R2b's validator, so the R3b harness and
R4 read it like any other version. One R3b run evaluates the composite family once,
including its holdout slices; the research store allows exactly one composite build
(one holdout evaluation) per R4 ledger, and an identical rerun is byte-identical.
:func:`evaluate_composites` is the in-memory form of that evaluation (the same engine).
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, replace
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import duckdb
import numpy as np
import pandas as pd

from . import evaluation as ev
from . import factor_returns as fr
from . import features as rf
from . import qualification as rq
from .panel import CALENDAR_FORMED
from .store import ResearchStore

COMPOSITE_VERSION = "research-composites-v1"
COMPOSITE_SCHEMA_VERSION = 1
POLICY_PATH = Path(__file__).resolve().parents[1] / "seeds" / "research_composite_policy.json"
#: Every committed composite policy version and its canonical content hash (RX7: a
#: changed policy needs a new version and a new pin).
FROZEN_POLICY_SHA256 = {
    "p5-composite-v1": "1e90aab500ac5e49bec85ab19a82a19f40e1a1cd302fa3c1e303cb04dc45d702",
}
NOVEL = "novel"
SPANNED = "spanned_by_known_factors"
DUPLICATE_PREFIX = "duplicate_of:"
SPAN_TESTED, SPAN_TOO_FEW, SPAN_DEGENERATE = "tested", "too_few_periods", "degenerate"
LEVEL_WITHIN, LEVEL_ACROSS = "within_class", "across_class"
WEIGHT_EQUAL, WEIGHT_IC = "equal", "ic_shrunk"
WEIGHT_TAGS = {WEIGHT_EQUAL: "eq", WEIGHT_IC: "icw"}
COMPOSITE_CLASS = "composite"
COMPOSITE_PRIOR_EVIDENCE = "composite_of_qualified_signals"
COMPOSITE_CAVEATS = ("composite_of_qualified_signals", "weights_fit_on_selection_sample")
UNCLASSIFIED = "unclassified"
_ID_CHARS = "abcdefghijklmnopqrstuvwxyz0123456789_"
_CODE_FILES = (Path(__file__), Path(rf.__file__), Path(ev.__file__))
_SECTION_KEYS = {
    "source": {"qualified_statuses", "basis", "constituent_variant", "fit_horizon_months", "min_names_per_formation"},
    "redundancy": {"rank_correlation_threshold", "linkage", "min_formations", "representative"},
    "spanning": {"factors", "series", "horizon_months", "alpha_min_z", "min_periods"},
    "composite": {"weightings", "ic_shrinkage", "min_class_constituents", "min_across_inputs",
                  "min_weight_coverage", "exclude_cross_class_duplicates"},
}
_TOP_KEYS = {"composite_policy_version", "title", "frozen_on", "authority", "source", "redundancy", "spanning",
             "composite", "policy_sha256"}
_SERIES_COLUMNS = ("ls_ew10", "ls_vw10", "ls_ew5", "ls_vw5")


class CompositeError(ValueError):
    """Build-forward inputs violate the contract (policy, ledger, store)."""


class CompositeLeakError(CompositeError):
    """A holdout row reached a fit (RX7)."""


def _canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False)


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _clean(value: Any) -> Any:
    if isinstance(value, (bool, np.bool_)):
        return bool(value)
    if isinstance(value, (int, np.integer)):
        return int(value)
    if isinstance(value, (float, np.floating)):
        return float(value) if math.isfinite(float(value)) else None
    if isinstance(value, Mapping):
        return {str(k): _clean(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_clean(v) for v in value]
    if isinstance(value, (dt.date, dt.datetime)):
        return value.isoformat()
    return value


def code_sha256() -> str:
    """EOL-normalized digest of this module and the R2b/R3b code it relies on."""
    digest = hashlib.sha256()
    for path in _CODE_FILES:
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes().replace(b"\r\n", b"\n"))
    return digest.hexdigest()


def _day(value: Any) -> dt.date:
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    return pd.Timestamp(value).date()


# ---------------------------------------------------------------------------
# Policy
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class CompositePolicy:
    version: str
    sha256: str
    content: Mapping[str, Any]
    qualified_statuses: tuple[str, ...]
    basis: str
    constituent_variant: str
    fit_horizon: int
    min_names: int
    rho_threshold: float
    linkage: str
    min_formations: int
    span_factors: tuple[str, ...]
    span_series: str
    span_horizon: int
    alpha_min_z: float
    span_min_periods: int
    weightings: tuple[str, ...]
    ic_shrinkage: float
    min_class_constituents: int
    min_across_inputs: int
    min_weight_coverage: float
    exclude_cross_class_duplicates: bool


def policy_sha256(content: Mapping[str, Any]) -> str:
    """Canonical content hash of a composite policy (its own ``policy_sha256`` excluded)."""
    return _sha(_canonical({key: value for key, value in content.items() if key != "policy_sha256"}))


def _number(section: Mapping[str, Any], key: str, low: float, high: float) -> float:
    value = section.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not low <= float(value) <= high:
        raise CompositeError(f"composite policy: {key} must be a number {low}..{high}")
    return float(value)


def _count(section: Mapping[str, Any], key: str, low: int, high: int) -> int:
    value = section.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise CompositeError(f"composite policy: {key} must be an integer {low}..{high}")
    return value


def load_composite_policy(source: Path | str | Mapping[str, Any] = POLICY_PATH, *,
                          allow_unpinned: bool = False) -> CompositePolicy:
    """Load and verify a frozen composite policy (declared hash, pin, exact keys)."""
    content: Any = source if isinstance(source, Mapping) else json.loads(Path(source).read_bytes().decode("utf-8"))
    if not isinstance(content, Mapping) or set(content) != _TOP_KEYS:
        raise CompositeError(f"composite policy keys must be exactly {sorted(_TOP_KEYS)}")
    for name, keys in _SECTION_KEYS.items():
        if not isinstance(content[name], Mapping) or set(content[name]) != keys:
            raise CompositeError(f"composite policy section {name} keys must be exactly {sorted(keys)}")
    version = content["composite_policy_version"]
    actual = policy_sha256(content)
    if content["policy_sha256"] != actual:
        raise CompositeError(f"composite policy {version}: declared sha256 {content['policy_sha256']} != content "
                             f"{actual}")
    pinned = FROZEN_POLICY_SHA256.get(version)
    if pinned is None and not allow_unpinned:
        raise CompositeError(f"composite policy {version} is not pinned in FROZEN_POLICY_SHA256")
    if pinned is not None and pinned != actual:
        raise CompositeError(f"composite policy {version} is frozen with sha256 {pinned}; an edit needs a new version")
    source_s, redundancy, spanning, composite = (content[k] for k in ("source", "redundancy", "spanning", "composite"))
    statuses = tuple(source_s["qualified_statuses"])
    if not statuses or set(statuses) - set(rq.QUALIFIED_STATUSES):
        raise CompositeError(f"qualified_statuses must be a subset of {rq.QUALIFIED_STATUSES}")
    if source_s["basis"] not in ev.BASES:
        raise CompositeError(f"basis must be one of {ev.BASES}")
    if source_s["constituent_variant"] not in ev.FEATURE_VARIANTS:
        raise CompositeError(f"constituent_variant must be one of {ev.FEATURE_VARIANTS}")
    fit_horizon = _count(source_s, "fit_horizon_months", 1, 12)
    if fit_horizon not in ev.HORIZON_SESSIONS:
        raise CompositeError(f"fit_horizon_months must be one of {sorted(ev.HORIZON_SESSIONS)}")
    if redundancy["linkage"] != "complete" or redundancy["representative"] != "max_abs_ic_z":
        raise CompositeError("v1 supports linkage 'complete' and representative 'max_abs_ic_z' only")
    factors = tuple(spanning["factors"])
    if not factors or len(set(factors)) != len(factors):
        raise CompositeError("spanning factors must be unique and non-empty")
    if spanning["series"] not in _SERIES_COLUMNS:
        raise CompositeError(f"spanning series must be one of {_SERIES_COLUMNS}")
    weightings = tuple(composite["weightings"])
    if not weightings or set(weightings) - set(WEIGHT_TAGS) or len(set(weightings)) != len(weightings):
        raise CompositeError(f"weightings must be unique values of {sorted(WEIGHT_TAGS)}")
    if not isinstance(composite["exclude_cross_class_duplicates"], bool):
        raise CompositeError("exclude_cross_class_duplicates must be a boolean")
    return CompositePolicy(
        version=str(version), sha256=actual, content=dict(content), qualified_statuses=statuses,
        basis=str(source_s["basis"]), constituent_variant=str(source_s["constituent_variant"]),
        fit_horizon=fit_horizon, min_names=_count(source_s, "min_names_per_formation", 3, 100_000),
        rho_threshold=_number(redundancy, "rank_correlation_threshold", 0.0, 1.0), linkage="complete",
        min_formations=_count(redundancy, "min_formations", 2, 10_000), span_factors=factors,
        span_series=str(spanning["series"]), span_horizon=_count(spanning, "horizon_months", 1, 1),
        alpha_min_z=_number(spanning, "alpha_min_z", 0.0, 10.0),
        span_min_periods=_count(spanning, "min_periods", 3, 10_000), weightings=weightings,
        ic_shrinkage=_number(composite, "ic_shrinkage", 0.0, 1.0),
        min_class_constituents=_count(composite, "min_class_constituents", 2, 1_000),
        min_across_inputs=_count(composite, "min_across_inputs", 2, 1_000),
        min_weight_coverage=_number(composite, "min_weight_coverage", 0.0, 1.0),
        exclude_cross_class_duplicates=bool(composite["exclude_cross_class_duplicates"]))


# ---------------------------------------------------------------------------
# The R4 ledger and R3b series (read-only)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class LedgerSignal:
    feature_id: str
    status: str
    status_basis: str | None
    anomaly_class: str
    expected_sign: int
    raw_direction: int | None
    ic_z: float | None
    ic_mean: float | None
    hypothesis_family: str | None

    @property
    def orientation(self) -> int:
        """+1 for a signed hypothesis (values are oriented); the realized sign for a two-sided one."""
        if self.expected_sign != 0:
            return 1
        return -1 if (self.raw_direction or 1) < 0 else 1


@dataclass(frozen=True)
class LedgerSource:
    ledger_id: str
    ledger_sha256: str
    run_id: str
    r4_policy_version: str
    split_sha256: str | None
    post_hoc_policy: bool
    signals: tuple[LedgerSignal, ...]


def _signal(row: Mapping[str, Any]) -> LedgerSignal:
    def number(value: Any) -> float | None:
        return None if value is None or (isinstance(value, float) and not math.isfinite(value)) else float(value)

    sign = row.get("expected_sign")
    return LedgerSignal(str(row["feature_id"]), str(row["status"]), row.get("status_basis"),
                        str(row.get("anomaly_class") or UNCLASSIFIED), int(sign) if sign is not None else 1,
                        None if row.get("raw_direction") is None else int(row["raw_direction"]),
                        number(row.get("ic_z")), number(row.get("ic_mean")), row.get("hypothesis_family"))


def ledger_source(ledger: rq.QualificationLedger) -> LedgerSource:
    """The build-forward view of an in-memory R4 ledger."""
    manifest = ledger.manifest
    return LedgerSource(ledger.ledger_id, ledger.sha256, str(manifest["evaluation"]["run_id"]),
                        str(manifest["policy"]["version"]), manifest["policy"].get("split_sha256"),
                        bool(manifest.get("post_hoc_policy")),
                        tuple(_signal(row) for row in sorted(ledger.features, key=lambda r: r["feature_id"])))


def load_ledger(con: duckdb.DuckDBPyConnection, ledger_id: str) -> LedgerSource:
    """A sealed R4 ledger from the research store (its stored rows must still hash to its seal)."""
    row = con.execute("SELECT ledger_sha256, manifest_json FROM research_qualification_ledgers WHERE ledger_id = ?",
                      [ledger_id]).fetchone()
    if row is None:
        raise CompositeError(f"no stored R4 ledger {ledger_id}")
    manifest = json.loads(row[1])
    frame = con.execute("SELECT * FROM research_qualification_features WHERE ledger_id = ? ORDER BY feature_id",
                        [ledger_id]).df()
    frame = frame.astype(object).where(pd.notna(frame), None)
    signals = tuple(_signal(record) for record in frame.to_dict("records"))
    return LedgerSource(ledger_id, str(row[0]), str(manifest["evaluation"]["run_id"]),
                        str(manifest["policy"]["version"]), manifest["policy"].get("split_sha256"),
                        bool(manifest.get("post_hoc_policy")), signals)


def load_selection_series(con: duckdb.DuckDBPyConnection, run_id: str, policy: CompositePolicy,
                          feature_ids: Sequence[str]) -> pd.DataFrame:
    """R3b per-formation long-short series of the spanning test: selection formations only.

    Columns ``feature_id``, ``formation_date``, ``value`` (the policy's series column at
    its horizon, primary variant, composite basis). A holdout row is refused.
    """
    frame = con.execute(f"""
        SELECT feature_id, formation_date, segment, in_selection, {policy.span_series} AS value
        FROM research_eval_series
        WHERE run_id = ? AND basis = ? AND variant = ? AND horizon_months = ? AND list_contains(?::VARCHAR[], feature_id)
          AND in_selection
        ORDER BY feature_id, formation_date
    """, [run_id, policy.basis, policy.constituent_variant, policy.span_horizon, list(feature_ids)]).df()
    if (frame["segment"] == "holdout").any():
        raise CompositeLeakError("the R3b selection series carries holdout rows")
    return frame[["feature_id", "formation_date", "value"]]


# ---------------------------------------------------------------------------
# Fit sample and the holdout guard
# ---------------------------------------------------------------------------

def fit_sample(inputs: ev.BasisInputs, split: ev.FrozenSplit, horizon: int) -> pd.DataFrame:
    """Formed train/validation formations whose ``horizon`` label window ends before the holdout.

    Columns ``month_index``, ``formation_date``, ``label_end``, ``cutoff``.
    """
    calendar = inputs.calendar
    formed = calendar[calendar["status"] == CALENDAR_FORMED]
    maturity = inputs.maturity
    ends = maturity[maturity["horizon_months"] == horizon].set_index("month_index")["expected_end"]
    segments = {name: (start, end) for name, start, end in split.segments}
    rows = []
    for month, formation, cutoff in zip(formed["month_index"].tolist(), formed["formation_date"].tolist(),
                                        formed["cutoff"].tolist(), strict=True):
        day = _day(formation)
        in_fit = any(start <= day <= end for name, (start, end) in segments.items() if name != "holdout")
        end = ends.get(int(month))
        if not in_fit or end is None or pd.isna(end) or _day(end) >= split.holdout_start:
            continue
        rows.append({"month_index": int(month), "formation_date": day, "label_end": _day(end),
                     "cutoff": pd.Timestamp(cutoff).to_pydatetime()})
    return pd.DataFrame(rows, columns=["month_index", "formation_date", "label_end", "cutoff"])


def fit_weights(ic_rows: pd.DataFrame, members: Sequence[str], split: ev.FrozenSplit, *, weighting: str,
                shrinkage: float) -> dict[str, dict[str, Any]]:
    """Weights of ``members`` from their monthly ICs on the fit sample (train + validation only).

    ``ic_rows``: ``member_id``, ``formation_date``, ``label_end``, ``ic``. Any row dated in
    the holdout, or whose label window ends on/after the holdout start (or is unknown),
    raises :class:`CompositeLeakError`: the holdout is evaluated once, never fit.
    """
    if not members:
        return {}
    if len(ic_rows):
        formed = pd.to_datetime(ic_rows["formation_date"])
        ends = pd.to_datetime(ic_rows["label_end"])
        holdout = pd.Timestamp(split.holdout_start)
        leak = (formed >= holdout) | ends.isna() | (ends >= holdout)
        if bool(leak.any()):
            first = ic_rows.loc[leak.to_numpy(), "formation_date"].min()
            raise CompositeLeakError(f"fit refuses {int(leak.sum())} holdout row(s) (first {first}; holdout starts "
                                     f"{split.holdout_start}): composites are fit on train + validation only")
    stats: dict[str, tuple[float | None, int]] = {}
    for member in members:
        values = ic_rows.loc[ic_rows["member_id"] == member, "ic"].to_numpy(dtype=float) if len(ic_rows) else \
            np.array([])
        values = values[np.isfinite(values)]
        stats[member] = (float(values.mean()) if len(values) else None, len(values))
    k = len(members)
    equal = 1.0 / k
    if weighting == WEIGHT_EQUAL:
        weights = {member: equal for member in members}
    elif weighting == WEIGHT_IC:
        positive = {member: max(stats[member][0] or 0.0, 0.0) for member in members}
        total = sum(positive.values())
        weights = {member: (1.0 - shrinkage) * (positive[member] / total if total > 0 else equal) + shrinkage * equal
                   for member in members}
    else:
        raise CompositeError(f"unknown weighting {weighting!r}")
    return {member: {"weight": weights[member], "fit_ic": stats[member][0], "fit_formations": stats[member][1]}
            for member in members}


# ---------------------------------------------------------------------------
# Dense month batches
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class _Member:
    member_id: str
    month: np.ndarray
    security: np.ndarray
    value: np.ndarray
    clock: np.ndarray  # epoch microseconds, -1 unknown


def _load_member(inputs: ev.BasisInputs, feature_id: str, variant: str, orientation: int) -> _Member:
    if variant not in inputs.variants_by_feature.get(feature_id, ()):
        raise CompositeError(f"{feature_id}: the {inputs.basis} basis has no {variant} values")
    data = inputs.load_feature(feature_id)
    frame = data.values
    value = frame["value"].to_numpy(dtype=float)
    keep = (frame["variant"].astype(str) == variant).to_numpy() & np.isfinite(value)
    if "unlinked_line" in frame:  # composites rank linked primary lines only
        keep = keep & ~frame["unlinked_line"].to_numpy(dtype=bool)
    month = frame["month_index"].to_numpy(dtype=np.int64)[keep]
    security = frame["security"].to_numpy(dtype=np.int64)[keep]
    if "available_at" in frame:
        stamps = pd.to_datetime(frame["available_at"]).to_numpy()[keep]
        clock = np.where(pd.isna(stamps), -1, stamps.astype("datetime64[us]").astype(np.int64))
    else:
        clock = np.full(len(month), -1, dtype=np.int64)
    order = np.lexsort((security, month))
    return _Member(feature_id, month[order], security[order], orientation * value[keep][order], clock[order])


def _scatter(month: np.ndarray, security: np.ndarray, values: np.ndarray, batch: np.ndarray, width: int,
             fill: float | int = np.nan) -> np.ndarray:
    out = np.full((len(batch), width), fill, dtype=np.int64 if isinstance(fill, int) else float)
    lo, hi = np.searchsorted(month, batch[0], "left"), np.searchsorted(month, batch[-1], "right")
    part = month[lo:hi]
    if not len(part):
        return out
    pos = np.minimum(np.searchsorted(batch, part), len(batch) - 1)
    keep = batch[pos] == part
    out[pos[keep], security[lo:hi][keep]] = values[lo:hi][keep]
    return out


def _zscore_rows(block: np.ndarray) -> np.ndarray:
    finite = np.isfinite(block)
    n = finite.sum(axis=1)
    with np.errstate(invalid="ignore", divide="ignore"):
        mean = np.where(finite, block, 0.0).sum(axis=1) / n
        centered = np.where(finite, block - mean[:, None], 0.0)
        sd = np.sqrt((centered * centered).sum(axis=1) / n)
        out = (block - mean[:, None]) / sd[:, None]
    out[(n < 3) | ~(sd > 0)] = np.nan
    return out


def _combine(blocks: Sequence[np.ndarray], clocks: Sequence[np.ndarray], weights: Sequence[float],
             min_coverage: float) -> tuple[np.ndarray, np.ndarray]:
    total = float(sum(weights))
    numerator = np.zeros(blocks[0].shape)
    denominator = np.zeros(blocks[0].shape)
    clock = np.full(blocks[0].shape, -1, dtype=np.int64)
    for block, stamps, weight in zip(blocks, clocks, weights, strict=True):
        valued = np.isfinite(block)
        numerator += np.where(valued, weight * np.where(valued, block, 0.0), 0.0)
        denominator += np.where(valued, weight, 0.0)
        clock = np.where(valued, np.maximum(clock, stamps), clock)
    with np.errstate(invalid="ignore", divide="ignore"):
        raw = numerator / denominator
    raw[~((denominator > 0) & (denominator >= min_coverage * total - 1e-12))] = np.nan
    out = _zscore_rows(raw)
    return out, np.where(np.isfinite(out), clock, -1)


def _batches(months: np.ndarray, width: int, depth: int) -> Iterable[np.ndarray]:
    # ~128 MB of float64 blocks per batch (members x months x securities), at most 24 months.
    size = int(max(1, min(24, 128 * 2**20 // (8 * max(width, 1) * max(depth, 1) * 3))))
    for start in range(0, len(months), size):
        yield months[start:start + size]


def _monthly_ic(block: np.ndarray, labels: np.ndarray, min_names: int) -> tuple[np.ndarray, np.ndarray]:
    batch = block.shape[0]
    group = np.repeat(np.arange(batch), block.shape[1])
    rho, counts = ev.grouped_rank_correlation(group, block.ravel(), labels.ravel(), batch)
    rho = np.where(counts >= min_names, rho, np.nan)
    return rho, counts


# ---------------------------------------------------------------------------
# Definitions, result
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class CompositeDefinition:
    composite_id: str
    level: str
    weighting: str
    anomaly_class: str | None
    #: (member id, weight, fit IC, fit formations): signal ids within a class, class
    #: composite ids across classes.
    members: tuple[tuple[str, float, float | None, int], ...]
    emitted: bool


@dataclass(frozen=True)
class BuildForwardResult:
    ledger_id: str
    basis: str
    manifest: Mapping[str, Any]
    redundancy: tuple[Mapping[str, Any], ...]
    definitions: tuple[CompositeDefinition, ...]
    orientation: Mapping[str, int]
    #: Emitted composite values: ``month_index``, ``security``, ``value``, ``clock_us``.
    composites: Mapping[str, pd.DataFrame]
    sha256: str

    def to_json_bytes(self) -> bytes:
        payload = {"manifest": self.manifest, "redundancy": list(self.redundancy),
                   "definitions": [_definition_json(d) for d in self.definitions], "sha256": self.sha256}
        return (json.dumps(payload, sort_keys=True, indent=2, ensure_ascii=True, allow_nan=False) + "\n").encode()

    @property
    def emitted(self) -> tuple[CompositeDefinition, ...]:
        return tuple(d for d in self.definitions if d.emitted)


def _definition_json(definition: CompositeDefinition) -> dict[str, Any]:
    return _clean({"composite_id": definition.composite_id, "level": definition.level,
                   "weighting": definition.weighting, "anomaly_class": definition.anomaly_class,
                   "emitted": definition.emitted,
                   "members": [{"member_id": m, "weight": w, "fit_ic": ic, "fit_formations": n}
                               for m, w, ic, n in definition.members]})


def _values_sha256(frame: pd.DataFrame) -> str:
    digest = hashlib.sha256()
    for column, kind in (("month_index", "<i8"), ("security", "<i8"), ("value", "<f8"), ("clock_us", "<i8")):
        digest.update(np.ascontiguousarray(frame[column].to_numpy().astype(kind)).tobytes())
    return digest.hexdigest()


def _slug(text: str) -> str:
    slug = "".join(ch if ch in _ID_CHARS else "_" for ch in text.lower()).strip("_") or UNCLASSIFIED
    return slug[:40]


# ---------------------------------------------------------------------------
# Build forward (pure: no store access)
# ---------------------------------------------------------------------------

def _complete_linkage(ids: Sequence[str], rho: Mapping[tuple[str, str], float], threshold: float
                      ) -> list[list[str]]:
    clusters = [[feature] for feature in sorted(ids)]

    def link(a: list[str], b: list[str]) -> float:
        values = [rho.get((min(x, y), max(x, y)), math.nan) for x in a for y in b]
        return math.nan if any(math.isnan(v) for v in values) else min(values)

    while True:
        best: tuple[float, str, str, int, int] | None = None
        for i in range(len(clusters)):
            for j in range(i + 1, len(clusters)):
                value = link(clusters[i], clusters[j])
                if math.isnan(value) or value < threshold:
                    continue
                key = (value, clusters[i][0], clusters[j][0], i, j)
                if best is None or value > best[0] or (value == best[0] and key[1:3] < best[1:3]):
                    best = key
        if best is None:
            break
        _, _, _, i, j = best
        clusters[i] = sorted(clusters[i] + clusters[j])
        del clusters[j]
        clusters.sort(key=lambda c: c[0])
    return sorted(clusters, key=lambda c: (-len(c), c[0]))


def _span(signal: LedgerSignal, series: pd.DataFrame, factors: pd.DataFrame, policy: CompositePolicy
          ) -> dict[str, Any]:
    empty = {"span_status": SPAN_TOO_FEW, "span_alpha": None, "span_alpha_z": None, "span_alpha_p": None,
             "span_r2": None, "span_n_obs": 0, "span_betas": None}
    rows = series[series["feature_id"] == signal.feature_id]
    if not len(rows):
        return empty
    y = pd.Series(signal.orientation * rows["value"].to_numpy(dtype=float),
                  index=pd.to_datetime(rows["formation_date"]).to_numpy()).sort_index()
    frame = factors.copy()
    frame.index = pd.to_datetime(frame.index)
    try:
        result = fr.span_test(y, frame[list(policy.span_factors)].reindex(y.index),
                              horizon_periods=policy.span_horizon)
    except fr.FactorInputError:
        return empty
    if result.n_obs < policy.span_min_periods:
        return {**empty, "span_n_obs": result.n_obs}
    z = result.alpha_z
    status = SPAN_TESTED if z is not None and math.isfinite(z) else SPAN_DEGENERATE
    return {"span_status": status, "span_alpha": result.alpha, "span_alpha_z": z,
            "span_alpha_p": result.alpha_robust_p, "span_r2": result.r2, "span_n_obs": result.n_obs,
            "span_betas": dict(sorted(result.betas.items()))}


def _factor_digest(factors: pd.DataFrame, names: Sequence[str]) -> str:
    frame = factors[list(names)].copy()
    frame.index = pd.to_datetime(frame.index)
    rows = [[day.date().isoformat(), *(_clean(float(v)) for v in values)]
            for day, values in zip(frame.index, frame.to_numpy(dtype=float), strict=True)]
    return _sha(_canonical(rows))


def build_forward(ledger: LedgerSource, inputs: ev.BasisInputs, split: ev.FrozenSplit, series: pd.DataFrame,
                  factors: pd.DataFrame, policy: CompositePolicy, *,
                  factor_source: Mapping[str, Any] | None = None) -> BuildForwardResult:
    """Annotate the ledger's qualified signals and fit and materialize the composites (pure).

    ``inputs`` is the composite basis (constituent values, R3b labels, calendar,
    maturity); ``series`` the R3b selection long-short series (:func:`load_selection_series`);
    ``factors`` the monthly factor returns indexed by formation date (P4
    :func:`~atx_db.research.factor_returns.load_factor_returns`). Deterministic: the same
    inputs give byte-identical :meth:`BuildForwardResult.to_json_bytes`.
    """
    if inputs.basis != policy.basis:
        raise CompositeError(f"the composite basis is {policy.basis}; inputs are {inputs.basis}")
    if ledger.split_sha256 is not None and ledger.split_sha256 != split.sha256:
        raise CompositeError(f"the ledger was graded under split {ledger.split_sha256}, not {split.sha256}")
    missing = sorted(set(policy.span_factors) - {str(c) for c in factors.columns})
    if missing:
        raise CompositeError(f"factor returns lack {missing}")
    qualified = [s for s in ledger.signals if s.status in policy.qualified_statuses]
    ids = [s.feature_id for s in qualified]
    by_id = {s.feature_id: s for s in qualified}
    fit = fit_sample(inputs, split, policy.fit_horizon)
    fit_months = fit["month_index"].to_numpy(dtype=np.int64)
    width = int(inputs.security_count)
    members = {fid: _load_member(inputs, fid, policy.constituent_variant, by_id[fid].orientation) for fid in ids}
    labels = inputs.labels
    chosen = (labels["horizon_months"] == policy.fit_horizon) & (labels["status"] == 0)
    label_month = labels.loc[chosen, "month_index"].to_numpy(dtype=np.int64)
    label_security = labels.loc[chosen, "security"].to_numpy(dtype=np.int64)
    label_value = labels.loc[chosen, "forward_return"].to_numpy(dtype=float)
    order = np.lexsort((label_security, label_month))
    label_month, label_security, label_value = label_month[order], label_security[order], label_value[order]
    fit_by_month = fit.set_index("month_index")

    def ic_frame(member_id: str, months: np.ndarray, rho: np.ndarray) -> list[dict[str, Any]]:
        return [{"member_id": member_id, "month_index": int(m), "formation_date": fit_by_month.at[int(m), "formation_date"],
                 "label_end": fit_by_month.at[int(m), "label_end"], "ic": float(r)}
                for m, r in zip(months.tolist(), rho.tolist(), strict=True) if math.isfinite(r)]

    # Pass A (fit sample): monthly rank correlations between signals, and each signal's IC.
    pair_sum: dict[tuple[str, str], float] = {}
    pair_n: dict[tuple[str, str], int] = {}
    signal_ics: list[dict[str, Any]] = []
    if len(fit_months) and ids:
        for batch in _batches(fit_months, width, len(ids) + 1):
            label_block = _scatter(label_month, label_security, label_value, batch, width)
            blocks = {fid: _scatter(m.month, m.security, m.value, batch, width) for fid, m in members.items()}
            group = np.repeat(np.arange(len(batch)), width)
            for fid in ids:
                rho, _ = _monthly_ic(blocks[fid], label_block, policy.min_names)
                signal_ics.extend(ic_frame(fid, batch, rho))
            for i, a in enumerate(ids):
                for b in ids[i + 1:]:
                    rho, counts = ev.grouped_rank_correlation(group, blocks[a].ravel(), blocks[b].ravel(), len(batch))
                    good = (counts >= policy.min_names) & np.isfinite(rho)
                    pair_sum[(a, b)] = pair_sum.get((a, b), 0.0) + float(rho[good].sum())
                    pair_n[(a, b)] = pair_n.get((a, b), 0) + int(good.sum())
    rho_mean = {pair: (pair_sum[pair] / pair_n[pair] if pair_n[pair] >= policy.min_formations else math.nan)
                for pair in pair_sum}
    clusters = _complete_linkage(ids, rho_mean, policy.rho_threshold)
    representative = {}
    cluster_of = {}
    for number, cluster in enumerate(clusters, start=1):
        head = min(cluster, key=lambda f: (-abs(by_id[f].ic_z or 0.0), f))
        for feature in cluster:
            representative[feature] = head
            cluster_of[feature] = (f"c{number:03d}", len(cluster))
    spans = {fid: _span(by_id[fid], series, factors, policy) for fid in ids}
    redundancy = []
    for fid in ids:
        signal, head = by_id[fid], representative[fid]
        span = spans[fid]
        if head != fid:
            status = f"{DUPLICATE_PREFIX}{head}"
        elif span["span_status"] == SPAN_TESTED and span["span_alpha_z"] < policy.alpha_min_z:
            status = SPANNED
        else:
            status = NOVEL
        pair = (min(fid, head), max(fid, head))
        redundancy.append(_clean({
            "feature_id": fid, "r4_status": signal.status, "status_basis": signal.status_basis,
            "anomaly_class": signal.anomaly_class, "ic_z": signal.ic_z, "redundancy_status": status,
            "cluster_id": cluster_of[fid][0], "cluster_size": cluster_of[fid][1], "representative": head,
            "rho_to_representative": 1.0 if head == fid else rho_mean.get(pair), **span}))

    # Constituents and within-class weights.
    constituents = [fid for fid in ids if not (
        policy.exclude_cross_class_duplicates and representative[fid] != fid
        and by_id[representative[fid]].anomaly_class != by_id[fid].anomaly_class)]
    classes: dict[str, list[str]] = {}
    for fid in constituents:
        classes.setdefault(by_id[fid].anomaly_class, []).append(fid)
    ic_rows = pd.DataFrame(signal_ics, columns=["member_id", "month_index", "formation_date", "label_end", "ic"])
    class_ids = {cls: f"cmp_{_slug(cls)}" for cls in classes}
    if len(set(class_ids.values())) != len(class_ids) or "cmp_classes" in class_ids.values():
        raise CompositeError(f"anomaly classes do not give distinct composite ids: {sorted(classes)}")
    definitions: list[CompositeDefinition] = []
    within: dict[str, dict[str, CompositeDefinition]] = {}
    for weighting in policy.weightings:
        tag = WEIGHT_TAGS[weighting]
        within[weighting] = {}
        for cls in sorted(classes):
            fitted = fit_weights(ic_rows[ic_rows["member_id"].isin(classes[cls])], classes[cls], split,
                                 weighting=weighting, shrinkage=policy.ic_shrinkage)
            definition = CompositeDefinition(
                f"{class_ids[cls]}_{tag}", LEVEL_WITHIN, weighting, cls,
                tuple((m, fitted[m]["weight"], fitted[m]["fit_ic"], fitted[m]["fit_formations"]) for m in classes[cls]),
                len(classes[cls]) >= policy.min_class_constituents)
            within[weighting][cls] = definition
            definitions.append(definition)

    # Pass B (fit sample): IC of every class composite, for the across-class weights.
    class_ics: list[dict[str, Any]] = []
    if len(fit_months) and constituents:
        for batch in _batches(fit_months, width, len(constituents) + 1):
            label_block = _scatter(label_month, label_security, label_value, batch, width)
            z = {fid: _zscore_rows(_scatter(members[fid].month, members[fid].security, members[fid].value, batch,
                                            width)) for fid in constituents}
            zero = np.full((len(batch), width), -1, dtype=np.int64)
            for weighting in policy.weightings:
                for definition in within[weighting].values():
                    block, _ = _combine([z[m] for m, *_ in definition.members], [zero] * len(definition.members),
                                        [w for _, w, *_ in definition.members], policy.min_weight_coverage)
                    rho, _ = _monthly_ic(block, label_block, policy.min_names)
                    class_ics.extend(ic_frame(definition.composite_id, batch, rho))
    class_rows = pd.DataFrame(class_ics, columns=["member_id", "month_index", "formation_date", "label_end", "ic"])
    if len(classes) >= policy.min_across_inputs:
        for weighting in policy.weightings:
            inputs_ids = [within[weighting][cls].composite_id for cls in sorted(classes)]
            fitted = fit_weights(class_rows[class_rows["member_id"].isin(inputs_ids)], inputs_ids, split,
                                 weighting=weighting, shrinkage=policy.ic_shrinkage)
            definitions.append(CompositeDefinition(
                f"cmp_classes_{WEIGHT_TAGS[weighting]}", LEVEL_ACROSS, weighting, None,
                tuple((m, fitted[m]["weight"], fitted[m]["fit_ic"], fitted[m]["fit_formations"]) for m in inputs_ids),
                True))
    orientation = {fid: by_id[fid].orientation for fid in constituents}
    composites = materialize(inputs, tuple(definitions), orientation, policy,
                             {fid: members[fid] for fid in constituents})
    blockers = ["research_only_not_release_eligible"]
    if ledger.post_hoc_policy:
        blockers.append("ledger_post_hoc_policy")
    untested = sum(1 for fid in ids if spans[fid]["span_status"] != SPAN_TESTED)
    if untested:
        blockers.append(f"spanning_untested:{untested}")
    source = dict(factor_source or {"kind": "injected"})
    if source.get("kind") != "p4_run":
        blockers.append("factors_not_a_p4_run")
    if not composites:
        blockers.append("no_composite_emitted")
    manifest = _clean({
        "composite_version": COMPOSITE_VERSION, "code_sha256": code_sha256(),
        "policy": {"version": policy.version, "sha256": policy.sha256},
        "ledger": {"ledger_id": ledger.ledger_id, "ledger_sha256": ledger.ledger_sha256,
                   "evaluation_run_id": ledger.run_id, "r4_policy_version": ledger.r4_policy_version},
        "basis": policy.basis, "split_sha256": split.sha256, "constituent_variant": policy.constituent_variant,
        "fit_sample": {"horizon_months": policy.fit_horizon, "formations": len(fit),
                       "first": fit["formation_date"].min() if len(fit) else None,
                       "last": fit["formation_date"].max() if len(fit) else None,
                       "holdout_start": split.holdout_start},
        "factors": {**source, "factors": list(policy.span_factors),
                    "sha256": _factor_digest(factors, policy.span_factors)},
        "qualified": len(ids), "constituents": constituents,
        "clusters": clusters,
        "rank_correlations": [[a, b, rho_mean[(a, b)], pair_n[(a, b)]] for a, b in sorted(rho_mean)],
        "composites": [{"composite_id": cid, "rows": len(frame), "values_sha256": _values_sha256(frame)}
                       for cid, frame in sorted(composites.items())],
        "blockers": sorted(blockers),
    })
    body = {"manifest": manifest, "redundancy": redundancy, "definitions": [_definition_json(d) for d in definitions]}
    return BuildForwardResult(ledger.ledger_id, policy.basis, manifest, tuple(redundancy), tuple(definitions),
                              orientation, composites, _sha(_canonical(body)))


def materialize(inputs: ev.BasisInputs, definitions: Sequence[CompositeDefinition], orientation: Mapping[str, int],
                policy: CompositePolicy, members: Mapping[str, Any] | None = None) -> dict[str, pd.DataFrame]:
    """Composite values on every formed formation of a basis (fitted definitions applied).

    Returns ``{composite_id: frame(month_index, security, value, clock_us)}`` for the
    emitted definitions. A constituent value whose clock is after its formation cutoff
    raises :class:`CompositeError`.
    """
    loaded = dict(members or {})
    for fid in orientation:
        if fid not in loaded:
            loaded[fid] = _load_member(inputs, fid, policy.constituent_variant, orientation[fid])
    calendar = inputs.calendar[inputs.calendar["status"] == CALENDAR_FORMED]
    months = calendar["month_index"].to_numpy(dtype=np.int64)
    cutoff = pd.to_datetime(calendar["cutoff"]).to_numpy().astype("datetime64[us]").astype(np.int64)
    cutoff_of = dict(zip(months.tolist(), cutoff.tolist(), strict=True))
    width = int(inputs.security_count)
    within = [d for d in definitions if d.level == LEVEL_WITHIN]
    across = [d for d in definitions if d.level == LEVEL_ACROSS]
    out: dict[str, list[pd.DataFrame]] = {d.composite_id: [] for d in definitions if d.emitted}
    fids = sorted(loaded)
    for batch in (_batches(months, width, len(fids) + 1) if fids and len(months) else ()):
        limit = np.array([cutoff_of[int(m)] for m in batch], dtype=np.int64)[:, None]
        z, clocks = {}, {}
        for fid in fids:
            member = loaded[fid]
            clock = _scatter(member.month, member.security, member.clock, batch, width, fill=-1)
            if bool((clock > limit).any()):
                raise CompositeError(f"{fid}: a constituent value is not visible at its formation cutoff")
            z[fid] = _zscore_rows(_scatter(member.month, member.security, member.value, batch, width))
            clocks[fid] = clock
        blocks: dict[str, tuple[np.ndarray, np.ndarray]] = {}
        for definition in within:
            blocks[definition.composite_id] = _combine(
                [z[m] for m, *_ in definition.members], [clocks[m] for m, *_ in definition.members],
                [w for _, w, *_ in definition.members], policy.min_weight_coverage)
        for definition in across:
            blocks[definition.composite_id] = _combine(
                [blocks[m][0] for m, *_ in definition.members], [blocks[m][1] for m, *_ in definition.members],
                [w for _, w, *_ in definition.members], policy.min_weight_coverage)
        for cid in out:
            block, clock = blocks[cid]
            rows, cols = np.nonzero(np.isfinite(block))
            out[cid].append(pd.DataFrame({"month_index": batch[rows], "security": cols.astype(np.int64),
                                          "value": block[rows, cols], "clock_us": clock[rows, cols]}))
    return {cid: (pd.concat(parts, ignore_index=True) if parts else
                  pd.DataFrame({"month_index": np.array([], np.int64), "security": np.array([], np.int64),
                                "value": np.array([], float), "clock_us": np.array([], np.int64)}))
            for cid, parts in out.items()}


# ---------------------------------------------------------------------------
# Composite features: R2b variants, catalog, in-memory R3b evaluation
# ---------------------------------------------------------------------------

def composite_catalog(result: BuildForwardResult) -> tuple[ev.CatalogFeature, ...]:
    """The composite family as R3b catalog rows (class ``composite``, every expected variant)."""
    return tuple(ev.CatalogFeature(d.composite_id, 1, COMPOSITE_CLASS, ev.expected_variants(COMPOSITE_CLASS),
                                   COMPOSITE_CLASS) for d in sorted(result.emitted, key=lambda d: d.composite_id))


def composite_catalog_entries(definitions: Iterable[CompositeDefinition]) -> list[SimpleNamespace]:
    """Catalog-shaped entries for R4 (``qualify_run(catalog=...)``) of a composite run."""
    return [SimpleNamespace(feature_id=d.composite_id, expected_sign=1, anomaly_class=COMPOSITE_CLASS,
                            hypothesis_family=COMPOSITE_CLASS, prior_evidence=COMPOSITE_PRIOR_EVIDENCE,
                            admission="eligible", caveat_codes=COMPOSITE_CAVEATS, is_research_eligible=True)
            for d in sorted(definitions, key=lambda d: d.composite_id) if d.emitted]


def standardize_composite(frame: pd.DataFrame, composite_id: str, standardization: rf.StandardizationPolicy
                          ) -> tuple[pd.DataFrame, dict[tuple[dt.date, str], dict[str, Any]]]:
    """Every R2b variant of one composite (R2b's own :func:`~atx_db.research.features.standardize_frame`).

    ``frame``: ``formation_date``, ``security_id``, ``raw_value`` (the composite z),
    ``industry_group``, ``log_size``; all rows in domain, orientation +1.
    """
    work = frame.assign(domain_status=rf.IN_DOMAIN)
    standardized, stats = rf.standardize_frame(work, feature_id=composite_id, orientation=1, log_base=False,
                                               variants=rf.VARIANTS, policy=standardization)
    return standardized, {(pd.Timestamp(day).date(), variant): item for (day, variant), item in stats.items()}


def composite_feature_data(result: BuildForwardResult, inputs: ev.BasisInputs,
                           standardization: rf.StandardizationPolicy, *,
                           covariates: pd.DataFrame | None = None,
                           values: Mapping[str, pd.DataFrame] | None = None) -> dict[str, ev.FeatureData]:
    """R3b :class:`~atx_db.research.evaluation.FeatureData` of each emitted composite (all variants).

    ``covariates`` (``month_index``, ``security``, ``log_size``, ``industry_group``) feed the
    neutral variants; by default the size covariate is the log of the basis context's
    verified ``market_cap`` and there are no industry groups (``industry_neutral`` is then
    never formed). The store path (:func:`write_composite_version`) uses the constituent
    version's own R2b covariates.
    """
    calendar = inputs.calendar.set_index("month_index")
    if covariates is None:
        context = inputs.context
        cap = context["market_cap"].to_numpy(dtype=float) if len(context) and "market_cap" in context else \
            np.array([])
        ok = np.isfinite(cap) & (cap > 0)
        covariates = pd.DataFrame({"month_index": context["month_index"].to_numpy()[ok] if len(cap) else [],
                                   "security": context["security"].to_numpy()[ok] if len(cap) else [],
                                   "log_size": np.log(cap[ok]), "industry_group": None})
    keyed = covariates.set_index(["month_index", "security"])[["log_size", "industry_group"]]
    out = {}
    for cid, frame in sorted((values or result.composites).items()):
        months = frame["month_index"].to_numpy(dtype=np.int64)
        securities = frame["security"].to_numpy(dtype=np.int64)
        joined = keyed.reindex(pd.MultiIndex.from_arrays([months, securities]))
        work = pd.DataFrame({
            "formation_date": calendar["formation_date"].reindex(months).to_numpy(),
            "security_id": securities, "raw_value": frame["value"].to_numpy(dtype=float),
            "industry_group": joined["industry_group"].to_numpy(dtype=object),
            "log_size": pd.to_numeric(joined["log_size"], errors="coerce").to_numpy(dtype=float)})
        standardized, stats = standardize_composite(work, cid, standardization)
        month_of = {_day(calendar.at[int(m), "formation_date"]): int(m) for m in np.unique(months)}
        standardized_month = np.array([month_of[_day(d)] for d in standardized["formation_date"]], dtype=np.int64)
        parts = []
        for variant in rf.VARIANTS:
            column = standardized[variant].to_numpy(dtype=float)
            keep = np.isfinite(column)
            parts.append(pd.DataFrame({"month_index": standardized_month[keep],
                                       "security": standardized["security_id"].to_numpy(dtype=np.int64)[keep],
                                       "variant": variant, "value": column[keep]}))
        dates = pd.DataFrame([{"month_index": month_of[day], "variant": variant, "date_status": item["date_status"],
                               "coverage_fraction": np.nan} for (day, variant), item in sorted(stats.items())])
        out[cid] = ev.FeatureData(cid, 1, pd.concat(parts, ignore_index=True), dates, COMPOSITE_CLASS, COMPOSITE_CLASS)
    return out


def evaluate_composites(result: BuildForwardResult, inputs: ev.BasisInputs, spec: ev.EvaluationSpec, *,
                        standardization: rf.StandardizationPolicy,
                        covariates: pd.DataFrame | None = None) -> ev.EvaluationTables:
    """The composite family's one evaluation through the R3b engine, in memory (holdout slices included).

    ``inputs`` must be a fresh basis of the fit basis (the engine may release its frames).
    """
    data = composite_feature_data(result, inputs, standardization, covariates=covariates)
    variants = {cid: tuple(sorted(set(item.values["variant"].astype(str)), key=rf.VARIANTS.index))
                for cid, item in data.items()}
    basis = replace(inputs, variants_by_feature=variants, load_feature=data.__getitem__)
    return ev.evaluate_bases([basis, ev.empty_basis("strict")], replace(spec, features=None, variants=None),
                             catalog=composite_catalog(result))


# ---------------------------------------------------------------------------
# Research store: build registry (one holdout evaluation per R4 ledger)
# ---------------------------------------------------------------------------

REDUNDANCY_COLUMNS: tuple[tuple[str, str], ...] = (
    ("feature_id", "VARCHAR"), ("r4_status", "VARCHAR"), ("status_basis", "VARCHAR"), ("anomaly_class", "VARCHAR"),
    ("ic_z", "DOUBLE"), ("redundancy_status", "VARCHAR"), ("cluster_id", "VARCHAR"), ("cluster_size", "INTEGER"),
    ("representative", "VARCHAR"), ("rho_to_representative", "DOUBLE"), ("span_status", "VARCHAR"),
    ("span_alpha", "DOUBLE"), ("span_alpha_z", "DOUBLE"), ("span_alpha_p", "DOUBLE"), ("span_r2", "DOUBLE"),
    ("span_n_obs", "INTEGER"),
)
MEMBER_COLUMNS: tuple[tuple[str, str], ...] = (
    ("composite_id", "VARCHAR"), ("level", "VARCHAR"), ("weighting", "VARCHAR"), ("anomaly_class", "VARCHAR"),
    ("emitted", "BOOLEAN"), ("member_id", "VARCHAR"), ("weight", "DOUBLE"), ("fit_ic", "DOUBLE"),
    ("fit_formations", "INTEGER"),
)


def ensure_composite_schema(con: duckdb.DuckDBPyConnection) -> None:
    """Create the ``research_composite_*`` tables (kept here until the store owner registers them)."""
    con.execute("""
        CREATE TABLE IF NOT EXISTS research_composite_schema (
            version INTEGER PRIMARY KEY, name VARCHAR NOT NULL, applied_at TIMESTAMP NOT NULL)""")
    con.execute("""
        CREATE TABLE IF NOT EXISTS research_composite_builds (
            ledger_id VARCHAR PRIMARY KEY, policy_version VARCHAR NOT NULL, policy_sha256 VARCHAR NOT NULL,
            ledger_sha256 VARCHAR NOT NULL, evaluation_run_id VARCHAR NOT NULL, basis VARCHAR NOT NULL,
            build_sha256 VARCHAR NOT NULL, manifest_json VARCHAR NOT NULL, composite_versions_json VARCHAR,
            holdout_run_id VARCHAR, created_at TIMESTAMP NOT NULL, evaluated_at TIMESTAMP)""")
    con.execute("CREATE TABLE IF NOT EXISTS research_composite_redundancy (ledger_id VARCHAR NOT NULL, "
                + ", ".join(f"{n} {k}" for n, k in REDUNDANCY_COLUMNS) + ", PRIMARY KEY (ledger_id, feature_id))")
    con.execute("CREATE TABLE IF NOT EXISTS research_composite_members (ledger_id VARCHAR NOT NULL, "
                + ", ".join(f"{n} {k}" for n, k in MEMBER_COLUMNS)
                + ", PRIMARY KEY (ledger_id, composite_id, member_id))")
    if not con.execute("SELECT count(*) FROM research_composite_schema WHERE version = ?",
                       [COMPOSITE_SCHEMA_VERSION]).fetchone()[0]:
        con.execute("INSERT INTO research_composite_schema VALUES (?, ?, ?)",
                    [COMPOSITE_SCHEMA_VERSION, "composite_builds_redundancy_members", _now()])


def _now() -> dt.datetime:
    return dt.datetime.now(dt.UTC).replace(tzinfo=None)


def _insert_rows(con: duckdb.DuckDBPyConnection, table: str, ledger_id: str, rows: Sequence[Mapping[str, Any]],
                 columns: Sequence[tuple[str, str]]) -> None:
    if not rows:
        return
    data: dict[str, Any] = {"ledger_id": [ledger_id] * len(rows)}
    for name, kind in columns:
        values = [row.get(name) for row in rows]
        data[name] = pd.array([None if v is None else float(v) for v in values], dtype="Float64") \
            if kind == "DOUBLE" else pd.array([None if v is None else int(v) for v in values], dtype="Int64") \
            if kind == "INTEGER" else pd.array(values, dtype="boolean") if kind == "BOOLEAN" \
            else pd.array(values, dtype=object)
    con.register("_cmp_rows", pd.DataFrame(data))
    try:
        names = ", ".join(["ledger_id", *(n for n, _ in columns)])
        con.execute(f"INSERT INTO {table} ({names}) SELECT {names} FROM _cmp_rows")
    finally:
        con.unregister("_cmp_rows")


def persist_build(con: duckdb.DuckDBPyConnection, result: BuildForwardResult) -> str:
    """Store the build once per R4 ledger: ``'stored'``, or ``'exists'`` for an identical build.

    One holdout evaluation per ledger: a different build (another composite policy, other
    inputs) for a ledger that already has one is refused.
    """
    ensure_composite_schema(con)
    row = con.execute("SELECT build_sha256, policy_version FROM research_composite_builds WHERE ledger_id = ?",
                      [result.ledger_id]).fetchone()
    if row is not None:
        if row[0] != result.sha256:
            raise CompositeError(f"R4 ledger {result.ledger_id} already has composite build {row[0]} (policy {row[1]}); "
                                 "its holdout is spent: one composite build and one holdout evaluation per ledger")
        return "exists"
    manifest = result.manifest
    members = [{"composite_id": d.composite_id, "level": d.level, "weighting": d.weighting,
                "anomaly_class": d.anomaly_class, "emitted": d.emitted, "member_id": m, "weight": w, "fit_ic": ic,
                "fit_formations": n} for d in result.definitions for m, w, ic, n in d.members]
    con.execute("BEGIN TRANSACTION")
    try:
        con.execute("""
            INSERT INTO research_composite_builds (ledger_id, policy_version, policy_sha256, ledger_sha256,
                evaluation_run_id, basis, build_sha256, manifest_json, created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, [result.ledger_id, manifest["policy"]["version"], manifest["policy"]["sha256"],
              manifest["ledger"]["ledger_sha256"], manifest["ledger"]["evaluation_run_id"], result.basis,
              result.sha256, _canonical(dict(manifest)), _now()])
        _insert_rows(con, "research_composite_redundancy", result.ledger_id, result.redundancy, REDUNDANCY_COLUMNS)
        _insert_rows(con, "research_composite_members", result.ledger_id, members, MEMBER_COLUMNS)
        con.execute("COMMIT")
    except Exception:
        con.execute("ROLLBACK")
        raise
    return "stored"


def record_holdout_evaluation(con: duckdb.DuckDBPyConnection, ledger_id: str, versions: Sequence[str],
                              run_id: str) -> None:
    """Record the one R3b run that evaluated the build's composites (a second one is refused)."""
    row = con.execute("SELECT holdout_run_id FROM research_composite_builds WHERE ledger_id = ?",
                      [ledger_id]).fetchone()
    if row is None:
        raise CompositeError(f"no composite build for ledger {ledger_id}")
    if row[0] is not None and row[0] != run_id:
        raise CompositeError(f"ledger {ledger_id}: the composites were already evaluated once (run {row[0]})")
    con.execute("UPDATE research_composite_builds SET composite_versions_json = ?, holdout_run_id = ?, "
                "evaluated_at = coalesce(evaluated_at, ?) WHERE ledger_id = ?",
                [_canonical(list(versions)), run_id, _now(), ledger_id])


# ---------------------------------------------------------------------------
# Composite feature versions (R2b schema v3; read by R3b and R4 like any version)
# ---------------------------------------------------------------------------

def _constituent_version(con: duckdb.DuckDBPyConnection, version: str) -> dict[str, Any]:
    row = con.execute("""
        SELECT status, basis, panel_run_id, panel_sha256, classification_basis, catalog_sha256, spec_json,
               values_sha256, blockers_json
        FROM research_feature_versions WHERE feature_version = ?
    """, [version]).fetchone()
    if row is None or row[0] not in rf.SEALED_STATUSES:
        raise CompositeError(f"constituent feature version {version!r} is absent or not sealed")
    spec = json.loads(row[6])
    if spec.get("store_schema") != rf.FEATURE_SCHEMA_VERSION or spec.get("query_version") != rf.QUERY_VERSION:
        raise CompositeError(f"constituent version {version} predates feature store schema "
                             f"{rf.FEATURE_SCHEMA_VERSION} / {rf.QUERY_VERSION}")
    return {"status": row[0], "basis": row[1], "panel_run_id": row[2], "panel_sha256": row[3],
            "classification_basis": row[4], "catalog_sha256": row[5], "spec": spec, "values_sha256": row[7],
            "blockers": json.loads(row[8] or "[]")}


def _standardization(spec: Mapping[str, Any]) -> rf.StandardizationPolicy:
    return rf.StandardizationPolicy(tuple(float(v) for v in spec["winsor_limits"]), int(spec["min_names"]),
                                    int(spec["industry_min_group_size"]), float(spec["neutral_min_coverage"]),
                                    str(spec["taxonomy_code"]))


def write_composite_version(store: ResearchStore, result: BuildForwardResult, constituent_version: str, *,
                            values: Mapping[str, pd.DataFrame] | None, security_ids: Sequence[str],
                            formation_dates: Mapping[int, dt.date], formation_chunk: int = 24) -> str:
    """Write the emitted composites as one R2b feature version beside ``constituent_version``.

    ``values`` are the composites materialized on the constituent version's basis (None for
    an ``untestable_strict`` constituent version: the composite version is untestable too).
    ``security_ids[code]`` and ``formation_dates[month_index]`` map the canonical keys.
    Rows are written on linked primary lines only (``universe_scope='valid_primary_lines'``),
    every variant through R2b's standardization under the constituent version's policy; the
    version passes :func:`atx_db.research.features.validate_feature_version`. The id is a
    content hash; an identical sealed version is reused.
    """
    con = store.con
    with store.transaction():
        rf.ensure_feature_schema(con)
    base = _constituent_version(con, constituent_version)
    untestable = base["status"] == rf.STATUS_UNTESTABLE
    if untestable != (values is None):
        raise CompositeError("composite values are required exactly when the constituent version is sealed")
    standardization = _standardization(base["spec"])
    panel = base["panel_run_id"]
    calendar = con.execute("SELECT formation_date, cutoff, eligible_members FROM research_panel_calendar "
                           "WHERE run_id = ? AND status = ? ORDER BY formation_date", [panel, CALENDAR_FORMED]).fetchall()
    formations = [row[0] for row in calendar]
    cutoff = {row[0]: row[1] for row in calendar}
    eligible = {row[0]: int(row[2] or 0) for row in calendar}
    linked = {day: int(n) for day, n in con.execute(f"""
        SELECT k.formation_date, count(*) FROM research_panel_cohort k
        WHERE k.run_id = ? AND {rf._expected_owner_basis_sql("k")} = ? GROUP BY 1
    """, [panel, rf.OWNER_BASIS_LINKED]).fetchall()}
    emitted = sorted(result.emitted, key=lambda d: d.composite_id)
    features = [[d.composite_id, "planned", None,
                 {"universe_scope": rf.UNIVERSE_SCOPE_LINKED, "level": d.level, "weighting": d.weighting,
                  "anomaly_class": d.anomaly_class, "members": [[m, w] for m, w, _, _ in d.members]},
                 list(rf.VARIANTS)] for d in emitted]
    spec = _clean({
        "query_version": rf.QUERY_VERSION, "store_schema": rf.FEATURE_SCHEMA_VERSION, "kind": COMPOSITE_CLASS,
        "composite_version": COMPOSITE_VERSION, "panel_run_id": panel, "basis": base["basis"],
        "universe_rule": rf.UNIVERSE_RULE, "constituent_feature_version": constituent_version,
        "constituent_variant": result.manifest["constituent_variant"],
        "composite_policy": result.manifest["policy"], "build_sha256": result.sha256,
        "orientation": dict(sorted(result.orientation.items())),
        "variants": list(rf.VARIANTS), "winsor_limits": list(standardization.winsor_limits),
        "min_names": standardization.min_names, "industry_min_group_size": standardization.industry_min_group_size,
        "neutral_min_coverage": standardization.neutral_min_coverage, "taxonomy_code": standardization.taxonomy_code,
        "features": features})
    spec_json = _canonical(spec)
    inputs = {"constituent_version": constituent_version, "constituent_values_sha256": base["values_sha256"],
              "build_sha256": result.sha256,
              "values_sha256": {cid: _values_sha256(frame) for cid, frame in sorted((values or {}).items())}}
    spec_sha, code_sha, inputs_json = _sha(spec_json), code_sha256(), _canonical(inputs)
    version = "cmp_" + _sha(_canonical({"spec_sha256": spec_sha, "code_sha256": code_sha,
                                        "inputs_sha256": _sha(inputs_json)}))[:40]
    existing = con.execute("SELECT status FROM research_feature_versions WHERE feature_version = ?",
                           [version]).fetchone()
    if existing is not None and existing[0] in rf.SEALED_STATUSES:
        return version
    blockers = sorted({*base["blockers"], "composite_feature_version"})
    catalog_common = {"source_kind": COMPOSITE_CLASS, "anomaly_class": COMPOSITE_CLASS,
                      "hypothesis_family": COMPOSITE_CLASS, "expected_sign": 1, "orientation_sign": 1,
                      "preferred_transform": "rank_normal", "preferred_variant": "rank_normal", "domain": "none",
                      "caveat_codes": _canonical(list(COMPOSITE_CAVEATS)), "admission": "eligible", "is_control": False,
                      "variants_json": _canonical(list(rf.VARIANTS))}
    with store.transaction():
        for table in ("research_feature_matrix", "research_feature_dates", "research_feature_owner_basis",
                      "research_feature_context", "research_feature_catalog", "research_feature_versions"):
            con.execute(f"DELETE FROM {table} WHERE feature_version = ?", [version])  # a failed earlier attempt
        con.execute("""
            INSERT INTO research_feature_versions (feature_version, status, basis, panel_run_id, panel_sha256,
                classification_basis, query_version, universe_rule, spec_json, spec_sha256, code_sha256,
                catalog_sha256, inputs_json, inputs_sha256, blockers_json, created_at)
            VALUES (?, 'building', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, [version, base["basis"], panel, base["panel_sha256"], base["classification_basis"], rf.QUERY_VERSION,
              rf.UNIVERSE_RULE, spec_json, spec_sha, code_sha, base["catalog_sha256"], inputs_json,
              _sha(inputs_json), _canonical(blockers), _now()])
        if untestable:
            date_rows = [{"feature_version": version, "formation_date": day, "feature_id": d.composite_id,
                          "variant": variant, "date_status": rf.DATE_EMPTY, "eligible_members": eligible[day],
                          "universe_names": 0, "valid_names": 0, "coverage_fraction": None, "candidate_names": 0,
                          "in_domain_names": 0, "covariate_names": None, "covariate_coverage": None,
                          "reasons_json": _canonical({}), "unlinked_excluded": None, "sample_conditioning": None}
                         for d in emitted for day in formations for variant in rf.VARIANTS]
            rf._insert_frame(con, "research_feature_dates", pd.DataFrame(date_rows, columns=list(rf._DATE_COLUMNS)),
                             rf._DATE_CASTS)
            _insert_catalog(con, version, emitted, catalog_common, rf.FEATURE_UNTESTABLE, "constituent_untestable_strict")
            status = rf.STATUS_UNTESTABLE
        else:
            con.execute("""
                INSERT INTO research_feature_context (feature_version, formation_date, security_id, owner_basis,
                                                      owner_cik, log_size, industry_group)
                SELECT ?, formation_date, security_id, owner_basis, owner_cik, log_size, industry_group
                FROM research_feature_context WHERE feature_version = ? AND owner_basis = ?
                ORDER BY formation_date, security_id
            """, [version, constituent_version, rf.OWNER_BASIS_LINKED])
            for definition in emitted:
                _write_composite(con, version, definition, values[definition.composite_id], security_ids,
                                 formation_dates, formations, cutoff, eligible, linked, standardization,
                                 formation_chunk, catalog_common)
            status = rf.STATUS_SEALED
        values_sha, _, _ = rf._combined_digest(con, version, rf._context_digest(con, version, formations,
                                                                                 rf._DIGEST_CHUNK))
        con.execute("UPDATE research_feature_versions SET status = ?, values_sha256 = ?, diagnostic_json = ?, "
                    "finished_at = ? WHERE feature_version = ?",
                    [status, values_sha, _canonical({"composites": [d.composite_id for d in emitted],
                                                     "formations": len(formations)}), _now(), version])
    return version


def _insert_catalog(con: duckdb.DuckDBPyConnection, version: str, definitions: Sequence[CompositeDefinition],
                    common: Mapping[str, Any], status: str, reason: str | None,
                    extra: Mapping[str, Mapping[str, Any]] | None = None) -> None:
    rows = []
    for d in definitions:
        inputs = {"universe_scope": rf.UNIVERSE_SCOPE_LINKED, "level": d.level, "weighting": d.weighting,
                  "anomaly_class": d.anomaly_class, "members": [[m, w] for m, w, _, _ in d.members]}
        item = {"feature_version": version, "feature_id": d.composite_id, **common,
                "universe_scope": rf.UNIVERSE_SCOPE_LINKED,
                "inputs_json": _canonical(_clean(inputs)), "status": status, "status_reason": reason,
                "value_rows": None, "in_domain_rows": None, "reasons_json": None, "values_sha256": None,
                **(extra or {}).get(d.composite_id, {})}
        rows.append([item[name] for name in rf._CATALOG_COLUMNS])
    con.executemany(f"INSERT INTO research_feature_catalog ({','.join(rf._CATALOG_COLUMNS)}) "
                    f"VALUES ({','.join('?' * len(rf._CATALOG_COLUMNS))})", rows)


def _write_composite(con: duckdb.DuckDBPyConnection, version: str, definition: CompositeDefinition,
                     frame: pd.DataFrame, security_ids: Sequence[str], formation_dates: Mapping[int, dt.date],
                     formations: Sequence[dt.date], cutoff: Mapping[dt.date, dt.datetime],
                     eligible: Mapping[dt.date, int], linked: Mapping[dt.date, int],
                     standardization: rf.StandardizationPolicy, chunk: int, common: Mapping[str, Any]) -> None:
    cid = definition.composite_id
    days = np.array([formation_dates[int(m)] for m in frame["month_index"].to_numpy()], dtype=object)
    stats: dict[tuple[dt.date, str], dict[str, Any]] = {}
    candidates: dict[dt.date, int] = {}
    total = 0
    for start in range(0, len(formations), chunk):
        part = set(formations[start:start + chunk])
        chosen = np.array([day in part for day in days], dtype=bool)
        if not chosen.any():
            continue
        clock = frame["clock_us"].to_numpy()[chosen]
        rows = pd.DataFrame({
            "formation_date": days[chosen],
            "security_id": [security_ids[int(code)] for code in frame["security"].to_numpy()[chosen]],
            "raw_value": frame["value"].to_numpy(dtype=float)[chosen],
            "available_at": [cutoff[day] if c < 0 else pd.Timestamp(int(c), unit="us").to_pydatetime()
                             for day, c in zip(days[chosen], clock, strict=True)]})
        con.register("_cmp_chunk", rows)
        try:
            staged = con.execute("""
                SELECT r.formation_date, r.security_id, r.raw_value, r.available_at, x.industry_group, x.log_size
                FROM _cmp_chunk r
                LEFT JOIN research_feature_context x
                  ON x.feature_version = ? AND x.formation_date = r.formation_date AND x.security_id = r.security_id
                ORDER BY r.formation_date, r.security_id
            """, [version]).df()
        finally:
            con.unregister("_cmp_chunk")
        standardized, part_stats = standardize_composite(staged, cid, standardization)
        stats.update(part_stats)
        for day, n in standardized.groupby(pd.to_datetime(standardized["formation_date"]).dt.date).size().items():
            candidates[day] = int(n)
        total += len(standardized)
        matrix = pd.DataFrame({
            "feature_version": version, "formation_date": standardized["formation_date"],
            "security_id": standardized["security_id"], "feature_id": cid, "owner_basis": rf.OWNER_BASIS_LINKED,
            "expected_sign": 1, "available_at": standardized["available_at"],
            "raw_value": standardized["raw_value"].to_numpy(dtype=float), "domain_status": rf.IN_DOMAIN,
            "age_days": None, **{v: standardized[v].to_numpy(dtype=float) for v in rf.VARIANTS}})
        rf._insert_frame(con, "research_feature_matrix", matrix, rf._MATRIX_CASTS)
    date_rows = []
    totals: dict[str, int] = {}
    for day in formations:
        names, members, valued = linked.get(day, 0), eligible.get(day, 0), candidates.get(day, 0)
        reasons = {key: n for key, n in (("in_domain", valued), ("composite_not_valued", names - valued),
                                         ("not_a_ranked_line", members - names)) if n}
        for key, n in reasons.items():
            totals[key] = totals.get(key, 0) + n
        for variant in rf.VARIANTS:
            item = stats.get((day, variant)) or {"date_status": rf.DATE_THIN, "valid_names": 0, "in_domain_names": 0,
                                                 "covariate_names": None, "covariate_coverage": None}
            date_rows.append({
                "feature_version": version, "formation_date": day, "feature_id": cid, "variant": variant,
                "date_status": item["date_status"], "eligible_members": members, "universe_names": names,
                "valid_names": int(item["valid_names"]),
                "coverage_fraction": (item["valid_names"] / names) if names else None, "candidate_names": valued,
                "in_domain_names": int(item["in_domain_names"]), "covariate_names": item["covariate_names"],
                "covariate_coverage": item["covariate_coverage"], "reasons_json": _canonical(dict(sorted(reasons.items()))),
                "unlinked_excluded": None, "sample_conditioning": None})
    rf._insert_frame(con, "research_feature_dates", pd.DataFrame(date_rows, columns=list(rf._DATE_COLUMNS)),
                     rf._DATE_CASTS)
    digest = rf._feature_digest(con, version, cid, formations, rf._DIGEST_CHUNK)
    _insert_catalog(con, version, [definition], common, rf.FEATURE_BUILT, None,
                    {cid: {"value_rows": total, "in_domain_rows": total,
                           "reasons_json": _canonical(dict(sorted(totals.items()))), "values_sha256": digest}})


# ---------------------------------------------------------------------------
# Store orchestration: annotate, build, write, evaluate once
# ---------------------------------------------------------------------------

def run_build_forward(store: ResearchStore, ledger_id: str, *, factor_run_id: str,
                      policy: CompositePolicy | None = None) -> dict[str, Any]:
    """Build forward from a stored R4 ledger and evaluate the composites once (R3b run).

    Reads the ledger, its R3b run (split, feature versions, label cutoff), the composite
    basis through R3b's store adapter, the R3b selection series and the P4 monthly factor
    run; stores the build (one per ledger), writes a composite version per basis of the
    run and runs one R3b evaluation of them with the source run's spec (the R4-pinned
    evaluation inputs), so ``research_qualify``/:func:`qualify_composites` can grade them.
    """
    policy = policy or load_composite_policy()
    con = store.con
    ledger = load_ledger(con, ledger_id)
    run = con.execute("SELECT spec_json, status FROM research_eval_runs WHERE run_id = ?", [ledger.run_id]).fetchone()
    if run is None or run[1] != "complete":
        raise CompositeError(f"R3b run {ledger.run_id} of ledger {ledger_id} is absent or not sealed")
    source_spec = ev.spec_from_payload(json.loads(run[0]), ledger.run_id)
    if source_spec.split is None:
        raise CompositeError("the source run has no frozen split")
    bases = {str(b): str(v) for v, b in con.execute(
        "SELECT feature_version, basis FROM research_feature_versions WHERE list_contains(?::VARCHAR[], feature_version)",
        [list(source_spec.feature_versions)]).fetchall()}
    if policy.basis not in bases:
        raise CompositeError(f"the source run has no {policy.basis} feature version")
    def keys(basis_inputs: ev.BasisInputs) -> tuple[dict[int, dt.date], list[str]]:
        # Read right after opening a basis: the next open re-registers R3b's key tables.
        calendar = basis_inputs.calendar
        formed = {int(m): _day(d) for m, d, s in zip(calendar["month_index"], calendar["formation_date"],
                                                      calendar["status"], strict=True) if s == CALENDAR_FORMED}
        return formed, [str(row[0]) for row in con.execute("SELECT security_id FROM _ev_securities ORDER BY code")
                        .fetchall()]

    inputs = ev.open_basis_inputs(store, bases[policy.basis], source_spec)
    main_keys = keys(inputs)
    qualified = [s.feature_id for s in ledger.signals if s.status in policy.qualified_statuses]
    series = load_selection_series(con, ledger.run_id, policy, qualified)
    factors = fr.load_factor_returns(store, factor_run_id, factors=policy.span_factors)
    digest = con.execute("SELECT results_sha256 FROM research_factor_runs WHERE run_id = ?", [factor_run_id]).fetchone()
    result = build_forward(ledger, inputs, source_spec.split, series, factors, policy,
                           factor_source={"kind": "p4_run", "run_id": factor_run_id,
                                          "results_sha256": None if digest is None else digest[0]})
    outcome = persist_build(con, result)
    stored = con.execute("SELECT holdout_run_id, composite_versions_json FROM research_composite_builds "
                         "WHERE ledger_id = ?", [ledger_id]).fetchone()
    if stored[0] is not None:
        return {"ledger_id": ledger_id, "build": outcome, "build_sha256": result.sha256,
                "composite_versions": json.loads(stored[1]), "holdout_run_id": stored[0], "evaluated": False}
    versions = []
    for basis, version in sorted(bases.items()):
        values: Mapping[str, pd.DataFrame] | None
        if basis == policy.basis:
            values, (formation_dates, security_ids) = result.composites, main_keys
        else:
            status = con.execute("SELECT status FROM research_feature_versions WHERE feature_version = ?",
                                 [version]).fetchone()[0]
            values, formation_dates, security_ids = None, {}, []
            if status != rf.STATUS_UNTESTABLE:
                other = ev.open_basis_inputs(store, version, source_spec)
                formation_dates, security_ids = keys(other)
                values = materialize(other, result.definitions, result.orientation, policy)
        versions.append(write_composite_version(store, result, version, values=values, security_ids=security_ids,
                                                formation_dates=formation_dates))
    run_id = f"p5_{result.sha256[:16]}"
    spec = replace(source_spec, run_id=run_id, feature_versions=tuple(versions), features=None, variants=None)
    evaluation = ev.run_evaluation(store, spec)
    with store.transaction():
        record_holdout_evaluation(con, ledger_id, versions, run_id)
    return {"ledger_id": ledger_id, "build": outcome, "build_sha256": result.sha256, "composite_versions": versions,
            "holdout_run_id": run_id, "evaluated": True, "family_complete": evaluation.family_complete,
            "blockers": list(evaluation.blockers)}


def qualify_composites(con: duckdb.DuckDBPyConnection, ledger_id: str, r4_policy: rq.QualificationPolicy
                       ) -> rq.QualificationLedger:
    """Grade the composites' one R3b run under the R4 policy (same gates as any signal)."""
    row = con.execute("SELECT holdout_run_id FROM research_composite_builds WHERE ledger_id = ?",
                      [ledger_id]).fetchone()
    if row is None or row[0] is None:
        raise CompositeError(f"ledger {ledger_id} has no evaluated composite build")
    definitions = [CompositeDefinition(cid, level, weighting, cls, (), bool(emitted))
                   for cid, level, weighting, cls, emitted in con.execute(
                       "SELECT DISTINCT composite_id, level, weighting, anomaly_class, emitted "
                       "FROM research_composite_members WHERE ledger_id = ? ORDER BY composite_id",
                       [ledger_id]).fetchall()]
    return rq.qualify_run(con, row[0], r4_policy, catalog=composite_catalog_entries(definitions))


# ---------------------------------------------------------------------------
# Generated documentation
# ---------------------------------------------------------------------------

def _fmt(value: Any, digits: int = 3) -> str:
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.{digits}f}" if math.isfinite(value) else "-"
    return str(value)


def render_composites_markdown(policy: CompositePolicy, result: BuildForwardResult | None = None) -> str:
    """``docs/research/COMPOSITES.md``: the frozen policy, and a build when given."""
    content = policy.content
    lines = [
        "# Build-forward composites (P5)",
        "",
        "Generated by `scripts/research_composite.py render-doc` from the frozen composite policy"
        + (" and a stored build." if result is not None else "; no real-data build exists yet (heavy run: RR5)."),
        "Research only: nothing here is release eligible.",
        "",
        "## Frozen policy",
        "",
        f"- Version `{policy.version}`, content sha256 `{policy.sha256}`.",
        f"- Authority: {content['authority']}.",
        f"- Signals: R4 statuses {', '.join(f'`{s}`' for s in policy.qualified_statuses)}; composite basis "
        f"`{policy.basis}`; constituent variant `{policy.constituent_variant}` (sign-oriented).",
        f"- Fit sample: train + validation formations of the frozen split whose {policy.fit_horizon}-month label "
        "window ends before the holdout; any holdout row refuses the fit.",
        "",
        "## Redundancy annotation (never alters an R4 status)",
        "",
        f"- Rank correlation: mean monthly Spearman of oriented values on names valued on both (at least "
        f"{policy.min_names} per formation, at least {policy.min_formations} formations).",
        f"- Clusters: complete linkage at the declared threshold rho >= {policy.rho_threshold}; representative = "
        "largest |ic_z| in the ledger.",
        f"- Spanning: R3b `{policy.span_series}` at {policy.span_horizon} month on the selection sample regressed on "
        f"P4 monthly {', '.join(f'`{f}`' for f in policy.span_factors)}; EWC fixed-b robust alpha z below "
        f"{policy.alpha_min_z} (at least {policy.span_min_periods} periods) is `spanned_by_known_factors`.",
        "- `redundancy_status`: `duplicate_of:<representative>` > `spanned_by_known_factors` > `novel`.",
        "",
        "## Composites",
        "",
        "- Per formation each constituent is a cross-sectional z; a class composite is the weighted mean over names "
        f"with at least {policy.min_weight_coverage:.0%} of the class weight valued, re-standardized; the "
        "across-class composite combines class composites the same way.",
        f"- Weightings: {', '.join(f'`{w}`' for w in policy.weightings)}; `ic_shrunk` = (1 - s) x positive-IC share + "
        f"s / k with s = {policy.ic_shrinkage} (IC: mean monthly rank IC at {policy.fit_horizon} months on the fit "
        "sample).",
        f"- Emitted: within-class composites of classes with >= {policy.min_class_constituents} constituents "
        f"(`cmp_<class>_eq` / `_icw`), across classes when >= {policy.min_across_inputs} classes "
        "(`cmp_classes_eq` / `_icw`)"
        + ("; a duplicate of a signal in another class is left out." if policy.exclude_cross_class_duplicates
           else "."),
        "- Storage: an R2b-shaped feature version (store schema 3, class `composite`, constituents and weights in "
        "the catalog row) evaluated once by the R3b harness; one composite build and one holdout evaluation per "
        "R4 ledger.",
    ]
    if result is not None:
        manifest = result.manifest
        lines += ["", "## Build", "",
                  f"- Ledger `{result.ledger_id}` (sha256 `{manifest['ledger']['ledger_sha256']}`), build sha256 "
                  f"`{result.sha256}`, fit formations {manifest['fit_sample']['formations']} "
                  f"({manifest['fit_sample']['first']} .. {manifest['fit_sample']['last']}).",
                  f"- Blockers: {', '.join(f'`{b}`' for b in manifest['blockers']) or 'none'}.", "",
                  "| signal | R4 status | class | redundancy | cluster | rho to rep | span alpha z | span R2 |",
                  "|---|---|---|---|---|---|---|---|"]
        for row in result.redundancy:
            lines.append(f"| `{row['feature_id']}` | `{row['r4_status']}` | {row['anomaly_class']} | "
                         f"`{row['redundancy_status']}` | {row['cluster_id']} ({row['cluster_size']}) | "
                         f"{_fmt(row['rho_to_representative'])} | {_fmt(row['span_alpha_z'], 2)} | "
                         f"{_fmt(row['span_r2'])} |")
        lines += ["", "| composite | level | weighting | members (weight, fit IC) |", "|---|---|---|---|"]
        for definition in result.emitted:
            members = ", ".join(f"`{m}` {w:.3f} ({_fmt(ic)})" for m, w, ic, _ in definition.members)
            lines.append(f"| `{definition.composite_id}` | {definition.level} | {definition.weighting} | {members} |")
    return "\n".join(lines) + "\n"


__all__ = [
    "COMPOSITE_CLASS",
    "COMPOSITE_VERSION",
    "DUPLICATE_PREFIX",
    "FROZEN_POLICY_SHA256",
    "NOVEL",
    "POLICY_PATH",
    "SPANNED",
    "BuildForwardResult",
    "CompositeDefinition",
    "CompositeError",
    "CompositeLeakError",
    "CompositePolicy",
    "LedgerSignal",
    "LedgerSource",
    "build_forward",
    "code_sha256",
    "composite_catalog",
    "composite_catalog_entries",
    "composite_feature_data",
    "ensure_composite_schema",
    "evaluate_composites",
    "fit_sample",
    "fit_weights",
    "ledger_source",
    "load_composite_policy",
    "load_ledger",
    "load_selection_series",
    "materialize",
    "persist_build",
    "policy_sha256",
    "qualify_composites",
    "record_holdout_evaluation",
    "render_composites_markdown",
    "run_build_forward",
    "standardize_composite",
    "write_composite_version",
]
