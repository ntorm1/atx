"""The research field builder with the session-bar field module registered (lane XWQ, draft; Ruling PM7-33).

DEPRECATED (P9 lane A1, DEC-5): use the one entry ``prepare_research_fields.py --registry field_registry.json --fields
<list|all>``. ``register`` and ``main`` are thin wrappers over it (``field_registry``) and stay for existing callers.

The bar fields are registered here, never in ``prepare_research_fields.py``: the plain builder, every v8 field list and
every existing field's formula and producer fingerprint stay exactly as they are, so no reuse count can move. Run this
file with the builder's own arguments (same argv, same manifest layout; each bar field's entry names
research_fields_ohlc.py as ``producer``):

  python atx-engine/tools/prepare_research_fields_ohlc.py --role ... --price-source <the role's TickerHistory3>
      --fields <the base list>,open_adj,high_adj,low_adj --reuse <the base fields directory> ...

The fields v13+ one-liner of the base commit (``d.register(vars(b)); x.register(vars(b)); o.register(vars(b));
b.main(argv)``) is replaced by the registry entry, which binds every module the registry names in the same order.
"""
from __future__ import annotations

import field_registry                      # same directory: the registry entry these wrappers call (K-P9-1)
import prepare_research_fields as builder  # same directory: the builder (it does not import this module)
import research_fields_ohlc                 # same directory

DRAFT_MODULES = (research_fields_ohlc,)
FIELDS_OHLC_DRAFT = ("open_adj", "high_adj", "low_adj")


def register(host_namespace: dict) -> list:
    """Deprecated (P9 A1): ``field_registry.bind_modules`` of ``DRAFT_MODULES``. Binds every draft module into the
    builder namespace ``host_namespace`` (``vars(prepare_research_fields)``) and appends it to its ``FIELD_MODULES``; a
    module already registered there is left as it is. Returns the bound module objects."""
    return field_registry.bind_modules(host_namespace, DRAFT_MODULES)


def main(argv=None):
    """Deprecated (P9 A1): ``register`` then the builder's own ``main`` with this file's argv (manifest bytes as before).
    The replacement is ``prepare_research_fields.py --registry field_registry.json`` with the same argv."""
    register(vars(builder))
    builder.main(argv)


if __name__ == "__main__":
    main()
