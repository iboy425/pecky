export type TabId = "jar" | "me";

export type RewardIconId =
  | "bag"
  | "electronics"
  | "ticket"
  | "jewelry"
  | "sports"
  | "toy"
  | "travel"
  | "gold"
  | "stocks";

export type EventSource = "mock" | "json" | "ble";

export interface PeckyEvent {
  eventId: string;
  source: EventSource;
  deviceId: string;
  sequence: number;
  peckCount: number;
  amountMinor: number;
  occurredAt: string;
  receivedAt: string;
  creditedAt: string;
  presentationStartedAt: string | null;
  shownAt: string | null;
}

export interface ExternalPeckyEvent {
  eventId: string;
  deviceId: string;
  sequence: number;
  peckCount: number;
  amountDelta: number;
  occurredAt: string;
}

export interface WishGoal {
  id: string;
  title: string;
  targetMinor: number;
  iconId: RewardIconId;
  createdAt: string;
}

export interface Purchase {
  id: string;
  goalId: string;
  title: string;
  iconId: RewardIconId;
  amountMinor: number;
  purchasedAt: string;
}

export interface PeckyProfile {
  nickname: string;
  handle: string;
  motto: string;
}

export interface PeckySettings {
  soundEnabled: boolean;
}

export interface PeckyState {
  schemaVersion: 1;
  currentBalanceMinor: number;
  lifetimeSavedMinor: number;
  lifetimePecks: number;
  firstUsedAt: string;
  profile: PeckyProfile;
  settings: PeckySettings;
  goals: WishGoal[];
  purchases: Purchase[];
  events: PeckyEvent[];
  sourceCursors: Partial<Record<EventSource, string>>;
  demoSeedDisabled: boolean;
}

export interface IngestResult {
  state: PeckyState;
  added: number;
  duplicates: number;
  invalid: string[];
  addedPecks: number;
  addedAmountMinor: number;
}

export interface AchievementDefinition {
  id: "first-grain" | "hundred-pecks" | "wish-achieved";
  title: string;
  description: string;
  image: string;
  unlocked: boolean;
}

export const rewardOptions: Array<{
  id: RewardIconId;
  label: string;
  image: string;
}> = [
  { id: "bag", label: "包包", image: "/assets/rewards/bag.webp" },
  { id: "electronics", label: "电子产品", image: "/assets/rewards/electronics.webp" },
  { id: "ticket", label: "门票", image: "/assets/rewards/ticket.webp" },
  { id: "jewelry", label: "珠宝", image: "/assets/rewards/jewelry.webp" },
  { id: "sports", label: "运动", image: "/assets/rewards/sports.webp" },
  { id: "toy", label: "玩偶", image: "/assets/rewards/toy.webp" },
  { id: "travel", label: "旅行", image: "/assets/rewards/travel.webp" },
  { id: "gold", label: "黄金", image: "/assets/rewards/gold.webp" },
  { id: "stocks", label: "股票", image: "/assets/rewards/stocks.webp" },
];

const rewardIds = new Set<RewardIconId>(rewardOptions.map((option) => option.id));

const yuanFormatter = new Intl.NumberFormat("zh-CN", {
  style: "currency",
  currency: "CNY",
  minimumFractionDigits: 0,
  maximumFractionDigits: 2,
});

export function formatMoney(amountMinor: number): string {
  if (!Number.isSafeInteger(amountMinor)) return "¥0";
  return yuanFormatter.format(amountMinor / 100).replace("CN¥", "¥");
}

export function formatGoalPrice(amountMinor: number): string {
  const value = amountMinor / 100;
  return `${new Intl.NumberFormat("zh-CN", {
    minimumFractionDigits: Number.isInteger(value) ? 0 : 2,
    maximumFractionDigits: 2,
  }).format(value)}元`;
}

export function toMinorUnits(value: number): number {
  if (!Number.isFinite(value) || value <= 0) {
    throw new Error("金额必须大于 0");
  }
  const scaled = value * 100;
  if (Math.abs(scaled - Math.round(scaled)) > 0.000001) {
    throw new Error("金额最多保留两位小数");
  }
  const result = Math.round(scaled);
  if (!Number.isSafeInteger(result) || result <= 0) {
    throw new Error("金额超出支持范围");
  }
  return result;
}

