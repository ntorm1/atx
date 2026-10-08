import { useCallback, useEffect, useRef, useState } from "react";
import { request } from "./api";
import { Empty, number, percent, useChartDimensions } from "./charts";
import type { Job, Point } from "./types";
type ComparedRun = {
  job_id: string;
  label: string;
  synthetic: boolean;
  evaluation: string;
  dataset_sha256: string;
  available_observations: number;
  excluded_observations: number;
  metrics: Record<string, number | null>;
  curve: Point[];
  yearly_returns: { year: string; value: number | null }[];
};
type Comparison = {
  schema_version: string;
  alignment: string;
  observations: number;
  start_date: string;
  end_date: string;
  mixed_evidence: boolean;
  has_gaps: boolean;
  chart_downsampled: boolean;
  correlation: (number | null)[][];
  runs: ComparedRun[];
  warnings: string[];
};
const colors = [
  "#285f89",
  "#6b7947",
  "#9a5345",
  "#786389",
  "#91703b",
  "#3d7f7b",
  "#41506b",
  "#9b6571",
  "#718462",
  "#7c694d",
  "#526968",
  "#635e76",
];
function ComparedCurves({ result }: { result: Comparison }) {
  const { ref, width, height } = useChartDimensions(700, true);
  const values = result.runs
    .flatMap((run) => run.curve.map((point) => point.value))
    .filter(Number.isFinite);
  const low = Math.min(...values),
    high = Math.max(...values),
    range = high - low || 1;
  const left = 52,
    right = Math.max(left + 1, width - 12),
    top = 14,
    bottom = Math.max(top + 1, height - 26);
  if (!values.length)
    return (
      <Empty>The backend supplied no finite comparison curve values.</Empty>
    );
  return (
    <div className="comparison-chart">
      <div className="comparison-legend">
        {result.runs.map((run, i) => (
          <span key={run.job_id} style={{ color: colors[i] }}>
            {run.label || run.job_id.slice(0, 8)}
          </span>
        ))}
      </div>
      <svg
        ref={ref}
        role="img"
        aria-label="Aligned comparison curves rebased to100"
        viewBox={`0 0 ${width} ${height}`}
        preserveAspectRatio="none"
      >
        {[0, 1, 2, 3].map((i) => (
          <g key={i}>
            <line
              className="grid-line"
              x1={left}
              x2={right}
              y1={top + (i * (bottom - top)) / 3}
              y2={top + (i * (bottom - top)) / 3}
            />
            <text
              x={left - 5}
              y={top + 4 + (i * (bottom - top)) / 3}
              textAnchor="end"
            >
              {number(high - (i * range) / 3, 1)}
            </text>
          </g>
        ))}
        {result.runs.map((run, i) => (
          <path
            key={run.job_id}
            d={(() => {
              let connected = false;
              return run.curve
                .map((point, j) => {
                  if (
                    typeof point.value !== "number" ||
                    !Number.isFinite(point.value)
                  ) {
                    connected = false;
                    return "";
                  }
                  const result = `${connected ? "L" : "M"}${left + (j / Math.max(1, run.curve.length - 1)) * (right - left)},${top + ((high - point.value) / range) * (bottom - top)}`;
                  connected = true;
                  return result;
                })
                .join(" ");
            })()}
            stroke={colors[i]}
            fill="none"
            strokeWidth="1.5"
            vectorEffect="non-scaling-stroke"
          />
        ))}
        <text x={left} y={height - 7}>
          {result.start_date}
        </text>
        <text x={right} y={height - 7} textAnchor="end">
          {result.end_date}
        </text>
      </svg>
      <div className="chart-caption">
        100 at common start · matched return periods only
        {result.chart_downsampled ? " · chart downsampled" : ""}
      </div>
    </div>
  );
}
export function ComparisonPanel({
  token,
  selected,
}: {
  token: string;
  selected: Job | null;
}) {
  const [jobs, setJobs] = useState<Job[]>([]);
  const [total, setTotal] = useState(0);
  const [offset, setOffset] = useState(0);
  const [ids, setIds] = useState<string[]>([]);
  const [start, setStart] = useState("");
  const [end, setEnd] = useState("");
  const [mixed, setMixed] = useState(false);
  const [result, setResult] = useState<Comparison | null>(null);
  const [error, setError] = useState("");
  const [listError, setListError] = useState("");
  const [busy, setBusy] = useState(false);
  const active = useRef<AbortController | null>(null),
    listActive = useRef<AbortController | null>(null);
  const generation = useRef(0);
  const refresh = useCallback(async () => {
    listActive.current?.abort();
    const controller = new AbortController();
    listActive.current = controller;
    try {
      const list = await request<{ jobs: Job[]; total: string }>(
        `/jobs?status=succeeded&limit=50&offset=${offset}`,
        token,
        { signal: controller.signal },
      );
      if (!controller.signal.aborted) {
        setJobs(list.jobs);
        setTotal(Number(list.total));
        setListError("");
      }
    } catch (failure) {
      if (!controller.signal.aborted) setListError((failure as Error).message);
    }
  }, [token, offset]);
  useEffect(() => {
    void refresh();
    return () => listActive.current?.abort();
  }, [refresh, selected?.id, selected?.status]);
  useEffect(() => {
    const timer = setInterval(() => void refresh(), 30000);
    return () => clearInterval(timer);
  }, [refresh]);
  useEffect(() => {
    generation.current++;
    active.current?.abort();
    setResult(null);
    setError("");
    setBusy(false);
    return () => active.current?.abort();
  }, [ids, start, end, mixed, token]);
  const toggle = (id: string) =>
    setIds((previous) =>
      previous.includes(id)
        ? previous.filter((value) => value !== id)
        : previous.length < 12
          ? [...previous, id]
          : previous,
    );
  const compare = async () => {
    active.current?.abort();
    const controller = new AbortController();
    active.current = controller;
    const version = ++generation.current;
    setBusy(true);
    setResult(null);
    setError("");
    try {
      const comparison = await request<Comparison>("/analysis/compare", token, {
        method: "POST",
        signal: controller.signal,
        body: JSON.stringify({
          job_ids: ids,
          start_date: start,
          end_date: end,
          max_chart_points: 1000,
          allow_mixed_evidence: mixed,
        }),
      });
      if (version === generation.current && !controller.signal.aborted)
        setResult(comparison);
    } catch (failure) {
      if (version === generation.current && !controller.signal.aborted)
        setError((failure as Error).message);
    } finally {
      if (version === generation.current) setBusy(false);
    }
  };
  const years = [
    ...new Set(
      result?.runs.flatMap((run) =>
        run.yearly_returns.map((row) => row.year),
      ) ?? [],
    ),
  ].sort();
  return (
    <div className="comparison-panel research-tool-panel">
      <div className="panel-toolbar">
        <span>{ids.length}/12 runs</span>
        <button
          disabled={
            !selected ||
            selected.status !== "succeeded" ||
            ids.includes(selected.id) ||
            ids.length >= 12
          }
          onClick={() => selected && toggle(selected.id)}
        >
          Add selected run
        </button>
        <button onClick={() => setIds([])}>Clear comparison</button>
        <label>
          From
          <input
            aria-label="Comparison start date"
            type="date"
            value={start}
            onChange={(e) => setStart(e.target.value)}
          />
        </label>
        <label>
          To
          <input
            aria-label="Comparison end date"
            type="date"
            value={end}
            onChange={(e) => setEnd(e.target.value)}
          />
        </label>
        <label>
          <input
            type="checkbox"
            aria-label="Allow mixed evidence"
            checked={mixed}
            onChange={(e) => setMixed(e.target.checked)}
          />{" "}
          Allow mixed evidence
        </label>
        <button
          disabled={
            busy || ids.length < 2 || Boolean(start && end && start > end)
          }
          onClick={() => void compare()}
        >
          {busy ? "Comparing…" : "Compare runs"}
        </button>
      </div>
      <div className="comparison-body">
        <div className="comparison-picker">
          <div className="panel-toolbar">
            <button onClick={() => void refresh()}>
              Refresh comparable runs
            </button>
            <span>{total} successful</span>
          </div>
          <div className="table-scroll">
            <table aria-label="Comparison run selection">
              <thead>
                <tr>
                  <th />
                  <th>Run / library revision</th>
                  <th>Created</th>
                </tr>
              </thead>
              <tbody>
                {jobs.map((job) => (
                  <tr key={job.id}>
                    <td>
                      <input
                        type="checkbox"
                        aria-label={`Compare run ${job.id}`}
                        checked={ids.includes(job.id)}
                        disabled={!ids.includes(job.id) && ids.length >= 12}
                        onChange={() => toggle(job.id)}
                      />
                    </td>
                    <td title={job.id}>
                      {job.label || job.id.slice(0, 10)}
                      {job.alpha_id && (
                        <small>
                          Alpha {job.alpha_id.slice(0, 8)} r{job.alpha_revision}
                        </small>
                      )}
                    </td>
                    <td>
                      {new Date(Number(job.created_at_ms)).toLocaleDateString()}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <div className="panel-toolbar">
            <button
              disabled={!offset}
              onClick={() => setOffset(Math.max(0, offset - 50))}
            >
              Previous runs
            </button>
            <span>
              {Math.min(offset + 1, total)}–{Math.min(offset + 50, total)}/
              {total}
            </span>
            <button
              disabled={offset + 50 >= total}
              onClick={() => setOffset(offset + 50)}
            >
              Next runs
            </button>
          </div>
        </div>
        <div className="comparison-results">
          {result ? (
            <>
              <div className="panel-toolbar">
                {result.observations} matched periods · {result.start_date} →{" "}
                {result.end_date}
                {result.mixed_evidence && " · MIXED EVIDENCE"}
                {result.has_gaps && " · Period gaps"}
              </div>
              <ComparedCurves result={result} />
              <div className="table-scroll comparison-metrics">
                <table aria-label="Aligned run metrics">
                  <thead>
                    <tr>
                      <th>Run / evidence</th>
                      <th>Return</th>
                      <th>Volatility</th>
                      <th>Sharpe</th>
                      <th>Drawdown</th>
                      <th>Excluded</th>
                    </tr>
                  </thead>
                  <tbody>
                    {result.runs.map((run) => (
                      <tr key={run.job_id}>
                        <td
                          title={`Dataset ${run.dataset_sha256 || "unspecified"}`}
                        >
                          {run.label || run.job_id.slice(0, 8)}
                          <small>
                            {run.synthetic ? "SYNTHETIC DEMO" : run.evaluation}
                          </small>
                        </td>
                        <td>{percent(run.metrics.total_return)}</td>
                        <td>{percent(run.metrics.annualized_volatility)}</td>
                        <td>{number(run.metrics.sharpe_ratio)}</td>
                        <td>{percent(run.metrics.max_drawdown)}</td>
                        <td>
                          {run.excluded_observations}/
                          {run.available_observations}
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
              <div className="comparison-matrices">
                <table aria-label="Return correlation matrix">
                  <thead>
                    <tr>
                      <th>Correlation</th>
                      {result.runs.map((run, i) => (
                        <th key={run.job_id} title={run.label || run.job_id}>
                          {i + 1}
                        </th>
                      ))}
                    </tr>
                  </thead>
                  <tbody>
                    {result.runs.map((run, i) => (
                      <tr key={run.job_id}>
                        <th title={run.label || run.job_id}>{i + 1}</th>
                        {result.correlation[i].map((value, j) => (
                          <td
                            key={j}
                            title={`${run.label || run.job_id} / ${result.runs[j].label || result.runs[j].job_id}`}
                          >
                            {number(value)}
                          </td>
                        ))}
                      </tr>
                    ))}
                  </tbody>
                </table>
                <table aria-label="Matched yearly returns">
                  <thead>
                    <tr>
                      <th>Year</th>
                      {result.runs.map((run, i) => (
                        <th key={run.job_id} title={run.label || run.job_id}>
                          {i + 1}
                        </th>
                      ))}
                    </tr>
                  </thead>
                  <tbody>
                    {years.map((year) => (
                      <tr key={year}>
                        <th>{year}</th>
                        {result.runs.map((run) => (
                          <td key={run.job_id}>
                            {percent(
                              run.yearly_returns.find(
                                (row) => row.year === year,
                              )?.value,
                            )}
                          </td>
                        ))}
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
              <div className="help">
                {result.warnings.map((warning) => (
                  <div key={warning}>{warning}</div>
                ))}
              </div>
            </>
          ) : (
            <div className="help">
              Select2–12 successful runs. Comparison uses the intersection of
              exact return periods and backend metrics. Legacy results without
              complete daily returns must be rerun.
            </div>
          )}
        </div>
      </div>
      {(error || listError) && (
        <div className="error" role="alert">
          {error || listError}
        </div>
      )}
    </div>
  );
}
