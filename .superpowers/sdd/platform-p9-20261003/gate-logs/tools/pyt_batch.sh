#!/usr/bin/env bash
# GATE step 2 (agent 2): the remaining pytest invocations of plan 4.2 / root-gate-brief step 2, each its own
# invocation through tools/pyt.sh (GATE-TMP short root C:/Users/natha/AppData/Local/Temp/g2<INV>, Release exes:
# GATE_BUILD=rel), seed 0, run one after another in the chain named by the argument (two chains may run side by side,
# GATE-PAR). Each run's line lands in gate-logs/pyt-exits.txt; the roots are deleted by literal name after the records.
# usage: pyt_batch.sh A|B
set -u
root=/c/atx-wt/pool-2
L=$root/.superpowers/sdd/platform-p9-20261003/gate-logs
cd "$root" || exit 2
export GATE_BUILD=rel
rest=$(ls atx-engine/tools/test_*.py | grep -v -E '/(test_stage_chain|test_research_fields_holdings|test_field_registry)\.py$')
case "$1" in
  A)
    # the names of the 3 skips of the whole scripts/tests runs (all in this file; -rs prints the reasons)
    bash $L/tools/pyt.sh research-cycle-skips 0 rs $L/pyt-research-cycle-rs-rel-s0.log -- -rs \
      scripts/tests/test_research_cycle.py
    bash $L/tools/pyt.sh stage-chain 0 ch $L/pyt-stage-chain-rel-s0.log -- atx-engine/tools/test_stage_chain.py
    bash $L/tools/pyt.sh engine-tools-rest 0 et $L/pyt-engine-tools-rel-s0.log -- $rest
    bash $L/tools/pyt.sh mirror-guard 0 mg $L/pyt-mirror-guard-rel-s0.log -- scripts/tests/test_no_python_mirror.py
    bash $L/tools/pyt.sh impl-strategies 0 is $L/pyt-impl-strategies-rel-s0.log -- atx-impl/strategies
    ;;
  B)
    bash $L/tools/pyt.sh impl-tools 0 it $L/pyt-impl-tools-rel-s0.log -- atx-impl/tools
    bash $L/tools/pyt.sh fields-holdings-pair 0 fh $L/pyt-fields-holdings-pair-rel-s0.log -- \
      atx-engine/tests/fixtures/research_fields_holdings/test_holdings_engine_path.py \
      atx-engine/tests/fixtures/research_fields_holdings/test_holdings_fixture.py
    bash $L/tools/pyt.sh fields-four 0 ff $L/pyt-fields-four-rel-s0.log -- \
      atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py \
      atx-engine/tests/fixtures/research_fields/test_research_fields_fixture.py \
      atx-engine/tests/fixtures/research_fields/test_vendor_engine_path.py \
      atx-engine/tests/fixtures/research_fields/test_vendor_panel_fixture.py
    bash $L/tools/pyt.sh fields-tools-holdings 0 th $L/pyt-fields-tools-holdings-rel-s0.log -- \
      atx-engine/tools/test_research_fields_holdings.py
    bash $L/tools/pyt.sh field-registry 0 tr $L/pyt-field-registry-rel-s0.log -- atx-engine/tools/test_field_registry.py
    ;;
  *) echo "usage: pyt_batch.sh A|B"; exit 2 ;;
esac
echo "pyt_batch $1 done"
