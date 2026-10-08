import { useEffect, useRef, useState } from "react";
import { request } from "./api";
import type { AlphaSnapshot, AlphaSummary } from "./alpha-library";
import type { Dataset, Job, ResearchRequest } from "./types";
import { realDatasetRequest } from "./research-tools";
import { percent, number } from "./charts";
export function portfolioRequest(
  strategy_id: string,
  dataset?: Dataset,
): ResearchRequest {
  const common = realDatasetRequest(strategy_id, dataset);
  delete common.strategy_dsl;
  return {
    ...common,
    mode: "real_portfolio",
    end_date: "2020-03-31",
    instrument_limit: 128,
    cadence: 5,
    gross_limit: 1,
    name_cap: 0.05,
    max_turnover: 1,
    signal_decay_sessions: 1,
    components: [
      {
        strategy_dsl: "-rank(delta(close,5)/delay(close,5))",
        expression_sha256:
          "39b682368b0df08fbd44d17d036269306a1838d3158cfe3610c0751e8381723c",
        weight: 0.5,
      },
      {
        strategy_dsl: "-rank(delta(close,10)/delay(close,10))",
        expression_sha256:
          "2ea579ea75900aa3471de6fc32f7ec66ec91a8bfa53364c719540356d2ee687f",
        weight: 0.5,
      },
    ],
  };
}
const hash = async (text: string) =>
  [
    ...new Uint8Array(
      await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text)),
    ),
  ]
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");
export function PortfolioPanel({
  token,
  draft,
  setDraft,
  datasets,
  selected,
}: {
  token: string;
  draft: ResearchRequest;
  setDraft: (draft: ResearchRequest) => void;
  datasets: Dataset[];
  selected: Job | null;
}) {
  const [alphas, setAlphas] = useState<AlphaSummary[]>([]);
  const [alphaId, setAlphaId] = useState(""),
    [revision, setRevision] = useState("");
  const [error, setError] = useState(""),
    [busy, setBusy] = useState(false),
    [notice, setNotice] = useState("");
  const active = useRef<AbortController | null>(null),
    generation = useRef(0),
    currentDraft = useRef(draft);
  currentDraft.current = draft;
  useEffect(() => {
    const controller = new AbortController();
    setBusy(false);
    request<{ alphas: AlphaSummary[] }>("/alphas?limit=100", token, {
      signal: controller.signal,
    })
      .then((page) => {
        if (controller.signal.aborted) return;
        setAlphas(page.alphas);
        setAlphaId(page.alphas[0]?.id ?? "");
        setRevision(page.alphas[0]?.revision ?? "");
      })
      .catch((failure: Error) => {
        if (!controller.signal.aborted) setError(failure.message);
      });
    return () => {
      controller.abort();
      active.current?.abort();
      generation.current++;
    };
  }, [token]);
  const apply = (key: keyof ResearchRequest, value: unknown) => {
    setDraft({ ...draft, [key]: value });
    setNotice("");
  };
  const updateComponent = (
    index: number,
    field: "strategy_dsl" | "weight",
    value: string | number,
  ) => {
    const components = (draft.components ?? []).map((component, position) =>
      position === index
        ? {
            ...component,
            [field]: value,
            ...(field === "strategy_dsl" ? { expression_sha256: "" } : {}),
          }
        : component,
    );
    apply("components", components);
  };
  const operation = async (saved: boolean) => {
    if (draft.mode !== "real_portfolio") return;
    const snapshot = structuredClone(draft),
      signature = JSON.stringify(snapshot);
    active.current?.abort();
    const controller = new AbortController();
    active.current = controller;
    const version = ++generation.current;
    setBusy(true);
    setError("");
    setNotice("");
    try {
      let components = snapshot.components ?? [];
      if (saved) {
        if (components.length >= 12)
          throw new Error("A portfolio supports at most 12 components.");
        if (!/^[1-9][0-9]{0,18}$/.test(revision))
          throw new Error("Enter an exact positive saved revision.");
        const alpha = await request<AlphaSnapshot>(
          `/alphas/${alphaId}?revision=${revision}`,
          token,
          { signal: controller.signal },
        );
        if (alpha.request.mode !== "real_dsl")
          throw new Error(
            "A component must be a saved observed-data DSL recipe.",
          );
        if (
          alpha.request.dataset_id !== snapshot.dataset_id ||
          alpha.request.dataset_sha256 !== snapshot.dataset_sha256
        )
          throw new Error(
            "The saved expression uses a different dataset or manifest pin. Select a matching recipe.",
          );
        const expression = alpha.request.strategy_dsl ?? "";
        components = [
          ...components,
          {
            strategy_dsl: expression,
            expression_sha256: await hash(expression),
            weight: 1,
          },
        ];
      } else {
        if (components.length < 2 || components.length > 12)
          throw new Error("Choose 2–12 embedded expression components.");
        if (
          components.some(
            (component) =>
              !Number.isFinite(component.weight) ||
              Math.abs(component.weight) > 1e6,
          ) ||
          components.reduce(
            (sum, component) => sum + Math.abs(component.weight),
            0,
          ) <= 1e-12
        )
          throw new Error(
            "Use finite signed weights with nonzero total absolute weight.",
          );
        components = await Promise.all(
          components.map(async (component) => {
            const compiled = await request<{
              valid: boolean;
              expression_sha256: string;
            }>("/analysis/validate", token, {
              method: "POST",
              signal: controller.signal,
              body: JSON.stringify({
                strategy_dsl: component.strategy_dsl,
                dataset_id: snapshot.dataset_id,
                dataset_sha256: snapshot.dataset_sha256,
              }),
            });
            const exact = await hash(component.strategy_dsl);
            if (!compiled.valid || compiled.expression_sha256 !== exact)
              throw new Error(
                "Native expression validation did not match the exact UTF-8 text pin.",
              );
            return { ...component, expression_sha256: exact };
          }),
        );
      }
      if (controller.signal.aborted || version !== generation.current) return;
      if (JSON.stringify(currentDraft.current) !== signature)
        throw new Error(
          "The draft changed during validation. Newer edits were preserved; retry explicitly.",
        );
      setDraft({ ...snapshot, components });
      setNotice(
        saved
          ? "Copied exact expression text from the selected saved revision. Native lineage is the embedded expression pin, not an alpha reference."
          : "Native expressions validated and exact UTF-8 SHA-256 pins stored in the draft.",
      );
    } catch (failure) {
      if (!controller.signal.aborted && version === generation.current)
        setError((failure as Error).message);
    } finally {
      if (version === generation.current) setBusy(false);
    }
  };
  const components = draft.components ?? [],
    gross = components.reduce(
      (sum, component) => sum + Math.abs(component.weight),
      0,
    );
  const allocations = selected?.result?.portfolio_allocations ?? [];
  return (
    <div className="research-tool-panel portfolio-panel">
      <div className="panel-toolbar">
        <strong>Executable signal portfolio</strong>
        <button
          disabled={busy || !datasets.length}
          onClick={() =>
            setDraft(portfolioRequest("real-data-portfolio-v1", datasets[0]))
          }
        >
          Start portfolio draft
        </button>
        <span>Observed TRAIN · unverified</span>
      </div>
      {error && (
        <div className="notice error" role="alert">
          {error}
        </div>
      )}
      {notice && <div className="notice">{notice}</div>}
      {draft.mode === "real_portfolio" ? (
        <div className="portfolio-builder">
          <div className="panel-toolbar">
            <button
              disabled={busy || components.length >= 12}
              onClick={() =>
                apply("components", [
                  ...components,
                  {
                    strategy_dsl: "rank(close)",
                    expression_sha256: "",
                    weight: 1,
                  },
                ])
              }
            >
              Add expression component
            </button>
            <button
              disabled={busy || components.length < 2}
              onClick={() => void operation(false)}
            >
              Validate and pin components
            </button>
            <span>{components.length}/12 components</span>
          </div>
          <div className="portfolio-components">
            {components.map((component, index) => (
              <div className="portfolio-component" key={index}>
                <label>
                  Expression {index + 1}
                  <textarea
                    aria-label={`Portfolio expression ${index + 1}`}
                    value={component.strategy_dsl}
                    maxLength={1024}
                    onChange={(e) =>
                      updateComponent(index, "strategy_dsl", e.target.value)
                    }
                  />
                </label>
                <label>
                  Signed weight
                  <input
                    aria-label={`Portfolio weight ${index + 1}`}
                    type="number"
                    min={-1000000}
                    max={1000000}
                    step="any"
                    value={
                      Number.isFinite(component.weight) ? component.weight : ""
                    }
                    onChange={(e) =>
                      updateComponent(index, "weight", e.target.valueAsNumber)
                    }
                  />
                </label>
                <span title={component.expression_sha256}>
                  {component.expression_sha256
                    ? `SHA ${component.expression_sha256.slice(0, 14)} · normalized ${gross ? percent(component.weight / gross) : "—"}`
                    : "Expression edited: pin required"}
                </span>
                <button
                  aria-label={`Remove portfolio component ${index + 1}`}
                  disabled={busy || components.length <= 2}
                  onClick={() =>
                    apply(
                      "components",
                      components.filter((_, position) => position !== index),
                    )
                  }
                >
                  Remove
                </button>
              </div>
            ))}
          </div>
          <div className="panel-toolbar">
            <select
              aria-label="Portfolio saved alpha"
              value={alphaId}
              onChange={(e) => {
                setAlphaId(e.target.value);
                setRevision(
                  alphas.find((alpha) => alpha.id === e.target.value)
                    ?.revision ?? "",
                );
              }}
            >
              {!alphas.length && <option value="">No saved alphas</option>}
              {alphas.map((alpha) => (
                <option key={alpha.id} value={alpha.id}>
                  {alpha.name}
                </option>
              ))}
            </select>
            <label>
              Exact revision{" "}
              <input
                aria-label="Portfolio saved alpha revision"
                value={revision}
                onChange={(e) => setRevision(e.target.value)}
                pattern="[1-9][0-9]*"
              />
            </label>
            <button
              disabled={busy || !alphaId || components.length >= 12}
              onClick={() => void operation(true)}
            >
              Copy saved expression
            </button>
          </div>
          <div className="portfolio-constraints">
            {(
              [
                ["gross_limit", "Target gross limit", 0, 2, 0.01],
                ["name_cap", "Target name cap", 0.001, 1, 0.001],
                ["max_turnover", "Decision turnover cap", 0, 2, 0.01],
                ["signal_decay_sessions", "Signal decay sessions", 1, 21, 1],
              ] as const
            ).map(([key, label, min, max, step]) => (
              <label key={key}>
                {label}
                <input
                  aria-label={label}
                  type="number"
                  min={min}
                  max={max}
                  step={step}
                  value={draft[key] ?? ""}
                  onChange={(e) => apply(key, e.target.valueAsNumber)}
                />
              </label>
            ))}
          </div>
          <div className="notice">
            Signed manual weights normalize by total absolute weight. The native
            engine combines standardized signals and solves constrained targets
            with identity geometry; this is not estimated covariance risk or
            averaging NAV curves. Targets use pretrade NAV. Costs and drift can
            change achieved posttrade exposures. Infeasible or unconverged
            solves fail visibly.
          </div>
          <div className="help">
            Dataset, dates, capital, costs and delay use Draft configuration.
            Submit with Run research after pinning edits. The aggregate native
            compute budget may require fewer names, shorter windows or larger
            cadence.
          </div>
        </div>
      ) : (
        <div className="notice">
          Start an explicit portfolio draft to edit frozen expressions and
          execution constraints. The selected run remains unchanged.
        </div>
      )}
      <div className="panel-toolbar">
        <strong>Selected native portfolio decisions</strong>
        <span>
          {selected?.id.slice(0, 12) ?? "No run"} · {allocations.length} rows
          {selected?.result?.provenance.portfolio_allocations_truncated
            ? " · native evidence truncated"
            : ""}
        </span>
      </div>
      <div className="table-scroll">
        <table>
          <thead>
            <tr>
              <th>Decision</th>
              <th>Execution</th>
              <th>Target net</th>
              <th>Target gross</th>
              <th>Achieved net</th>
              <th>Achieved gross</th>
              <th>Max name</th>
              <th>Turnover</th>
              <th>Trade cost</th>
              <th>Primal residual</th>
              <th>Dual residual</th>
              <th>Polished</th>
              <th>Held / target / achieved</th>
            </tr>
          </thead>
          <tbody>
            {allocations.map((row, index) => (
              <tr key={index}>
                {[
                  "decision_date",
                  "execution_date",
                  "target_net",
                  "target_gross",
                  "achieved_posttrade_net",
                  "achieved_posttrade_gross",
                  "target_max_name",
                  "target_turnover",
                  "trade_cost",
                  "solver_primal_residual",
                  "solver_dual_residual",
                  "solver_polished",
                ].map((key) => (
                  <td key={key}>
                    {typeof row[key] === "number"
                      ? key.includes("residual")
                        ? (row[key] as number).toExponential(3)
                        : number(row[key] as number, 6)
                      : String(row[key] ?? "—")}
                  </td>
                ))}
                <td>
                  <details>
                    <summary>Exact allocation vectors</summary>
                    <pre>
                      {JSON.stringify(
                        {
                          instrument_order:
                            selected?.result?.provenance.instrument_order,
                          pretrade_held_weights: row.pretrade_held_weights,
                          target_weights: row.target_weights,
                          achieved_posttrade_weights:
                            row.achieved_posttrade_weights,
                        },
                        null,
                        2,
                      )}
                    </pre>
                  </details>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        {!allocations.length && (
          <div className="notice">
            No native portfolio allocation evidence supplied for the selected
            run.
          </div>
        )}
      </div>
    </div>
  );
}