export function goalProgress(balanceMinor: number, targetMinor: number): number {
  if (targetMinor <= 0) return 0;
  return Math.min(Math.max(balanceMinor / targetMinor, 0), 1);
}

export function daysTogether(firstUsedAt: string): number {
  const start = new Date(firstUsedAt).getTime();
  if (!Number.isFinite(start)) return 1;
  return Math.max(1, Math.floor((Date.now() - start) / 86_400_000) + 1);
}

export function createId(prefix: string): string {
  if (typeof crypto !== "undefined" && "randomUUID" in crypto) {
    return `${prefix}-${crypto.randomUUID()}`;
  }
  return `${prefix}-${Date.now()}-${Math.random().toString(36).slice(2)}`;
}

export function createBlankState(options?: { disableDemo?: boolean }): PeckyState {
  return {
    schemaVersion: 1,
    currentBalanceMinor: 0,
    lifetimeSavedMinor: 0,
    lifetimePecks: 0,
    firstUsedAt: new Date().toISOString(),
    profile: {
      nickname: "小米",
      handle: "@littlerice",
      motto: "慢慢攒，愿望会实现。",
    },
    settings: { soundEnabled: false },
    goals: [],
    purchases: [],
    events: [],
    sourceCursors: {},
    demoSeedDisabled: options?.disableDemo ?? false,
  };
}

export function createDemoState(): PeckyState {
  const now = new Date();
  const firstUsed = new Date(now.getTime() - 127 * 86_400_000);
  const occurred = new Date(now.getTime() - 3_600_000).toISOString();

  return {
    schemaVersion: 1,
    currentBalanceMinor: 16_800,
    lifetimeSavedMinor: 268_000,
    lifetimePecks: 536,
    firstUsedAt: firstUsed.toISOString(),
    profile: {
      nickname: "小米",
      handle: "@littlerice",
      motto: "慢慢攒，愿望会实现。",
    },
    settings: { soundEnabled: false },
    goals: [
      { id: "demo-toy", title: "玩偶", targetMinor: 19_900, iconId: "toy", createdAt: "2026-08-20T09:00:00.000Z" },
      { id: "demo-ticket", title: "演唱会门票", targetMinor: 68_000, iconId: "ticket", createdAt: "2026-08-18T09:00:00.000Z" },
      { id: "demo-travel", title: "旅行", targetMinor: 300_000, iconId: "travel", createdAt: "2026-08-12T09:00:00.000Z" },
      { id: "demo-stocks", title: "股票", targetMinor: 1_000_000, iconId: "stocks", createdAt: "2026-08-08T09:00:00.000Z" },
    ],
    purchases: [
      { id: "demo-purchase-ticket", goalId: "archived-ticket", title: "演唱会之夜", iconId: "ticket", amountMinor: 89_900, purchasedAt: "2026-08-18T12:00:00.000Z" },
      { id: "demo-purchase-toy", goalId: "archived-toy", title: "心愿玩偶", iconId: "toy", amountMinor: 32_000, purchasedAt: "2026-06-02T12:00:00.000Z" },
      { id: "demo-purchase-milk-tea", goalId: "archived-milk-tea", title: "周五的奶茶", iconId: "electronics", amountMinor: 3_200, purchasedAt: "2026-05-16T12:00:00.000Z" },
    ],
    events: [
      {
        eventId: "demo-opening-10",
        source: "mock",
        deviceId: "PECKY-DEMO",
        sequence: 536,
        peckCount: 10,
        amountMinor: 1_000,
        occurredAt: occurred,
        receivedAt: now.toISOString(),
        creditedAt: now.toISOString(),
        presentationStartedAt: null,
        shownAt: null,
      },
    ],
    sourceCursors: {},
    demoSeedDisabled: false,
  };
}

