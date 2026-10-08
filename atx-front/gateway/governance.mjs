const failure = (message, code = 3) =>
  Object.assign(new Error(message), { code, details: message });
const envelope = (required, properties) => ({
  type: "object",
  additionalProperties: false,
  required,
  properties,
});
const id = { type: "string", pattern: "^[a-f0-9]{32}$" };
const revision = { type: "string", pattern: "^[1-9][0-9]{0,18}$" };
const paging = {
  limit: { type: "integer", minimum: 1, maximum: 100, default: 50 },
  offset: { type: "integer", minimum: 0, maximum: 1000000, default: 0 },
};
const params = envelope(["id"], { id });
const byteText = (value, limit, label, required = false) => {
  if (
    typeof value !== "string" ||
    Buffer.byteLength(value) > limit ||
    (required && !value.trim())
  )
    throw failure(`${label} exceeds its UTF-8 limit or is empty`);
  return value;
};
const exactRevision = (field) => async (req) => {
  const value = req.body?.[field];
  if (
    typeof value !== "string" ||
    !/^[1-9][0-9]{0,18}$/.test(value) ||
    BigInt(value) > 9223372036854775807n
  )
    throw failure("Revision must be an exact positive int64 decimal string");
};
const document = (value, maximum) => {
  try {
    if (typeof value !== "string" || Buffer.byteLength(value) > maximum)
      throw new Error();
    const parsed = JSON.parse(value);
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed))
      throw new Error();
    return parsed;
  } catch {
    throw failure("Invalid governance document from native service", 13);
  }
};
const review = ({ evidence_json, ...record }) => ({
  ...record,
  evidence: document(evidence_json, 96 * 1024),
});
export function registerGovernanceRoutes(app, unary) {
  app.get(
    "/api/audit",
    {
      schema: {
        querystring: envelope([], {
          ...paging,
          entity_type: {
            type: "string",
            pattern: "^[a-z_]{0,32}$",
            default: "",
          },
          entity_id: {
            type: "string",
            pattern: "^(?:[a-f0-9]{32})?$",
            default: "",
          },
          actor_id: {
            type: "string",
            pattern: "^[A-Za-z0-9_-]{0,64}$",
            default: "",
          },
        }),
      },
    },
    async (req) => {
      const page = await unary(req, "listAudit", req.query);
      return {
        ...page,
        records: page.records.map(({ metadata_json, ...record }) => ({
          ...record,
          metadata: document(metadata_json, 16384),
        })),
      };
    },
  );
  app.post(
    "/api/reviews",
    {
      bodyLimit: 16 * 1024,
      preValidation: exactRevision("alpha_revision"),
      schema: {
        body: envelope(["alpha_id", "alpha_revision", "job_ids"], {
          alpha_id: id,
          alpha_revision: revision,
          job_ids: {
            type: "array",
            minItems: 1,
            maxItems: 12,
            uniqueItems: true,
            items: id,
          },
          note: { type: "string", maxLength: 4096, default: "" },
          idempotency_key: { type: "string", maxLength: 128, default: "" },
        }),
      },
    },
    async (req, reply) => {
      byteText(req.body.note, 4096, "Review note");
      byteText(req.body.idempotency_key, 128, "Review key");
      const created = review(await unary(req, "submitReview", req.body));
      reply.code(201);
      return created;
    },
  );
  app.get(
    "/api/reviews",
    {
      schema: {
        querystring: envelope([], {
          ...paging,
          alpha_id: {
            type: "string",
            pattern: "^(?:[a-f0-9]{32})?$",
            default: "",
          },
          state: {
            type: "string",
            enum: ["", "submitted", "approved", "rejected", "withdrawn"],
            default: "",
          },
        }),
      },
    },
    async (req) => {
      const page = await unary(req, "listReviews", req.query);
      return {
        ...page,
        reviews: page.reviews.map(
          ({ evidence_json: _evidence, ...record }) => record,
        ),
      };
    },
  );
  app.get("/api/reviews/:id", { schema: { params } }, async (req) =>
    review(await unary(req, "getReview", req.params)),
  );
  app.post(
    "/api/reviews/:id/decision",
    {
      bodyLimit: 16 * 1024,
      preValidation: exactRevision("expected_revision"),
      schema: {
        params,
        body: envelope(["expected_revision", "state", "rationale"], {
          expected_revision: revision,
          state: {
            type: "string",
            enum: ["approved", "rejected", "withdrawn"],
          },
          rationale: { type: "string", minLength: 1, maxLength: 4096 },
        }),
      },
    },
    async (req) => {
      byteText(req.body.rationale, 4096, "Review rationale", true);
      return review(
        await unary(req, "decideReview", { ...req.params, ...req.body }),
      );
    },
  );
  app.get(
    "/api/reviews/:id/decisions",
    { schema: { params, querystring: envelope([], paging) } },
    async (req) =>
      unary(req, "listReviewDecisions", { ...req.params, ...req.query }),
  );
}
