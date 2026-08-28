# Pecky PWA

Pecky is a mobile-first savings companion for the Pecky hardware concept. It turns hardware peck events into a shared rice-jar balance, compares that balance with multiple wishes, and preserves completed wishes as personal achievements.

## Product behavior

- Jar and opening animation are one page. Pending, unseen events are claimed once at launch, shown in the opening experience, and then collapsed into the static Jar page.
- The opening cannot be manually replayed. Claiming is persisted before playback so a refresh or crash does not replay the same batch.
- One shared balance is compared with every active wish. Wishes are sorted by target price.
- A wish can only be marked purchased when the balance is sufficient. Purchase confirmation deducts the target amount, removes the wish, and adds a history record.
- Lifetime saved amount and lifetime pecks never decrease after a purchase.
- The Me page contains profile details, lifetime totals, achievements, purchase history, and data settings.
- Version 1 uses local IndexedDB only. There are no accounts, cloud sync, or active Bluetooth permissions.

## Hardware-ready event contract

The simulator, JSON importer, and future BLE adapter implement the same data-source interface in `app/lib/sources.ts`. Every event carries peck count and money independently; the web app does not assume a conversion rate.

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

## Local development

Requirements: Node.js 22.13 or newer.

```bash
npm install
npm run dev
npm run lint
npm test
```

`npm test` builds the vinext/Cloudflare output and verifies the rendered PWA shell, production metadata, opening state machine, persistence contract, adapters, and required web assets.

## Project structure

- `app/components/PeckyApp.tsx` — mobile UI, interaction state, and opening transition
- `app/lib/model.ts` — money, wishes, purchases, achievements, and event rules
- `app/lib/storage.ts` — atomic IndexedDB persistence and cross-tab refresh
- `app/lib/sources.ts` — simulator, JSON import, and reserved BLE adapter contract
- `public/manifest.webmanifest` and `public/sw.js` — installable/offline PWA shell
- `scripts/prepare-assets.mjs` — read-only derivation of approved source artwork into web assets

Approved source artwork remains untouched outside this project. Runtime derivatives are stored under `public/assets`.
