import assert from "node:assert/strict";
import test from "node:test";
import {
  claimOpeningEvents,
  createDemoState,
  ingestExternalEvents,
  purchaseGoal,
  toMinorUnits,
} from "../app/lib/model.ts";

test("currency conversion rejects fractions smaller than one cent", () => {
  assert.equal(toMinorUnits(19.99), 1999);
  assert.equal(toMinorUnits(0.01), 1);
  assert.throws(() => toMinorUnits(1.001), /两位小数/);
  assert.throws(() => toMinorUnits(Number.POSITIVE_INFINITY), /大于 0/);
});

test("opening events can only be claimed once", () => {
  const demo = createDemoState();
  const first = claimOpeningEvents(demo);
  const second = claimOpeningEvents(first.state);

  assert.deepEqual(first.eventIds, ["demo-opening-10"]);
  assert.deepEqual(second.eventIds, []);
  assert.ok(first.state.events[0].presentationStartedAt);
  assert.equal(first.state.events[0].shownAt, null);
});

test("purchase uses the confirmed goal snapshot and preserves lifetime totals", () => {
  const demo = createDemoState();
  const funded = { ...demo, currentBalanceMinor: 30_000 };
  const confirmed = funded.goals[0];
  const edited = {
    ...funded,
    goals: funded.goals.map((goal) =>
      goal.id === confirmed.id ? { ...goal, targetMinor: goal.targetMinor + 100 } : goal,
    ),
  };

  assert.throws(
    () => purchaseGoal(edited, confirmed.id, confirmed),
    /其他页面更新/,
  );

  const purchased = purchaseGoal(funded, confirmed.id, confirmed);
  assert.equal(
    purchased.currentBalanceMinor,
    funded.currentBalanceMinor - confirmed.targetMinor,
  );
  assert.equal(purchased.lifetimeSavedMinor, funded.lifetimeSavedMinor);
  assert.equal(purchased.lifetimePecks, funded.lifetimePecks);
  assert.equal(purchased.goals.some((goal) => goal.id === confirmed.id), false);
  assert.equal(purchased.purchases[0].goalId, confirmed.id);
});

test("event adapters stamp their own source and reject duplicate sequences", () => {
  const demo = createDemoState();
  const event = {
    eventId: "import-1",
    source: "ble",
    deviceId: "PECKY-TEST",
    sequence: 1,
    peckCount: 3,
    amountDelta: 1.5,
    occurredAt: "2026-08-28T10:00:00.000Z",
  };

  const first = ingestExternalEvents(demo, [event], "json");
  const second = ingestExternalEvents(first.state, [{ ...event, eventId: "import-2" }], "json");

  assert.equal(first.added, 1);
  assert.equal(first.state.events.at(-1).source, "json");
  assert.equal(second.added, 0);
  assert.equal(second.duplicates, 1);
});
