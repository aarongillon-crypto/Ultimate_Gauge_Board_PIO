# M5Stack Tab5 Remote Monitor — Implementation Plan

> ## ⏸️ STATUS: PAUSED (2026-06-23)
> **ESP-NOW is not available on the Tab5 (ESP32-P4) in either Arduino or ESP-IDF**
> — verified at the library level. The C6-hosting layer (`esp_wifi_remote` 1.6.1)
> only remotes the `esp_wifi` API, not `esp_now_*`; regular WiFi/TCP-IP works, but
> ESP-NOW does not. The chosen transport is therefore not viable, and the work is
> paused. **When resumed, the recommended path is the "gateway gauge" bridge — see
> §9.** The ESP-NOW protocol design below (telemetry packet, command reuse) is
> still valid; only the *Tab5 leg* changes from ESP-NOW to WiFi.
>
> Phase-0 spike lives in `tab5_monitor/` (compiles for the P4; ESP-NOW fails to
> link — that's the evidence). See also memory `reference_tab5_espnow_blocked.md`.

Status: **planning** (not started). Branch TBD.

A secondary display on a M5Stack Tab5 that monitors **the whole gauge fleet plus
GlowCraft strip status** over **ESP-NOW**, and can **control** the gauges
(mode / brightness / page / theme / test) — mirroring the web UI's functions.

---

## 1. Decisions locked in

| Decision | Choice |
|---|---|
| Data scope | Whole fleet (all gauge nodes) + GlowCraft strip status |
| Interaction | Monitor **and** control |
| Transport | ESP-NOW |

## 2. Target hardware — M5Stack Tab5

- **MCU:** ESP32-P4 (RISC-V dual-core), 16 MB flash, 32 MB PSRAM.
- **Wireless:** *separate* **ESP32-C6-MINI-1U** (Wi-Fi 6 / BT / 802.15.4) connected
  to the P4 over SDIO. **The P4 has no built-in radio** — all wireless, ESP-NOW
  included, runs on the C6 via Espressif's `esp_wifi_remote` / ESP-Hosted layer.
- **Display:** 5″ 1280×720 IPS, MIPI-DSI. **Touch:** GT911 capacitive (I²C).
- **Toolchains:** Arduino, ESP-IDF, PlatformIO, UIFlow. Note: Arduino board
  support and M5GFX/M5Unified libs are recent and still maturing.

> ⚠️ **Top risk:** ESP-NOW over the P4→C6 hosted radio is not yet proven in our
> stack. M5's published material covers WiFi but not ESP-NOW specifically.
> **Phase 0 de-risks this before any UI work.**

## 3. How the existing ESP-NOW design helps us

From the gauge firmware (`src/main.cpp`):

- Gauges run `WIFI_AP_STA` on **channel 1** (`WIFI_CHANNEL`), broadcast via the
  **AP interface** to `FF:FF:FF:FF:FF:FF`, and filter out their own MAC.
- `EspNowPacket` (packed, **25 bytes**): `{ uint8_t type; int mode; uint32_t c1,c2,c3,c4; int value; }`.
- Command types already handled by `OnDataRecv()` **from any sender**:
  | Type | Meaning |
  |---|---|
  | 1 | Presence (mode) — broadcast every 2 s |
  | 2 | Set mode (targeted) |
  | 3 | Theme colours |
  | 4 | Test mode |
  | 5 | Brightness |
  | 6 | Stats overlay |
  | 7 | UI colours |

**Implication for control:** gauges already act on types 2–7 regardless of
source. So the Tab5 can control them with **little or no gauge-side change** —
it just sends the same packets.

**Gap for monitoring:** gauges currently broadcast only *presence*, not
telemetry. So the only new gauge-side work is **broadcasting a telemetry packet**.

## 4. Protocol additions

### 4.1 New telemetry packet (gauge → Tab5), `type = 8`

A **separate** packed struct (do **not** bloat `EspNowPacket`). Leads with
`uint8_t type` at offset 0 so the receiver dispatches on byte 0. Carries one
node's full state; fits comfortably in the 250-byte ESP-NOW limit (~110 bytes).

```c
// DRAFT — finalise field set/types in Phase 1
typedef struct __attribute__((packed)) {
  uint8_t  type;            // = 8
  uint8_t  node_mode;       // current_mode
  char     name[12];        // device_name (for the dashboard)
  // Haltech telemetry (compact types)
  float    boost_psi, afr_gas, oil_press_psi, oil_temp_c, fuel_press_psi;
  float    ign_timing_deg, baro_kpa, fuel_temp_c, vehicle_speed_kph;
  int16_t  rpm, water_temp_c, intake_air_temp_c, tps_percent, engine_load_pct;
  int8_t   gear;
  // GlowCraft strip summary: 8 strips x {r,g,b,state}
  struct { uint8_t r, g, b, state; } strips[8];
} TelemetryPacket;
```

### 4.2 Shared protocol header

Factor all packet definitions (types, `EspNowPacket`, `TelemetryPacket`, strip
summary) into a single **`EspNowProtocol.h`** shared by **both** the gauge
firmware and the Tab5 firmware, so they never drift. Single source of truth.

### 4.3 Gauge-side changes (small)

1. `broadcast_telemetry()` — fill a `TelemetryPacket` from `HaltechData` +
   `glowcraft_strips[]` and broadcast it. Call from `loop()` at ~5–10 Hz
   (rate TBD — fixed heartbeat vs on-change).
2. Make `OnDataRecv()`'s length guard **type-aware**: dispatch on byte 0, only
   require `sizeof(EspNowPacket)` for command types, and **ignore** `type 8`
   (gauges don't need each other's telemetry).
3. Control path: **no change** (types 2–7 already handled).

## 5. Tab5-side firmware (new project)

- **New PlatformIO project** targeting the Tab5 (separate from this S3 repo —
  different arch/board). Shares `EspNowProtocol.h`.
- **ESP-NOW init via the C6** (`esp_wifi_remote`/ESP-Hosted); **lock to channel 1**
  to match the gauges; register the receive callback.
- **Fleet model:** table keyed by source MAC → latest `TelemetryPacket` +
  `last_seen` (→ online/offline timeout).
- **UI (1280×720):**
  - Dashboard: one tile per gauge node — name, mode, key metrics, a small
    GlowCraft strip mini-view, online indicator.
  - Tap a node → detail + **control panel** (mode / brightness / page / test /
    theme) that sends the existing command packet types.
  - GlowCraft strip detail reusing the same silhouette concept as the gauge page.
- **UI framework:** M5GFX/M5Unified vs LVGL — **decide in Phase 2** (LVGL would
  let us reuse the gauge's rendering patterns; M5Unified is the native path).

## 6. Phased build plan

| Phase | Goal | Gate |
|---|---|---|
| **0 — De-risk** | ESP-NOW hello-world between an S3 gauge and the Tab5 C6: receive a broadcast, confirm channel 1, send a packet back. | **Everything depends on this.** If ESP-NOW won't run on the C6 path, fall back to WiFi+UDP/WebSocket. |
| **1 — Protocol** | `EspNowProtocol.h`; add `broadcast_telemetry()` to gauge; make `OnDataRecv` type-aware. Verify Tab5 sniffs telemetry over serial. | Telemetry visible on Tab5 |
| **2 — Monitor UI** | Fleet table + dashboard rendering telemetry + GlowCraft strips; offline handling; 1280×720 layout. | Live multi-node dashboard |
| **3 — Control** | Tap-to-control sends command packets; gauges respond (targeted + broadcast). | Two-way confirmed |
| **4 — Polish** | Multi-node layout, strip detail view, theming, robustness. | — |

## 7. Open decisions (for when we start)

- **Repo layout:** separate repo vs a `tab5/` subfolder in this one (sharing the
  protocol header).
- **Control targeting:** per-node (targeted MAC) vs broadcast-to-all.
- **Telemetry rate:** fixed heartbeat vs on-change + keepalive.
- **Tab5 UI framework:** M5Unified/M5GFX vs LVGL.
- **Channel strategy:** confirm all gauges + Tab5 stay on channel 1; document the
  constraint (ESP-NOW peers must share a channel).

## 8. Sources

- [M5Stack Tab5 product page (ESP32-P4)](https://shop.m5stack.com/products/m5stack-tab5-iot-development-kit-esp32-p4)
- [Tab5 — m5-docs](https://docs.m5stack.com/en/core/Tab5)
- [Tab5 WiFi — m5-docs](https://docs.m5stack.com/en/arduino/m5tab5/wifi)
- [CNX Software — Tab5 review part 1 (P4 + C6 teardown)](https://www.cnx-software.com/2025/05/14/m5stack-tab5-review-part-1-unboxing-teardown-and-first-try-of-the-esp32-p4-and-esp32-c6-5-inch-iot-devkit/)
- [CNX Software — Tab5 review part 2 (ESP-IDF / Arduino dev)](https://www.cnx-software.com/2025/05/18/m5stack-tab5-review-getting-started-esp32-p4-esp-idf-framework-arduino-ide/)

## 9. Findings & recommended resume path (2026-06-23)

### What we proved (Phase 0)
- Board `esp32-p4-evboard` compiles under pioarduino (arduino-esp32 **3.3.8**).
- **ESP-NOW symbols are undefined at link** — `esp_now_*` exist in no prebuilt P4
  archive. `esp_wifi_remote` (1.6.1) remotes only `esp_wifi`, not ESP-NOW.
- Conclusion: **no standard ESP-NOW on the Tab5** (Arduino or ESP-IDF). Regular
  WiFi/TCP-IP over the hosted C6 *does* work. True ESP-NOW would require custom C6
  firmware (out of scope).

### Recommended resume path — "gateway gauge"
Confines the change to one gauge + the Tab5 and keeps the ESP-NOW fleet intact:

1. Gauges keep ESP-NOW with each other; add the **telemetry broadcast** (§4.1).
2. **One gauge = WiFi gateway:** it already hears all peers over ESP-NOW, so it
   aggregates fleet telemetry and serves it to the Tab5 over its WiFi AP
   (WebSocket / UDP / JSON). It relays Tab5 commands back onto ESP-NOW.
3. **Tab5 = pure WiFi client** (proven on P4). No ESP-NOW, no exotic toolchain.

Alternative if the gateway role is unwanted: **Tab5 hosts an AP, all gauges join
over WiFi/UDP** — drops ESP-NOW and touches every gauge (more disruptive).

### To pick up later
- Decide gateway-gauge vs Tab5-as-AP.
- Choose the WiFi data channel (WebSocket vs UDP vs JSON-HTTP).
- Re-confirm Tab5 WiFi stability (known P4↔C6 SDIO init issues on some boards).
- The `tab5_monitor/` spike can be repurposed as the Tab5 WiFi-client starting point.
