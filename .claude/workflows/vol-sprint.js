export const meta = {
  name: 'vol-sprint',
  description: 'Fail-closed oracle sprint while the complete ready-state transaction is migrated to the v3 lane broker.',
  whenToUse: 'Do not dispatch until the broker-only ready migration lands. Args remain { task: string, base?: string, run_key?: string }.',
  phases: [
    { title: 'Blocked', detail: 'no planner, mutation agent, lease, build, test, or integration dispatch is reachable' },
  ],
}

if (!args || !args.task) throw new Error('vol-sprint needs args: { task: "<what to build>", base?: "<ref>", run_key?: "<caller identity>" }')

return {
  passed: false,
  blocked: ['ORACLE_BROKER_MIGRATION_REQUIRED'],
  failure: 'ORACLE_BROKER_MIGRATION_REQUIRED',
  integration_branch: null,
  integration_sha: null,
  gate_evidence: [],
}
