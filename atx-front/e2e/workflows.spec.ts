import { test, expect } from "@playwright/test";
test.use({ baseURL: "http://127.0.0.1:3002" });
test.beforeEach(async ({ request }) => {
  await request.post("/fixture/workflows", { data: { reset: true } });
});
test.afterEach(async ({ request }) => {
  await request.post("/fixture/team", {
    data: { role: "admin", actor_id: "local", team_mode: false },
  });
});
test("a lost creation response retries the identical first-slot epoch and idempotency key", async ({
  page,
  request,
}) => {
  await page.goto("/?panel=workflows");
  await page.getByRole("button", { name: "Maximize active panel" }).click();
  await page.getByLabel("Workflow name").fill("Idempotent recurrence");
  await page.getByRole("button", { name: "Freeze current recipe" }).click();
  await request.post("/fixture/workflows", { data: { lose_response: true } });
  const first = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/workflows") &&
      response.request().method() === "POST",
  );
  await page.getByRole("button", { name: "Create recurring workflow" }).click();
  const failed = await first;
  expect(failed.status()).toBe(503);
  const original = failed.request().postDataJSON();
  await expect(page.getByRole("alert")).toBeVisible();
  const retry = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/workflows") &&
      response.request().method() === "POST",
  );
  await page.getByRole("button", { name: "Create recurring workflow" }).click();
  const recovered = await retry;
  expect(recovered.status()).toBe(201);
  expect(recovered.request().postDataJSON()).toEqual(original);
  await expect(page.getByText("active / r1", { exact: true })).toBeVisible();
  expect((await (await request.get("/api/workflows")).json()).total).toBe("1");
});
test("named Operations desks retain unsent frozen workflow composer across browsers", async ({
  page,
  browser,
  request,
}) => {
  await request.post("/fixture/workspaces", { data: { reset: true } });
  await page.goto("/");
  await page
    .getByLabel("Workspace", { exact: true })
    .selectOption("template:operations");
  await page.getByLabel("Workflow name").fill("Persist unsent recurrence");
  await page.getByLabel("Workflow interval", { exact: true }).fill("90000");
  await page.getByLabel("Workflow interval unit").selectOption("milliseconds");
  await page.getByLabel("Workflow maximum runs").fill("7");
  await page.getByRole("button", { name: "Freeze current recipe" }).click();
  await page.getByRole("button", { name: "Workspace actions" }).click();
  await page.getByRole("button", { name: "Save as…", exact: true }).click();
  await page
    .getByLabel("Workspace name", { exact: true })
    .fill("Operations recurrence desk");
  await page
    .getByRole("button", { name: "Create workspace", exact: true })
    .click();
  await expect(page.locator(".workspace-status")).toHaveText(/r1/);
  const context = await browser.newContext();
  try {
    const second = await context.newPage();
    await second.goto(page.url());
    await expect(second.getByLabel("Workflow name")).toHaveValue(
      "Persist unsent recurrence",
    );
    await expect(
      second.getByLabel("Workflow interval", { exact: true }),
    ).toHaveValue("90000");
    await expect(second.getByLabel("Workflow interval unit")).toHaveValue(
      "milliseconds",
    );
    await expect(second.getByLabel("Workflow maximum runs")).toHaveValue("7");
    await expect(second.locator(".workflow-source").first()).toContainText(
      "synthetic-long-short-v1 / frozen draft",
    );
    expect((await (await request.get("/api/workflows")).json()).total).toBe(
      "0",
    );
  } finally {
    await context.close();
  }
});
test("recurring workflows freeze recipes, retain exact interval and lineage, and pause independently of active jobs", async ({
  page,
  request,
}) => {
  await page.goto("/?panel=workflows");
  await expect(page.getByLabel("Workflow name")).toBeVisible({ timeout: 1500 });
  await page.getByRole("button", { name: "Maximize active panel" }).click();
  await page.getByLabel("Workflow name").fill("Frozen research recurrence");
  await page.getByLabel("Workflow interval", { exact: true }).fill("60");
  await page.getByLabel("Workflow maximum runs").fill("3");
  await page.getByRole("button", { name: "Freeze current recipe" }).click();
  const creating = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/workflows") &&
      response.request().method() === "POST",
  );
  await page.getByRole("button", { name: "Create recurring workflow" }).click();
  const admitted = await creating;
  expect(admitted.status()).toBe(201);
  const body = admitted.request().postDataJSON();
  expect(body.interval_ms).toBe("3600000");
  expect(body.request.strategy_dsl).toBe("rank(delta(close,5))");
  expect(body.max_runs).toBe(3);
  const workflow = await admitted.json();
  await expect(page.getByText("active / r1", { exact: true })).toBeVisible();
  await request.post("/fixture/workflows", {
    data: { active_id: workflow.id },
  });
  await page
    .getByRole("button", { name: "Refresh workflows", exact: true })
    .click();
  await expect(
    page.getByRole("button", { name: "Cancel active workflow run" }),
  ).toBeEnabled();
  await page
    .getByRole("button", { name: "Pause workflow", exact: true })
    .click();
  await expect(page.getByText("paused / r2", { exact: true })).toBeVisible();
  await expect(
    page.getByRole("button", { name: "Cancel active workflow run" }),
  ).toBeEnabled();
  await page
    .getByRole("button", { name: "Inspect workflow run", exact: true })
    .click();
  await page
    .getByRole("button", { name: "Resume workflow", exact: true })
    .click();
  await expect(page.getByText("active / r3", { exact: true })).toBeVisible();
});
test("workflow revision conflicts preserve composer edits and viewer controls remain read-only", async ({
  page,
  request,
}) => {
  await request.post("/fixture/workflows", { data: { seed: true } });
  await page.goto("/?panel=workflows");
  await expect(page.getByLabel("Workflow name")).toBeVisible({ timeout: 1500 });
  await page.getByRole("button", { name: "Maximize active panel" }).click();
  await page
    .getByRole("button", { name: "Fixture recurring workflow", exact: true })
    .click();
  await page.getByLabel("Workflow name").fill("Keep unsent composer");
  await request.post("/fixture/workflows", { data: { conflict: true } });
  await page
    .getByRole("button", { name: "Pause workflow", exact: true })
    .click();
  await expect(page.getByRole("alert")).toContainText("revision changed");
  await page.getByRole("button", { name: "Reload workflow revision" }).click();
  await expect(page.getByText("active / r2", { exact: true })).toBeVisible();
  await expect(page.getByLabel("Workflow name")).toHaveValue(
    "Keep unsent composer",
  );
  await request.post("/fixture/team", {
    data: {
      role: "viewer",
      actor_id: "reader",
      name: "Read-only member",
      team_mode: true,
    },
  });
  await page.reload();
  await page
    .getByRole("button", { name: "Fixture recurring workflow", exact: true })
    .click();
  await expect(
    page.getByRole("button", { name: "Create recurring workflow" }),
  ).toBeDisabled();
  await expect(
    page.getByRole("button", { name: "Pause workflow", exact: true }),
  ).toBeDisabled();
  await request.post("/fixture/team", {
    data: { role: "admin", actor_id: "local", team_mode: false },
  });
});
