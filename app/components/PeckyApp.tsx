"use client";

/* eslint-disable @next/next/no-img-element -- Approved local artwork is already sized and compressed for this fixed mobile canvas. */

import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type ChangeEvent,
  type FormEvent,
  type RefObject,
  type ReactNode,
} from "react";
import {
  claimOpeningEvents,
  completeOpeningEvents,
  createBlankState,
  daysTogether,
  deleteGoal,
  formatGoalPrice,
  formatMoney,
  getAchievements,
  goalProgress,
  ingestExternalEvents,
  purchaseGoal,
  rewardOptions,
  saveGoal,
  toMinorUnits,
  type PeckyProfile,
  type PeckyState,
  type RewardIconId,
  type TabId,
  type WishGoal,
} from "../lib/model";
import {
  clearLocalState,
  loadPeckyState,
  replaceWithDemoState,
  subscribeToPeckyState,
  updatePeckyState,
} from "../lib/storage";
import {
  JsonImportDataSource,
  MockPeckyDataSource,
  sampleImportPayload,
  type PeckyEventBatch,
  type PeckyDataSource,
} from "../lib/sources";
import { PeckyIcon } from "./PeckyIcons";

type OpeningPhase = "booting" | "opening" | "collapsing" | "ready";

interface OpeningBatch {
  eventIds: string[];
  pecks: number;
  amountMinor: number;
}

interface BootResult {
  state: PeckyState;
  batch: OpeningBatch | null;
}

type ModalState =
  | { kind: "goal"; goal?: WishGoal }
  | { kind: "goals" }
  | { kind: "profile" }
  | { kind: "settings" }
  | { kind: "achievements" }
  | { kind: "purchases" }
  | null;

type ConfirmationState =
  | { kind: "purchase"; goal: WishGoal }
  | { kind: "delete-goal"; goal: WishGoal }
  | { kind: "clear" }
  | { kind: "demo" }
  | null;

interface BeforeInstallPromptEvent extends Event {
  prompt(): Promise<void>;
  userChoice: Promise<{ outcome: "accepted" | "dismissed" }>;
}

const rewardImages = new Map(
  rewardOptions.map((option) => [option.id, option.image]),
);

function imageForReward(iconId: RewardIconId): string {
  return rewardImages.get(iconId) ?? "/assets/rewards/gold.webp";
}

function dateLabel(value: string): string {
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return "日期未知";
  return new Intl.DateTimeFormat("zh-CN", {
    year: "numeric",
    month: "long",
    day: "numeric",
  }).format(date);
}

function getSeedDemoMode(): boolean {
  if (process.env.NODE_ENV === "development") return true;
  if (typeof window === "undefined") return false;
  return new URLSearchParams(window.location.search).get("demo") === "1";
}

