### Task T31: fitter `ew-theme-aim-v1`, pipeline `--work-dir`, netting ratio

**Lane:** B · **Pool:** pool-4 (`git checkout -B feat/mega-alpha-v5-aimfit-20260927 <pool-2 HEAD>`) · **Model:** Opus 5.5 · **Depends on:** T29 · **Tokens:** FIT, STUDIES · **Peak:** 0.3 GiB

**Files:**
- Modify: `atx-impl/tools/fit_composition_weights.py:110-123` (ids), `:1042-1049` (weights), `:1168-1173` (gating), `:1380-1381,1407-1411,1488-1491` (provenance pattern), `:1433-1462` (summary/provenance), `:1515-1523` (argparse)
- Modify: `atx-impl/tools/test_fit_composition_weights.py` (append)
- Create: `.superpowers/sdd/mega-alpha-20260926/studies/v5_train.sh`
- Modify: `.superpowers/sdd/mega-alpha-20260926/studies/nav_summ.py` (netting ratio, gross/net leverage, paired ΔSR)

**Interfaces:**
- Consumes: per-candidate cached VM payloads via `CacheLayout` (FIT:373-458), live mask and `Context.book` (FIT:754-767), `taus` (FIT:1197), `SCRIPT_SHA256` work records (FIT:159, 802).
- Produces: `--composition ew-theme-aim-v1` (schema unchanged, `provenance.rule = "ew-theme-aim-v1"`, `provenance.aim = {theta, lags, rho: {id: [...]}, gain: {id: g}, coverage_effective_theme_weight: {theme: x}}`), `weighted_standalone_turnover` unchanged; `nav_summ.py --weights <W> --reference <nav dir>` prints netting ratio and paired ΔSR.

- [ ] **Step 1:** Constants and gating:

```python
AIM_RULE_ID = "ew-theme-aim-v1"
AIM_THETA = 0.05                                   # R4': fixed, equals the reference construction theta
AIM_LAGS = list(range(0, 22)) + list(range(28, 127, 7))   # exact lags; others linearly interpolated
AIM_GAIN_MIN, AIM_MAX_LAG = 0.05, 126
PRIOR_COMPOSITIONS = (EW_THEME_RULE_ID, AIM_RULE_ID)
COMPOSITIONS = (RULE_ID, NETCOST_RULE_ID, EW_THEME_RULE_ID, AIM_RULE_ID)
# FIT:1170 becomes:
require(prior == (args.composition in PRIOR_COMPOSITIONS), "--composition ew-theme-v1|ew-theme-aim-v1 and --screen v4-prior-v1/v2 go together")
```

- [ ] **Step 2:** Autocorrelation and gain (pure numpy; per candidate; cached in the work record keyed by `SCRIPT_SHA256`):

```python
def standardized_ranks(signal: np.ndarray, live: np.ndarray, min_names: int = 50) -> np.ndarray:
    """Per-day centered, unit-variance ranks over live names; NaN elsewhere."""
    z = np.full(signal.shape, np.nan)
    for d in range(signal.shape[0]):
        m = live[d]
        if m.sum() < min_names: continue
        r = scipy.stats.rankdata(signal[d, m]).astype(float)
        s = r.std()
        if s > 0: z[d, m] = (r - r.mean()) / s
    return z

def rank_autocorrelation(z: np.ndarray, lags: list[int], min_names: int = 50) -> np.ndarray:
    """rho_bar(j) for j in lags: mean over days of the cross-sectional correlation of z_d and z_{d-j} over names finite on both days."""
    out = np.full(len(lags), np.nan)
    for n, j in enumerate(lags):
        a, b = (z, z) if j == 0 else (z[j:], z[:-j])
        both = np.isfinite(a) & np.isfinite(b)
        cnt = both.sum(axis=1); num = np.where(both, a * b, 0.0).sum(axis=1)
        ok = cnt >= min_names
        out[n] = float(np.mean(num[ok] / cnt[ok])) if ok.any() else np.nan
    return out

def aim_gain(rho_at_lags: np.ndarray, lags: list[int], theta: float = AIM_THETA, max_lag: int = AIM_MAX_LAG) -> float:
    """g = theta * sum_{j=0..max_lag} (1-theta)^j rho(j), rho interpolated linearly between exact lags; NaN lags -> 0."""
    rho = np.interp(np.arange(max_lag + 1), lags, np.nan_to_num(rho_at_lags, nan=0.0))
    g = theta * float(np.sum((1 - theta) ** np.arange(max_lag + 1) * rho))
    return float(min(1.0, max(AIM_GAIN_MIN, g)))

def ew_theme_aim_weights(themes: list[str], gains: list[float]) -> tuple[np.ndarray, dict]:
    present = sorted(set(themes)); counts = {t: themes.count(t) for t in present}
    raw = np.array([g / (len(present) * counts[t]) for t, g in zip(themes, gains)])
    weights = raw / raw.sum()
    table = {t: {"admitted_count": counts[t], "nominal_theme_weight": 1.0 / len(present),
                 "aim_theme_weight": float(sum(w for w, th in zip(weights, themes) if th == t))} for t in present}
    return weights, table
```
Coverage-effective theme weight (report only): mean over TRAIN decisions of Σ_{k∈theme} w_k·live_k(d) / Σ_k w_k·live_k(d).

