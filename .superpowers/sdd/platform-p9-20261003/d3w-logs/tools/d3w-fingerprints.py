"""Fingerprints of the composition modules and the fitter's producer tuples (run with cwd atx-impl/tools)."""
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path.cwd()))
import importlib
import fit_composition_weights as fcw
out = {}
for m in ("composition_engine_fit", "composition_ic_shrink", "composition_mom_volman", "composition_resid",
          "composition_rules", "composition_theme_erc", "composition_theme_tsmom"):
    mod = importlib.import_module(m)
    out[m] = fcw.composition_rules.module_fingerprint(mod.__file__, mod.PRODUCERS)
for name in ("AIM_ERA_PRODUCERS", "AIM_PRODUCERS", "CONTEXT_PRODUCERS", "FACTOR_PRODUCERS"):
    out[f"fitter {name}"] = fcw.producer_fingerprint(getattr(fcw, name))
out["fitter OUTPUT_PRODUCERS (fit, fit_prior)"] = fcw.producer_fingerprint(fcw.OUTPUT_PRODUCERS)
print(json.dumps(out, indent=1, sort_keys=True))