export function PeckyApp() {
  const [seedDemo] = useState(getSeedDemoMode);
  const [state, setState] = useState<PeckyState | null>(null);
  const [tab, setTab] = useState<TabId>("jar");
  const [openingPhase, setOpeningPhase] = useState<OpeningPhase>("booting");
  const [openingBatch, setOpeningBatch] = useState<OpeningBatch | null>(null);
  const [modal, setModal] = useState<ModalState>(null);
  const [confirmation, setConfirmation] = useState<ConfirmationState>(null);
  const [toast, setToast] = useState<{ message: string; tone: "info" | "success" | "error" } | null>(null);
  const [installPrompt, setInstallPrompt] = useState<BeforeInstallPromptEvent | null>(null);
  const finishingOpening = useRef(false);
  const bootPromise = useRef<Promise<BootResult> | null>(null);
  const openingHeroRef = useRef<HTMLDivElement>(null);
  const homeHeroRef = useRef<HTMLDivElement>(null);
  const rewardPauseTimer = useRef<number | null>(null);
  const transitionEndTimer = useRef<number | null>(null);

  const showToast = useCallback(
    (message: string, tone: "info" | "success" | "error" = "info") => {
      setToast({ message, tone });
    },
    [],
  );

  useEffect(() => {
    if (!toast) return;
    const timeout = window.setTimeout(() => setToast(null), 3_600);
    return () => window.clearTimeout(timeout);
  }, [toast]);

  useEffect(() => {
    let active = true;

    if (!bootPromise.current) {
      let claimedBatch: OpeningBatch | null = null;
      bootPromise.current = updatePeckyState(
        (current) => {
          const reconciled: PeckyState = {
            ...current,
            events: current.events.map((event) =>
              event.presentationStartedAt && !event.shownAt
                ? { ...event, shownAt: event.presentationStartedAt }
                : event,
            ),
          };
          const claimed = claimOpeningEvents(reconciled);
          const ids = new Set(claimed.eventIds);
          const selected = reconciled.events.filter((event) => ids.has(event.eventId));
          if (selected.length > 0) {
            claimedBatch = {
              eventIds: claimed.eventIds,
              pecks: selected.reduce((sum, event) => sum + event.peckCount, 0),
              amountMinor: selected.reduce((sum, event) => sum + event.amountMinor, 0),
            };
          }
          return claimed.state;
        },
        { seedDemo },
      ).then((next) => ({ state: next, batch: claimedBatch }));
    }

    bootPromise.current
      .then(({ state: next, batch }) => {
        if (!active) return;
        setState(next);
        if (batch) {
          setOpeningBatch(batch);
          setOpeningPhase("opening");
        } else {
          setOpeningPhase("ready");
        }
      })
      .catch((error: unknown) => {
        if (!active) return;
        setState(createBlankState({ disableDemo: true }));
        setOpeningPhase("ready");
        showToast(error instanceof Error ? error.message : "本地数据加载失败", "error");
      });

    return () => {
      active = false;
    };
  }, [seedDemo, showToast]);

  useEffect(() => {
    return subscribeToPeckyState(() => {
      loadPeckyState({ seedDemo }).then(setState).catch(() => undefined);
    });
  }, [seedDemo]);

  useEffect(() => {
    const onBeforeInstall = (event: Event) => {
      event.preventDefault();
      setInstallPrompt(event as BeforeInstallPromptEvent);
    };
    window.addEventListener("beforeinstallprompt", onBeforeInstall);
    return () => window.removeEventListener("beforeinstallprompt", onBeforeInstall);
  }, []);

  useEffect(() => {
    if (process.env.NODE_ENV !== "production" || !("serviceWorker" in navigator)) return;
    navigator.serviceWorker.register("/sw.js").catch(() => undefined);
  }, []);

  useEffect(() => {
    if (!modal && !confirmation) return;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key !== "Escape") return;
      if (confirmation) setConfirmation(null);
      else setModal(null);
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [confirmation, modal]);

  const finishOpening = useCallback(() => {
    if (!openingBatch || finishingOpening.current) return;
    finishingOpening.current = true;

    if (rewardPauseTimer.current !== null) {
      window.clearTimeout(rewardPauseTimer.current);
      rewardPauseTimer.current = null;
    }

    const sourceHero = openingHeroRef.current;
    const targetHero = homeHeroRef.current;
    const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

    if (!reducedMotion && sourceHero && targetHero) {
      const sourceRect = sourceHero.getBoundingClientRect();
      const targetRect = targetHero.getBoundingClientRect();
      const scale = Math.max(
        targetRect.width / sourceRect.width,
        targetRect.height / sourceRect.height,
      );
      const visibleWidth = targetRect.width / scale;
      const visibleHeight = targetRect.height / scale;
      const clipX = Math.max((sourceRect.width - visibleWidth) / 2, 0);
      const clipY = Math.max((sourceRect.height - visibleHeight) / 2, 0);
      const shiftX = targetRect.left - sourceRect.left - clipX * scale;
      const shiftY = targetRect.top - sourceRect.top - clipY * scale;
      const targetRadius = Number.parseFloat(getComputedStyle(targetHero).borderRadius) || 22;

      sourceHero.style.setProperty("--hero-shift-x", `${shiftX}px`);
      sourceHero.style.setProperty("--hero-shift-y", `${shiftY}px`);
      sourceHero.style.setProperty("--hero-scale", String(scale));
      sourceHero.style.setProperty(
        "--hero-final-clip",
        `inset(${clipY}px ${clipX}px ${clipY}px ${clipX}px round ${targetRadius / scale}px)`,
      );
    }

    setOpeningPhase("collapsing");

    void updatePeckyState(
        (current) => completeOpeningEvents(current, openingBatch.eventIds),
        { seedDemo },
      )
      .then(setState)
      .catch((error: unknown) => {
        showToast(error instanceof Error ? error.message : "开屏记录保存失败", "error");
      });

    transitionEndTimer.current = window.setTimeout(() => {
      setOpeningPhase("ready");
      setOpeningBatch(null);
      transitionEndTimer.current = null;
    }, reducedMotion ? 90 : 940);
  }, [openingBatch, seedDemo, showToast]);

  const finishOpeningAfterReward = useCallback(() => {
    if (finishingOpening.current || rewardPauseTimer.current !== null) return;
    const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    rewardPauseTimer.current = window.setTimeout(() => {
      rewardPauseTimer.current = null;
      finishOpening();
    }, reducedMotion ? 20 : 100);
  }, [finishOpening]);

  useEffect(() => {
    if (openingPhase !== "opening") return;
    const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    const timeout = window.setTimeout(finishOpening, reducedMotion ? 80 : 7_200);
    return () => window.clearTimeout(timeout);
  }, [finishOpening, openingPhase]);

  useEffect(() => {
    return () => {
      if (rewardPauseTimer.current !== null) window.clearTimeout(rewardPauseTimer.current);
      if (transitionEndTimer.current !== null) window.clearTimeout(transitionEndTimer.current);
    };
  }, []);

  const commit = useCallback(
    async (mutate: (current: PeckyState) => PeckyState): Promise<PeckyState | null> => {
      try {
        const next = await updatePeckyState(mutate, { seedDemo });
        setState(next);
        return next;
      } catch (error) {
        showToast(error instanceof Error ? error.message : "操作失败，请再试一次", "error");
        return null;
      }
    },
    [seedDemo, showToast],
  );

  const handleSaveGoal = useCallback(
    async (input: { id?: string; title: string; targetMinor: number; iconId: RewardIconId }) => {
      const next = await commit((current) => saveGoal(current, input));
      if (!next) return false;
      setModal(null);
      showToast(input.id ? "愿望已更新" : "愿望已加入米罐", "success");
      return true;
    },
    [commit, showToast],
  );

  const handleSaveProfile = useCallback(
    async (profile: PeckyProfile) => {
      const nickname = profile.nickname.trim();
      const handle = profile.handle.trim();
      const motto = profile.motto.trim();
      if (!nickname || nickname.length > 12 || !handle || !motto || motto.length > 32) {
        showToast("请完整填写昵称、账号和一句话", "error");
        return false;
      }
      const next = await commit((current) => ({
        ...current,
        profile: {
          nickname,
          handle: handle.startsWith("@") ? handle : `@${handle}`,
          motto,
        },
      }));
      if (!next) return false;
      setModal(null);
      showToast("个人资料已保存", "success");
      return true;
    },
    [commit, showToast],
  );

  const ingestFromSource = useCallback(
    async (
      source: PeckyDataSource,
      options: { notify?: boolean } = {},
    ): Promise<boolean> => {
      let unsubscribe: () => void = () => undefined;
      let connected = false;
      let primaryFailure = false;
      let subscriptionQueue = Promise.resolve();
      let addedAnyEvents = false;

      const reportResult = (finalResult: ReturnType<typeof ingestExternalEvents>) => {
        if (finalResult.added > 0) {
          const ignored = finalResult.invalid.length + finalResult.duplicates;
          if (options.notify !== false) {
            showToast(
              `已入账 ${formatMoney(finalResult.addedAmountMinor)}（${finalResult.addedPecks} 次啄米）${ignored ? `，忽略 ${ignored} 条` : ""}。刷新页面或彻底关闭后重开，会播放一次开屏。`,
              "success",
            );
          }
          return true;
        } else if (finalResult.duplicates > 0 && finalResult.invalid.length === 0) {
          if (options.notify !== false) showToast("没有新数据：这些记录已经入账", "info");
        } else {
          if (options.notify !== false) {
            showToast(finalResult.invalid[0] ?? "数据包中没有可入账记录", "error");
          }
        }
        return false;
      };

      const applyBatch = async (batch: PeckyEventBatch) => {
        let result: ReturnType<typeof ingestExternalEvents> | null = null;
        const next = await commit((current) => {
          result = ingestExternalEvents(current, batch.events, source.id);
          const ingested = result.state;
          if (!batch.cursor) return ingested;
          return {
            ...ingested,
            sourceCursors: {
              ...ingested.sourceCursors,
              [source.id]: batch.cursor,
            },
          };
        });
        return next && result
          ? (result as ReturnType<typeof ingestExternalEvents>)
          : null;
      };

      try {
        const latest = await loadPeckyState({ seedDemo });
        await source.connect();
        connected = true;
        unsubscribe = source.subscribe((batch) => {
          subscriptionQueue = subscriptionQueue.then(async () => {
            const result = await applyBatch(batch);
            if (result) addedAnyEvents = reportResult(result) || addedAnyEvents;
          });
        });
        const batch = await source.pull(latest.sourceCursors[source.id]);
        const result = await applyBatch(batch);
        if (result) addedAnyEvents = reportResult(result) || addedAnyEvents;
        await subscriptionQueue;
      } catch (error) {
        primaryFailure = true;
        showToast(error instanceof Error ? error.message : "数据接收失败", "error");
      } finally {
        unsubscribe();
        if (connected) {
          try {
            await source.disconnect();
          } catch {
            if (!primaryFailure) showToast("数据已处理，但设备连接未完全关闭", "info");
          }
        }
      }

      return addedAnyEvents;
    },
    [commit, seedDemo, showToast],
  );

  const handleJsonFile = useCallback(
    async (file: File) => {
      if (file.size > 2_000_000) {
        showToast("JSON 文件不能超过 2MB", "error");
        return;
      }
      try {
        const payload = JSON.parse(await file.text()) as unknown;
        await ingestFromSource(new JsonImportDataSource(payload));
      } catch (error) {
        showToast(error instanceof Error ? error.message : "JSON 文件无法解析", "error");
      }
    },
    [ingestFromSource, showToast],
  );

  const simulateOpeningPreview = useCallback(
    async (pecks: number, amount: number): Promise<boolean> => {
      const received = await ingestFromSource(
        new MockPeckyDataSource(pecks, amount),
        { notify: false },
      );
      if (!received) return false;
      setToast(null);

      const result: { batch: OpeningBatch | null } = { batch: null };
      try {
        const next = await updatePeckyState(
          (current) => {
            const claimed = claimOpeningEvents(current);
            const ids = new Set(claimed.eventIds);
            const selected = current.events.filter((event) => ids.has(event.eventId));
            if (selected.length > 0) {
              result.batch = {
                eventIds: claimed.eventIds,
                pecks: selected.reduce((sum, event) => sum + event.peckCount, 0),
                amountMinor: selected.reduce((sum, event) => sum + event.amountMinor, 0),
              };
            }
            return claimed.state;
          },
          { seedDemo },
        );

        if (!result.batch) {
          showToast("数据已入账，但没有待播放的开屏记录", "info");
          return false;
        }

        setState(next);
        setTab("jar");
        setModal(null);
        finishingOpening.current = false;
        setOpeningBatch(result.batch);
        setOpeningPhase("opening");
        return true;
      } catch (error) {
        showToast(error instanceof Error ? error.message : "开屏准备失败", "error");
        return false;
      }
    },
    [ingestFromSource, seedDemo, showToast],
  );

  const handleInstall = useCallback(async () => {
    if (installPrompt) {
      await installPrompt.prompt();
      const choice = await installPrompt.userChoice;
      if (choice.outcome === "accepted") setInstallPrompt(null);
      return;
    }
    showToast("iPhone 请在 Safari 的分享菜单中选择“添加到主屏幕”", "info");
  }, [installPrompt, showToast]);

  const runConfirmation = useCallback(async () => {
    if (!confirmation) return;

    if (confirmation.kind === "purchase") {
      const next = await commit((current) =>
        purchaseGoal(current, confirmation.goal.id, confirmation.goal),
      );
      if (next) {
        showToast(`心愿达成：${confirmation.goal.title}`, "success");
      }
    }

    if (confirmation.kind === "delete-goal") {
      const next = await commit((current) =>
        deleteGoal(current, confirmation.goal.id, confirmation.goal),
      );
      if (next) {
        setModal(null);
        showToast("愿望已删除", "info");
      }
    }

    if (confirmation.kind === "clear") {
      try {
        const blank = await clearLocalState();
        setState(blank);
        setModal(null);
        showToast("本机数据已清空，将保持从 ¥0 开始", "success");
      } catch (error) {
        showToast(error instanceof Error ? error.message : "清空失败", "error");
      }
    }

    if (confirmation.kind === "demo") {
      try {
        const demo = await replaceWithDemoState();
        setState(demo);
        setModal(null);
        showToast("演示数据已载入；刷新页面或彻底关闭后重开，会播放 10 次啄米", "success");
      } catch (error) {
        showToast(error instanceof Error ? error.message : "载入失败", "error");
      }
    }

    setConfirmation(null);
  }, [commit, confirmation, showToast]);

  if (!state || openingPhase === "booting") {
    return (
      <main className="app-shell boot-screen" aria-busy="true">
        <img src="/assets/pecky-avatar.webp" alt="啄米小鸡" />
        <p>正在看看米罐…</p>
      </main>
    );
  }

  return (
    <main
      className="app-shell"
      data-opening-phase={openingPhase}
      data-opening-event-count={openingBatch?.eventIds.length ?? 0}
    >
      <div className="page-stage">
        {tab === "jar" ? (
          <JarPage
            state={state}
            artActive={openingPhase === "ready"}
            heroRef={homeHeroRef}
            onAdd={() => setModal({ kind: "goal" })}
            onEdit={(goal) => setModal({ kind: "goal", goal })}
            onPurchase={(goal) => setConfirmation({ kind: "purchase", goal })}
            onShowAll={() => setModal({ kind: "goals" })}
          />
        ) : (
          <MePage
            state={state}
            onEditProfile={() => setModal({ kind: "profile" })}
            onShowAchievements={() => setModal({ kind: "achievements" })}
            onShowPurchases={() => setModal({ kind: "purchases" })}
            onOpenSettings={() => setModal({ kind: "settings" })}
          />
        )}
      </div>

      <BottomNavigation
        current={tab}
        onChange={(next) => {
          setTab(next);
          window.scrollTo({ top: 0, behavior: "smooth" });
        }}
      />

      {openingBatch && openingPhase !== "ready" && (
        <OpeningExperience
          batch={openingBatch}
          phase={openingPhase}
          balanceMinor={state.currentBalanceMinor}
          soundEnabled={state.settings.soundEnabled}
          heroRef={openingHeroRef}
          onFinish={finishOpening}
          onRewardComplete={finishOpeningAfterReward}
        />
      )}

      {modal?.kind === "goal" && (
        <GoalEditor
          goal={modal.goal}
          onClose={() => setModal(null)}
          onSave={handleSaveGoal}
          onDelete={
            modal.goal
              ? () => setConfirmation({ kind: "delete-goal", goal: modal.goal! })
              : undefined
          }
        />
      )}

      {modal?.kind === "profile" && (
        <ProfileEditor
          profile={state.profile}
          onClose={() => setModal(null)}
          onSave={handleSaveProfile}
        />
      )}

      {modal?.kind === "achievements" && (
        <AchievementsSheet state={state} onClose={() => setModal(null)} />
      )}

      {modal?.kind === "goals" && (
        <GoalsSheet
          state={state}
          onClose={() => setModal(null)}
          onEdit={(goal) => setModal({ kind: "goal", goal })}
          onPurchase={(goal) => setConfirmation({ kind: "purchase", goal })}
        />
      )}

      {modal?.kind === "purchases" && (
        <PurchasesSheet state={state} onClose={() => setModal(null)} />
      )}

      {modal?.kind === "settings" && (
        <SettingsSheet
          state={state}
          onClose={() => setModal(null)}
          onSoundChange={async (enabled) => {
            await commit((current) => ({
              ...current,
              settings: { ...current.settings, soundEnabled: enabled },
            }));
          }}
          onSimulate={(pecks, amount) =>
            ingestFromSource(new MockPeckyDataSource(pecks, amount))
          }
          onPreview={simulateOpeningPreview}
          onJsonFile={handleJsonFile}
          onInstall={handleInstall}
          installAvailable={Boolean(installPrompt)}
          onLoadDemo={() => setConfirmation({ kind: "demo" })}
          onClear={() => setConfirmation({ kind: "clear" })}
        />
      )}

      {confirmation && (
        <ConfirmDialog
          confirmation={confirmation}
          onCancel={() => setConfirmation(null)}
          onConfirm={runConfirmation}
        />
      )}

      {toast && (
        <div className={`toast toast-${toast.tone}`} role="status">
          {toast.message}
        </div>
      )}
    </main>
  );
}

