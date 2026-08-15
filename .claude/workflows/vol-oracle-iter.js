export const meta = {
  name: 'vol-oracle-iter',
  description: 'One deterministic SpiderRock oracle capability/bootstrap step or one ready-state RSI iteration',
  whenToUse: 'Run exactly one oracle step. Args: { base?: string, focus?: string }. Bootstrap states dispatch one fixed lane; only ready runs the improvement loop.',
  phases: [
    { title: 'Capability', detail: 'freeze base SHA + holdout hash and select one ordered state' },
    { title: 'Bootstrap', detail: 'for a missing capability, dispatch exactly one fixed lane and stop' },
    { title: 'Measure', detail: 'ready only: aggregate smoke+tune Mode A+B scorecard' },
    { title: 'Attribute', detail: 'ready only: aggregate smoke+tune attribution' },
    { title: 'Improve', detail: 'ready only: gated vol-sprint child workflow' },
    { title: 'Ratchet', detail: 'ready + passed sprint only: frozen-hash holdout gate and memory' },
  ],
}

const BASE_REF = (args && args.base) || 'main'
const FOCUS = (args && args.focus) || ''
const RUN_ID = `vol-oracle-${Date.now()}-${Math.random().toString(16).slice(2)}`

const EVIDENCE_ITEM = {
  type: 'object', required: ['command', 'exit_code', 'output'],
  properties: {
    command: { type: 'string' },
    exit_code: { type: 'integer' },
    output: { type: 'string', description: 'verbatim, aggregate-only output excerpt; never licensed rows' },
  },
}
const CAPABILITY = {
  type: 'object', required: ['state', 'base_ref', 'base_sha', 'holdout_hash', 'next_iter', 'evidence'],
  properties: {
    state: { type: 'string', enum: ['missing_data', 'missing_mode_a', 'missing_conventions', 'missing_mode_b', 'ready'] },
    base_ref: { type: 'string' },
    base_sha: { type: 'string' },
    holdout_hash: { type: 'string', description: '64 hex chars; empty only for missing_data' },
    next_iter: { type: 'string' },
    evidence: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const BOOTSTRAP_REPORT = {
  type: 'object', required: ['state', 'outcome', 'branch', 'sha', 'base_sha', 'holdout_hash', 'lease_released', 'evidence', 'deviations'],
  properties: {
    state: { type: 'string', enum: ['missing_data', 'missing_mode_a', 'missing_conventions', 'missing_mode_b'] },
    outcome: { type: 'string', enum: ['DONE', 'BLOCKED'] },
    branch: { type: 'string' }, sha: { type: 'string' }, base_sha: { type: 'string' },
    holdout_hash: { type: 'string' }, lease_released: { type: 'boolean' },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, deviations: { type: 'string' },
  },
}
const MEASURE = {
  type: 'object', required: ['status', 'iter', 'scorecard_path', 'branch', 'sha', 'base_sha', 'lease_released', 'summary', 'top_cells', 'evidence'],
  properties: {
    status: { type: 'string', enum: ['ok', 'failed'] },
    iter: { type: 'string' }, scorecard_path: { type: 'string' },
    branch: { type: 'string' }, sha: { type: 'string' }, base_sha: { type: 'string' },
    lease_released: { type: 'boolean' },
    summary: { type: 'string', description: 'aggregate smoke+tune metrics and speed only' },
    top_cells: { type: 'array', items: { type: 'string' } },
    evidence: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const ATTR = {
  type: 'object', required: ['hypotheses', 'new_suspect_candidates'],
  properties: {
    hypotheses: { type: 'array', items: {
      type: 'object', required: ['id', 'target_cells', 'mechanism', 'prediction', 'blast_radius', 'effort'],
      properties: {
        id: { type: 'string' }, target_cells: { type: 'array', items: { type: 'string' } },
        mechanism: { type: 'string' }, prediction: { type: 'string' },
        blast_radius: { type: 'string' }, effort: { type: 'string', enum: ['S', 'M', 'L'] },
      },
    } },
    new_suspect_candidates: { type: 'array', items: { type: 'string' } },
  },
}
const RATCHET = {
  type: 'object', required: ['verdict', 'holdout_hash_verified', 'holdout_summary', 'evidence', 'hypotheses_confirmed', 'hypotheses_refuted', 'ledger_appended', 'northstar_updated'],
  properties: {
    verdict: { type: 'string', enum: ['ACCEPT', 'REJECT'] },
    holdout_hash_verified: { type: 'string' },
    holdout_summary: { type: 'string' },
    evidence: { type: 'array', items: EVIDENCE_ITEM },
    hypotheses_confirmed: { type: 'array', items: { type: 'string' } },
    hypotheses_refuted: { type: 'array', items: { type: 'string' } },
    ledger_appended: { type: 'array', items: { type: 'string' } },
    northstar_updated: { type: 'boolean' },
  },
}

const BOOTSTRAP_LANES = {
  missing_data: {
    stage: '1', slug: 'data',
    scope: 'ingest plus smoke/tune/holdout manifests and frozen holdout membership hash',
    contract: `Read atx-vol/bench/oracle/CHARTER.md stage 1. Verify at least 15 GiB free on the work drive before ingest; BLOCKED if not. Create or repair the partitioned store and smoke/tune/holdout manifests. Holdout must be disjoint from tune in both underliers and buckets. Write atx-vol/bench/oracle/cohorts/holdout.sha256 from canonical membership. NEVER run atx-vol-oracle-bench on holdout and never paste licensed rows; report only counts, hashes, paths, and command output.`,
  },
  missing_mode_a: {
    stage: '2', slug: 'mode-a',
    scope: 'Mode A oracle bench capability only',
    contract: `Read atx-vol/bench/oracle/CHARTER.md stage 2. Implement and target-test atx-vol-oracle-bench Mode A only, run smoke only, and write aggregate capability receipt atx-vol/bench/oracle/bootstrap/mode-a.json. Preserve the isolated convention seam. Do not implement or stub Mode B. NEVER benchmark holdout and never paste licensed rows.`,
  },
  missing_conventions: {
    stage: '3', slug: 'conventions',
    scope: 'iteration-0 convention resolution and Mode A residual floor',
    contract: `Read atx-vol/bench/oracle/CHARTER.md stage 3. Resolve conventions using aggregate smoke+tune Mode A only, commit CONVENTIONS.md and scorecards/iter-000.json, and record the evidenced residual floor in NORTHSTAR plus append-only LEDGER. NEVER benchmark holdout, never read Mode B, and never paste licensed rows.`,
  },
  missing_mode_b: {
    stage: '4', slug: 'mode-b',
    scope: 'Mode B oracle bench capability after conventions',
    contract: `Read atx-vol/bench/oracle/CHARTER.md stage 4. Implement and target-test Mode B, run aggregate smoke+tune only, and write capability receipt atx-vol/bench/oracle/bootstrap/mode-b.json. Do not change frozen conventions or holdout membership. NEVER benchmark holdout and never paste licensed rows.`,
  },
}

function validEvidence(evidence) {
  return Array.isArray(evidence) && evidence.length > 0 && evidence.every(item =>
    item && typeof item.command === 'string' && item.command.trim() &&
    Number.isInteger(item.exit_code) && typeof item.output === 'string' && item.output.trim())
}

phase('Capability')
const capability = await agent(
  `Read-only oracle capability inspection for run_id=${RUN_ID}. Resolve ${BASE_REF}^{commit} to a full immutable SHA. Inspect committed metadata and aggregate receipts only; do not build, edit, lease, benchmark, or read licensed parquet rows. Compute/verify the canonical holdout membership SHA-256 without revealing membership or row data. Select the FIRST missing state in exactly this order:\n1 missing_data: store manifest, smoke/tune/holdout manifests, or holdout.sha256 missing/invalid\n2 missing_mode_a: Mode A target/tests or bootstrap/mode-a.json aggregate receipt missing/invalid\n3 missing_conventions: CONVENTIONS.md or scorecards/iter-000.json residual floor missing/invalid\n4 missing_mode_b: Mode B target/tests or bootstrap/mode-b.json aggregate receipt missing/invalid\n5 ready.\nDetermine next_iter from accepted scorecards. Return only paths, hashes, state, and pasted metadata command output; never row values.`,
  { schema: CAPABILITY, label: 'capability' },
)
if (!capability || capability.base_ref !== BASE_REF || !/^[0-9a-f]{40}$/i.test(capability.base_sha || '') || !validEvidence(capability.evidence)) {
  throw new Error('capability inspection did not freeze a valid base SHA with evidence')
}
if (capability.state !== 'missing_data' && !/^[0-9a-f]{64}$/i.test(capability.holdout_hash || '')) {
  throw new Error(`${capability.state} requires a frozen 64-character holdout membership hash`)
}
const BASE_SHA = capability.base_sha
const HOLDOUT_HASH = capability.holdout_hash

if (capability.state !== 'ready') {
  phase('Bootstrap')
  const lane = BOOTSTRAP_LANES[capability.state]
  if (!lane) throw new Error(`no fixed bootstrap lane for ${capability.state}`)
  const branch = `lane/oracle-bootstrap-${lane.slug}-${Date.now()}`
  const bootstrap = await agent(
    `ONE fixed oracle bootstrap lane; no planner, vol-sprint, ordinary Measure, analyst, holdout ratchet, or unrelated full gate. State=${capability.state}; stage=${lane.stage}; scope=${lane.scope}. Frozen base SHA=${BASE_SHA}; run_id=${RUN_ID}. Lease an isolated pool with powershell scripts\\lease-worktree.ps1 -Branch ${branch} -Base ${BASE_SHA} -Agent oracle-bootstrap-${lane.slug} -RunId ${RUN_ID} -MaxPool 20; never work in C:\\atx. ${lane.contract} Use only the stage's targeted tests/static checks. Commit explicit paths, then safely release with -RunId ${RUN_ID}. Return command/output evidence and no licensed row data.`,
    { agentType: 'vol-builder', schema: BOOTSTRAP_REPORT, label: `bootstrap:${capability.state}` },
  )
  const bootstrapPassed = bootstrap && bootstrap.outcome === 'DONE' &&
    bootstrap.state === capability.state && bootstrap.base_sha === BASE_SHA &&
    bootstrap.branch === branch && /^[0-9a-f]{40}$/i.test(bootstrap.sha || '') && bootstrap.lease_released &&
    validEvidence(bootstrap.evidence) &&
    (capability.state === 'missing_data'
      ? /^[0-9a-f]{64}$/i.test(bootstrap.holdout_hash || '')
      : bootstrap.holdout_hash === HOLDOUT_HASH)
  return {
    iteration: 'bootstrap-' + lane.stage,
    capability_state: capability.state,
    verdict: bootstrapPassed ? 'BOOTSTRAP' : 'FAILED',
    holdout: null,
    confirmed: [], refuted: [],
    sprint: null,
    ledger: [],
    bootstrap: bootstrap || null,
    run_id: RUN_ID,
    base_sha: BASE_SHA,
    holdout_hash: capability.state === 'missing_data' && bootstrap ? bootstrap.holdout_hash : HOLDOUT_HASH,
  }
}

phase('Measure')
const measureBranch = `lane/oracle-measure-${capability.next_iter}-${Date.now()}`
const measure = await agent(
  `Oracle ready-state Measure, iteration ${capability.next_iter}. Frozen base SHA=${BASE_SHA}; frozen holdout membership hash=${HOLDOUT_HASH}. Lease with powershell scripts\\lease-worktree.ps1 -Branch ${measureBranch} -Base ${BASE_SHA} -Agent oracle-measure -RunId ${RUN_ID} -MaxPool 20; never work in C:\\atx. Run atx-vol-oracle-bench on smoke+tune only, modes A+B, correctness from dev and speed from pinned rel-avx2. Write and commit scorecards/iter-${capability.next_iter}.json, then release the measurement lease with -RunId ${RUN_ID}. Return the measurement commit plus aggregate metrics/cells and pasted command output only. Do not open or benchmark holdout; do not emit licensed rows.${FOCUS ? `\nOperator focus: ${FOCUS}` : ''}`,
  { schema: MEASURE, label: 'measure' },
)
if (!measure || measure.status !== 'ok' || measure.iter !== capability.next_iter ||
    measure.branch !== measureBranch || measure.base_sha !== BASE_SHA ||
    !/^[0-9a-f]{40}$/i.test(measure.sha || '') || !measure.lease_released ||
    !validEvidence(measure.evidence)) {
  return {
    iteration: capability.next_iter, capability_state: 'ready', verdict: 'FAILED', holdout: null,
    confirmed: [], refuted: [], sprint: null, ledger: [], run_id: RUN_ID,
    base_sha: BASE_SHA, holdout_hash: HOLDOUT_HASH,
    failure: 'Measure failed or returned no command/output evidence; no sprint or holdout ran',
  }
}

phase('Attribute')
const attribution = await agent(
  `Oracle ready-state Attribute, iteration ${measure.iter}. Read the aggregate smoke+tune scorecard from exact commit ${measure.sha} using git show ${measure.sha}:${measure.scorecard_path}. Aggregate summary: ${measure.summary}. Aggregate worst cells: ${JSON.stringify(measure.top_cells)}. Read NORTHSTAR and grep LEDGER for refuted hypotheses. Return 1-3 falsifiable hypotheses ranked by expected error reduction per effort. You are forbidden from reading holdout manifests, membership, scorecards, or row data; the hash is not provided.${FOCUS ? `\nOperator focus: ${FOCUS}` : ''}`,
  { agentType: 'vol-analyst', schema: ATTR, label: 'attribute' },
)
if (!attribution || !Array.isArray(attribution.hypotheses) || attribution.hypotheses.length < 1) {
  return {
    iteration: measure.iter, capability_state: 'ready', verdict: 'FAILED', holdout: null,
    confirmed: [], refuted: [], sprint: null, ledger: [], run_id: RUN_ID,
    base_sha: BASE_SHA, holdout_hash: HOLDOUT_HASH,
    failure: 'Attribution returned no hypotheses; no sprint or holdout ran',
  }
}

phase('Improve')
const task = `Oracle RSI iteration ${measure.iter}. Implement these aggregate smoke/tune hypotheses:\n${JSON.stringify(attribution.hypotheses, null, 2)}\nStructural mandate: hard cutover, no compatibility shim or opt-in flag, CHANGELOG BREAKING entry. Never expose licensed row data. Target cells/predictions are lane done criteria; blast radius names mandatory suites.`
let sprint
try {
  sprint = await workflow('vol-sprint', { task, base: measure.sha })
} catch (error) {
  sprint = { passed: false, error: String(error) }
}
if (!sprint || !sprint.passed) {
  return {
    iteration: measure.iter, capability_state: 'ready', verdict: 'FAILED', holdout: null,
    confirmed: [], refuted: [],
    sprint: { passed: false, integration: sprint ? sprint.integration || null : null },
    ledger: [], run_id: RUN_ID, base_sha: BASE_SHA, holdout_hash: HOLDOUT_HASH,
    failure: 'Mandatory sprint lane or gate failed/incomplete; no holdout ran and the REJECT counter must not change',
  }
}

phase('Ratchet')
const ratchet = await agent(
  `Oracle Ratchet, iteration ${measure.iter}. Passed isolated sprint integration: ${sprint.integration}. Frozen base SHA=${BASE_SHA}. Frozen holdout membership hash=${HOLDOUT_HASH}. Hypotheses: ${JSON.stringify(attribution.hypotheses)}. Oracle-suspect candidates from aggregate smoke/tune only: ${JSON.stringify(attribution.new_suspect_candidates || [])}.\nBefore reading holdout, recompute canonical membership hash and FAIL without benchmarking if it differs from ${HOLDOUT_HASH}. Then run aggregate holdout Mode A+B on the sprint integration. ACCEPT iff every target cell improved, no aggregate metric regressed >2%, and rel-avx2 speed is at least the pin; otherwise REJECT and do not merge. Exclude vetted oracle-suspect cells where market evidence sides with atx-vol. Mark hypotheses confirmed/refuted with measured deltas. Append LEDGER and update NORTHSTAR, including consecutive REJECT count. Return pasted command output for every number; never emit membership or licensed rows.`,
  { agentType: 'vol-verifier', schema: RATCHET, label: 'ratchet' },
)
const ratchetValid = ratchet && ratchet.holdout_hash_verified === HOLDOUT_HASH &&
  validEvidence(ratchet.evidence) && ratchet.northstar_updated

return {
  iteration: measure.iter,
  capability_state: 'ready',
  verdict: ratchetValid ? ratchet.verdict : 'FAILED',
  holdout: ratchetValid ? ratchet.holdout_summary : null,
  confirmed: ratchetValid ? ratchet.hypotheses_confirmed : [],
  refuted: ratchetValid ? ratchet.hypotheses_refuted : [],
  sprint: { passed: true, integration: sprint.integration },
  ledger: ratchetValid ? ratchet.ledger_appended : [],
  run_id: RUN_ID,
  base_sha: BASE_SHA,
  holdout_hash: HOLDOUT_HASH,
}
