# Evo X (SST) Support — Phased Plan

**Date:** 2026-07-20
**Author:** Aaron Gillon
**Covers:** extending the gauge firmware to a second vehicle — a Mitsubishi Lancer Evolution X (CZ4A) with the TC-SST transmission — with a focus on reading SST/clutch temperatures and gear/mode.
**Related:** `Haltech_CAN_Bus_Reference.md` (the car #1 bus map + sniffer), memory `project_evo_x_support.md`.
**Status:** discovery build shipped; everything past Phase 0 is unstarted and gated on an on-car sniff.

---

## 1. Decision & Goal

Support a 2nd car (Evo X) **by extending this repo, NOT by forking.** ~80% of the code — the web SPA, Gauge Designer/layout engine, theming, ESP-NOW fleet, OTA, `config_store`, dimming, the CAN sniffer — is vehicle-agnostic. Only the **decode layer** is Haltech-specific. A fork would duplicate all the agnostic infrastructure and guarantee permanent divergence (every layout-engine/OTA/theming fix ported twice). Not worth it.

**Target architecture:** a *signal-source* seam. A vehicle driver produces named channels; the layout/gauge consumes them, not caring which car produced "gear" or "clutch_temp_a".

```
Vehicle driver (produces named channels) ──► BehaviorConfig / layout engine ──► gauge render
   ├── HaltechSource   (existing: passive broadcast decode)
   └── EvoSource       (new: passive broadcast + active request/response poller)
```

Target is selected at **compile time** via PlatformIO envs — one web UI, two firmware binaries. No runtime multi-vehicle (it's two physical boards in two cars).

---

## 2. The three fork-points (why this isn't just "add a registry")

1. **Bitrate.** Evo OEM bus is **500 kbit**; Haltech is 1 Mbit. One line in `CANBus_Driver.cpp` (now `#ifdef EVO_SNIFFER`).
2. **Passive broadcast vs. active polling — the big one.** Haltech broadcasts everything. The Evo broadcasts RPM/speed/coolant/TPS, **but the SST clutch/trans temps are request/response PIDs, NOT broadcast** — they never appear on the bus unless you ask for them. This is a fundamentally different data-flow model and the main body of new work. It is also exactly why `canbus_send()` (kept in v2.6.2) is load-bearing.
3. **Channel identity.** `BehaviorConfig.chan_key` is a Haltech concept. The signal-source seam needs a generic channel-key namespace so SST channels are first-class.

---

## 3. Current State (what's shipped — v2.7.0, `feature/glowcraft-integration`)

Bus speed is a **runtime setting**, not a build flag — one firmware covers both cars. (Superseded the earlier `[env:evo-sniffer]` compile-time approach; that scratch branch is folded in.)

- **Web "CAN Bus" card** → selectable speed: **1 Mbit (Haltech)** / **500 kbit (Evo/OEM)** / 250 kbit. Persisted to NVS (`canbaud`); changing it **restarts the device** to re-init TWAI cleanly. Action `POST /api/action/canbaud?v=<bps>`; reported in `/api/state` as `canBaud`.
- Boot reads the speed early (`cfg_peek_can_bitrate()`) and calls `canbus_set_bitrate()` **before** `drivers_init()` (CAN installs there, ahead of the full `cfg_load_all`).
- **Decode behaviour keys off the speed** (`haltech_decode.cpp`): at **1 Mbit** = normal Haltech registry decode + non-Haltech sniffer; at **any other speed** = Haltech decode skipped, **every** frame captured raw (the registry would mis-claim overlapping IDs like `0x380` and hide them). `SNIFF_SLOTS` = 48; `can_label()` starter labels always compiled.
- **On 500 kbit the gauge face won't populate** — there's no Evo channel decoder yet; the sniffer is the tool. That's expected for discovery mode.
- Binaries: `releases/v2.7.0.bin` (app-only, OTA) and `releases/v2.7.0.factory.bin` (full image for a fresh board — a blank board needs this, not the app-only bin).

**Field note:** a "no WiFi AP" scare on first flash was two red herrings — (a) a fresh board needs the *factory* image (bootloader+partitions+app), not the app-only bin; (b) the test board had **no antenna fitted**, so the AP barely radiated. Firmware was fine. Native USB-CDC also loses the one-time boot banner (CDC enumerates ~1–2 s into boot), so serial silence is **not** a crash signal on this hardware — use the screen (backlit/gauge UI = past `drivers_init`) as the boot-progress signal instead.

---

## 4. CAN Topology & Tap Point

SST temps are **never visible passively** at any location — they only exist as a *reply* to a request. So the question is "which bus does the TCU answer diagnostic requests on," which differs from "which bus carries broadcast RPM."

Sources disagree (this is reverse-engineered, not spec):

| Source | Tap | Claim |
|---|---|---|
| ECUMaster ADU | OBD2 pins **6 (H) / 14 (L)** | Reads ECU broadcast (RPM) **and** OBD2 request/response simultaneously |
| Racelogic | ECU pins **90 (H) / 91 (L)** | "CAN data is **not** available via the OBD port" (passive logger, taps ECU) |
| EvoScan + Tactrix | OBD2 port | Gets SST clutch/trans temps via request/response — through **OBD2** |

**Decision: tap the OBD2 port (pins 6/14).** It is the one location shown to give *both* the passive broadcast frames *and* the request/response SST temps, so we don't gamble on TCU reachability, and it's plug-in (no ECU-harness splicing). **Caveat:** OBD2 pin 16 is permanent battery+ — power the gauge from switched ignition (or accept key-off draw). 500 kbit, no terminator either way.

Phase 0 confirms this before any code is written.

---

## 5. Known Evo X frames (starter map — UNVERIFIED)

Pre-loaded into `can_label()` (community-sourced: ECUMaster ADU note, EvolutionM/AutosportLabs captures, EvoScan). Trailing `?` = confirm on car by watching the byte move.

| ID (hex) | Guess |
|---|---|
| `0x308` | RPM |
| `0x210` | Throttle / TPS |
| `0x212` | Idle RPM target |
| `0x380` | Brake / Clutch switch |
| `0x415` | A/C switch |
| `0x608` | Coolant temp (ECT) |

**Semantics known, carrying-ID not yet found** (pin down by watching sniffer while operating the control):
- Gear: `0=Park  8=Reverse  16=Neutral  32=Drive`
- Gearbox mode: `1=S-Sport  2=Sport  3=Normal`
- Diff (S-AWC) mode: `1=Tarmac  2=Gravel  3=Snow`

**SST temps (request/response, EvoScan naming):**
- Trans oil temp: request `CAN28-0`, formula `x − 50`, 1 byte
- Clutch temp (odd): request `CAN33-0`, formula `x / 4`, 2 bytes
- Clutch temp (even): request `CAN33-2`, formula `x / 4`, 2 bytes

We have the *formulas* but NOT the raw request bytes (request arbitration ID + service/PID payload + response ID/offset). Getting those is Phase 1's gate (§6).

---

## 6. Phased Plan

### Phase 0 — On-car discovery (no new code; uses the shipped sniffer)
**Goal:** validate the tap point and gather the raw request/response spec in one sitting.
1. Fit antenna. Flash `v2.7.0.factory.bin` to a spare board (fresh board needs the factory image, not the app-only bin).
2. Wire to **OBD2 pins 6/14**, power from ignition. Connect to the AP, open the **CAN Bus** card and select **500 kbit**; the gauge restarts into discovery mode. Open the **CAN Sniffer** card.
3. Confirm broadcast frames appear at OBD2 (RPM `0x308?` moves with revs, coolant `0x608?` rises, etc.). ✅ → OBD2 is the single tap.
4. Verify the starter labels; correct any that are wrong.
5. Shift P-R-N-D / change drive mode / diff mode while watching — record which ID carries **gear**, **gearbox mode**, **diff mode**.
6. **Capture the SST request/response:** run an EvoScan (Tactrix OpenPort 2.0) session on the car with our sniffer listening passively. Record exactly what EvoScan *sends* (request ID + bytes) and what the TCU *replies* (response ID + bytes) for the CAN28/CAN33 temps.
   - *Faster alternative:* lift the request definitions straight from EvoScan's PID config files (the formulas we already have came from there; the requests sit next to them).

**Exit criteria:** OBD2 confirmed as tap; broadcast IDs for gear/mode identified; exact SST request/response frames captured.

### Phase 1 — EvoSource, passive channels (build after Phase 0)
**Goal:** the gauge shows the *broadcast* Evo data natively (no polling yet).
- New `EvoSource` decode module + `evo_channels` registry (RPM, speed, coolant, TPS, gear, mode, diff mode) with the IDs/offsets/scales confirmed in Phase 0.
- Introduce the **generic channel-identity** namespace so Evo channels are addressable by the layout engine / `BehaviorConfig` the same way Haltech channels are (fork-point #3).
- Promote `[env:evo-sniffer]` toward a real `[env:evo]` target (keep the sniffer card as a diagnostic).

**Exit criteria:** Evo broadcast channels render on the gauge and in the Designer channel list.

### Phase 2 — SST request/response poller (the core new capability)
**Goal:** SST clutch/trans temps on the gauge. This is the request/response engine.
1. **Request scheduler** — send the Phase-0 request frame(s) on a timer (~5–10 Hz, staggered) via `canbus_send()`.
2. **Response handling** — 1–2-byte temps fit a **single CAN frame**, so likely only single-frame ISO-TP parsing is needed (read PCI length byte, extract data). *Full ISO-TP (ISO 15765-2: First Frame / Flow Control / Consecutive Frames) is only required if we batch many values per request — almost certainly not needed for a few temps.*
3. **Match + extract + scale** — recognise response ID, pull byte(s) at known offset, apply `x−50` / `x/4`.
4. **Staleness/timeout** — reuse the existing `s_seen[]`/age model so a non-responding TCU shows stale, not a frozen value.
5. **Channel plumbing** — expose SST temps as pseudo-channels via the Phase-1 namespace so the gauge/layout renders them like any other channel.

**Effort:** ~a few hundred lines if single-frame (likely). The gate is Phase 0's capture, not the code.

**Safety:** read-only requests only (ReadDataByIdentifier / EvoScan read PIDs — these run continuously while driving, so benign). **Never** send anything that opens a writing session or a non-driving diagnostic mode. Rate-limit; coexist politely with a factory scan tool if present.

### Phase 3 — Productionise
- Default Evo layout/theme; units (°C SST temps, gear text P/R/N/D + numeric).
- Fold `[env:evo]` into a clean multi-env split (shared core lib + two thin vehicle sources); retire `scratch/evo-sniffer-500k`.
- Doc the Evo bus in a companion to `Haltech_CAN_Bus_Reference.md`.

---

## 7. Dependency chain (planning summary)

```
Phase 0 (sniff OBD2)  ─┬─►  confirm tap point + broadcast IDs   ─►  Phase 1 (passive EvoSource)
                       └─►  capture SST request/response bytes  ─►  Phase 2 (poller) ─► Phase 3
```

The single on-car sniff (Phase 0) does double duty: it validates OBD2 as the tap **and** gathers the SST request spec. Nothing in Phase 1/2 can start until it's done.

---

## 8. Open questions / risks

- **Gateway topology:** does the OBD2 CAN (pins 6/14) actually carry the 500k broadcast bus on *this* car, or only diagnostics? Racelogic's "no CAN on OBD" note is the yellow flag. **Resolved by Phase 0 step 3.**
- **Exact SST request frames** are unknown until Phase 0 step 6 (or EvoScan config extraction).
- **Single-frame vs. multi-frame** responses — determines whether we need a full ISO-TP layer. Expected single-frame; confirm from the Phase-0 capture.
- **Bus citizenship / TCU session state** — validate our polling doesn't upset the TCU or a coexisting scan tool. Keep strictly read-only.
- **Power** — OBD2 pin 16 is always-hot; wire the gauge to switched ignition.

---

## 9. Sources

- ECUMaster ADU Application Note — MITSUBISHI LANCER EVOLUTION X (rev 1.01): channel semantics (gear/mode/diff encodings), OBD2 pinout, 500 kbps.
- Racelogic Vehicle CAN Database — Mitsubishi EVO X: ECU-pin tap (90/91), 500 kbps.
- EvoScan / Tactrix community (ClubRalliart, EvolutionM, AutosportLabs): SST temp request/response PIDs (CAN28-0, CAN33-0/2) + formulas; broadcast IDs (0x308 RPM, 0x608 ECT, 0x210 TPS, 0x380 brake/clutch, 0x415 A/C).