function OpeningExperience({
  batch,
  phase,
  balanceMinor,
  soundEnabled,
  heroRef,
  onFinish,
  onRewardComplete,
}: {
  batch: OpeningBatch;
  phase: OpeningPhase;
  balanceMinor: number;
  soundEnabled: boolean;
  heroRef: RefObject<HTMLDivElement | null>;
  onFinish: () => void;
  onRewardComplete: () => void;
}) {
  const videoRef = useRef<HTMLVideoElement>(null);
  const [muted, setMuted] = useState(!soundEnabled);
  const [videoFailed, setVideoFailed] = useState(false);

  const toggleSound = () => {
    const video = videoRef.current;
    const nextMuted = !muted;
    setMuted(nextMuted);
    if (video) {
      video.muted = nextMuted;
      video.play().catch(() => undefined);
    }
  };

  return (
    <section
      className={`opening-experience ${phase === "collapsing" ? "opening-collapsing" : ""}`}
      aria-label="本次啄米入账"
    >
      <div className="opening-toolbar">
        <button className="icon-button translucent" type="button" onClick={toggleSound} aria-label={muted ? "打开声音" : "静音"}>
          <PeckyIcon name={muted ? "muted" : "volume"} />
        </button>
        <button className="text-button opening-skip" type="button" onClick={onFinish}>
          跳过
        </button>
      </div>

      <div className="opening-copy">
        <h1>欢迎回来</h1>
        <p>今天又攒下了 {batch.pecks} 粒米</p>
      </div>

      <div className="opening-pill">+{batch.pecks} 粒米 · {formatMoney(batch.amountMinor)}</div>

      <div ref={heroRef} className={`opening-media ${videoFailed ? "opening-media-fallback" : ""}`}>
        {!videoFailed && (
          <video
            key={batch.eventIds.join(":")}
            ref={videoRef}
            src="/assets/media/pecky-opening.mp4?v=5"
            autoPlay
            playsInline
            muted={muted}
            preload="auto"
            poster="/assets/media/pecky-opening-poster.jpg?v=5"
            onEnded={onRewardComplete}
            onCanPlay={(event) => {
              event.currentTarget.play().catch(() => {
                event.currentTarget.muted = true;
                setMuted(true);
                event.currentTarget.play().catch(() => setVideoFailed(true));
              });
            }}
            onError={() => {
              setVideoFailed(true);
              window.setTimeout(onRewardComplete, 700);
            }}
          />
        )}
        {videoFailed && <img src="/assets/jar-scene.webp" alt="小鸡把米装进米罐" />}
      </div>

      <div className="opening-footer">
        <p>正在装进你的小米罐…</p>
        <span>每一粒，都在靠近小愿望。</span>
      </div>

      <div className="opening-jar-peek" aria-hidden="true">
        <span className="opening-sheet-handle" />
        <div className="opening-jar-summary">
          <div>
            <strong>我的米罐</strong>
            <span>上拉看看愿望</span>
          </div>
          <b>{formatMoney(balanceMinor)}</b>
        </div>
        <div className="opening-mini-nav">
          <div className="opening-mini-nav-active">
            <PeckyIcon name="jar" />
            <span>米罐</span>
          </div>
          <div>
            <PeckyIcon name="person" />
            <span>我的</span>
          </div>
        </div>
      </div>
    </section>
  );
}