- [ ] **Step 3:** Wire at FIT:1445: `weights, theme_table = ew_theme_aim_weights(...) if args.composition == AIM_RULE_ID else ew_theme_weights(...)`; replace the hard-coded `EW_THEME_RULE_ID` at `:1434` and `:1460-1462` by `args.composition`; add the `provenance.aim` block only for the aim rule (keeps ew-theme-v1 bytes). Store `rho`/`gain` in the per-candidate work record so passes resume.
- [ ] **Step 4:** Tests (append):

```python
def test_aim_gain_ar1_matches_closed_form():
    phi, theta = 0.02, 0.05; lags = AIM_LAGS
    rho = np.array([(1 - phi) ** j for j in lags])
    g = aim_gain(rho, lags, theta)
    closed = theta * sum((1 - theta) ** j * (1 - phi) ** j for j in range(AIM_MAX_LAG + 1))
    assert abs(g - closed) < 1e-9

def test_aim_gain_degenerate_and_gappy():
    z = np.full((300, 200), np.nan); z[:150] = standardized_ranks(np.random.default_rng(0).normal(size=(150, 200)), np.ones((150, 200), bool))
    rho = rank_autocorrelation(z, AIM_LAGS); assert np.isfinite(rho[0]) and abs(rho[0] - 1) < 1e-9
    assert AIM_GAIN_MIN <= aim_gain(rho, AIM_LAGS) <= 1.0
    const = np.ones((300, 200)); zc = standardized_ranks(const, np.ones((300, 200), bool)); assert np.isnan(zc).all()  # rankdata ties -> std 0 -> NaN -> degenerate

def test_aim_weights_global_normalization():
    w, t = ew_theme_aim_weights(["a", "a", "b"], [1.0, 1.0, 0.25])
    assert abs(w.sum() - 1) < 1e-12 and t["a"]["aim_theme_weight"] > t["b"]["aim_theme_weight"]

def test_ew_theme_v1_bytes_unchanged(tmp_path, v4_fixture):
    """The existing byte_stability fixture (test_fit_composition_weights.py:1473) already pins the ew-theme-v1 digest.
    Run the same fixture through the aim rule too and assert the v1 output is untouched by the new code path."""
    v1 = run_fitter(v4_fixture, composition="ew-theme-v1", out=tmp_path / "v1")
    aim = run_fitter(v4_fixture, composition="ew-theme-aim-v1", out=tmp_path / "aim")
    assert sha256(v1 / "composition_weights.json") == BYTE_STABILITY_V1_SHA256   # constant already in the test module
    assert json.load(open(aim / "composition_weights.json"))["provenance"]["rule"] == "ew-theme-aim-v1"
    assert "aim" not in json.load(open(v1 / "composition_weights.json"))["provenance"]
```
(`run_fitter`, `v4_fixture`, `BYTE_STABILITY_V1_SHA256` are the helpers/constants the byte-stability test already uses; reuse them, do not duplicate.)
`standardized_ranks` uses `scipy.stats.rankdata` only if scipy is already imported by the fitter; otherwise use the numpy tie-aware rank the fitter already has for its own tied ranks (`Context.book` path) — do not add a dependency.

- [ ] **Step 5:** `studies/v5_train.sh` = copy of `v4_train.sh` with: fit phase `--composition "$COMP"` (env `COMP=ew-theme-aim-v1|ew-theme-v1`), `--work-dir build-equity/mega-fit-work-v5 --max-seconds 150` and up to 3 retries, output prefixes `WA`/`NA-<cell>`; nav phase reads env `COMBINED` (`ew` → `build-equity/mega-v4w-train-1` sha `24a6cc76…`; `aim` → `build-equity/mega-v5w-train-aim` + its sha from the receipt), `THETA`, `DUST`, `RATE` (`fixed|per-name`), `LEV`, writes to `build-equity/mega-nav-v5-$COMBINED-t$THETA-d$DUST-$RATE`, and passes `--rule aim-partial-v5 --trade-fraction $THETA --dust-multiple $DUST --aim-leverage $LEV` plus, when `RATE=per-name`, `--rate per-name-v1 --rate-rra 10 --rate-min .01 --rate-max .15`. Never `--band-multiple`. Every SHA is passed explicitly (the tools refuse unpinned inputs).
- [ ] **Step 6:** `nav_summ.py`: add `--weights W` (reads `weighted_standalone_turnover`) → prints `netting_ratio = daily_turnover_mean / weighted_standalone_turnover`; add `--reference <nav dir>` → paired ΔSR(net), ρ of daily nets, Memmel SE, and a 2,000-draw circular block bootstrap (block 21) CI; add mean gross/net leverage and `held_share`.
- [ ] **Step 7:** Report (root command lines for the fit, expected ≤ 3 passes) and commit: `feat(fitter): ew-theme-aim-v1 aim gains from TRAIN rank autocorrelation; v5 pipeline; netting ratio (T31)`.

**Acceptance (root, T38):** fit completes within ≤ 3 bounded passes; `byte_stability` green and the re-fit of ew-theme-v1 reproduces `9a9c949a…`; all g_k ∈ [0.05, 1]; slow themes (value, profitability) have g ≥ 0.8 and reversal/IV members g ≤ 0.5 (sanity, not a gate).

---

