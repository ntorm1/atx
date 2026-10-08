import { test, expect } from "@playwright/test";
test.use({ baseURL: "http://127.0.0.1:3002" });
test.afterEach(async ({ request }) => {
  await request.post("/fixture/team", {
    data: { role: "admin", actor_id: "local", team_mode: false },
  });
});
test("configured session expiry disables submission immediately and marks the cached identity expired", async ({
  page,
  request,
}) => {
  await request.post("/fixture/team", {
    data: {
      role: "researcher",
      actor_id: "timed-researcher",
      name: "Expiring researcher",
      team_mode: true,
      expires_at_ms: String(Date.now() + 2500),
    },
  });
  await page.goto("/");
  await expect(
    page.getByRole("button", {
      name: "Expiring researcher / researcher",
      exact: true,
    }),
  ).toBeVisible();
  await expect(
    page.getByRole("button", { name: "▶ Run research" }),
  ).toBeEnabled();
  await expect(
    page.getByRole("button", {
      name: "Expiring researcher / researcher / expired",
      exact: true,
    }),
  ).toBeVisible({ timeout: 5000 });
  await expect(
    page.getByRole("button", { name: "▶ Run research" }),
  ).toBeDisabled();
  await page.getByRole("button", { name: "Team", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("credential has expired");
});
test("viewer identity disables durable mutations while keeping database and audit browsing available", async ({
  page,
  request,
}) => {
  await request.post("/fixture/team", {
    data: {
      role: "viewer",
      actor_id: "reader",
      name: "Read-only member",
      team_mode: true,
      seed: true,
    },
  });
  await page.goto("/");
  await expect(
    page.getByRole("button", { name: "Read-only member / viewer" }),
  ).toBeVisible();
  await expect(
    page.getByRole("button", { name: "▶ Run research" }),
  ).toBeDisabled();
  await page.getByRole("button", { name: "Workspace actions" }).click();
  await expect(
    page.getByRole("button", { name: "Save as…", exact: true }),
  ).toBeDisabled();
  await page.getByRole("button", { name: "Workspace actions" }).click();
  await page.getByRole("button", { name: "Team", exact: true }).click();
  await expect(
    page.getByRole("cell", { name: "reader", exact: true }),
  ).toBeVisible();
  await page.getByRole("button", { name: "Database", exact: true }).click();
  await expect(
    page.getByLabel("Database source", { exact: true }),
  ).toBeVisible();
});
test("review pins evidence and an independent reviewer resolves CAS conflicts without losing rationale", async ({
  page,
  request,
}) => {
  await request.post("/fixture/team", {
    data: {
      role: "researcher",
      actor_id: "author",
      team_mode: true,
      seed: true,
    },
  });
  await page.goto("/?panel=reviews");
  await expect(page.getByLabel("Review alpha revision")).toHaveValue("1");
  await page.getByLabel("Linked fixture evidence").check();
  await page
    .getByLabel("Review submission note")
    .fill("Inspect exact recipe and evidence.");
  await page
    .getByRole("button", { name: "Submit review", exact: true })
    .click();
  await expect(page.getByText("submitted / r1", { exact: true })).toBeVisible();
  await page
    .getByLabel("Review decision rationale")
    .fill("I am the original author.");
  await expect(
    page.getByRole("button", { name: "Approve evidence" }),
  ).toBeDisabled();
  await request.post("/fixture/team", {
    data: {
      role: "reviewer",
      actor_id: "independent",
      name: "Independent reviewer",
      team_mode: true,
    },
  });
  await page.reload();
  await page.getByRole("cell", { name: "submitted", exact: true }).click();
  await page
    .getByLabel("Review decision rationale")
    .fill("Evidence inspected; historical qualification remains unverified.");
  await request.post("/fixture/review-conflict");
  await page.getByRole("button", { name: "Approve evidence" }).click();
  await expect(page.getByRole("alert")).toContainText("revision changed");
  await page.getByRole("button", { name: "Reload review revision" }).click();
  await expect(page.getByText("submitted / r2", { exact: true })).toBeVisible();
  await expect(page.getByLabel("Review decision rationale")).toHaveValue(
    "Evidence inspected; historical qualification remains unverified.",
  );
  await page.getByRole("button", { name: "Approve evidence" }).click();
  await expect(page.getByText("approved / r3", { exact: true })).toBeVisible();
  await expect(
    page.getByRole("cell", { name: "independent", exact: true }),
  ).toBeVisible();
  await expect(
    page.getByRole("button", { name: "Withdraw review" }),
  ).toBeDisabled();
  await page.getByText("Frozen native evidence JSON", { exact: true }).click();
  await expect(page.locator(".review-evidence pre")).toContainText(
    "recipe_sha256",
  );
  await page.getByRole("button", { name: "Team", exact: true }).click();
  await expect(
    page.getByRole("cell", { name: "review.submitted", exact: true }),
  ).toBeVisible();
  const stored = await page.evaluate(() => JSON.stringify(localStorage));
  expect(stored).not.toContain("Bearer");
});