function JarPage({
  state,
  artActive,
  heroRef,
  onAdd,
  onEdit,
  onPurchase,
  onShowAll,
}: {
  state: PeckyState;
  artActive: boolean;
  heroRef: RefObject<HTMLDivElement | null>;
  onAdd: () => void;
  onEdit: (goal: WishGoal) => void;
  onPurchase: (goal: WishGoal) => void;
  onShowAll: () => void;
}) {
  const goals = [...state.goals].sort((a, b) => a.targetMinor - b.targetMinor);

  return (
    <section className="page page-jar" aria-labelledby="jar-title">
      <header className="jar-header">
        <span className="collapse-handle" aria-hidden="true" />
        <p>和小鸡一起攒米</p>
        <span className="eyebrow">今天也在向愿望靠近</span>
        <h1 id="jar-title">我的米罐</h1>
      </header>

      <section className="balance-card" aria-label={`米罐余额 ${formatMoney(state.currentBalanceMinor)}`}>
        <div className="balance-copy">
          <span>已经攒下</span>
          <strong>{formatMoney(state.currentBalanceMinor)}</strong>
          <p>每一粒，都算数</p>
        </div>
        <JarBalanceArt heroRef={heroRef} active={artActive} />
      </section>

      <div className="jar-home-lower">
        <div className="section-heading">
          <div>
            <h2>想要的小奖励</h2>
            <p>{goals.length > 0 ? `${goals.length} 个愿望，共用这一罐米` : "同一份余额，一起靠近每个愿望"}</p>
          </div>
          <div className="section-heading-actions">
            {goals.length > 0 && <button className="link-button all-goals" type="button" onClick={onShowAll}>全部</button>}
            <button className="icon-button add-goal" type="button" onClick={onAdd} aria-label="添加愿望">
              <PeckyIcon name="plus" />
            </button>
          </div>
        </div>

        {goals.length === 0 ? (
          <div className="empty-card">
            <img src="/assets/rewards/gold.webp" alt="" />
            <h3>先放进一个小愿望吧</h3>
            <p>奶茶、玩偶、门票，想攒什么都可以。</p>
            <button className="primary-button" type="button" onClick={onAdd}>添加第一个愿望</button>
          </div>
        ) : (
          <div className="goal-list">
            {goals.slice(0, 4).map((goal) => {
            const progress = goalProgress(state.currentBalanceMinor, goal.targetMinor);
            const percentage = Math.round(progress * 100);
            const remaining = Math.max(goal.targetMinor - state.currentBalanceMinor, 0);
            const ready = remaining === 0;

            return (
              <article className={`goal-card ${ready ? "goal-ready" : ""}`} key={goal.id}>
                <div className="goal-card-top">
                  <img className="goal-icon" src={imageForReward(goal.iconId)} alt="" />
                  <button className="icon-button goal-edit" type="button" onClick={() => onEdit(goal)} aria-label={`编辑愿望：${goal.title}`}>
                    <PeckyIcon name="edit" />
                  </button>
                </div>
                <div className="goal-content">
                  <div className="goal-title-row">
                    <h3>{goal.title}</h3>
                  </div>
                  <div className="goal-meta">
                    <span>{ready ? "可以实现了" : `差 ${formatMoney(remaining)}`}</span>
                    <strong>{percentage}%</strong>
                  </div>
                  <div className="progress-track" aria-label={`已完成 ${percentage}%`}>
                    <span style={{ width: `${percentage}%` }} />
                  </div>
                  <span className="goal-price">目标 {formatGoalPrice(goal.targetMinor)}</span>
                  {ready && (
                    <button className="purchase-button" type="button" onClick={() => onPurchase(goal)}>
                      <PeckyIcon name="check" />
                      已购买
                    </button>
                  )}
                </div>
              </article>
            );
            })}
          </div>
        )}
      </div>
    </section>
  );
}

