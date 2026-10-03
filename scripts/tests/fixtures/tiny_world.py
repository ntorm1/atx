"""tiny_world: a deterministic synthetic research world for the end-to-end cycle test (platform v8, task E-3).

  build(root, seed=7, *, bin_dir=None, python=None, build_type="Debug") -> dict

writes, under ROOT (a new or empty directory; nothing in it is ever overwritten):

  role/                       atx.recent-research-role/v1 (the layout prepare_recent_research.create_role publishes
                              and engine::data::read_strategy_role admits): 656 weekday sessions ending 2022-12-30 x
                              64 names, score_begin 384 (runner admission: score_begin >= 383, lookback <= 321)
  fields/                     atx.research-role-fields/v1 (the prepare_research_fields manifest layout the IC runner's
                              bind_fields and the NAV's load_fields read): shares_out, si_shares, tiny_signal; with
                              the producers' seal block {exclusive_end: the research window's seal date, rule}
                              (P9 ruling P13: the seal readers refuse a present-and-different exclusive_end)
  tiny_world_v1.json          atx.dsl-ic-library/v1, 4 members: 2 planted signals, 1 noise member, 1 copy
  tiny_world_v1.recipe.json   the recipe the fitter reads the lineage (theme, tier, prior sign) from
  registry.json               atx.alpha-registry/v1 (the task A-1 schema) of the 4 members and the 3 fields
  tiny.json                   scripts/specs/tiny.json with the scripts made absolute (this checkout), the executables
                              under BIN_DIR, the spec's python replaced by PYTHON (default: this interpreter) and the
                              input pins verified against the template's locked pins; BUILD_TYPE "Release" drops the
                              template's Debug DLL directory (vcpkg .../debug/bin) from env_path_prepend, so Release
                              executables load the Release runtime DLLs (research_cycle.py BUILDS["equity-rel"])

and returns {"role_manifest_sha256", "fields_manifest_sha256", "library_sha256", "recipe_sha256", "registry_sha256",
"spec": path of ROOT/tiny.json, "seed"}.

The world (every quantity float64, date-major):
  per name i     beta_i = 0.6 + 0.8 u, start price 40 + 160 u, shares 2e7 + 9.8e8 u, volume scale 5e5 + 4.5e6 u
  states         zA, zB, n: independent AR(1) processes, phi = .995, unit stationary variance
  market         m(t) = .008 e(t)
  returns        r_i(t) = beta_i m(t) + .02 (.05 / sqrt(21) (zA_i(t-1) + zB_i(t-1)) + e_i(t)), t >= 1: the planted
                 term is in noise-SD units, so each planted state's rank IC at horizon 21 is .05 to first order
                 (x .95 for the decay of an AR(.995) state over the 21 sessions)
  close          close(0) = start price, close(t) = close(t-1) (1 + r(t)); raw_close = close (no corporate action)
  volume         volume scale x (.5 + u)
  fields         shares_out = shares (1 + .05 n); si_shares = shares_out x .04 (1 - .15 zA); tiny_signal = zB
  mask           every cell present; member from session 63 on (the 63-session membership warm-up): the declared
                 membership rule (prior-63 ADV above $5M, prior raw price above $5, top 64) selects every name, which
                 build() asserts
Members of the library (prior sign +1 embedded in the DSL; redundancy order is (tier, roster order)):
  planted_a  short_interest     B+  rank(decay_linear((-1 * (si_shares / shares_out)), 21))   (the v7.1 si_ratio form)
  planted_b  earnings_momentum  B   rank(tiny_signal)
  noise_c    value              B-  rank(shares_out)                                           (independent of returns)
  copy_b     earnings_momentum  C+  rank((2 * tiny_signal))            (the ranks of planted_b: rejected as redundant)

Byte determinism across machines: uniforms come from numpy.random.Generator(PCG64(seed)).random, normals are
Irwin-Hall sums of 12 uniforms added in a fixed order, and everything else uses only +, -, *, / and a correctly
rounded sqrt, so no transcendental library call can move a bit. JSON is written as bytes with LF line ends.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import sys

import numpy as np

DEFAULT_SEED = 7
DATES, NAMES, SCORE_BEGIN = 656, 64, 384
MEMBER_WARMUP = 63                     # read_strategy_role: no member before session 63
LAST_SESSION = dt.date(2022, 12, 30)   # a synthetic calendar (weekdays), inside every research window since v1
IC, HORIZON, NOISE_SD = 0.05, 21, 0.02
PHI, MARKET_SD = 0.995, 0.008
DAY_NS = 86_400_000_000_000
FIRST_ID = 1001
ROLE_DIR, FIELDS_DIR = "role", "fields"
LIBRARY, RECIPE, REGISTRY, SPEC = "tiny_world_v1.json", "tiny_world_v1.recipe.json", "registry.json", "tiny.json"
LIBRARY_ID = "tiny_world_v1"
FIELD_NAMES = ("shares_out", "si_shares", "tiny_signal")
REPO = Path(__file__).resolve().parents[3]
TEMPLATE = REPO / "scripts" / "specs" / SPEC
EXES = {"ic": "atx-equity-strategy-ic.exe", "nav": "atx-equity-strategy-targets.exe"}
SCRIPT_SECTIONS = ("runner", "fit", "card", "monitor", "summ", "static_check")
GENERATOR = "scripts/tests/fixtures/tiny_world.py"
BUILD_TYPES = ("Debug", "Release")
DEBUG_DLL_SUFFIX = "/debug/bin"        # the vcpkg Debug runtime directory in the template's env_path_prepend
WINDOW_TOOL = REPO / "atx-engine" / "tools" / "research_window.py"

THEMES = {
    "short_interest": "tiny_world: short interest relative to shares outstanding (low short interest predicts higher "
                      "returns); planted",
    "earnings_momentum": "tiny_world: a directly observed planted state (and a copy of it)",
    "value": "tiny_world: shares outstanding, independent of every return (the noise member)",
}
# (id, family = theme, tier, dsl, origin, citation, formula, domain): roster order; tier ranks A 1 .. C+ 6 (v7.1 scale)
MEMBERS = (
    ("planted_a", "short_interest", "B+", "rank(decay_linear((-1 * (si_shares / shares_out)), 21))", "prior",
     "tiny_world planted state zA through the short-interest ratio (synthetic; rank IC .05 at h 21 by construction)",
     "rank of the 21-session linear decay of -(si_shares / shares_out)", "every cell finite"),
    ("planted_b", "earnings_momentum", "B", "rank(tiny_signal)", "prior",
     "tiny_world planted state zB (synthetic; rank IC .05 at h 21 by construction)",
     "rank of tiny_signal", "every cell finite"),
    ("noise_c", "value", "B-", "rank(shares_out)", "grid",
     "tiny_world noise member: shares outstanding follow a state independent of every return",
     "rank of shares_out", "every cell finite"),
    ("copy_b", "earnings_momentum", "C+", "rank((2 * tiny_signal))", "mined",
     "tiny_world copy of planted_b: the same ranks under another DSL spelling",
     "rank of 2 x tiny_signal (the ranks of planted_b)", "every cell finite"),
)
TIER_RANK = {"A": 1, "A-": 2, "B+": 3, "B": 4, "B-": 5, "C+": 6}
FIELD_DOCS = {
    "shares_out": ("shares", "shares outstanding: per-name scale x (1 + .05 n), n an AR(.995) state independent of "
                             "returns"),
    "si_shares": ("shares", "shares short: shares_out x .04 (1 - .15 zA), zA the planted state A"),
    "tiny_signal": ("unitless", "the planted state B (AR(.995), unit variance)"),
}


# ------------------------------------------------------------------ bytes
def canonical_json(doc) -> bytes:
    """The producer layout of the role and fields manifests: sorted keys, indent 2, LF, trailing newline."""
    return (json.dumps(doc, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_new(path: Path, data: bytes) -> str:
    """Write bytes to a new file (exclusive create) and return their SHA-256."""
    with Path(path).open("xb") as f:
        f.write(data)
    return sha256_bytes(data)


# ------------------------------------------------------------------ the world
def weekdays_ending(last: dt.date, count: int) -> list[dt.date]:
    out, day = [], last
    while len(out) < count:
        if day.weekday() < 5:
            out.append(day)
        day -= dt.timedelta(days=1)
    return out[::-1]


def uniforms(rng: np.random.Generator, shape) -> np.ndarray:
    return rng.random(shape)


def normals(rng: np.random.Generator, shape) -> np.ndarray:
    """Irwin-Hall(12) - 6: mean 0, variance 1, bounded by 6; twelve uniforms summed in a fixed order."""
    shape = (shape,) if isinstance(shape, int) else tuple(shape)
    u = rng.random((12,) + shape)
    acc = u[0].copy()
    for k in range(1, 12):
        acc += u[k]
    return acc - 6.0


def ar1(e: np.ndarray, phi: float) -> np.ndarray:
    """z(0) = e(0), z(t) = phi z(t-1) + sqrt(1 - phi^2) e(t): unit stationary variance."""
    c = math.sqrt(1.0 - phi * phi)
    z = np.empty_like(e)
    z[0] = e[0]
    for t in range(1, e.shape[0]):
        z[t] = phi * z[t - 1] + c * e[t]
    return z


def simulate(seed: int = DEFAULT_SEED) -> dict:
    """Every payload of the world as float64 / uint8 arrays (dates x names), drawn in a fixed order."""
    rng = np.random.Generator(np.random.PCG64(seed))
    d, n = DATES, NAMES
    beta = 0.6 + 0.8 * uniforms(rng, n)
    start = 40.0 + 160.0 * uniforms(rng, n)
    shares = 2e7 + 9.8e8 * uniforms(rng, n)
    vscale = 5e5 + 4.5e6 * uniforms(rng, n)
    za = ar1(normals(rng, (d, n)), PHI)
    zb = ar1(normals(rng, (d, n)), PHI)
    noise_state = ar1(normals(rng, (d, n)), PHI)
    eps = normals(rng, (d, n))
    market = MARKET_SD * normals(rng, d)
    vol_u = uniforms(rng, (d, n))

    drift = IC / math.sqrt(HORIZON)
    ret = np.zeros((d, n))
    planted = za[:-1] + zb[:-1]
    ret[1:] = beta[None, :] * market[1:, None] + NOISE_SD * (drift * planted + eps[1:])
    close = np.empty((d, n))
    close[0] = start
    for t in range(1, d):
        close[t] = close[t - 1] * (1.0 + ret[t])
    volume = vscale[None, :] * (0.5 + vol_u)
    shares_out = shares[None, :] * (1.0 + 0.05 * noise_state)
    si_shares = shares_out * (0.04 * (1.0 - 0.15 * za))
    member = np.zeros((d, n), dtype=np.uint8)
    member[MEMBER_WARMUP:] = 1
    world = {"sessions": weekdays_ending(LAST_SESSION, d),
             "ids": np.arange(FIRST_ID, FIRST_ID + n, dtype=np.uint64),
             "close": close, "raw_close": close.copy(), "volume": volume,
             "present": np.ones((d, n), dtype=np.uint8), "member": member,
             "shares_out": shares_out, "si_shares": si_shares, "tiny_signal": zb.copy(),
             "za": za, "zb": zb, "noise": noise_state}
    check_world(world)
    return world


def check_world(w: dict) -> None:
    """The contracts the role readers and the declared membership rule need, asserted on the drawn world."""
    close, volume = w["close"], w["volume"]
    if not (np.all(np.isfinite(close)) and float(close.min()) > 5.0):
        raise ValueError("tiny_world: a close is not finite or not above $5 (the membership price floor)")
    if not (np.all(np.isfinite(volume)) and float(volume.min()) > 0.0):
        raise ValueError("tiny_world: a volume is not finite and positive")
    dollars = close * volume
    for t in range(MEMBER_WARMUP, DATES):
        if float(dollars[t - MEMBER_WARMUP:t].mean(axis=0).min()) <= 5e6:
            raise ValueError(f"tiny_world: a name's prior-63 ADV is not above $5M at session {t}")
    for name in ("shares_out", "si_shares"):
        x = w[name]
        if not (np.all(np.isfinite(x)) and float(x.min()) > 0.0):
            raise ValueError(f"tiny_world: {name} is not finite and positive")
    if not (1e5 <= float(w["shares_out"].min()) and float(w["shares_out"].max()) <= 5e10):
        raise ValueError("tiny_world: shares_out outside the NAV borrow model's domain [1e5, 5e10]")


def session_ns(day: dt.date) -> int:
    return (day - dt.date(1970, 1, 1)).days * DAY_NS


def research_seal() -> str:
    """The repository window's seal date (YYYY-MM-DD) from atx-engine/tools/research_window.py, loaded as a private
    module and read through ``current()`` (the JSON, not the module constants a test harness may rebind)."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("tiny_world_research_window", WINDOW_TOOL)
    if spec is None or spec.loader is None:
        raise ImportError(f"tiny_world: cannot load {WINDOW_TOOL}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return str(module.current()["SEAL_DATE"])


def seal_block(sessions: list[dt.date]) -> dict:
    """The fields manifest's seal block (the prepare_research_fields shape); refuses a world that reaches the seal."""
    seal = research_seal()
    if sessions[-1].isoformat() >= seal:
        raise ValueError(f"tiny_world: the last session {sessions[-1]} is at or after the research seal {seal}")
    return {"exclusive_end": seal,
            "rule": f"synthetic tiny_world: every session and every field cell is dated before {seal}; no source row "
                    "exists to drop"}


def generator_identity(seed: int) -> dict:
    return {"generator": GENERATOR, "version": 1, "seed": seed, "dates": DATES, "names": NAMES,
            "score_begin": SCORE_BEGIN, "last_session": LAST_SESSION.isoformat(), "ic": IC, "horizon": HORIZON,
            "noise_sd": NOISE_SD, "phi": PHI, "market_sd": MARKET_SD, "normals": "irwin-hall-12",
            "bit_generator": "PCG64"}


# ------------------------------------------------------------------ role and fields
def write_role(directory: Path, w: dict, seed: int) -> tuple[str, dict]:
    """The atx.recent-research-role/v1 directory; returns (manifest SHA-256, manifest)."""
    directory.mkdir()
    d, n = DATES, NAMES
    sessions = np.array([session_ns(s) for s in w["sessions"]], dtype="<i8")
    payloads = {"sessions.i64": sessions.tobytes(), "ids.u64": w["ids"].astype("<u8").tobytes(),
                "close.f64": w["close"].astype("<f8").tobytes(),
                "raw_close.f64": w["raw_close"].astype("<f8").tobytes(),
                "volume.f64": w["volume"].astype("<f8").tobytes(),
                "present.u8": w["present"].astype("u1").tobytes(), "member.u8": w["member"].astype("u1").tobytes()}
    files = {name: {"bytes": len(data), "sha256": write_new(directory / name, data)}
             for name, data in payloads.items()}
    membership = json.dumps({"rule": "research-prior63-usd-adv-topn-v1", "top_n": n, "min_raw_price_exclusive": 5,
                             "min_adv_exclusive": 5_000_000, "lookback_sessions": 63, "lag_sessions": 1,
                             "ties": "securityID-ascending", "missing": "complete-prior-calendar-window-required",
                             "common_stock_verified": False}, sort_keys=True, separators=(",", ":"))
    identity = generator_identity(seed)
    manifest = {
        "schema": "atx.recent-research-role/v1", "status": "complete", "dates": d, "instruments": n,
        "instrument_namespace": "spiderrock.securityID", "score_begin": SCORE_BEGIN, "score_end": d,
        "score_start_ns": int(sessions[SCORE_BEGIN]), "score_end_ns": int(sessions[-1]) + DAY_NS,
        "warmup_start": w["sessions"][0].isoformat(),
        "source_sha256": sha256_bytes(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()),
        "membership_recipe": membership, "clock_recipe": "modeled-session+22h-mark+23h-decision-v1",
        "close_basis": "f64(raw-f32-close)*f64-cumulReturnFactor", "volume_basis": "raw-share-volume",
        "common_stock_verified": False, "historical_vintage_verified": False,
        "source_identity": "synthetic tiny_world ids 1001..1064",
        "physical_presence": "accepted-current-row-independent-from-prior-membership",
        "declared_output_bytes": d * n * 26 + d * 8 + n * 8,
        "score_member_counts": [int(w["member"][t].sum()) for t in range(SCORE_BEGIN, d)],
        "synthetic": dict(identity, note="close_basis and clock_recipe are the loader's pinned strings; the prices "
                                         "are synthetic (raw_close = close, no corporate action)"),
        "files": files}
    data = canonical_json(manifest)
    return write_new(directory / "manifest.json", data), manifest


def write_fields(directory: Path, w: dict, role_sha: str, role: dict) -> str:
    """The atx.research-role-fields/v1 directory bound to the role; returns the manifest SHA-256."""
    directory.mkdir()
    d, n = DATES, NAMES
    files, rows = {}, []
    for name in FIELD_NAMES:
        data = w[name].astype("<f8").tobytes()
        sha = write_new(directory / f"{name}.f64", data)
        files[f"{name}.f64"] = {"bytes": len(data), "sha256": sha}
        units, definition = FIELD_DOCS[name]
        rows.append({"name": name, "file": f"{name}.f64", "dtype": "<f8", "layout": "date-major", "shape": [d, n],
                     "sha256": sha, "point_in_time": True, "non_pit_aspects": [], "units": units,
                     "definition": definition, "clock": "tiny_world: known at the session-date 22:00 UTC mark",
                     "staleness": "none (a value every session)", "source_columns": [name],
                     "sources": [{"generator": GENERATOR}]})
    receipts = role["files"]
    manifest = {
        "schema": "atx.research-role-fields/v1", "status": "complete",
        "role": {"path": f"{ROLE_DIR}/manifest.json", "manifest_sha256": role_sha,
                 "sessions_sha256": receipts["sessions.i64"]["sha256"], "ids_sha256": receipts["ids.u64"]["sha256"],
                 "member_sha256": receipts["member.u8"]["sha256"], "dates": d, "instruments": n,
                 "first_session": w["sessions"][0].isoformat(), "last_session": w["sessions"][-1].isoformat(),
                 "clock_recipe": role["clock_recipe"]},
        "visibility_mark": "every finite cell of every field is known by the session-date 22:00 UTC mark (the role "
                           "close clock), before the 23:00 UTC decision",
        "seal": seal_block(w["sessions"]),
        "cell_rule": "every cell finite (synthetic)", "non_point_in_time_fields": [],
        "common_stock_verified": False, "historical_vintage_verified": False,
        "instrument_namespace": "spiderrock.securityID", "fields": rows, "files": files}
    return write_new(directory / "manifest.json", canonical_json(manifest))


# ------------------------------------------------------------------ library, recipe, registry
def library_doc() -> dict:
    base = [{"name": "close", "basis": "role adjusted close"}, {"name": "raw_close", "basis": "role raw close"},
            {"name": "volume", "basis": "role raw share volume"}]
    extra = [{"name": name, "basis": FIELD_DOCS[name][1]} for name in FIELD_NAMES]
    families = [{"id": theme, "description": THEMES[theme]} for theme in ("short_interest", "earnings_momentum",
                                                                          "value")]
    candidates = [{"id": cid, "family": fam, "dsl": dsl, "horizons": [5, 21, 63], "sign_policy": "train-rank-ic21",
                   "theme": fam, "tier": tier, "tier_rank": TIER_RANK[tier], "prior_sign": 1, "citation": cite}
                  for cid, fam, tier, dsl, _origin, cite, _formula, _domain in MEMBERS]
    return {"schema": "atx.dsl-ic-library/v1", "id": LIBRARY_ID, "fields": base + extra, "families": families,
            "candidates": candidates}


def recipe_doc(library_sha: str) -> dict:
    lineage = [{"id": cid, "family": fam, "roster_order": k, "theme": fam, "tier": tier, "tier_rank": TIER_RANK[tier],
                "prior_sign": 1, "citation": cite, "origin": origin}
               for k, (cid, fam, tier, _dsl, origin, cite, _formula, _domain) in enumerate(MEMBERS, 1)]
    return {"schema": "atx.dsl-ic-experiment/v1", "id": f"{LIBRARY_ID}_initial",
            "library": {"path": LIBRARY, "sha256": library_sha},
            "generation": {"rule": "tiny-world-v1", "synthetic": True, "candidates": len(MEMBERS),
                           "families": 3, "family_is_theme": True},
            "orientation": {"rule": "prior sign embedded in the DSL; every candidate has prior_sign +1",
                            "prior_sign": 1},
            "lineage": lineage}


def registry_doc() -> dict:
    alphas = [{"id": cid, "dsl": dsl, "theme": fam, "tier": tier, "prior_sign": 1, "citation": cite,
               "prior_sign_source": "planted by construction (tiny_world)" if origin == "prior" else
               "synthetic fixture member", "form": "R(x)" if "decay_linear" not in dsl else "R(decay_linear(x, 21))",
               "origin": origin, "notes": {"formula": formula, "domain": domain, "deviation": "none"},
               "added_in": LIBRARY_ID}
              for cid, fam, tier, dsl, origin, cite, formula, domain in MEMBERS]
    fields = {name: {"formula_id": f"tiny-world/{name}/v1", "origin": "synthetic", "producer": GENERATOR,
                     "clock": "session-date 22:00 UTC mark"} for name in FIELD_NAMES}
    return {"schema": "atx.alpha-registry/v1", "alphas": alphas, "fields": fields, "themes": dict(THEMES),
            "tier_scores": {"A": 1.0, "A-": 0.9, "B+": 0.8, "B": 0.7, "B-": 0.55, "C+": 0.4}}


# ------------------------------------------------------------------ the spec
def resolve_spec(template: dict, pins: dict, bin_dir: Path, python: str, build_type: str = "Debug") -> dict:
    """The template with this checkout's scripts, BIN_DIR's executables, PYTHON and the computed pins; a Release
    build keeps only the template's non-Debug DLL directories."""
    if build_type not in BUILD_TYPES:
        raise ValueError(f"tiny_world: build_type {build_type!r} is not one of {BUILD_TYPES}")
    spec = json.loads(json.dumps(template))
    spec["python"] = python
    if build_type == "Release" and "env_path_prepend" in spec:
        spec["env_path_prepend"] = [p for p in spec["env_path_prepend"] if not p.endswith(DEBUG_DLL_SUFFIX)]
    for section in SCRIPT_SECTIONS:
        if section in spec and "script" in spec[section]:
            spec[section]["script"] = (REPO / spec[section]["script"]).as_posix()
    spec["exes"] = {key: (Path(bin_dir) / EXES[key]).as_posix() for key in spec.get("exes", {})}
    for key in ("library", "recipe", "role"):
        spec["inputs"][key]["sha256"] = pins[key]
    spec["fields"]["manifest_sha256"] = pins["fields"]
    return spec


def template_pins(template: dict) -> dict:
    return {"library": template["inputs"]["library"]["sha256"], "recipe": template["inputs"]["recipe"]["sha256"],
            "role": template["inputs"]["role"]["sha256"], "fields": template["fields"].get("manifest_sha256")}


def default_bin_dir() -> Path:
    import os
    env = os.environ.get("ATX_EQUITY_BIN")
    return Path(env) if env else REPO / "build-equity" / "bin"


def build(root: Path, seed: int = DEFAULT_SEED, *, bin_dir: Path | None = None, python: str | None = None,
          build_type: str = "Debug") -> dict:
    """Write the world under ROOT (see the module doc) and return its manifest SHAs."""
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    if any(root.iterdir()):
        raise FileExistsError(f"tiny_world: {root} is not empty (the fixture never overwrites)")
    world = simulate(seed)
    role_sha, role = write_role(root / ROLE_DIR, world, seed)
    fields_sha = write_fields(root / FIELDS_DIR, world, role_sha, role)
    library_sha = write_new(root / LIBRARY, canonical_json(library_doc()))
    recipe_sha = write_new(root / RECIPE, canonical_json(recipe_doc(library_sha)))
    registry_sha = write_new(root / REGISTRY, canonical_json(registry_doc()))
    pins = {"library": library_sha, "recipe": recipe_sha, "role": role_sha, "fields": fields_sha}
    template = json.loads(TEMPLATE.read_bytes())
    locked = template_pins(template)
    if seed == DEFAULT_SEED and all(locked.values()) and locked != pins:
        differ = sorted(k for k in pins if pins[k] != locked[k])
        raise ValueError(f"tiny_world: the world no longer matches the locked pins of {TEMPLATE.name} ({differ}); "
                         "a deliberate generator change relocks them: copy the four SHAs that "
                         "`python scripts/tests/fixtures/tiny_world.py <new dir>` prints into inputs.library, "
                         "inputs.recipe, inputs.role and fields.manifest_sha256, and re-record the goldens")
    spec = resolve_spec(template, pins, bin_dir or default_bin_dir(), python or Path(sys.executable).as_posix(),
                        build_type)
    write_new(root / SPEC, (json.dumps(spec, indent=2) + "\n").encode("utf-8"))
    return {"role_manifest_sha256": role_sha, "fields_manifest_sha256": fields_sha, "library_sha256": library_sha,
            "recipe_sha256": recipe_sha, "registry_sha256": registry_sha, "spec": str(root / SPEC), "seed": seed}


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="write the tiny_world fixture into a new directory")
    ap.add_argument("root", type=Path)
    ap.add_argument("--seed", type=int, default=DEFAULT_SEED)
    ap.add_argument("--bin", type=Path, default=None, help="directory of the equity executables (ATX_EQUITY_BIN)")
    ap.add_argument("--build-type", choices=BUILD_TYPES, default="Debug", help="the executables' build type")
    a = ap.parse_args()
    print(json.dumps(build(a.root, a.seed, bin_dir=a.bin, build_type=a.build_type), indent=2))
