// Provider selection, SQL policy and data access live in the native service.
// This adapter validates the browser envelope and preserves exact cell text.
export function registerDataRoutes(app, unary) {
  const source = {
    type: "string",
    minLength: 1,
    maxLength: 64,
    pattern: "^[a-zA-Z0-9_-]+$",
  };
  const identifier = { type: "string", minLength: 1, maxLength: 256 };
  const namespace = { type: "string", maxLength: 256 };
  const rows = { type: "integer", minimum: 1, maximum: 500, default: 200 };
  const envelope = (required, properties) => ({
    type: "object",
    additionalProperties: false,
    required,
    properties,
  });
  app.get("/api/data/sources", async (req) =>
    unary(req, "listDataSources", {}),
  );
  app.get(
    "/api/data/catalog",
    {
      schema: { querystring: envelope(["source_id"], { source_id: source }) },
    },
    async (req) => unary(req, "getDataCatalog", { ...req.query }),
  );
  app.get(
    "/api/data/preview",
    {
      schema: {
        querystring: envelope(["source_id", "catalog", "schema", "table"], {
          source_id: source,
          catalog: namespace,
          schema: namespace,
          table: identifier,
          limit: rows,
          offset: {
            type: "integer",
            minimum: 0,
            maximum: 1_000_000,
            default: 0,
          },
        }),
      },
    },
    async (req) => unary(req, "previewData", { ...req.query }),
  );
  app.post(
    "/api/data/query",
    {
      schema: {
        body: envelope(["source_id", "sql"], {
          source_id: source,
          sql: {
            type: "string",
            minLength: 1,
            maxLength: 65536,
            pattern: "\\S",
          },
          max_rows: rows,
        }),
      },
    },
    async (req, reply) => {
      if (Buffer.byteLength(req.body.sql, "utf8") > 64 * 1024)
        return reply
          .code(400)
          .send({ error: "SQL exceeds the 64 KiB UTF-8 limit" });
      return unary(req, "queryData", req.body);
    },
  );
}