function JarBalanceArt({
  active,
  heroRef,
}: {
  active: boolean;
  heroRef: RefObject<HTMLDivElement | null>;
}) {
  const [videoFailed, setVideoFailed] = useState(false);

  return (
    <div ref={heroRef} className="balance-art" aria-hidden="true">
      {!active || videoFailed ? (
        <img src="/assets/jar-still.webp" alt="" />
      ) : (
        <video
          className="balance-art-video"
          src="/assets/media/pecky-orbit.mp4?v=4"
          autoPlay
          loop
          muted
          playsInline
          preload="auto"
          poster="/assets/jar-still.webp"
          onError={() => setVideoFailed(true)}
        />
      )}
    </div>
  );
}

function MePage({
  state,
  onEditProfile,
  onShowAchievements,
  onShowPurchases,
  onOpenSettings,
}: {
  state: PeckyState;
  onEditProfile: () => void;
  onShowAchievements: () => void;
  onShowPurchases: () => void;
  onOpenSettings: () => void;
}) {
  const achievements = getAchievements(state);

  return (
    <section className="page page-me" aria-labelledby="me-title">
      <header className="page-title">
        <h1 id="me-title">我的</h1>
      </header>

      <section className="profile-card">
        <img className="profile-avatar" src="/assets/pecky-avatar.webp" alt="小鸡头像" />
        <div className="profile-copy">
          <h2>{state.profile.nickname}</h2>
          <span>{state.profile.handle}</span>
          <p>{state.profile.motto}</p>
        </div>
        <button className="soft-button" type="button" onClick={onEditProfile}>编辑</button>
      </section>

      <section aria-labelledby="stats-title">
        <h2 className="content-heading" id="stats-title">一路积累</h2>
        <div className="stats-card">
          <div><strong>{formatMoney(state.lifetimeSavedMinor)}</strong><span>累计攒下</span></div>
          <div><strong>{state.lifetimePecks.toLocaleString("zh-CN")}</strong><span>啄米次数</span></div>
          <div><strong>{daysTogether(state.firstUsedAt)}</strong><span>陪伴天数</span></div>
        </div>
      </section>

      <section aria-labelledby="achievements-title">
        <div className="section-heading compact">
          <h2 id="achievements-title">我的成就</h2>
          <button className="link-button" type="button" onClick={onShowAchievements}>
            查看全部 <PeckyIcon name="chevron" />
          </button>
        </div>
        <div className="achievement-card">
          {achievements.map((achievement) => (
            <button
              className={`achievement-item ${achievement.unlocked ? "" : "achievement-locked"}`}
              type="button"
              key={achievement.id}
              onClick={onShowAchievements}
              aria-label={`${achievement.title}，${achievement.unlocked ? "已解锁" : "未解锁"}`}
            >
              <img src={achievement.image} alt="" />
              <strong>{achievement.title}</strong>
              <span>{achievement.unlocked ? "已解锁" : "继续加油"}</span>
            </button>
          ))}
        </div>
      </section>

      <section aria-labelledby="purchases-title">
        <div className="section-heading compact purchase-heading">
          <h2 id="purchases-title">已经实现的愿望</h2>
          {state.purchases.length > 0 && (
            <button className="link-button" type="button" onClick={onShowPurchases}>
              全部 <PeckyIcon name="chevron" />
            </button>
          )}
        </div>
        {state.purchases.length === 0 ? (
          <div className="empty-purchases">完成购买后，愿望会留在这里。</div>
        ) : (
          <div className="purchase-list">
            {state.purchases.slice(0, 2).map((purchase) => (
              <article className="purchase-row" key={purchase.id}>
                <img
                  src={purchase.title.includes("奶茶") ? "/assets/achievements/milk-tea.webp" : imageForReward(purchase.iconId)}
                  alt=""
                />
                <div><h3>{purchase.title}</h3><span>{dateLabel(purchase.purchasedAt)}</span></div>
                <div className="purchase-value"><strong>{formatMoney(purchase.amountMinor)}</strong><span>已实现</span></div>
              </article>
            ))}
          </div>
        )}
      </section>

      <button className="settings-row" type="button" onClick={onOpenSettings}>
        <span>设置与数据</span>
        <PeckyIcon name="chevron" />
      </button>
    </section>
  );
}

