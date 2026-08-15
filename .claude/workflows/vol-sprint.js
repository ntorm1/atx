export const meta = {
  name: 'vol-sprint',
  description: 'atx-vol DAG: plan, parallel pool-leased build lanes, adversarial review, fix, integrate + gate',
  whenToUse: 'Multi-lane atx-vol feature/refactor work with >=2 file-disjoint work streams. Args: { task: string, base?: string }',
  phases: [
    { title: 'Plan', detail: 'decompose task into <=4 disjoint lane briefs' },
    { title: 'Build', detail: 'one builder per lane in a leased pool worktree' },
    { title: 'Review', detail: 'fresh-context refuter per lane diff' },
    { title: 'Fix', detail: 'builder addresses blockers in the same warm tree' },
    { title: 'Gate', detail: 'integrate lanes, full fast suite + module gates, ledger append' },
  ],
}

if (!args || !args.task) throw new Error('vol-sprint needs args: { task: "<what to build>", base?: "<ref, default main>" }')
const BASE = (args && args.base) || 'main'

const LANE = {
  type: 'object',
  required: ['id', 'title', 'goal', 'branch', 'files_in_scope', 'files_forbidden', 'check_targets', 'build_targets', 'suites', 'done_criteria', 'out_of_scope'],
  properties: {
    id: { type: 'string' }, title: { type: 'string' }, goal: { type: 'string' },
    branch: { type: 'string' },
    files_in_scope: { type: 'array', items: { type: 'string' } },
    files_forbidden: { type: 'array', items: { type: 'string' } },
    check_targets: { type: 'array', items: { type: 'string' } },
    build_targets: { type: 'array', items: { type: 'string' } },
    suites: { type: 'array', items: { type: 'string' } },
    done_criteria: { type: 'string' }, out_of_scope: { type: 'string' },
  },
}
const PLAN = {
  type: 'object', required: ['lanes', 'integration_branch', 'shared_files_note'],
  properties: {
    lanes: { type: 'array', items: LANE },
    integration_branch: { type: 'string' },
    shared_files_note: { type: 'string', description: 'which lane (or the gate stage) owns CMakeLists/umbrella header/CHANGELOG' },
  },
}
const REPORT = {
  type: 'object', required: ['lane_id', 'outcome', 'branch', 'sha', 'worktree', 'files_changed', 'evidence', 'deviations', 'ledger_candidates'],
  properties: {
    lane_id: { type: 'string' }, outcome: { type: 'string', enum: ['DONE', 'BLOCKED'] },
    branch: { type: 'string' }, sha: { type: 'string' },
    worktree: { type: 'string', description: 'leased pool tree path, or empty if lease failed' },
    files_changed: { type: 'array', items: { type: 'string' } },
    evidence: { type: 'string', description: 'commands run with pasted pass/fail output' },
    deviations: { type: 'string' },
    ledger_candidates: { type: 'array', items: { type: 'string' } },
  },
}
const REVIEW = {
  type: 'object', required: ['verdict', 'findings'],
  properties: {
    verdict: { type: 'string', enum: ['APPROVE', 'BLOCK'] },
    findings: { type: 'array', items: {
      type: 'object', required: ['location', 'severity', 'problem', 'fix'],
      properties: {
        location: { type: 'string' }, severity: { type: 'string', enum: ['blocker', 'major', 'minor'] },
        problem: { type: 'string' }, fix: { type: 'string' },
      } } },
  },
}
const GATE = {
  type: 'object', required: ['passed', 'integration_branch', 'sha', 'gate_results', 'leases_released', 'ledger_appended'],
  properties: {
    passed: { type: 'boolean' }, integration_branch: { type: 'string' }, sha: { type: 'string' },
    gate_results: { type: 'string', description: 'per-gate PASS/FAIL/SKIPPED with evidence excerpts' },
    leases_released: { type: 'array', items: { type: 'string' } },
    ledger_appended: { type: 'array', items: { type: 'string' } },
  },
}

