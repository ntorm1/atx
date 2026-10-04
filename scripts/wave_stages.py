"""The stages of one research wave (research_wave.py), in order; each is a stage_chain.Stage.

  preflight  the manifest is committed (its commit is the pre-registration) and every pin holds: the parent cell spec
             loads, may run and has its NAV; the fields manifest hashes to the pin, is complete and sealed no later
             than the research window's seal; a candidate's declared fields are in it; no path names a sealed year;
             the ledger's chain verifies, its N equals expect.n_before, and the budget holds the wave's new admission
             trials (and the cell); the code pathspec is clean (the bounded runner refuses otherwise)
  register   (library wave) one add-alpha per frozen string into library NAME (K1 plan of record saved under the wave
             dir), then one commit of exactly the files add-alpha wrote
  screen     research_cycle.py run lib-NAME.json --screen (u, fit, card, marginal, gate: the admission trials are
             ledgered by the gate); the sign rule applied by code to every string's row of the fit's admission.json
             (a string without a row stops the stage)
  spec       the cell: the screen library when every string stays, the b library (add-alpha --name NAME+b on the kept
             strings, same trial ids) when the sign rule dropped some, none when the gate stopped; a rule wave writes
             the template's cell file on the parent with its constants and locks it; one commit
  run        research_cycle.py run CELL --stop-after nav: the calibration run at the parent's L (no summ, no ledger
             line)
  match      gross matching by the named mode: the mechanics reader (keys only) on the calibration NAV and the parent's;
             within tolerance the calibration run is the cell, else the -gm copy at L', locked, committed, run
  verify     the mechanics rule on the cell's mechanics keys (before any return is read), the NAV's C-13 binding, a
             scan of every log the wave produced for a date at or after the seal (wave_seal.py); a failure stops for
             a ruling (no ledger line)
  judge      research_cycle.py run CELL (monitor and summ: nav_summ scores and ledgers the cell), the PM5-23 bundle
             against the parent, the book reader, the verdict by the named acceptance rule
  record     the ledger re-read (chain, the cell's line, N advanced by exactly the cell), the seal scan again over
             every log (the judge's included: the hidden-data line), wave-result.json, the ready-to-paste log
             section, copies to record.copy_to
A stage that does not apply (no strings in a rule wave, no cell after the gate) records why and passes. From screen
on, each stage's inputs re-check the digests its predecessor recorded (wave_stage_util.pinned).

Layout (Ruling PM8-12: no file over ~400 lines, split by stage): wave_stage_preflight.py (preflight and the budget),
wave_stage_library.py (register, screen, spec), wave_stage_cell.py (run, match, the readers), wave_stage_record.py
(verify, judge, record), wave_stage_util.py (shared pieces); this module assembles the chain, every stage through
``timed`` (under driver.timings its receipt records the processes it ran: P9 OR section 5).
"""
from __future__ import annotations

import functools

from wave_stage_util import Stage, fold_processes, timings_on
from wave_stage_preflight import (admission_new, admission_trial_id, admission_used, budget_check,  # noqa: F401
                                  preflight, preflight_inputs, preflight_plan)
from wave_stage_library import (register, register_plan, screen, screen_inputs, screen_plan, spec_inputs,
                                spec_plan, spec_stage)
from wave_stage_cell import match, match_inputs, match_plan, run_inputs, run_plan, run_stage
from wave_stage_record import (judge, judge_inputs, judge_plan, record, record_inputs, record_plan, verify,
                               verify_inputs, verify_plan)


def timed(run):
    """A stage's run that starts with an empty process list (wave_context.Wave.processes); under driver.timings (P9 OR
    section 5) its outputs gain "processes": every command and git query it ran, folded by what (wave_stage_util.
    fold_processes); without the key the outputs are those of before."""
    @functools.wraps(run)
    def go(w, done, log):
        w.processes = []
        out = run(w, done, log)
        if timings_on(w) and isinstance(out, dict):
            out = dict(out, processes=fold_processes(w.processes))
        return out
    return go


STAGES = [Stage("preflight", timed(preflight), preflight_inputs, preflight_plan),
          Stage("register", timed(register), None, register_plan),
          Stage("screen", timed(screen), screen_inputs, screen_plan),
          Stage("spec", timed(spec_stage), spec_inputs, spec_plan),
          Stage("run", timed(run_stage), run_inputs, run_plan),
          Stage("match", timed(match), match_inputs, match_plan),
          Stage("verify", timed(verify), verify_inputs, verify_plan),
          Stage("judge", timed(judge), judge_inputs, judge_plan),
          Stage("record", timed(record), record_inputs, record_plan)]