export function normalizePeckyState(value: PeckyState): PeckyState {
  return {
    ...value,
    schemaVersion: 1,
    profile: value.profile ?? createBlankState().profile,
    settings: value.settings ?? { soundEnabled: false },
    goals: Array.isArray(value.goals) ? value.goals : [],
    purchases: Array.isArray(value.purchases) ? value.purchases : [],
    events: Array.isArray(value.events)
      ? value.events.map((event) => ({
          ...event,
          presentationStartedAt: event.presentationStartedAt ?? null,
          shownAt: event.shownAt ?? null,
        }))
      : [],
    sourceCursors: value.sourceCursors ?? {},
    demoSeedDisabled: Boolean(value.demoSeedDisabled),
  };
}

export function getAchievements(state: PeckyState): AchievementDefinition[] {
  return [
    { id: "first-grain", title: "第一粒米", description: "完成第一次啄米", image: "/assets/achievements/first-grain.webp", unlocked: state.lifetimePecks >= 1 },
    { id: "hundred-pecks", title: "百次啄米", description: "累计完成100次啄米", image: "/assets/achievements/hundred-pecks.webp", unlocked: state.lifetimePecks >= 100 },
    { id: "wish-achieved", title: "清闲椅子", description: "解锁一把清闲椅子", image: "/assets/achievements/wish-achieved.webp", unlocked: state.purchases.length >= 1 },
  ];
}

export function ingestExternalEvents(
  current: PeckyState,
  candidates: unknown[],
  adapterSource: EventSource = "json",
): IngestResult {
  const next = structuredClone(current);
  const invalid: string[] = [];
  let added = 0;
  let duplicates = 0;
  let addedPecks = 0;
  let addedAmountMinor = 0;
  const knownIds = new Set(next.events.map((event) => event.eventId));
  const knownSequences = new Set(next.events.map((event) => `${event.deviceId}:${event.sequence}`));

  candidates.forEach((candidate, index) => {
    if (!candidate || typeof candidate !== "object") {
      invalid.push(`第 ${index + 1} 条不是有效对象`);
      return;
    }

    const raw = candidate as Partial<ExternalPeckyEvent>;
    const eventId = typeof raw.eventId === "string" ? raw.eventId.trim() : "";
    const deviceId = typeof raw.deviceId === "string" ? raw.deviceId.trim() : "";
    const sequence = Number(raw.sequence);
    const peckCount = Number(raw.peckCount);
    const amountDelta = Number(raw.amountDelta);
    const occurredAt = typeof raw.occurredAt === "string" ? raw.occurredAt : "";
    const sequenceKey = `${deviceId}:${sequence}`;

    let amountMinor = 0;
    try {
      amountMinor = toMinorUnits(amountDelta);
    } catch {
      amountMinor = 0;
    }

    if (
      !eventId ||
      !deviceId ||
      !Number.isSafeInteger(sequence) ||
      sequence < 0 ||
      !Number.isSafeInteger(peckCount) ||
      peckCount <= 0 ||
      amountMinor <= 0 ||
      !occurredAt ||
      Number.isNaN(Date.parse(occurredAt))
    ) {
      invalid.push(`第 ${index + 1} 条缺少字段或数值无效`);
      return;
    }

    if (knownIds.has(eventId) || knownSequences.has(sequenceKey)) {
      duplicates += 1;
      return;
    }

    const nextBalance = next.currentBalanceMinor + amountMinor;
    const nextLifetime = next.lifetimeSavedMinor + amountMinor;
    const nextPecks = next.lifetimePecks + peckCount;
    if (
      !Number.isSafeInteger(nextBalance) ||
      !Number.isSafeInteger(nextLifetime) ||
      !Number.isSafeInteger(nextPecks)
    ) {
      invalid.push(`第 ${index + 1} 条会使累计数值超出支持范围`);
      return;
    }

    const timestamp = new Date().toISOString();
    next.events.push({
      eventId,
      source: adapterSource,
      deviceId,
      sequence,
      peckCount,
      amountMinor,
      occurredAt: new Date(occurredAt).toISOString(),
      receivedAt: timestamp,
      creditedAt: timestamp,
      presentationStartedAt: null,
      shownAt: null,
    });
    knownIds.add(eventId);
    knownSequences.add(sequenceKey);
    next.currentBalanceMinor = nextBalance;
    next.lifetimeSavedMinor = nextLifetime;
    next.lifetimePecks = nextPecks;
    added += 1;
    addedPecks += peckCount;
    addedAmountMinor += amountMinor;
  });

  return { state: next, added, duplicates, invalid, addedPecks, addedAmountMinor };
}