phase('Plan')
const plan = await agent(
  `Task: ${args.task}\nBase ref: ${BASE}\n\nDecompose per your process into 1-4 file-disjoint lanes. Lane branches: lane/<id> off ${BASE}. Remember: 2+ lanes ONLY if truly compile-independent; assign shared files (CMakeLists.txt, umbrella header, CHANGELOG.md) to at most one lane or reserve for the gate stage.`,
  { agentType: 'vol-planner', schema: PLAN, label: 'plan' },
)
if (!plan || !plan.lanes || !plan.lanes.length) throw new Error('planner returned no lanes')
const lanes = plan.lanes.slice(0, 4) // pool has 4 warm trees; one lease per lane
if (plan.lanes.length > 4) log(`plan had ${plan.lanes.length} lanes; capped at 4 (pool size) — rerun for the remainder`)
log(`${lanes.length} lane(s): ${lanes.map(l => l.id).join(', ')} → integration ${plan.integration_branch}`)

const results = await pipeline(
  lanes,
  // Build
  lane => agent(
    `Lane brief (JSON):\n${JSON.stringify(lane, null, 2)}\n\nBase ref: ${BASE}. Execute this lane per your agent instructions: lease a pool tree for branch ${lane.branch}, TDD up the ladder, commit everything, keep the lease held, report.`,
    { agentType: 'vol-builder', schema: REPORT, phase: 'Build', label: `build:${lane.id}` },
  ),
  // Review (skip if lane blocked)
  (report, lane) => {
    if (!report || report.outcome !== 'DONE') return { report, review: null }
    return agent(
      `Review lane ${lane.id}.\nBrief (JSON):\n${JSON.stringify(lane, null, 2)}\n\nBuilder evidence (verify, don't trust):\n${report.evidence}\n\nDiff: git diff ${BASE}...${report.branch} (three dots). Apply your process.`,
      { agentType: 'vol-reviewer', schema: REVIEW, phase: 'Review', label: `review:${lane.id}` },
    ).then(review => ({ report, review }))
  },
  // Fix (one round, only on BLOCK)
  (state, lane) => {
    if (!state || !state.report) return state
    if (!state.review || state.review.verdict === 'APPROVE') return state
    const blockers = state.review.findings.filter(f => f.severity !== 'minor')
    return agent(
      `Fix round for lane ${lane.id} on branch ${state.report.branch} in your already-leased tree ${state.report.worktree}.\nBrief (JSON):\n${JSON.stringify(lane, null, 2)}\n\nAddress EXACTLY these findings, nothing else:\n${JSON.stringify(blockers, null, 2)}\n\nRe-run the lane's suites, commit, keep the lease held, report.`,
      { agentType: 'vol-builder', schema: REPORT, phase: 'Fix', label: `fix:${lane.id}` },
    ).then(fixed => ({ report: fixed || state.report, review: state.review, fixRound: true }))
  },
)

phase('Gate')
const done = results.filter(Boolean).filter(s => s.report && s.report.outcome === 'DONE')
const blocked = lanes.filter((l, i) => !results[i] || !results[i].report || results[i].report.outcome !== 'DONE')
if (blocked.length) log(`blocked lanes (excluded from integration): ${blocked.map(l => l.id).join(', ')}`)
if (!done.length) throw new Error('no lane completed; nothing to gate')

const gate = await agent(
  `Gate the vol-sprint run.\nBase: ${BASE}. Integration branch: ${plan.integration_branch}. Shared-files ownership: ${plan.shared_files_note}\n\nLane states (JSON):\n${JSON.stringify(done.map(s => ({ report: s.report, verdict: s.review ? s.review.verdict : 'UNREVIEWED', findings: s.review ? s.review.findings : [] })), null, 2)}\n\nExecute your gate process: merge lane branches, run the gates with evidence, release ALL pool leases (check -Status), append the ledger. Blocked lanes not listed here: ${blocked.map(l => l.id).join(', ') || 'none'}.`,
  { agentType: 'vol-verifier', schema: GATE, label: 'gate' },
)

return {
  passed: gate ? gate.passed : false,
  integration: gate ? `${gate.integration_branch} @ ${gate.sha}` : 'gate agent died',
  gate_results: gate ? gate.gate_results : null,
  lanes: results.filter(Boolean).map(s => s.report && { lane: s.report.lane_id, outcome: s.report.outcome, branch: `${s.report.branch}@${s.report.sha}`, verdict: s.review ? s.review.verdict : null }).filter(Boolean),
  blocked: blocked.map(l => l.id),
  ledger: gate ? gate.ledger_appended : [],
}
