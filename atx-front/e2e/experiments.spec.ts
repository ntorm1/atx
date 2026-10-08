import { test, expect } from "@playwright/test";
test.use({ baseURL: "http://127.0.0.1:3002" });
test.beforeEach(async ({ request }) => {
  await request.post("http://127.0.0.1:3002/fixture/library");
  await request.post("http://127.0.0.1:3002/fixture/workspaces", {
    data: { reset: true },
  });
});
test("scheduled cohorts preserve frozen recipes, cancel all members and persist composer context", async ({
  page,
  request,
}) => {
  await page.goto("/?panel=experiments");
  await page
    .getByLabel("Experiment name", { exact: true })
    .fill("Scheduled cohort");
  await page
    .getByLabel("Experiment description", { exact: true })
    .fill("Browser fixture scheduler check");
  await page
    .getByLabel("Experiment expression variants")
    .fill("rank(close)\n-rank(delta(close,5))");
  await page
    .getByRole("button", { name: "Add expression variants", exact: true })
    .click();
  await expect(
    page
      .getByRole("table", { name: "Draft experiment simulations" })
      .locator("tbody tr"),
  ).toHaveCount(2);
  await page.getByRole("button", { name: "Workspace actions" }).click();
  await page.getByRole("button", { name: "Save as…", exact: true }).click();
  await page
    .getByLabel("Workspace name", { exact: true })
    .fill("Experiments desk");
  await page
    .getByRole("button", { name: "Create workspace", exact: true })
    .click();
  await expect(page.locator(".workspace-status")).toHaveText("r1");
  const workspace = new URL(page.url()).searchParams.get("workspace")!;
  const saved = await (
    await request.get(`http://127.0.0.1:3002/api/workspaces/${workspace}`)
  ).json();
  expect(saved.document.panel_state.experiment.simulations).toHaveLength(2);
  await page.reload();
  await expect(page.getByLabel("Experiment name", { exact: true })).toHaveValue(
    "Scheduled cohort",
  );
  await expect(
    page
      .getByRole("table", { name: "Draft experiment simulations" })
      .locator("tbody tr"),
  ).toHaveCount(2);
  const future = new Date(Date.now() + 180000);
  const local = new Date(future.getTime() - future.getTimezoneOffset() * 60000)
    .toISOString()
    .slice(0, 16);
  await page.getByLabel("Experiment schedule", { exact: true }).fill(local);
  const submitting = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/experiments") &&
      response.request().method() === "POST",
  );
  await page
    .getByRole("button", { name: "Schedule experiment", exact: true })
    .click();
  const response = await submitting;
  expect(response.ok()).toBe(true);
  const experiment = await response.json();
  expect(experiment.total).toBe(2);
  expect(experiment.queued).toBe(2);
  expect(
    response.request().postDataJSON().simulations[1].request.strategy_dsl,
  ).toBe("-rank(delta(close,5))");
  await expect(
    page
      .getByRole("table", { name: "Experiment member jobs" })
      .locator("tbody tr"),
  ).toHaveCount(2);
  await page
    .getByRole("button", { name: "Cancel experiment", exact: true })
    .click();
  await expect(
    page
      .getByRole("table", { name: "Experiment member jobs" })
      .getByText("cancelled", { exact: true }),
  ).toHaveCount(2);
  await expect(
    page.getByRole("button", { name: "Cancel experiment", exact: true }),
  ).toBeDisabled();
  await page
    .getByRole("table", { name: "Experiment member jobs" })
    .getByRole("button", { name: "Inspect run", exact: true })
    .first()
    .click();
  await expect(page.locator(".properties")).toContainText(
    experiment.jobs[0].id,
  );
});
test("expression cohorts enforce the 32-member limit before admission", async ({
  page,
}) => {
  await page.goto("/?panel=experiments");
  await page
    .getByLabel("Experiment expression variants")
    .fill(Array.from({ length: 33 }, () => "rank(close)").join("\n"));
  await page
    .getByRole("button", { name: "Add expression variants", exact: true })
    .click();
  await expect(
    page.locator(".experiments-panel").getByRole("alert"),
  ).toContainText("at most 32");
  await expect(
    page
      .getByRole("table", { name: "Draft experiment simulations" })
      .locator("tbody tr"),
  ).toHaveCount(0);
});