function BottomNavigation({ current, onChange }: { current: TabId; onChange: (tab: TabId) => void }) {
  return (
    <nav className="bottom-navigation" aria-label="主要页面">
      <button className={current === "jar" ? "nav-active" : ""} type="button" onClick={() => onChange("jar")} aria-current={current === "jar" ? "page" : undefined}>
        <PeckyIcon name="jar" />
        <span>米罐</span>
      </button>
      <button className={current === "me" ? "nav-active" : ""} type="button" onClick={() => onChange("me")} aria-current={current === "me" ? "page" : undefined}>
        <PeckyIcon name="person" />
        <span>我的</span>
      </button>
    </nav>
  );
}

function Sheet({ title, onClose, children, className = "" }: { title: string; onClose: () => void; children: ReactNode; className?: string }) {
  return (
    <div className="modal-layer" role="presentation" onMouseDown={(event) => event.target === event.currentTarget && onClose()}>
      <section className={`sheet ${className}`} role="dialog" aria-modal="true" aria-label={title}>
        <div className="sheet-handle" aria-hidden="true" />
        <header className="sheet-header">
          <h2>{title}</h2>
          <button className="icon-button" type="button" onClick={onClose} aria-label="关闭">
            <PeckyIcon name="close" />
          </button>
        </header>
        <div className="sheet-body">{children}</div>
      </section>
    </div>
  );
}

function GoalEditor({
  goal,
  onClose,
  onSave,
  onDelete,
}: {
  goal?: WishGoal;
  onClose: () => void;
  onSave: (input: { id?: string; title: string; targetMinor: number; iconId: RewardIconId }) => Promise<boolean>;
  onDelete?: () => void;
}) {
  const [title, setTitle] = useState(goal?.title ?? "");
  const [amount, setAmount] = useState(goal ? String(goal.targetMinor / 100) : "");
  const [iconId, setIconId] = useState<RewardIconId>(goal?.iconId ?? "toy");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    setError("");
    try {
      const targetMinor = toMinorUnits(Number(amount));
      setBusy(true);
      await onSave({ id: goal?.id, title, targetMinor, iconId });
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "请检查输入");
    } finally {
      setBusy(false);
    }
  };

  return (
    <Sheet title={goal ? "编辑愿望" : "添加小愿望"} onClose={onClose}>
      <form className="form-stack" onSubmit={submit}>
        <label>
          <span>愿望名称</span>
          <input value={title} onChange={(event) => setTitle(event.target.value)} maxLength={24} placeholder="例如：周五的奶茶" autoFocus />
        </label>
        <label>
          <span>目标金额（元）</span>
          <input value={amount} onChange={(event) => setAmount(event.target.value)} inputMode="decimal" type="number" min="0.01" step="0.01" placeholder="50" />
        </label>
        <fieldset className="icon-picker">
          <legend>选择奖励图标</legend>
          <div>
            {rewardOptions.map((option) => (
              <button className={iconId === option.id ? "icon-selected" : ""} type="button" key={option.id} onClick={() => setIconId(option.id)} aria-pressed={iconId === option.id}>
                <img src={option.image} alt="" />
                <span>{option.label}</span>
              </button>
            ))}
          </div>
        </fieldset>
        {error && <p className="form-error" role="alert">{error}</p>}
        <button className="primary-button full-button" type="submit" disabled={busy}>{busy ? "保存中…" : goal ? "保存修改" : "加入米罐"}</button>
        {onDelete && (
          <button className="danger-link" type="button" onClick={onDelete}>
            <PeckyIcon name="trash" /> 删除这个愿望
          </button>
        )}
      </form>
    </Sheet>
  );
}

