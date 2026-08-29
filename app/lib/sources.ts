import { createId, type EventSource, type ExternalPeckyEvent, type PeckyAction } from "./model";

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
  private connectionState: DataSourceConnectionState = "disconnected";
  private device: BluetoothDevice | null = null;
  private server: BluetoothRemoteGATTServer | null = null;
  private eventCharacteristic: BluetoothRemoteGATTCharacteristic | null = null;
  private snapshotCharacteristic: BluetoothRemoteGATTCharacteristic | null = null;
  private commandCharacteristic: BluetoothRemoteGATTCharacteristic | null = null;
  private listeners = new Set<(batch: PeckyEventBatch) => void>();
  private lastTotalReps = 0;

  static readonly SERVICE_UUID = "2f6f1000-8d0a-4e3d-bbc6-9f536a6ed001";
  static readonly EVENT_UUID = "2f6f1001-8d0a-4e3d-bbc6-9f536a6ed001";
  static readonly SNAPSHOT_UUID = "2f6f1002-8d0a-4e3d-bbc6-9f536a6ed001";
  static readonly COMMAND_UUID = "2f6f1003-8d0a-4e3d-bbc6-9f536a6ed001";

  getConnectionState(): DataSourceConnectionState {
    if (typeof navigator === "undefined" || !("bluetooth" in navigator)) {
      return "unavailable";
    }
    return this.connectionState;
  }

  async connect(): Promise<void> {
    if (this.connectionState === "connected" && this.server?.connected) return;
    const bluetooth = (navigator as Navigator & { bluetooth?: Bluetooth }).bluetooth;
    if (!bluetooth) throw new Error("当前浏览器不支持 Web Bluetooth，请用 Android Chrome 打开");
    this.connectionState = "connecting";
    try {
      this.device = await bluetooth.requestDevice({
        filters: [{ services: [BlePeckyDataSource.SERVICE_UUID] }],
      });
      if (!this.device.gatt) throw new Error("设备没有可用的 BLE GATT 服务");
      this.device.addEventListener("gattserverdisconnected", this.handleDisconnect);
      this.server = await this.device.gatt.connect();
      const service = await this.server.getPrimaryService(BlePeckyDataSource.SERVICE_UUID);
      [this.eventCharacteristic, this.snapshotCharacteristic, this.commandCharacteristic] =
        await Promise.all([
          service.getCharacteristic(BlePeckyDataSource.EVENT_UUID),
          service.getCharacteristic(BlePeckyDataSource.SNAPSHOT_UUID),
          service.getCharacteristic(BlePeckyDataSource.COMMAND_UUID),
        ]);
      await this.eventCharacteristic.startNotifications();
      this.eventCharacteristic.addEventListener("characteristicvaluechanged", this.handleEvent);
      this.connectionState = "connected";
    } catch (error) {
      this.connectionState = "disconnected";
      throw error;
    }
  }

  async disconnect(): Promise<void> {
    if (this.eventCharacteristic) {
      this.eventCharacteristic.removeEventListener("characteristicvaluechanged", this.handleEvent);
      if (this.eventCharacteristic.service.device.gatt?.connected) {
        await this.eventCharacteristic.stopNotifications().catch(() => undefined);
      }
    }
    this.device?.removeEventListener("gattserverdisconnected", this.handleDisconnect);
    this.server?.disconnect();
    this.eventCharacteristic = null;
    this.snapshotCharacteristic = null;
    this.commandCharacteristic = null;
    this.server = null;
    this.device = null;
    this.connectionState = "disconnected";
  }

  async pull(cursor?: string): Promise<PeckyEventBatch> {
    if (!this.snapshotCharacteristic || !this.commandCharacteristic) {
      throw new Error("请先连接 Pecky 帽子");
    }
    const value = await this.snapshotCharacteristic.readValue();
    const snapshot = this.decodeMessage(value);
    if (snapshot.t !== "z") throw new Error("帽子返回了无效的状态快照");
    const sequence = this.safeInteger(snapshot.q);
    const totalReps = this.safeInteger(snapshot.tr);
    const previous = this.decodeCursor(cursor);
    this.lastTotalReps = totalReps;
    const events: ExternalPeckyEvent[] = [];
    if (previous && totalReps > previous.totalReps) {
      const missed = totalReps - previous.totalReps;
      events.push(this.toExternalEvent(sequence, missed));
    }
    const command = new TextEncoder().encode('{"v":1,"cmd":"sync"}');
    await this.commandCharacteristic.writeValueWithoutResponse(command);
    return { events, cursor: this.encodeCursor(sequence, totalReps) };
  }

  subscribe(listener: (batch: PeckyEventBatch) => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private readonly handleDisconnect = () => {
    this.connectionState = "disconnected";
  };

  private readonly handleEvent = (event: Event) => {
    const characteristic = event.target as BluetoothRemoteGATTCharacteristic;
    if (!characteristic.value) return;
    try {
      const message = this.decodeMessage(characteristic.value);
      if (message.t !== "r") return; // Progress and group-complete never count.
      const sequence = this.safeInteger(message.q);
      const action = this.actionFromCode(message.c);
      this.lastTotalReps += 1;
      const batch = {
        events: [this.toExternalEvent(sequence, 1, action)],
        cursor: this.encodeCursor(sequence, this.lastTotalReps),
      };
      this.listeners.forEach((listener) => listener(batch));
    } catch {
      // A corrupt notification is ignored; the next SNAPSHOT read reconciles it.
    }
  };

  private decodeMessage(value: DataView): Record<string, unknown> {
    const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    const parsed: unknown = JSON.parse(new TextDecoder().decode(bytes));
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
      throw new Error("BLE 消息不是对象");
    }
    return parsed as Record<string, unknown>;
  }

  private safeInteger(value: unknown): number {
    const result = Number(value);
    if (!Number.isSafeInteger(result) || result < 0) throw new Error("BLE 序号无效");
    return result;
  }

  private actionFromCode(value: unknown): PeckyAction {
    if (value === 1) return "neck_extension";
    if (value === 2) return "chin_tuck";
    if (value === 3) return "head_resistance";
    throw new Error("BLE 动作编号无效");
  }

  private toExternalEvent(sequence: number, peckCount: number, action?: PeckyAction): ExternalPeckyEvent {
    const deviceId = this.device?.name || this.device?.id || "PECKY-BLE";
    return {
      eventId: `${deviceId}-${sequence}`,
      deviceId,
      sequence,
      peckCount,
      ...(action ? { action } : {}),
      amountDelta: peckCount,
      occurredAt: new Date().toISOString(),
    };
  }

  private encodeCursor(sequence: number, totalReps: number): string {
    return `${sequence}:${totalReps}`;
  }

  private decodeCursor(cursor?: string): { sequence: number; totalReps: number } | null {
    if (!cursor) return null;
    const [sequence, totalReps] = cursor.split(":").map(Number);
    return Number.isSafeInteger(sequence) && Number.isSafeInteger(totalReps)
      ? { sequence, totalReps }
      : null;
  }
}

