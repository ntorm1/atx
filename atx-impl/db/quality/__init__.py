from __future__ import annotations

import sys as _sys
from types import ModuleType as _ModuleType

from . import _checks as _checks
from . import _runner as _runner
from . import _types as _quality_types


for _module in (_quality_types, _checks, _runner):
    for _name, _value in vars(_module).items():
        if not (_name.startswith("__") and _name.endswith("__")):
            globals()[_name] = _value


class _QualityModule(_ModuleType):
    _propagation_modules = (_quality_types, _checks, _runner)

    def __setattr__(self, name: str, value: object) -> None:
        super().__setattr__(name, value)
        for module in self._propagation_modules:
            if hasattr(module, name):
                setattr(module, name, value)


_sys.modules[__name__].__class__ = _QualityModule

for _name in (
    "_analytic_check_specs",
    "_check_common",
    "_estimate_check_specs",
    "_feature_catalog_check_specs",
    "_fundamental_check_specs",
    "_market_reference_check_specs",
    "_ownership_check_specs",
    "checks_analytics",
    "checks_estimates",
    "checks_features_catalog",
    "checks_fundamentals",
    "checks_market_reference",
    "checks_ownership",
    "checks_survivorship",
    "survivorship_dqc_results",
):
    globals().pop(_name, None)

del _ModuleType, _QualityModule, _checks, _module, _name, _quality_types, _runner, _sys, _types, _value
