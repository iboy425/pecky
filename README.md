# Pecky PWA

项目仓库：<https://github.com/iboy425/pecky>  
硬件物料清单：[`docs/BOM.md`](docs/BOM.md)  
GitHub Topic：`shenicest-fission`

Pecky is a mobile-first savings companion for the Pecky hardware concept. It turns hardware peck events into a shared rice-jar balance, compares that balance with multiple wishes, and preserves completed wishes as personal achievements.

## Product behavior

- Jar and opening animation are one page. Pending, unseen events are claimed once at launch, shown in the opening experience, and then collapsed into the static Jar page.
- After the opening collapses, the Jar hero starts the muted chick-orbiting-the-jar scene and keeps the balance copy readable in its own left panel.
- The opening cannot be manually replayed. Claiming is persisted before playback so a refresh or crash does not replay the same batch.
- One shared balance is compared with every active wish. Wishes are sorted by target price.
- A wish can only be marked purchased when the balance is sufficient. Purchase confirmation deducts the target amount, removes the wish, and adds a history record.
- Lifetime saved amount and lifetime pecks never decrease after a purchase.
- The Me page contains profile details, lifetime totals, achievements, purchase history, and data settings.
- Version 1 stores data in local IndexedDB and can connect directly to the Pecky cap through Web Bluetooth. There are no accounts or cloud sync.

## Hardware-ready event contract

The simulator, JSON importer, and live BLE adapter implement the same data-source interface in `app/lib/sources.ts`. BLE completion events preserve the recognized cap action (`neck_extension`, `chin_tuck`, or `head_resistance`). The app also connects to the chair independently: chair actions never alter the rice balance and instead display a UI-styled “已识别” caption for two seconds, then fade out.

```json
{
  "version": 1,
  "events": [
    {
      "eventId": "event-001",
      "deviceId": "PECKY-001",
      "sequence": 1,
      "peckCount": 10,
      "amountDelta": 10,
      "occurredAt": "2026-08-28T10:00:00.000Z"
    }
  ]
}
```

Event IDs and `deviceId + sequence` pairs are deduplicated. Money is stored as integer minor units. IndexedDB updates use serial read-write transactions so imports and purchases cannot overwrite each other across tabs.

## Demo and formal first use

- Development mode seeds the approved demo state on an empty browser profile.
- A production first visit starts at ¥0.
- Add `?demo=1` to a production URL to seed demo data on an empty browser profile.
- “Load demo data” in Settings replaces the current local state after confirmation.
- “Clear local data” writes a blank state with demo seeding disabled, preventing demo data from returning on the next launch.

## Test the opening flow

1. Open **Me → Settings & Data**.
2. Enter a peck count and amount, then choose **Simulate and play opening**.
3. The event is written through the same adapter contract as future hardware data, then the page performs a fresh launch and shows the opening once.
4. When it collapses into the Jar, refresh again to confirm it does not replay. This is intentional one-way behavior.

Choosing **Simulate data** instead only records the event. Refresh the page or fully relaunch the installed PWA to see its one-time opening later.

## Local development

Requirements: Node.js 22.13 or newer.

```bash
npm install
npm run dev
npm run prepare-assets
npm run lint
npm test
```

The cap firmware uses `NimBLE-Arduino` 2.5.x. Install it once before compiling `firmware/04_hat_recognition_ble`:

```powershell
arduino-cli lib install "NimBLE-Arduino@2.5.1"
```

## Real hardware startup

Flash the existing cap recognizer to the cap ESP32-S3, and flash the chair recognizer to the chair ESP32-S3. Keep still during each device's initial calibration. Both advertise their own BLE service, so they can be connected independently from **我的 → 设置与数据** in Android Chrome (or an installed Android PWA).

```bash
# In WSL / Ubuntu, from the repository root
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/04_hat_recognition_ble
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32s3 firmware/04_hat_recognition_ble

arduino-cli compile --fqbn esp32:esp32:esp32s3 Chair/firmware/01_chair_recognition_ble
arduino-cli upload -p /dev/ttyUSB1 --fqbn esp32:esp32:esp32s3 Chair/firmware/01_chair_recognition_ble

# Start the web app (use the HTTPS URL shown by the dev server on a phone)
npm run dev -- --host 0.0.0.0
```

Use the actual serial device paths shown by `arduino-cli board list`; do not assume `/dev/ttyUSB0` and `/dev/ttyUSB1` on every computer. Web Bluetooth requires a secure context (HTTPS, except localhost) and a browser that supports it.

### Terminal-controlled sessions

Both devices boot calibrated but **paused**. Use the controller to decide exactly when a test starts, pauses, or recalibrates. Paused devices do not create BLE action notifications, so the app will not show captions or add cap events.

The chair Bluetooth remains enabled while recognition is paused: `Qingxian-Chair` remains discoverable and reconnects automatically after an app disconnect. The current cap prototype uses its proven USB event stream because its radio brownouts at startup; do not select the cap BLE option until its 5 V supply and decoupling are repaired.

```bash
# WSL terminal; the wrapper accesses Windows COM7/COM8 for you
bash tools/control_devices_wsl.sh all status
bash tools/control_devices_wsl.sh all start
bash tools/control_devices_wsl.sh hat pause
bash tools/control_devices_wsl.sh chair calibrate
```

For the full on-site runbook—including calibration, the recommended action order, connection recovery, and reflashing—see [the Chinese demo command manual](docs/demo-command-manual.md).

### Recommended cap connection: terminal bridge

The cap's COM7 is exclusively owned by a small terminal bridge, preventing browser serial permissions and terminal controls from competing. Start it in one WSL terminal and leave it running:

```bash
bash tools/start_serial_bridge_wsl.sh
```

Then choose **终端桥接（推荐）→ 连接** in the app. Connecting starts recognition and streams action events into the rice jar; disconnecting pauses recognition automatically.

`npm test` builds the vinext/Cloudflare output and verifies the rendered PWA shell, production metadata, opening state machine, persistence contract, adapters, and required web assets.

## Project structure

- `app/components/PeckyApp.tsx` — mobile UI, interaction state, and opening transition
- `app/lib/model.ts` — money, wishes, purchases, achievements, and event rules
- `app/lib/storage.ts` — atomic IndexedDB persistence and cross-tab refresh
- `app/lib/sources.ts` — simulator, JSON import, and live Web Bluetooth adapter
- `public/manifest.webmanifest` and `public/sw.js` — installable/offline PWA shell
- `scripts/prepare-assets.mjs` — read-only derivation of approved source artwork into web assets

Approved source artwork remains untouched outside this project. Runtime derivatives are stored under `public/assets`.
