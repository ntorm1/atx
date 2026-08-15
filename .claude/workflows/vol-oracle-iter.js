export const meta = {
  name: 'vol-oracle-iter',
  description: 'One SpiderRock-oracle RSI iteration: measure, attribute, improve via vol-sprint, ratchet gate on holdout, memory',
  whenToUse: 'Run one iteration of the atx-vol oracle self-improvement loop. Args: { base?: string, focus?: string }. Self-bootstraps: missing data/tooling turns the Improve stage into the bootstrap charter.',
  phases: [
    { title: 'Measure', detail: 'oracle bench on smoke+tune, modes A+B, scorecard iter-NNN' },
    { title: 'Attribute', detail: 'vol-analyst ranks cells, forms falsifiable hypotheses' },
    { title: 'Improve', detail: 'vol-sprint child workflow implements the work items' },
    { title: 'Ratchet', detail: 'oracle gate on HOLDOUT cohort; accept/reject; ledger + NORTHSTAR' },
  ],
}

const BASE = (args && args.base) || 'main'
const FOCUS = (args && args.focus) || ''

const MEASURE = {
  type: 'object', required: ['status', 'iter', 'scorecard_path', 'summary'],
  properties: {
    status: { type: 'string', enum: ['ok', 'missing_data', 'missing_tooling'] },
    iter: { type: 'string', description: 'zero-padded iteration number, next after existing scorecards (e.g. 003); "000" when bootstrapping' },
    scorecard_path: { type: 'string', description: 'written scorecard path, or empty when status != ok' },
    summary: { type: 'string', description: 'headline metrics + speed, or what is missing and the exact evidence' },
    top_cells: { type: 'array', items: { type: 'string' } },
  },
}
const ATTR = {
  type: 'object', required: ['hypotheses'],
  properties: {
    hypotheses: { type: 'array', items: {
      type: 'object', required: ['id', 'target_cells', 'mechanism', 'prediction', 'blast_radius', 'effort'],
      properties: {
        id: { type: 'string' }, target_cells: { type: 'array', items: { type: 'string' } },
        mechanism: { type: 'string' }, prediction: { type: 'string' },
        blast_radius: { type: 'string' }, effort: { type: 'string', enum: ['S', 'M', 'L'] },
      } } },
    new_suspect_candidates: { type: 'array', items: { type: 'string' } },
  },
}
const RATCHET = {
  type: 'object', required: ['verdict', 'holdout_summary', 'ledger_appended', 'northstar_updated'],
  properties: {
    verdict: { type: 'string', enum: ['ACCEPT', 'REJECT', 'BOOTSTRAP'] },
    holdout_summary: { type: 'string', description: 'per-target-cell deltas + aggregate regression check + speed vs pin, with evidence; or bootstrap outcome' },
    hypotheses_confirmed: { type: 'array', items: { type: 'string' } },
    hypotheses_refuted: { type: 'array', items: { type: 'string' } },
    ledger_appended: { type: 'array', items: { type: 'string' } },
    northstar_updated: { type: 'boolean' },
  },
}

phase('Measure')
const measure = await agent(
  `Oracle RSI loop, Measure stage. Read atx-vol/docs/oracle/NORTHSTAR.md and atx-vol/bench/oracle/cohorts/README.md first.
Determine the next iteration number from atx-vol/bench/oracle/scorecards/ (highest iter-NNN + 1; 000 if none).
Then run the oracle benchmark: atx-vol-oracle-bench on the smoke and tune cohorts, modes A and B, dev preset via powershell scripts\\atx-build.ps1 (build the tool target-scoped first if stale). Write the scorecard to atx-vol/bench/oracle/scorecards/iter-<NNN>.json and report headline metrics + the worst cells.
If the parquet oracle store (see cohorts/README.md for the path) is missing -> status missing_data. If the bench tool does not exist or cannot run -> status missing_tooling. Paste exact evidence either way. Do NOT run the 15 GB ingest yourself and do NOT build missing tooling yourself — report and stop.${FOCUS ? `\nFocus note from operator: ${FOCUS}` : ''}`,
  { schema: MEASURE, label: 'measure' },
)

