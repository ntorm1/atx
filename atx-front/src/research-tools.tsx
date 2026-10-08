import { useEffect, useRef, useState } from "react";
import { request } from "./api";
import type { Dataset, OperatorCatalog, ResearchRequest } from "./types";

export function realDatasetRequest(
  strategy_id: string,
  dataset?: Dataset,
): ResearchRequest {
  return {
    mode: "real_dsl",
    strategy_id,
    dataset_id: dataset?.id ?? "",
    dataset_sha256: dataset?.manifest_sha256 ?? "",
    strategy_dsl: "-rank(delta(close,5)/delay(close,5))",
    start_date: "2020-01-01",
    end_date: "2022-12-31",
    instrument_limit: 256,
    initial_capital: 1000000,
    trade_bps: 6,
    annual_borrow_bps: 300,
    cadence: 1,
    execution_delay_sessions: 1,
    neutralization: "dollar",
  };
}
export function RealDatasetFields({
  draft,
  setDraft,
  datasets,
}: {
  draft: ResearchRequest;
  setDraft: (draft: ResearchRequest) => void;
  datasets: Dataset[];
}) {
  const dataset = datasets.find((d) => d.id === draft.dataset_id);
  const update = (key: keyof ResearchRequest, value: unknown) =>
    setDraft({ ...draft, [key]: value });
  return (
    <>
      <div className="notice">
        Observed TRAIN market data · research unverified. Historical vintage,
        common-stock history, capacity and borrow are not qualified.
      </div>
      {draft.mode !== "real_portfolio" && (
        <label>
          Alpha expression
          <input
            required
            maxLength={1024}
            value={draft.strategy_dsl ?? ""}
            onChange={(e) => update("strategy_dsl", e.target.value)}
          />
        </label>
      )}
      <label>
        Dataset
        <select
          aria-label="Research dataset"
          required
          value={draft.dataset_id ?? ""}
          onChange={(e) => {
            const d = datasets.find((d) => d.id === e.target.value);
            setDraft({
              ...draft,
              dataset_id: d?.id ?? "",
              dataset_sha256: d?.manifest_sha256 ?? "",
            });
          }}
        >
          {!dataset && (
            <option value={draft.dataset_id ?? ""}>
              {draft.dataset_id
                ? `${draft.dataset_id} (unavailable)`
                : "Select configured dataset"}
            </option>
          )}
          {datasets.map((d) => (
            <option key={d.id} value={d.id}>
              {d.name} / {d.evaluation}
            </option>
          ))}
        </select>
      </label>
      <label>
        Manifest SHA-256
        <input
          aria-label="Dataset manifest SHA-256"
          readOnly
          required
          pattern="[0-9a-f]{64}"
          value={draft.dataset_sha256 ?? ""}
          title={draft.dataset_sha256}
        />
      </label>
      {dataset && dataset.manifest_sha256 !== draft.dataset_sha256 && (
        <div className="error" role="alert">
          The saved dataset pin differs from the registered revision. Reselect
          the dataset explicitly to use its current pin.
          <button
            type="button"
            onClick={() =>
              setDraft({ ...draft, dataset_sha256: dataset.manifest_sha256 })
            }
          >
            Use registered revision
          </button>
        </div>
      )}
      {(["start_date", "end_date"] as const).map((key) => (
        <label key={key}>
          {key === "start_date" ? "Start date" : "End date"}
          <input
            type="date"
            required
            min={dataset?.start_date ?? "2018-01-01"}
            max={dataset?.end_date ?? "2022-12-31"}
            value={draft[key] ?? ""}
            onChange={(e) => update(key, e.target.value)}
          />
        </label>
      ))}
      <label>
        Instruments
        <input
          type="number"
          required
          min="2"
          max={Math.min(dataset?.max_instruments ?? 512, 512)}
          value={draft.instrument_limit ?? 256}
          onChange={(e) => update("instrument_limit", e.target.valueAsNumber)}
        />
      </label>
      <label>
        Initial capital
        <input
          type="number"
          required
          min="1"
          max="1000000000000"
          step="any"
          value={draft.initial_capital ?? 1000000}
          onChange={(e) => update("initial_capital", e.target.valueAsNumber)}
        />
      </label>
      <label>
        Trading cost (bps)
        <input
          type="number"
          required
          min="0"
          max="100"
          step=".1"
          value={draft.trade_bps ?? 6}
          onChange={(e) => update("trade_bps", e.target.valueAsNumber)}
        />
      </label>
      <label>
        Annual borrow (bps)
        <input
          type="number"
          required
          min="0"
          max="5000"
          value={draft.annual_borrow_bps ?? 300}
          onChange={(e) => update("annual_borrow_bps", e.target.valueAsNumber)}
        />
      </label>
      <label>
        Cadence (sessions)
        <input
          type="number"
          required
          min="1"
          max="252"
          value={draft.cadence ?? 1}
          onChange={(e) => update("cadence", e.target.valueAsNumber)}
        />
      </label>
      <label>
        Execution delay (sessions)
        <input
          type="number"
          required
          min="1"
          max="5"
          value={draft.execution_delay_sessions ?? 1}
          onChange={(e) =>
            update("execution_delay_sessions", e.target.valueAsNumber)
          }
        />
      </label>
      <label>
        Neutralization
        <select value="dollar" aria-label="Neutralization" disabled>
          <option value="dollar">Dollar neutral</option>
        </select>
      </label>
      <div className="help" title={JSON.stringify(dataset?.field_contract)}>
        Fields: {dataset?.fields.join(", ") ?? "No dataset registered"}. Signals
        execute with the configured session delay; TRAIN only.
      </div>
    </>
  );
}
type Analysis = {
  valid: boolean;
  required_lookback: number;
  fields: string[];
  unique_nodes: number;
  peak_live_slots: number;
  instruction_count: number;
  expression_sha256: string;
  scope: string;
};
export function ExpressionEditor({
  draft,
  setDraft,
  token,
  catalog,
}: {
  draft: ResearchRequest;
  setDraft: (draft: ResearchRequest) => void;
  token: string;
  catalog: OperatorCatalog | null;
}) {
  const [analysis, setAnalysis] = useState<Analysis | null>(null);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const active = useRef<AbortController | null>(null);
  const generation = useRef(0);
  const supported = ["real_dsl", "synthetic_demo"].includes(draft.mode);
  useEffect(() => {
    generation.current++;
    active.current?.abort();
    setAnalysis(null);
    setError("");
    setBusy(false);
    return () => active.current?.abort();
  }, [
    draft.strategy_dsl,
    draft.dataset_id,
    draft.dataset_sha256,
    draft.mode,
    token,
  ]);
  const validate = async () => {
    active.current?.abort();
    const controller = new AbortController();
    active.current = controller;
    const version = ++generation.current;
    setBusy(true);
    setAnalysis(null);
    setError("");
    try {
      const result = await request<Analysis>("/analysis/validate", token, {
        method: "POST",
        signal: controller.signal,
        body: JSON.stringify({
          strategy_dsl: draft.strategy_dsl ?? "",
          dataset_id: draft.mode === "real_dsl" ? (draft.dataset_id ?? "") : "",
          dataset_sha256:
            draft.mode === "real_dsl" ? (draft.dataset_sha256 ?? "") : "",
        }),
      });
      if (version === generation.current && !controller.signal.aborted)
        setAnalysis(result);
    } catch (failure) {
      if (version === generation.current && !controller.signal.aborted)
        setError((failure as Error).message);
    } finally {
      if (version === generation.current) setBusy(false);
    }
  };
  if (!supported) return null;
  const bytes = new TextEncoder().encode(draft.strategy_dsl ?? "").length;
  const limit = Math.min(catalog?.limits.max_bytes ?? 1024, 1024);
  return (
    <section className="expression-editor" aria-label="DSL validation editor">
      <div className="panel-toolbar">
        <span>
          alpha.dsl ·{" "}
          {draft.mode === "real_dsl" ? "Observed TRAIN" : "SYNTHETIC DEMO"}
        </span>
        <span className="spacer" />
        <button
          disabled={
            busy ||
            !bytes ||
            bytes > limit ||
            (draft.mode === "real_dsl" &&
              (!draft.dataset_id || !draft.dataset_sha256))
          }
          onClick={() => void validate()}
        >
          {busy ? "Validating…" : "Validate expression"}
        </button>
        <span>
          {bytes}/{limit} bytes
        </span>
      </div>
      <textarea
        aria-label="Alpha DSL editor"
        spellCheck={false}
        value={draft.strategy_dsl ?? ""}
        onChange={(e) => setDraft({ ...draft, strategy_dsl: e.target.value })}
        onKeyDown={(e) => {
          if (e.ctrlKey && e.key === "Enter") {
            e.preventDefault();
            void validate();
          }
        }}
      />
      {bytes > limit && (
        <div className="error" role="alert">
          Expression exceeds the native {limit}-byte limit.
        </div>
      )}
      {error && (
        <div className="error" role="alert">
          {error}
        </div>
      )}
      {analysis && (
        <div
          className="expression-validation"
          role="status"
          title={`${analysis.scope}\nSHA-256 ${analysis.expression_sha256}`}
        >
          Valid · lookback {analysis.required_lookback} ·{" "}
          {analysis.unique_nodes} nodes / {analysis.peak_live_slots} live slots
          · fields {analysis.fields.join(", ")}. Syntax and causality only.
        </div>
      )}
    </section>
  );
}
export function OperatorsPanel({
  catalog,
  draft,
  setDraft,
}: {
  catalog: OperatorCatalog | null;
  draft: ResearchRequest;
  setDraft: (draft: ResearchRequest) => void;
}) {
  const [filter, setFilter] = useState("");
  const [category, setCategory] = useState("");
  const operators = catalog?.operators ?? [];
  const visible = operators.filter(
    (op) =>
      (!category || op.category === category) &&
      `${op.name} ${op.description} ${op.example}`
        .toLowerCase()
        .includes(filter.toLowerCase()),
  );
  return (
    <div className="research-tool-panel">
      <div className="panel-toolbar">
        <input
          aria-label="Filter alpha operators"
          placeholder="Filter operators…"
          value={filter}
          onChange={(e) => setFilter(e.target.value)}
        />
        <select
          aria-label="Operator category"
          value={category}
          onChange={(e) => setCategory(e.target.value)}
        >
          <option value="">All categories</option>
          {[...new Set(operators.map((op) => op.category))].map((c) => (
            <option key={c}>{c}</option>
          ))}
        </select>
        <span>
          {visible.length}/{operators.length}
        </span>
      </div>
      <div className="table-scroll">
        <table aria-label="Alpha operators">
          <thead>
            <tr>
              <th>Operator</th>
              <th>Category / policy</th>
              <th>Example / description</th>
              <th />
            </tr>
          </thead>
          <tbody>
            {visible.map((op) => (
              <tr key={op.name}>
                <td>{op.name}</td>
                <td>
                  {op.category}
                  <br />
                  {op.window_argument ? "Literal window ≤252" : "Causal"}
                </td>
                <td>
                  <code>{op.example}</code>
                  <br />
                  {op.description}
                </td>
                <td>
                  <button
                    disabled={
                      !["real_dsl", "synthetic_demo"].includes(draft.mode)
                    }
                    onClick={() =>
                      setDraft({ ...draft, strategy_dsl: op.example })
                    }
                  >
                    Use example
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        {!catalog && (
          <div className="help">
            The backend operator catalog is unavailable.
          </div>
        )}
      </div>
      <div className="help">
        {catalog?.arithmetic} · {catalog?.missing_values}
      </div>
    </div>
  );
}
