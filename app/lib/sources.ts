import { createId, type EventSource, type ExternalPeckyEvent } from "./model";

export type DataSourceConnectionState =
  | "disconnected"
  | "connecting"
  | "connected"
  | "unavailable";

export interface PeckyEventBatch {
  events: unknown[];
  cursor?: string;
}

export interface PeckyDataSource {
  readonly id: EventSource;
  getConnectionState(): DataSourceConnectionState;
  connect(): Promise<void>;
  disconnect(): Promise<void>;
  pull(cursor?: string): Promise<PeckyEventBatch>;
  subscribe(listener: (batch: PeckyEventBatch) => void): () => void;
}

let mockSequence = Date.now();

export class MockPeckyDataSource implements PeckyDataSource {
  readonly id = "mock" as const;
  private connectionState: DataSourceConnectionState = "disconnected";

  constructor(
    private readonly peckCount: number,
    private readonly amountDelta: number,
  ) {}

  getConnectionState(): DataSourceConnectionState {
    return this.connectionState;
  }

  async connect(): Promise<void> {
    this.connectionState = "connected";
  }

  async disconnect(): Promise<void> {
    this.connectionState = "disconnected";
  }

  async pull(): Promise<PeckyEventBatch> {
    if (this.connectionState !== "connected") await this.connect();
    mockSequence += 1;
    return {
      cursor: String(mockSequence),
      events: [
        {
          eventId: createId("mock-event"),
          deviceId: "PECKY-WEB-SIMULATOR",
          sequence: mockSequence,
          peckCount: this.peckCount,
          amountDelta: this.amountDelta,
          occurredAt: new Date().toISOString(),
        } satisfies ExternalPeckyEvent,
      ],
    };
  }

  subscribe(): () => void {
    return () => undefined;
  }
}

export class JsonImportDataSource implements PeckyDataSource {
  readonly id = "json" as const;
  private connectionState: DataSourceConnectionState = "disconnected";

  constructor(private readonly payload: unknown) {}

  getConnectionState(): DataSourceConnectionState {
    return this.connectionState;
  }

  async connect(): Promise<void> {
    if (!this.payload || typeof this.payload !== "object" || Array.isArray(this.payload)) {
      throw new Error("JSON 根节点必须是对象");
    }
    const raw = this.payload as { version?: unknown; events?: unknown };
    if (raw.version !== 1) throw new Error("只支持 version: 1 的数据包");
    if (!Array.isArray(raw.events)) throw new Error("JSON 缺少 events 数组");
    this.connectionState = "connected";
  }

  async disconnect(): Promise<void> {
    this.connectionState = "disconnected";
  }

  async pull(): Promise<PeckyEventBatch> {
    if (this.connectionState !== "connected") await this.connect();
    const raw = this.payload as { events: unknown[] };
    return { events: raw.events };
  }

  subscribe(): () => void {
    return () => undefined;
  }
}

export class BlePeckyDataSource implements PeckyDataSource {
  readonly id = "ble" as const;

  getConnectionState(): DataSourceConnectionState {
    return "unavailable";
  }

  async connect(): Promise<void> {
    throw new Error("BLE 适配器已预留，将在后续硬件联调版本中启用");
  }

  async disconnect(): Promise<void> {
    return undefined;
  }

  async pull(): Promise<PeckyEventBatch> {
    throw new Error("BLE 数据源将在后续硬件联调版本中启用");
  }

  subscribe(): () => void {
    return () => undefined;
  }
}

export function sampleImportPayload(): { version: 1; events: ExternalPeckyEvent[] } {
  return {
    version: 1,
    events: [
      {
        eventId: "sample-event-001",
        deviceId: "PECKY-SAMPLE",
        sequence: 1,
        peckCount: 10,
        amountDelta: 10,
        occurredAt: new Date().toISOString(),
      },
    ],
  };
}