let attribution = null
let task
if (measure.status === 'ok') {
  phase('Attribute')
  attribution = await agent(
    `Oracle RSI loop, Attribute stage, iteration ${measure.iter}. Scorecard: ${measure.scorecard_path}
Measure summary: ${measure.summary}
Worst cells: ${JSON.stringify(measure.top_cells || [])}
Apply your process (predecessor diff, contribution ranking, A/B decomposition, refuted-list check). Return 1-3 hypotheses ranked by expected-error-reduction per effort.${FOCUS ? `\nOperator focus: ${FOCUS}` : ''}`,
    { agentType: 'vol-analyst', schema: ATTR, label: 'attribute' },
  )
  task = `Oracle RSI iteration ${measure.iter} improvement work. Implement these hypotheses from the attribution stage (structural changes welcome: NO backwards-compat shims, NO opt-in flags — hard cutover + CHANGELOG BREAKING entries; goal is state-of-the-art American options pricing accuracy vs the SpiderRock oracle and state-of-the-art speed):
${JSON.stringify(attribution.hypotheses, null, 2)}
Each hypothesis's target cells and prediction define its lane's done criteria. Blast radius names the suites that must stay green.`
} else {
  log(`bootstrap path: ${measure.status} — ${measure.summary.slice(0, 200)}`)
  task = `Oracle RSI loop BOOTSTRAP (${measure.status}). Execute the bootstrap charter at atx-vol/bench/oracle/CHARTER.md, stage ${measure.status === 'missing_data' ? '1 (ingest + cohort selection)' : '2 (oracle bench tool, Mode A first)'}. Measure-stage evidence of what is missing:
${measure.summary}`
}

phase('Improve')
let sprint
try {
  sprint = await workflow('vol-sprint', { task, base: BASE })
} catch (e) {
  sprint = { passed: false, error: String(e) }
}
log(`vol-sprint: ${sprint && sprint.passed ? 'gate PASSED' : 'did not pass'}${sprint && sprint.integration ? ` — ${sprint.integration}` : ''}`)

phase('Ratchet')
const ratchet = await agent(
  `Oracle RSI loop, Ratchet stage, iteration ${measure.iter}. The vol-sprint child already ran the standard correctness gates; you run the ORACLE gate and the loop's memory.
Sprint result: ${JSON.stringify({ passed: sprint.passed, integration: sprint.integration || null, lanes: sprint.lanes || [], error: sprint.error || null })}
Hypotheses under test: ${attribution ? JSON.stringify(attribution.hypotheses) : 'none — bootstrap iteration'}
New oracle-suspect candidates to vet: ${attribution ? JSON.stringify(attribution.new_suspect_candidates || []) : '[]'}

Process:
1. Bootstrap iteration (no hypotheses): verify the bootstrap deliverable works (evidence), verdict BOOTSTRAP, skip holdout comparison.
2. Otherwise: rerun atx-vol-oracle-bench on the HOLDOUT cohort (modes A+B, dev preset) on the sprint's integration branch. Compare against the previous accepted holdout scorecard: ACCEPT iff every hypothesis's target cells improved AND no aggregate metric regressed > 2% AND speed >= the pinned baseline (rel-avx2 for speed only). Otherwise REJECT — leave the branch for inspection, do not merge further.
3. Mark each hypothesis CONFIRMED (prediction held on holdout) or REFUTED (with the measured delta).
4. Memory, both verdicts: append atx-vol/docs/LEDGER.md (append-only) — one line for the iteration verdict with metric deltas, one per refuted hypothesis. Update atx-vol/docs/oracle/NORTHSTAR.md: current-metrics table, open/confirmed/refuted hypothesis lists, vetted oracle-suspect additions, iteration counter (and consecutive-reject counter; at 3 consecutive REJECTs write ESCALATE at the top).
Evidence discipline: every number you report carries pasted command output.`,
  { agentType: 'vol-verifier', schema: RATCHET, label: 'ratchet' },
)

return {
  iteration: measure.iter,
  measure_status: measure.status,
  verdict: ratchet ? ratchet.verdict : 'RATCHET-AGENT-DIED',
  holdout: ratchet ? ratchet.holdout_summary : null,
  confirmed: ratchet ? ratchet.hypotheses_confirmed : [],
  refuted: ratchet ? ratchet.hypotheses_refuted : [],
  sprint: { passed: !!(sprint && sprint.passed), integration: (sprint && sprint.integration) || null },
  ledger: ratchet ? ratchet.ledger_appended : [],
}