/** USB fallback for the prototype board. It consumes the exact EVENT JSON
 * already emitted by the firmware, so recognition still happens on the hat. */
export class SerialPeckyDataSource implements PeckyDataSource {
  readonly id = "serial" as const;
  private connectionState: DataSourceConnectionState = "disconnected";
  private port: SerialPort | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private listeners = new Set<(batch: PeckyEventBatch) => void>();
  private stopped = false;

  getConnectionState(): DataSourceConnectionState {
    return typeof navigator === "undefined" || !("serial" in navigator) ? "unavailable" : this.connectionState;
  }

  async connect(): Promise<void> {
    const serial = (navigator as Navigator & { serial?: Serial }).serial;
    if (!serial) throw new Error("当前浏览器不支持 USB 串口，请用桌面 Chrome 打开");
    this.connectionState = "connecting";
    try {
      this.port = await serial.requestPort();
      await this.port.open({ baudRate: 115200 });
      this.connectionState = "connected";
      this.stopped = false;
      void this.readLoop();
    } catch (error) {
      this.connectionState = "disconnected";
      throw error;
    }
  }

  async disconnect(): Promise<void> {
    this.stopped = true;
    await this.reader?.cancel().catch(() => undefined);
    this.reader?.releaseLock();
    this.reader = null;
    await this.port?.close().catch(() => undefined);
    this.port = null;
    this.connectionState = "disconnected";
  }

  async pull(): Promise<PeckyEventBatch> { return { events: [] }; }
  subscribe(listener: (batch: PeckyEventBatch) => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private async readLoop(): Promise<void> {
    if (!this.port?.readable) return;
    this.reader = this.port.readable.getReader();
    const decoder = new TextDecoder();
    let pending = "";
    try {
      while (!this.stopped) {
        const { value, done } = await this.reader.read();
        if (done) break;
        pending += value ? decoder.decode(value, { stream: true }) : "";
        const lines = pending.split(/\r?\n/);
        pending = lines.pop() ?? "";
        lines.forEach((line) => this.handleLine(line));
      }
    } catch {
      // Disconnects are reflected by the UI state when the user reconnects.
    }
  }

  private handleLine(line: string): void {
    if (!line.startsWith("EVENT,")) return;
    const jsonAt = line.indexOf("{", 6);
    if (jsonAt < 0) return;
    try {
      const message = JSON.parse(line.slice(jsonAt)) as Record<string, unknown>;
      if (message.t !== "r") return;
      const sequence = Number(message.q);
      const code = Number(message.c);
      if (!Number.isSafeInteger(sequence) || sequence < 0) return;
      const action: PeckyAction | null = code === 1 ? "neck_extension" : code === 2 ? "chin_tuck" : code === 3 ? "head_resistance" : null;
      if (!action) return;
      const event: ExternalPeckyEvent = {
        eventId: `PECKY-USB-${sequence}`, deviceId: "PECKY-USB", sequence,
        peckCount: 1, action, amountDelta: 1, occurredAt: new Date().toISOString(),
      };
      const batch = { events: [event], cursor: String(sequence) };
      this.listeners.forEach((listener) => listener(batch));
    } catch { /* malformed serial diagnostics are ignored */ }
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
