import {
  createBlankState,
  createDemoState,
  normalizePeckyState,
  type PeckyState,
} from "./model";

const DATABASE_NAME = "pecky-local-v1";
const STORE_NAME = "app";
const STATE_KEY = "state";
const CHANNEL_NAME = "pecky-state-changed";

export interface StorageOptions {
  seedDemo?: boolean;
}

function openDatabase(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DATABASE_NAME, 1);
    request.onupgradeneeded = () => {
      const database = request.result;
      if (!database.objectStoreNames.contains(STORE_NAME)) {
        database.createObjectStore(STORE_NAME);
      }
    };
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error("无法打开本地数据"));
  });
}

function initialState(options?: StorageOptions): PeckyState {
  return options?.seedDemo ? createDemoState() : createBlankState();
}

function notifyOtherTabs(): void {
  if (typeof BroadcastChannel === "undefined") return;
  try {
    const channel = new BroadcastChannel(CHANNEL_NAME);
    channel.postMessage({ type: "state-changed", at: Date.now() });
    channel.close();
  } catch {
    // IndexedDB is authoritative; cross-tab refresh is best-effort only.
  }
}

async function readStoredState(): Promise<PeckyState | undefined> {
  const database = await openDatabase();
  return new Promise((resolve, reject) => {
    const transaction = database.transaction(STORE_NAME, "readonly");
    const request = transaction.objectStore(STORE_NAME).get(STATE_KEY);
    let result: PeckyState | undefined;

    request.onsuccess = () => {
      result = request.result as PeckyState | undefined;
    };
    request.onerror = () => transaction.abort();
    transaction.oncomplete = () => {
      database.close();
      resolve(result ? normalizePeckyState(result) : undefined);
    };
    transaction.onabort = () => {
      database.close();
      reject(request.error ?? transaction.error ?? new Error("本地数据读取失败"));
    };
    transaction.onerror = () => undefined;
  });
}

export async function loadPeckyState(options?: StorageOptions): Promise<PeckyState> {
  const existing = await readStoredState();
  if (existing) return existing;
  return updatePeckyState((state) => state, options);
}

export async function updatePeckyState(
  mutate: (state: PeckyState) => PeckyState,
  options?: StorageOptions,
): Promise<PeckyState> {
  const database = await openDatabase();
  return new Promise((resolve, reject) => {
    const transaction = database.transaction(STORE_NAME, "readwrite");
    const store = transaction.objectStore(STORE_NAME);
    const getRequest = store.get(STATE_KEY);
    let nextState: PeckyState | undefined;

    getRequest.onsuccess = () => {
      try {
        const current = getRequest.result
          ? normalizePeckyState(getRequest.result as PeckyState)
          : initialState(options);
        nextState = normalizePeckyState(mutate(current));
        store.put(nextState, STATE_KEY);
      } catch (error) {
        transaction.abort();
        reject(error);
      }
    };
    getRequest.onerror = () => transaction.abort();
    transaction.oncomplete = () => {
      database.close();
      if (!nextState) {
        reject(new Error("本地状态更新失败"));
        return;
      }
      resolve(nextState);
      notifyOtherTabs();
    };
    transaction.onabort = () => {
      database.close();
      reject(getRequest.error ?? transaction.error ?? new Error("本地事务已取消"));
    };
    transaction.onerror = () => undefined;
  });
}

export async function savePeckyState(state: PeckyState): Promise<void> {
  const database = await openDatabase();
  await new Promise<void>((resolve, reject) => {
    const transaction = database.transaction(STORE_NAME, "readwrite");
    transaction.objectStore(STORE_NAME).put(normalizePeckyState(state), STATE_KEY);
    transaction.oncomplete = () => {
      database.close();
      resolve();
      notifyOtherTabs();
    };
    transaction.onabort = () => {
      database.close();
      reject(transaction.error ?? new Error("本地数据保存失败"));
    };
    transaction.onerror = () => undefined;
  });
}

export async function replaceWithDemoState(): Promise<PeckyState> {
  const demo = createDemoState();
  await savePeckyState(demo);
  return demo;
}

export async function clearLocalState(): Promise<PeckyState> {
  const blank = createBlankState({ disableDemo: true });
  await savePeckyState(blank);
  return blank;
}

export function subscribeToPeckyState(listener: () => void): () => void {
  if (typeof BroadcastChannel === "undefined") return () => undefined;
  try {
    const channel = new BroadcastChannel(CHANNEL_NAME);
    channel.onmessage = () => listener();
    return () => channel.close();
  } catch {
    return () => undefined;
  }
}