function ProfileEditor({ profile, onClose, onSave }: { profile: PeckyProfile; onClose: () => void; onSave: (profile: PeckyProfile) => Promise<boolean> }) {
  const [draft, setDraft] = useState(profile);
  const [busy, setBusy] = useState(false);

  return (
    <Sheet title="编辑个人资料" onClose={onClose}>
      <form
        className="form-stack"
        onSubmit={async (event) => {
          event.preventDefault();
          setBusy(true);
          await onSave(draft);
          setBusy(false);
        }}
      >
        <div className="profile-editor-avatar"><img src="/assets/pecky-avatar.webp" alt="当前小鸡头像" /></div>
        <label><span>昵称</span><input value={draft.nickname} onChange={(event) => setDraft({ ...draft, nickname: event.target.value })} maxLength={12} autoFocus /></label>
        <label><span>账号名</span><input value={draft.handle} onChange={(event) => setDraft({ ...draft, handle: event.target.value })} maxLength={20} /></label>
        <label><span>一句话</span><input value={draft.motto} onChange={(event) => setDraft({ ...draft, motto: event.target.value })} maxLength={32} /></label>
        <button className="primary-button full-button" type="submit" disabled={busy}>{busy ? "保存中…" : "保存资料"}</button>
      </form>
    </Sheet>
  );
}

function AchievementsSheet({ state, onClose }: { state: PeckyState; onClose: () => void }) {
  return (
    <Sheet title="我的成就" onClose={onClose}>
      <div className="achievement-detail-list">
        {getAchievements(state).map((achievement) => (
          <article className={achievement.unlocked ? "" : "achievement-locked"} key={achievement.id}>
            <img src={achievement.image} alt="" />
            <div><h3>{achievement.title}</h3><p>{achievement.description}</p></div>
            <span>{achievement.unlocked ? "已解锁" : "未解锁"}</span>
          </article>
        ))}
      </div>
    </Sheet>
  );
}

function GoalsSheet({
  state,
  onClose,
  onEdit,
  onPurchase,
}: {
  state: PeckyState;
  onClose: () => void;
  onEdit: (goal: WishGoal) => void;
  onPurchase: (goal: WishGoal) => void;
}) {
  const goals = [...state.goals].sort((a, b) => a.targetMinor - b.targetMinor);
  return (
    <Sheet title="全部愿望" onClose={onClose}>
      <div className="wish-detail-list">
        {goals.map((goal) => {
          const percentage = Math.round(goalProgress(state.currentBalanceMinor, goal.targetMinor) * 100);
          const ready = state.currentBalanceMinor >= goal.targetMinor;
          return (
            <article key={goal.id}>
              <img src={imageForReward(goal.iconId)} alt="" />
              <div className="wish-detail-copy">
                <h3>{goal.title}</h3>
                <p>{formatGoalPrice(goal.targetMinor)} · 已完成 {percentage}%</p>
                <div className="progress-track"><span style={{ width: `${percentage}%` }} /></div>
              </div>
              <div className="wish-detail-actions">
                <button className="icon-button" type="button" onClick={() => onEdit(goal)} aria-label={`编辑愿望：${goal.title}`}><PeckyIcon name="edit" /></button>
                {ready && <button className="mini-purchase-button" type="button" onClick={() => onPurchase(goal)}>购买</button>}
              </div>
            </article>
          );
        })}
      </div>
    </Sheet>
  );
}

function PurchasesSheet({ state, onClose }: { state: PeckyState; onClose: () => void }) {
  return (
    <Sheet title="已经实现的愿望" onClose={onClose}>
      <div className="purchase-list full-purchase-list">
        {state.purchases.map((purchase) => (
          <article className="purchase-row" key={purchase.id}>
            <img src={purchase.title.includes("奶茶") ? "/assets/achievements/milk-tea.webp" : imageForReward(purchase.iconId)} alt="" />
            <div><h3>{purchase.title}</h3><span>{dateLabel(purchase.purchasedAt)}</span></div>
            <div className="purchase-value"><strong>{formatMoney(purchase.amountMinor)}</strong><span>已实现</span></div>
          </article>
        ))}
      </div>
    </Sheet>
  );
}

