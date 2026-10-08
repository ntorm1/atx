const failure = (message, code = 3) =>
  Object.assign(new Error(message), { code, details: message });
const envelope = (required, properties) => ({
  type: "object",
  additionalProperties: false,
  required,
  properties,
});
const document = (text) => {
  try {
    if (typeof text !== "string" || Buffer.byteLength(text) > 16 * 1024 * 1024)
      throw new Error("Invalid response size");
    const value = JSON.parse(text);
    if (!value || typeof value !== "object" || Array.isArray(value))
      throw new Error("Invalid response shape");
    return value;
  } catch {
    throw failure("Invalid analysis document from native service", 13);
  }
};

export function registerAnalysisRoutes(app, unary) {
  const date = {
    type: "string",
    pattern: "^(?:|[0-9]{4}-[0-9]{2}-[0-9]{2})$",
    default: "",
  };
  app.post(
    "/api/analysis/compare",
    {
      bodyLimit: 8192,
      schema: {
        body: envelope(["job_ids"], {
          job_ids: {
            type: "array",
            minItems: 2,
            maxItems: 12,
            uniqueItems: true,
            items: { type: "string", pattern: "^[0-9a-f]{32}$" },
          },
          start_date: date,
          end_date: date,
          max_chart_points: {
            type: "integer",
            minimum: 2,
            maximum: 2000,
            default: 1000,
          },
          allow_mixed_evidence: { type: "boolean", default: false },
        }),
      },
    },
    async (req) =>
      document((await unary(req, "compareJobs", req.body)).comparison_json),
  );

  app.post(
    "/api/analysis/validate",
    {
      bodyLimit: 8192,
      schema: {
        body: envelope(["strategy_dsl"], {
          strategy_dsl: { type: "string", minLength: 1, maxLength: 1024 },
          dataset_id: {
            type: "string",
            pattern: "^[A-Za-z0-9_-]{0,64}$",
            default: "",
          },
          dataset_sha256: {
            type: "string",
            pattern: "^(?:|[0-9a-f]{64})$",
            default: "",
          },
        }),
      },
    },
    async (req) => {
      if (Buffer.byteLength(req.body.strategy_dsl, "utf8") > 1024)
        throw failure("Expression exceeds 1024 UTF-8 bytes");
      return document(
        (await unary(req, "validateExpression", req.body)).analysis_json,
      );
    },
  );
}
