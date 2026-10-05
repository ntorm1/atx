"""The research cycle (scripts/research_cycle.py), split move-only by P9 lane E2 (orchestration review section 1):

  spec      constants, CycleError, spec load (templates resolved) and validation, override parsers
  resolver  root-relative files: existence, SHA-256, JSON
  phases    Step, the IC exe's capabilities, Cycle (every phase's argv with its pins)
  gate      fields check, admission gate and trials, cycle_verdict.json, ledger state and copy
  compare   the spec's identity checks
  lock      `lock` (input and exe pins)
  cli       plan / status / run and the verbs (the usage is this module's docstring)

research_cycle.py stays the entry point and re-exports every name it had; scripts/ must be on sys.path."""
