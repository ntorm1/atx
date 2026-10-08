import { randomUUID } from "node:crypto";
import { test, expect } from "@playwright/test";
test("native standalone review freezes successful alpha evidence and records acknowledgement and withdrawal", async ({
  page,
  request,
}) => {
  test.skip(
    process.env.ATX_E2E_GOVERNANCE_LIVE !== "1",
    "Requires current native governance service",
  );
  const session = await (await request.get("/api/session")).json();
  expect(session.team_mode).toBe(false);
  expect(session.principal.role).toBe("admin");
  const alphaResponse = await request.post("/api/alphas", {
    data: {
      name: `Browser review ${randomUUID().slice(0, 8)}`,
      description: "Disposable browser qualification",
      tags: ["browser-qualification"],
      hypothesis: "Synthetic validation evidence, no investment certification.",
      project_id: "local",
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
    const jobResponse = await request.post("/api/jobs", {
      data: {
        alpha_id: alpha.id,
        alpha_revision: alpha.revision,
        label: "Native review evidence",
        idempotency_key: randomUUID(),
      },
    });
    expect(jobResponse.status()).toBe(201);
    const job = await jobResponse.json();
    await expect
      .poll(
        async () =>
          (await (await request.get(`/api/jobs/${job.id}`)).json()).status,
      )
      .toBe("succeeded");
    await page.goto("/?panel=reviews");
    await expect(
      page.getByRole("button", { name: "Local operator / admin" }),
    ).toBeVisible();
    await page.getByRole("button", { name: "Maximize active panel" }).click();
    await page
      .getByLabel("Review alpha", { exact: true })
      .selectOption(alpha.id);
    await expect(page.getByLabel("Review alpha revision")).toHaveValue(
      alpha.revision,
    );
    await page.getByLabel("Native review evidence").check();
    await page
      .getByLabel("Review submission note")
      .fill(
        "Native synthetic engine evidence reviewed as system validation only.",
      );
    await page
      .getByRole("button", { name: "Submit review", exact: true })
      .click();
    await expect(
      page.getByText("submitted / r1", { exact: true }),
    ).toBeVisible();
    await page
      .getByLabel("Review decision rationale")
      .fill(
        "Standalone acknowledgement; synthetic evidence is not capacity qualified.",
      );
    await page.getByRole("button", { name: "Approve evidence" }).click();
    await expect(
      page.getByText("approved / r2", { exact: true }),
    ).toBeVisible();
    await page
      .getByText("Frozen native evidence JSON", { exact: true })
      .click();
    await expect(page.locator(".review-evidence pre")).toContainText(job.id);
    await expect(page.locator(".review-evidence pre")).toContainText(
      alpha.request_sha256,
    );
    await page.screenshot({ path: "artifacts/workstation-reviews-live.png" });
    await page
      .getByLabel("Review decision rationale")
      .fill("Withdraw disposable qualification acknowledgement.");
    await page.getByRole("button", { name: "Withdraw review" }).click();
    await expect(
      page.getByText("withdrawn / r3", { exact: true }),
    ).toBeVisible();
  } finally {
    await request.post(`/api/alphas/${alpha.id}/archive`, {
      data: { expected_revision: alpha.revision, archived: true },
    });
  }
});