function SettingsSheet({
  state,
  onClose,
  onSoundChange,
  onSimulate,
  onPreview,
  onJsonFile,
  onInstall,
  installAvailable,
  onLoadDemo,
  onClear,
}: {
  state: PeckyState;
  onClose: () => void;
  onSoundChange: (enabled: boolean) => Promise<void>;
  onSimulate: (pecks: number, amount: number) => Promise<boolean>;
  onPreview: (pecks: number, amount: number) => Promise<boolean>;
  onJsonFile: (file: File) => Promise<void>;
  onInstall: () => Promise<void>;
  installAvailable: boolean;
  onLoadDemo: () => void;
  onClear: () => void;
}) {
  const [pecks, setPecks] = useState("10");
  const [amount, setAmount] = useState("10");
  const [busy, setBusy] = useState(false);
  const fileInput = useRef<HTMLInputElement>(null);

  const simulate = async (event: FormEvent) => {
    event.preventDefault();
    setBusy(true);
    await onSimulate(Number(pecks), Number(amount));
    setBusy(false);
  };

  const simulateAndPreviewOpening = async () => {
    setBusy(true);
    const started = await onPreview(Number(pecks), Number(amount));
    if (!started) setBusy(false);
  };

  const chooseFile = async (event: ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0];
    if (!file) return;
    setBusy(true);
    await onJsonFile(file);
    setBusy(false);
    event.target.value = "";
  };

  const downloadSample = () => {
    const blob = new Blob([`${JSON.stringify(sampleImportPayload(), null, 2)}\n`], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = "pecky-event-sample.json";
    anchor.click();
    URL.revokeObjectURL(url);
  };

  return (
    <Sheet title="设置与数据" onClose={onClose} className="settings-sheet">
      <div className="settings-stack">
        <section className="settings-group">
          <h3>体验</h3>
          <label className="toggle-row">
            <span><strong>开屏声音</strong><small>默认静音，可为下一次开屏开启</small></span>
            <input type="checkbox" checked={state.settings.soundEnabled} onChange={(event) => onSoundChange(event.target.checked)} />
          </label>
          <button className="settings-action" type="button" onClick={onInstall}>
            <PeckyIcon name="download" />
            <span><strong>{installAvailable ? "安装 Pecky" : "添加到主屏幕"}</strong><small>像 App 一样全屏使用</small></span>
            <PeckyIcon name="chevron" />
          </button>
        </section>

        <section className="settings-group">
          <h3>硬件数据模拟器</h3>
          <p className="settings-note">第一版用模拟器和 JSON 走同一条入账接口。想直接检查开屏，可模拟入账并立即播放；这次数据只会播放一次。</p>
          <form className="simulator-form" onSubmit={simulate}>
            <label><span>啄米次数</span><input type="number" inputMode="numeric" min="1" step="1" value={pecks} onChange={(event) => setPecks(event.target.value)} /></label>
            <label><span>入账金额（元）</span><input type="number" inputMode="decimal" min="0.01" step="0.01" value={amount} onChange={(event) => setAmount(event.target.value)} /></label>
            <button className="primary-button full-button" type="submit" disabled={busy}>{busy ? "接收中…" : "模拟接收数据"}</button>
          </form>
          <button className="secondary-button full-button opening-preview-button" type="button" disabled={busy} onClick={simulateAndPreviewOpening}>
            {busy ? "准备开屏…" : "模拟并播放开屏"}
          </button>
          <input ref={fileInput} className="visually-hidden" type="file" accept="application/json,.json" onChange={chooseFile} />
          <div className="settings-button-grid">
            <button className="secondary-button" type="button" onClick={() => fileInput.current?.click()} disabled={busy}><PeckyIcon name="upload" /> 导入 JSON</button>
            <button className="secondary-button" type="button" onClick={downloadSample}><PeckyIcon name="download" /> 示例文件</button>
          </div>
        </section>

        <section className="ble-card">
          <div className="ble-icon"><PeckyIcon name="bluetooth" /></div>
          <div><h3>Pecky 硬件</h3><p>BLE 连接生命周期、订阅和游标接口已预留。</p></div>
          <span>待联调</span>
        </section>

        <section className="settings-group">
          <h3>本机数据</h3>
          <p className="settings-note">数据只保存在当前设备的浏览器中，不会上传云端。</p>
          <button className="settings-action" type="button" onClick={onLoadDemo}>
            <PeckyIcon name="database" />
            <span><strong>载入演示数据</strong><small>覆盖当前数据，用于预览完整页面</small></span>
            <PeckyIcon name="chevron" />
          </button>
          <button className="settings-action danger-action" type="button" onClick={onClear}>
            <PeckyIcon name="trash" />
            <span><strong>清空本机数据</strong><small>清空后保持从 ¥0 开始</small></span>
            <PeckyIcon name="chevron" />
          </button>
        </section>

        <p className="version-label">Pecky Web · 本地数据版 1.0</p>
      </div>
    </Sheet>
  );
}

function ConfirmDialog({ confirmation, onCancel, onConfirm }: { confirmation: Exclude<ConfirmationState, null>; onCancel: () => void; onConfirm: () => Promise<void> }) {
  const [busy, setBusy] = useState(false);
  const copy =
    confirmation.kind === "purchase"
      ? { title: "确认已购买？", message: `将从米罐扣除 ${formatMoney(confirmation.goal.targetMinor)}，并把“${confirmation.goal.title}”移到已实现愿望。`, action: "确认购买", danger: false }
      : confirmation.kind === "delete-goal"
        ? { title: "删除这个愿望？", message: `“${confirmation.goal.title}”会从愿望清单移除，米罐余额不会改变。`, action: "删除愿望", danger: true }
        : confirmation.kind === "clear"
          ? { title: "清空本机数据？", message: "余额、愿望、成就和历史记录都会从这台设备删除，无法恢复。", action: "确认清空", danger: true }
          : { title: "载入演示数据？", message: "当前本机数据会被完整的演示内容覆盖。", action: "载入演示", danger: false };

  return (
    <div className="confirmation-layer" role="presentation">
      <section className="confirmation-card" role="alertdialog" aria-modal="true" aria-labelledby="confirmation-title">
        <h2 id="confirmation-title">{copy.title}</h2>
        <p>{copy.message}</p>
        <div>
          <button className="secondary-button" type="button" onClick={onCancel} disabled={busy}>取消</button>
          <button
            className={copy.danger ? "danger-button" : "primary-button"}
            type="button"
            disabled={busy}
            onClick={async () => {
              setBusy(true);
              await onConfirm();
              setBusy(false);
            }}
          >
            {busy ? "处理中…" : copy.action}
          </button>
        </div>
      </section>
    </div>
  );
}
