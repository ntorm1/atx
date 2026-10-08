import { randomUUID } from "node:crypto";
import { test, expect } from "@playwright/test";
test("native finite workflows freeze saved alpha revisions, persist slots, and enforce pause and role policy", async ({
  page,
  request,
}) => {
  test.skip(
    process.env.ATX_E2E_WORKFLOWS_LIVE !== "1",
    "Requires native schema7 recurrence services",
  );
  test.setTimeout(90000);
  page.setDefaultTimeout(10000);
  const suffix = randomUUID().slice(0, 8),
    workflowIds: string[] = [];
  let teamWorkflowId = "";
  const alphaResponse = await request.post("/api/alphas", {
    data: {
      name: `Workflow alpha ${suffix}`,
      description: "Disposable browser qualification",
      tags: [],
      project_id: "local",
      hypothesis: "Synthetic system validation only.",
      request: {
        mode: "synthetic_demo",
        strategy_id: "synthetic-long-short-v1",
        strategy_dsl: "rank(delta(close,5))",
        sessions: 63,
        initial_capital: 1000000,
        trade_bps: 6,
        annual_borrow_bps: 300,
      },
    },
  });
  expect(alphaResponse.status()).toBe(201);
  const alpha = await alphaResponse.json();
  try {
    await page.goto("/?panel=workflows");
    await page.getByRole("button", { name: "Maximize active panel" }).click();
    await page
      .getByLabel("Workflow name")
      .fill(`Finite native workflow ${suffix}`);
    await page.getByLabel("Workflow saved alpha").selectOption(alpha.id);
    await page.getByRole("button", { name: "Freeze saved revision" }).click();
    await page.getByLabel("Workflow interval", { exact: true }).fill("60");
    await page.getByLabel("Workflow interval unit").selectOption("seconds");
    await page.getByLabel("Workflow maximum runs").fill("1");
    const creating = page.waitForResponse(
      (response) =>
        response.url().endsWith("/api/workflows") &&
        response.request().method() === "POST",
    );
    await page
      .getByRole("button", { name: "Create recurring workflow" })
      .click();
    const admitted = await creating;
    expect(admitted.status()).toBe(201);
    const workflow = await admitted.json();
    workflowIds.push(workflow.id);
    expect(admitted.request().postDataJSON().alpha_revision).toBe(
      alpha.revision,
    );
    expect(admitted.request().postDataJSON().request).toBeUndefined();
    await expect
      .poll(
        async () =>
          (await (await request.get(`/api/workflows/${workflow.id}`)).json())
            .state,
        { timeout: 30000 },
      )
      .toBe("completed");
    const complete = await (
      await request.get(`/api/workflows/${workflow.id}`)
    ).json();
    expect(complete.admitted_runs).toBe("1");
    expect(complete.succeeded).toBe("1");
    expect(complete.request_sha256).toBe(alpha.request_sha256);
    const runs = await (
      await request.get(`/api/workflows/${workflow.id}/runs`)
    ).json();
    expect(runs.total).toBe("1");
    expect(runs.jobs[0].workflow_id).toBe(workflow.id);
    expect(runs.jobs[0].workflow_slot_ms).toBe(complete.first_run_at_ms);
    expect(runs.jobs[0].alpha_revision).toBe(alpha.revision);
    const detail = await (
      await request.get(`/api/jobs/${runs.jobs[0].id}`)
    ).json();
    expect(detail.result.provenance.synthetic).toBe(true);
    expect(detail.request.strategy_dsl).toBe(alpha.request.strategy_dsl);
    const changed = await request.put(`/api/alphas/${alpha.id}`, {
      data: {
        expected_revision: alpha.revision,
        name: alpha.name,
        description: alpha.description,
        tags: alpha.tags,
        hypothesis: alpha.hypothesis,
        request: { ...alpha.request, strategy_dsl: "rank(close)" },
        source_job_id: "",
      },
    });
    expect(changed.ok()).toBe(true);
    const unchanged = await (
      await request.get(`/api/workflows/${workflow.id}`)
    ).json();
    expect(unchanged.alpha_revision).toBe("1");
    expect(unchanged.request_sha256).toBe(alpha.request_sha256);
    await page.reload();
    await page
      .getByRole("button", {
        name: `Finite native workflow ${suffix}`,
        exact: true,
      })
      .click();
    await expect(
      page.getByText(`completed / r${complete.revision}`, { exact: true }),
    ).toBeVisible();
    await expect(
      page.getByRole("button", { name: "Completed", exact: true }),
    ).toBeDisabled();
    await page
      .getByRole("button", { name: "Inspect workflow run", exact: true })
      .click();
    const scheduled = await request.post("/api/workflows", {
      data: {
        name: `Pause native workflow ${suffix}`,
        description: "No active admission",
        alpha_id: alpha.id,
        alpha_revision: "1",
        interval_ms: "60000",
        max_runs: 3,
        first_run_at_ms: String(Date.now() + 180000),
        idempotency_key: randomUUID(),
      },
    });
    expect(scheduled.status()).toBe(201);
    const future = await scheduled.json();
    workflowIds.push(future.id);
    await page
      .getByRole("button", { name: "Refresh workflows", exact: true })
      .click();
    await page.getByRole("button", { name: future.name, exact: true }).click();
    await page
      .getByRole("button", { name: "Pause workflow", exact: true })
      .click();
    await expect(page.getByText("paused / r2", { exact: true })).toBeVisible();
    await page
      .getByRole("button", { name: "Resume workflow", exact: true })
      .click();
    await expect(page.getByText("active / r3", { exact: true })).toBeVisible();
    await page
      .getByRole("button", { name: "Pause workflow", exact: true })
      .click();
    await expect(page.getByText("paused / r4", { exact: true })).toBeVisible();
    const maximize = page.getByRole("button", {
      name: "Maximize active panel",
    });
    if (await maximize.isVisible()) await maximize.click();
    await page.screenshot({ path: "artifacts/workstation-workflows-live.png" });
    if (process.env.ATX_E2E_TEAM_LIVE === "1") {
      const base = process.env.ATX_E2E_TEAM_BASE_URL ?? "http://127.0.0.1:3004",
        actor = process.env.ATX_E2E_TEAM_RESEARCHER!,
        viewer = process.env.ATX_E2E_TEAM_VIEWER!;
      const body = {
        name: `Team recurrence ${suffix}`,
        request: alpha.request,
        interval_ms: "60000",
        max_runs: 1,
        first_run_at_ms: String(Date.now() + 180000),
        idempotency_key: randomUUID(),
      };
      const denied = await request.post(`${base}/api/workflows`, {
        headers: { Authorization: `Bearer ${viewer}` },
        data: body,
      });
      expect(denied.status()).toBe(403);
      const accepted = await request.post(`${base}/api/workflows`, {
        headers: { Authorization: `Bearer ${actor}` },
        data: body,
      });
      expect(accepted.status()).toBe(201);
      const owned = await accepted.json();
      teamWorkflowId = owned.id;
      expect(owned.actor_id).toBe("browser-researcher");
      const denyPause = await request.post(
        `${base}/api/workflows/${owned.id}/pause`,
        {
          headers: { Authorization: `Bearer ${viewer}` },
          data: { expected_revision: owned.revision, paused: true },
        },
      );
      expect(denyPause.status()).toBe(403);
      const pause = await request.post(
        `${base}/api/workflows/${owned.id}/pause`,
        {
          headers: { Authorization: `Bearer ${actor}` },
          data: { expected_revision: owned.revision, paused: true },
        },
      );
      expect(pause.status()).toBe(200);
      expect((await pause.json()).state).toBe("paused");
    }
  } finally {
    if (teamWorkflowId) {
      const base = process.env.ATX_E2E_TEAM_BASE_URL ?? "http://127.0.0.1:3004";
      const headers = {
        Authorization: `Bearer ${process.env.ATX_E2E_TEAM_RESEARCHER}`,
      };
      const response = await request.get(
        `${base}/api/workflows/${teamWorkflowId}`,
        {
          headers,
        },
      );
      if (response.ok()) {
        const record = await response.json();
        if (record.enabled)
          await request.post(`${base}/api/workflows/${teamWorkflowId}/pause`, {
            headers,
            data: { expected_revision: record.revision, paused: true },
          });
      }
    }
    for (const id of workflowIds) {
      const response = await request.get(`/api/workflows/${id}`);
      if (response.ok()) {
        const record = await response.json();
        if (record.enabled)
          await request.post(`/api/workflows/${id}/pause`, {
            data: { expected_revision: record.revision, paused: true },
          });
      }
    }
    const response = await request.get(`/api/alphas/${alpha.id}`);
    if (response.ok()) {
      const record = await response.json();
      await request.post(`/api/alphas/${alpha.id}/archive`, {
        data: { expected_revision: record.revision, archived: true },
      });
    }
  }
});
