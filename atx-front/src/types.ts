export type ResearchRequest = {
  mode:
    | "synthetic_demo"
    | "import_nav"
    | "real_nav"
    | "real_dsl"
    | "real_portfolio";
  strategy_id: string;
  strategy_dsl?: string;
  sessions?: number;
  initial_capital?: number;
  trade_bps?: number;
  annual_borrow_bps?: number;
  artifact_path?: string;
  start_date?: string;
  end_date?: string;
  summary_path?: string;
  combined_path?: string;
  combined_sha256?: string;
  role_path?: string;
  role_sha256?: string;
  rule?: string;
  cadence?: number;
  trade_fraction?: number;
  monthly_budget?: number;
  dataset_id?: string;
  dataset_sha256?: string;
  instrument_limit?: number;
  execution_delay_sessions?: number;
  neutralization?: "dollar";
  components?: {
    strategy_dsl: string;
    expression_sha256: string;
    weight: number;
  }[];
  gross_limit?: number;
  name_cap?: number;
  max_turnover?: number;
  signal_decay_sessions?: number;
};
export type Dataset = {
  id: string;
  name: string;
  manifest_sha256: string;
  evaluation: string;
  start_date: string;
  end_date: string;
  fields: string[];
  max_instruments: number;
  historical_vintage_verified: boolean;
  common_stock_history_verified: boolean;
  field_contract: Record<string, string>;
};
export type ResearchOperator = {
  name: string;
  category: string;
  example: string;
  description: string;
  window_argument: boolean;
  causal: boolean;
};
export type OperatorCatalog = {
  operators: ResearchOperator[];
  limits: Record<string, number>;
  arithmetic: string;
  missing_values: string;
};
export type Strategy = {
  id: string;
  name: string;
  description: string;
  mode: ResearchRequest["mode"];
  synthetic: boolean;
  enabled?: boolean;
  parameters?: Record<string, unknown>;
};
export type Point = { date: string; value: number };
export type Result = {
  schema_version: string | number;
  strategy_id: string;
  metrics: Record<string, number | null>;
  equity_curve: Point[];
  drawdown: Point[];
  returns_histogram: { lower: number; upper: number; count: number }[];
  monthly_returns: { month: string; value: number }[];
  trades: Record<string, unknown>[];
  positions: Record<string, unknown>[];
  factor_exposures: Record<string, unknown>[];
  portfolio_allocations?: Record<string, unknown>[];
  provenance: Record<string, unknown> & {
    synthetic?: boolean;
    warnings?: string[];
  };
};
export type Job = {
  id: string;
  status: string;
  request: ResearchRequest | null;
  result: Result | null;
  progress: number;
  error: string;
  label: string;
  created_at_ms: string;
  scheduled_at_ms: string;
  started_at_ms: string;
  finished_at_ms: string;
  alpha_id?: string;
  alpha_revision?: string;
  experiment_id?: string;
  actor_id?: string;
  workflow_id?: string;
  workflow_slot_ms?: string;
};
export type Health = Record<string, string | number>;
export type WorkspaceEvent = {
  sequence: string;
  channel: string;
  type: string;
  job_id: string;
  created_at_ms: string;
  payload: unknown;
  gap: boolean;
};
