"""The research field builder with the session-bar field module registered (lane XWQ, draft; Ruling PM7-33).

The bar fields are registered here, never in ``prepare_research_fields.py``: the plain builder, every v8 field list and
every existing field's formula and producer fingerprint stay exactly as they are, so no reuse count can move. Run this
file with the builder's own arguments (same argv, same manifest layout; each bar field's entry names
research_fields_ohlc.py as ``producer``):

  python atx-engine/tools/prepare_research_fields_ohlc.py --role ... --price-source <the role's TickerHistory3>
      --fields <the base list>,open_adj,high_adj,low_adj --reuse <the base fields directory> ...

A build that also needs the other draft modules registers each entry's ``register`` on the builder namespace in one
process (the fields v13 route: ``d.register(vars(b)); x.register(vars(b)); o.register(vars(b)); b.main(argv)``).
"""
from __future__ import annotations

import prepare_research_fields as builder  # same directory: the builder (it does not import this module)
import research_fields_ohlc                 # same directory

DRAFT_MODULES = (research_fields_ohlc,)
FIELDS_OHLC_DRAFT = ("open_adj", "high_adj", "low_adj")


def register(host_namespace: dict) -> list:
    """Bind every draft module into the builder namespace ``host_namespace`` (``vars(prepare_research_fields)``) and
    append it to its ``FIELD_MODULES``; a module already registered there is left as it is. Returns the bound module
    objects."""
    modules = host_namespace["FIELD_MODULES"]
    bound = []
    for module in DRAFT_MODULES:
        present = [m for m in modules if type(m).__module__ == module.__name__]
        if present:
            bound += present
            continue
        m = module.bind(host_namespace)
        modules.append(m)
        bound.append(m)
    return bound


def main(argv=None):
    register(vars(builder))
    builder.main(argv)


if __name__ == "__main__":
    main()
