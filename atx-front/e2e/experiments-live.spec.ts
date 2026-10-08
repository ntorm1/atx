import { test, expect, type Page } from "@playwright/test";
test.skip(
  process.env.ATX_E2E_EXPERIMENTS_LIVE !== "1",
  "Requires native experiment scheduler",
);
async function panel(page: Page) {
  await page.getByRole("button", { name: "Windows", exact: false }).click();
  await page.getByRole("button", { name: "Experiments", exact: true }).click();
}
test("native experiments execute member recipes with lineage and cancel a scheduled cohort", async ({
  page,
  request,
}) => {
  test.setTimeout(90000);
  await page.goto("/");
  await page.getByLabel("Sessions", { exact: true }).fill("63");
  await panel(page);
  await page
    .getByLabel("Experiment name", { exact: true })
    .fill(`Native browser cohort ${Date.now()}`);
  await page
    .getByLabel("Experiment expression variants")
    .fill("rank(delta(close,5))\n-rank(ts_mean(returns,20))");
  await page
    .getByRole("button", { name: "Add expression variants", exact: true })
    .click();
  const create = page.waitForResponse(
    (response) =>
      response.request().method() === "POST" &&
      response.url().endsWith("/api/experiments"),
  );
  await page
    .getByRole("button", { name: "Run experiment", exact: true })
    .click();
  const response = await create;
  expect(response.ok()).toBe(true);
  const cohort = await response.json();
  expect(cohort.total).toBe(2);
  const members = page.getByRole("table", { name: "Experiment member jobs" });
  await expect(members.getByText("succeeded", { exact: true })).toHaveCount(2, {
    timeout: 60000,
  });
  const stored = await (
    await request.get(`/api/experiments/${cohort.id}`)
  ).json();
  expect(stored.succeeded).toBe(2);
  expect(
    stored.jobs.every(
      (job: { experiment_id: string }) => job.experiment_id === cohort.id,
    ),
  ).toBe(true);
  await members
    .getByRole("button", { name: "Inspect run", exact: true })
    .first()
    .click();
  await expect(page.locator(".properties")).toContainText(stored.jobs[0].id);
  const result = await (
    await request.get(`/api/jobs/${stored.jobs[0].id}`)
  ).json();
  expect(result.result.provenance.synthetic).toBe(true);
  expect(result.request.sessions).toBe(63);
  const future = new Date(Date.now() + 180000);
  const local = new Date(future.getTime() - future.getTimezoneOffset() * 60000)
    .toISOString()
    .slice(0, 16);
  await page
    .getByLabel("Experiment name", { exact: true })
    .fill(`Native scheduled cohort ${Date.now()}`);
  await page.getByLabel("Experiment schedule", { exact: true }).fill(local);
  const schedule = page.waitForResponse(
    (response) =>
      response.request().method() === "POST" &&
      response.url().endsWith("/api/experiments"),
  );
  await page
    .getByRole("button", { name: "Schedule experiment", exact: true })
    .click();
  const scheduledResponse = await schedule;
  expect(scheduledResponse.ok()).toBe(true);
  const scheduled = await scheduledResponse.json();
  expect(scheduled.queued).toBe(2);
  await page
    .getByRole("button", { name: "Cancel experiment", exact: true })
    .click();
  await expect(members.getByText("cancelled", { exact: true })).toHaveCount(2);
  const cancelled = await (
    await request.get(`/api/experiments/${scheduled.id}`)
  ).json();
  expect(cancelled.cancelled).toBe(2);
  expect(cancelled.running).toBe(0);
  await page
    .getByRole("button", { name: "Maximize active panel", exact: true })
    .click();
  await page.screenshot({
    path: "artifacts/workstation-experiments-live.png",
    fullPage: true,
  });
});
