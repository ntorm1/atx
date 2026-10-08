import { test, expect } from "@playwright/test";
test("native observed-data signal portfolio executes delayed constrained targets with exact diagnostic evidence", async ({
  page,
  request,
}) => {
  test.skip(
    process.env.ATX_E2E_PORTFOLIO_LIVE !== "1",
    "Requires configured TRAIN data and current native portfolio solver",
  );
  test.setTimeout(180000);
  await page.goto("/?panel=portfolio");
  await page.getByRole("button", { name: "Maximize active panel" }).click();
  await page.getByRole("button", { name: "Start portfolio draft" }).click();
  await expect(
    page.getByLabel("Portfolio expression 1", { exact: true }),
  ).toHaveValue("-rank(delta(close,5)/delay(close,5))");
  await expect(
    page.getByLabel("Portfolio expression 2", { exact: true }),
  ).toHaveValue("-rank(delta(close,10)/delay(close,10))");
  await page
    .getByRole("button", { name: "Validate and pin components" })
    .click();
  await expect(
    page.getByText(
      "Native expressions validated and exact UTF-8 SHA-256 pins stored in the draft.",
      { exact: true },
    ),
  ).toBeVisible();
  await page.getByRole("button", { name: "Restore docking layout" }).click();
  await expect(page.getByLabel("Instruments", { exact: true })).toHaveValue(
    "128",
  );
  await expect(page.getByLabel("End date", { exact: true })).toHaveValue(
    "2020-03-31",
  );
  const submit = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/jobs") &&
      response.request().method() === "POST",
  );
  await page
    .getByRole("button", { name: "Run research", exact: false })
    .click();
  const admitted = await submit;
  expect(admitted.status()).toBe(201);
  const job = await admitted.json();
  await expect
    .poll(
      async () =>
        (await (await request.get(`/api/jobs/${job.id}`)).json()).status,
      { timeout: 120000 },
    )
    .toBe("succeeded");
  const completed = await (await request.get(`/api/jobs/${job.id}`)).json();
  expect(completed.request.mode).toBe("real_portfolio");
  expect(completed.result.provenance.synthetic).toBe(false);
  expect(completed.result.provenance.portfolio_geometry).toBe(
    "identity_signal_projection_not_estimated_risk",
  );
  expect(completed.result.provenance.components).toHaveLength(2);
  expect(completed.result.provenance.instrument_order).toHaveLength(128);
  expect(completed.result.provenance.historical_vintage_verified).toBe(false);
  expect(completed.result.portfolio_allocations.length).toBeGreaterThan(5);
  const orders = completed.result.provenance.instrument_order;
  for (const row of completed.result.portfolio_allocations) {
    expect(row.execution_date > row.decision_date).toBe(true);
    expect(Math.abs(row.target_net)).toBeLessThanOrEqual(1e-6);
    expect(row.target_gross).toBeLessThanOrEqual(1 + 1e-6);
    expect(row.target_max_name).toBeLessThanOrEqual(0.05 + 1e-6);
    expect(row.target_turnover).toBeLessThanOrEqual(1 + 1e-6);
    expect(row.target_weights).toHaveLength(orders.length);
    expect(row.pretrade_held_weights).toHaveLength(orders.length);
    expect(row.achieved_posttrade_weights).toHaveLength(orders.length);
    expect(Number.isFinite(row.solver_primal_residual)).toBe(true);
    expect(Number.isFinite(row.solver_dual_residual)).toBe(true);
  }
  await page.getByRole("button", { name: "Windows", exact: false }).click();
  await page
    .getByRole("button", { name: "Portfolio construction", exact: true })
    .click();
  await page.getByRole("button", { name: "Maximize active panel" }).click();
  await expect(page.locator(".portfolio-panel table tbody tr")).toHaveCount(
    completed.result.portfolio_allocations.length,
  );
  await expect(page.locator(".portfolio-panel")).toContainText(
    "identity geometry",
  );
  await page.screenshot({ path: "artifacts/workstation-portfolio-live.png" });
});
