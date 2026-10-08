import type { SerializedDockview } from "dockview-react";
import type { ResearchRequest } from "./types";
import type { ExperimentDraft } from "./experiments";
import type { WorkflowDraft } from "./workflows";
export type LayoutPreset = "research" | "database" | "operations";
export type DatabaseContext = {
  source_id: string;
  catalog: string;
  schema: string;
  table: string;
  sql: string;
  page_size: number;
  offset: number;
  filter: string;
  mode: "preview" | "query";
  previous_offsets?: number[];
};
export const emptyDatabaseContext: DatabaseContext = {
  source_id: "",
  catalog: "",
  schema: "",
  table: "",
  sql: "",
  page_size: 200,
  offset: 0,
  filter: "",
  mode: "preview",
  previous_offsets: [],
};
export type WorkspaceDocument = {
  schema_version: 1;
  layout: SerializedDockview;
  panel_state: {
    draft_request: ResearchRequest;
    selected_job_id: string;
    database: DatabaseContext;
    experiment?: ExperimentDraft;
    workflow?: WorkflowDraft;
  };
};
export type SavedWorkspaceSummary = {
  id: string;
  name: string;
  description: string;
  revision: string;
  created_at_ms: string;
  updated_at_ms: string;
};
export type SavedWorkspace = SavedWorkspaceSummary & {
  document: WorkspaceDocument;
};
const object = (value: unknown): value is Record<string, unknown> =>
  Boolean(value && typeof value === "object" && !Array.isArray(value));
export function validateWorkspaceDocument(
  document: unknown,
): asserts document is WorkspaceDocument {
  if (
    !object(document) ||
    document.schema_version !== 1 ||
    !object(document.layout) ||
    !object(document.layout.grid) ||
    !object(document.layout.panels) ||
    !object(document.panel_state)
  )
    throw new Error(
      "This workspace does not contain a supported version 1 docking layout.",
    );
  if (new TextEncoder().encode(JSON.stringify(document)).byteLength > 96 * 1024)
    throw new Error(
      "Workspace document exceeds 96 KiB. Shorten the SQL or request draft before saving.",
    );
  const inspect = (value: unknown, depth: number) => {
    if (depth > 32)
      throw new Error("Workspace document exceeds the depth limit.");
    if (!value || typeof value !== "object") return;
    for (const [key, child] of Object.entries(value)) {
      if (
        /^(?:__proto__|constructor|prototype|token|access_token|auth_token|authorization|password|credentials|connection_string)$/i.test(
          key,
        )
      )
        throw new Error(
          "Credentials and unsafe object keys cannot be saved in workspaces.",
        );
      inspect(child, depth + 1);
    }
  };
  inspect(document, 0);
  const supportedPanels = new Set([
    "workspace",
    "configuration",
    "editor",
    "database",
    "metrics",
    "equity",
    "drawdown",
    "distribution",
    "monthly",
    "jobs",
    "trades",
    "positions",
    "factors",
    "provenance",
    "logs",
    "operators",
    "alphas",
    "comparison",
    "experiments",
    "team",
    "reviews",
    "portfolio",
    "workflows",
  ]);
  const panels = Object.values(document.layout.panels);
  if (
    !panels.length ||
    panels.length > 64 ||
    panels.some(
      (p) =>
        !object(p) ||
        p.contentComponent !== "research" ||
        !object(p.params) ||
        typeof p.params.kind !== "string" ||
        !supportedPanels.has(p.params.kind),
    )
  )
    throw new Error(
      "The saved workspace contains unsupported panel definitions.",
    );
  const state = document.panel_state;
  const draft = state.draft_request;
  if (
    !object(draft) ||
    ![
      "synthetic_demo",
      "import_nav",
      "real_nav",
      "real_dsl",
      "real_portfolio",
    ].includes(String(draft.mode)) ||
    typeof draft.strategy_id !== "string"
  )
    throw new Error("The saved research request is unsupported.");
  if (
    typeof state.selected_job_id !== "string" ||
    !/^[a-zA-Z0-9_-]{0,128}$/.test(state.selected_job_id)
  )
    throw new Error("The saved job selection is invalid.");
  const database = state.database;
  const experiment = state.experiment;
  const workflow = state.workflow;
  if (
    workflow !== undefined &&
    (!object(workflow) ||
      ["name", "description", "interval_value", "first_run"].some(
        (key) => typeof workflow[key] !== "string",
      ) ||
      !["minutes", "seconds", "milliseconds"].includes(
        String(workflow.interval_unit),
      ) ||
      !Number.isInteger(workflow.max_runs) ||
      Number(workflow.max_runs) < 1 ||
      Number(workflow.max_runs) > 1000 ||
      (workflow.source !== null &&
        (!object(workflow.source) ||
          typeof workflow.source.label !== "string" ||
          (!object(workflow.source.request) &&
            (typeof workflow.source.alpha_id !== "string" ||
              typeof workflow.source.alpha_revision !== "string")))))
  )
    throw new Error("The saved workflow composer is invalid.");
  if (
    experiment !== undefined &&
    (!object(experiment) ||
      ["name", "description", "schedule", "expressions"].some(
        (key) => typeof experiment[key] !== "string",
      ) ||
      !Array.isArray(experiment.simulations) ||
      experiment.simulations.length > 32 ||
      experiment.simulations.some(
        (member) =>
          !object(member) ||
          typeof member.label !== "string" ||
          (!object(member.request) &&
            (typeof member.alpha_id !== "string" ||
              typeof member.alpha_revision !== "string")),
      ))
  )
    throw new Error("The saved experiment composer is invalid.");
  if (
    !object(database) ||
    ["source_id", "catalog", "schema", "table", "sql", "filter"].some(
      (field) => typeof database[field] !== "string",
    ) ||
    !["preview", "query"].includes(String(database.mode)) ||
    !Number.isInteger(database.page_size) ||
    Number(database.page_size) < 1 ||
    Number(database.page_size) > 500 ||
    !Number.isInteger(database.offset) ||
    Number(database.offset) < 0 ||
    Number(database.offset) > 1_000_000
  )
    throw new Error("The saved database context is invalid.");
  if (
    String(database.source_id).length > 64 ||
    new TextEncoder().encode(String(database.sql)).byteLength > 65536 ||
    ["catalog", "schema", "table"].some(
      (key) => String(database[key]).length > 256,
    ) ||
    String(database.filter).length > 512
  )
    throw new Error("The saved database context exceeds its limits.");
  if (
    database.previous_offsets !== undefined &&
    (!Array.isArray(database.previous_offsets) ||
      database.previous_offsets.length > 1000 ||
      database.previous_offsets.some(
        (value) => !Number.isInteger(value) || value < 0 || value > 1_000_000,
      ))
  )
    throw new Error("The saved database pagination history is invalid.");
}