export function claimOpeningEvents(state: PeckyState): {
  state: PeckyState;
  eventIds: string[];
} {
  const eventIds = state.events
    .filter((event) => !event.presentationStartedAt && !event.shownAt)
    .map((event) => event.eventId);
  if (eventIds.length === 0) return { state, eventIds };
  const ids = new Set(eventIds);
  const presentationStartedAt = new Date().toISOString();
  return {
    eventIds,
    state: {
      ...state,
      events: state.events.map((event) =>
        ids.has(event.eventId)
          ? { ...event, presentationStartedAt }
          : event,
      ),
    },
  };
}

export function completeOpeningEvents(state: PeckyState, eventIds: string[]): PeckyState {
  const ids = new Set(eventIds);
  const shownAt = new Date().toISOString();
  return {
    ...state,
    events: state.events.map((event) =>
      ids.has(event.eventId) && !event.shownAt ? { ...event, shownAt } : event,
    ),
  };
}

export function saveGoal(
  state: PeckyState,
  input: { id?: string; title: string; targetMinor: number; iconId: RewardIconId },
): PeckyState {
  const title = input.title.trim();
  if (!title || title.length > 24) throw new Error("愿望名称需为 1–24 个字");
  if (!Number.isSafeInteger(input.targetMinor) || input.targetMinor <= 0) {
    throw new Error("请输入有效的目标金额");
  }
  if (!rewardIds.has(input.iconId)) throw new Error("请选择奖励图标");

  if (input.id) {
    if (!state.goals.some((goal) => goal.id === input.id)) {
      throw new Error("这个愿望已经不存在了");
    }
    return {
      ...state,
      goals: state.goals.map((goal) =>
        goal.id === input.id
          ? { ...goal, title, targetMinor: input.targetMinor, iconId: input.iconId }
          : goal,
      ),
    };
  }

  return {
    ...state,
    goals: [
      ...state.goals,
      { id: createId("goal"), title, targetMinor: input.targetMinor, iconId: input.iconId, createdAt: new Date().toISOString() },
    ],
  };
}

function goalMatchesSnapshot(goal: WishGoal, snapshot: WishGoal): boolean {
  return (
    goal.id === snapshot.id &&
    goal.title === snapshot.title &&
    goal.targetMinor === snapshot.targetMinor &&
    goal.iconId === snapshot.iconId
  );
}

export function deleteGoal(
  state: PeckyState,
  goalId: string,
  expected?: WishGoal,
): PeckyState {
  const goal = state.goals.find((item) => item.id === goalId);
  if (!goal) {
    throw new Error("这个愿望已经不存在了");
  }
  if (expected && !goalMatchesSnapshot(goal, expected)) {
    throw new Error("愿望已在其他页面更新，请重新确认");
  }
  return { ...state, goals: state.goals.filter((goal) => goal.id !== goalId) };
}

export function purchaseGoal(
  state: PeckyState,
  goalId: string,
  expected?: WishGoal,
): PeckyState {
  const goal = state.goals.find((item) => item.id === goalId);
  if (!goal) throw new Error("这个愿望已经不存在了");
  if (expected && !goalMatchesSnapshot(goal, expected)) {
    throw new Error("愿望已在其他页面更新，请重新确认");
  }
  if (!Number.isSafeInteger(goal.targetMinor) || goal.targetMinor <= 0) {
    throw new Error("愿望金额无效，请先编辑后再购买");
  }
  if (!Number.isSafeInteger(state.currentBalanceMinor) || state.currentBalanceMinor < 0) {
    throw new Error("米罐余额数据无效");
  }
  if (state.currentBalanceMinor < goal.targetMinor) {
    throw new Error("米罐余额还不够完成这个愿望");
  }

  return {
    ...state,
    currentBalanceMinor: state.currentBalanceMinor - goal.targetMinor,
    goals: state.goals.filter((item) => item.id !== goalId),
    purchases: [
      { id: createId("purchase"), goalId: goal.id, title: goal.title, iconId: goal.iconId, amountMinor: goal.targetMinor, purchasedAt: new Date().toISOString() },
      ...state.purchases,
    ],
  };
}
