# Gauge Designer + Layout Engine — Progress & Test Plan

**Date:** 2026-07-12
**Author:** Aaron Gillon
**Covers:** the runtime layout engine (firmware) and the Gauge Designer desktop app.
**Related docs:** `Gauge_Designer_PRD.md` (product brief), `Layout_Engine_Spec.md` (schema v1 + engine contract).

---

## 1. Progress Summary

The goal (PRD): design gauge faces visually and deploy them to the ESP32-S3 gauge over WiFi with **no firmware recompile**. That loop is now functional end-to-end. Designs are **Layout Schema v1** JSON documents; the firmware **layout engine** interprets them at runtime and renders via LVGL; the **Gauge Designer** app authors them and pushes them to the device.

### 1.1 Two codebases

| Repo | Location | Branch | Remote |
|---|---|---|---|
| Firmware | `Ultimate_Gauge_Board_PIO` | `feature/glowcraft-integration` | GitHub (pushed through `v2.2.0-m1`) |
| Designer | `Gauge_Designer` | `main` | **none yet** (to be created) |

### 1.2 Firmware milestones

| Version | Tag | What | Device status |
|---|---|---|---|
| 2.1.0 | `v2.1.0` | Full Haltech Broadcast CAN V2 registry, channel-selectable modes | ✅ on-car validated, pushed |
| 2.2.0 (M1) | `v2.2.0-m1` | Layout engine spike: LittleFS store, `/api/layout` POST/GET/DELETE, 7 element types, safety fallback ladder, CORS | ✅ bench round-trip validated, pushed |
| 2.3.0 | `v2.3.0` | Multi-page (build-active-page, `/api/layout/page`), validate-ALL-pages on upload | ⛔ **built, not flashed** |
| 2.4.0 | `v2.4.0` | `visible_if`, `stale_ms`, `shape:"line"` (completes Schema v1) | ⛔ **built, not flashed** |

Firmware branch is **2 commits ahead of origin** (`v2.3.0`, `v2.4.0` unpushed). Archived binaries live in `releases/` (gitignored). **`v2.4.0.bin` is cumulative** — it contains everything below it; flashing it covers all layout-engine testing.

### 1.3 Designer milestones (all on `main`, none flashed against hardware yet beyond M4)

| Commit | What | Status |
|---|---|---|
| `dd524da` | Skeleton: round canvas, Schema v1 types + validator, channel registry snapshot (212 ch) | built |
| `07ea915` | M4: device connect, Deploy/Revert, Live data preview | ✅ end-to-end validated (user) |
| `727b133` | Load-from-Gauge (pull stored layout back into editor) | ✅ validated (user) |
| `ecf17a8` | WYSIWYG fonts (real DSEG14/FiraMono/Montserrat cuts) | built |
| `3b9804e` | Multi-page: page tabs + Live-mode device page follow | ⛔ not device-tested |
| `15eb6fe` | M1.5: visibility inspector, line element, renderer gating | ⛔ not device-tested |

### 1.4 What is proven vs. pending

- **Proven on hardware:** v2.1.0 (on-car); the layout round-trip (deploy → render → revert → persist) at M1; the Designer connect/deploy/live/load loop.
- **Pending first flash + bench pass:** multi-page (v2.3.0), the M1.5 features (v2.4.0), and the corresponding Designer features (multi-page tabs, visibility controls, line element).

### 1.5 Known limitations / out of scope (do NOT log as bugs)

- **Page switching is API/Designer-driven only** — no physical on-car page switch (rotary/button) yet.
- **`visible_if` binds to scalar channels only.** True bit-channels (e.g. the actual Check Engine Light bit) are not key-addressable in the registry; scalar booleans like "Engine Limiting Active" work.
- **`shape:"line"`, `image`, custom fonts:** lines are supported; `image`/custom fonts are Phase 2 (need `LV_USE_IMAGE`, asset pipeline).
- **Designer text fidelity** uses the real font cuts but LVGL's exact glyph metrics may differ by a pixel or two; the device is the source of truth.
- **Theme tokens** render as preview colours in the Designer but are read-only in the inspector (edit via JSON for now).
- **The split int/dec numeric** of the original built-in face isn't expressible in Schema v1 (single `numeric` used instead).

---

## 2. Test Methodology

### 2.1 Principles

1. **Device is the source of truth.** The Designer simulator is a high-fidelity preview, but every visual/behaviour claim is confirmed on the panel.
2. **Test Mode drives data without an engine.** The gauge's Test Mode injects synthetic sweeps for the common channels, so all data-bound behaviour (needles, zones, `visible_if`, `stale_ms`) can be exercised on the bench.
3. **Always return to a known state.** `DELETE /api/layout` reverts to the built-in face; a power cycle re-loads the stored layout. Use these between cases.
4. **Never trust a green build as a pass.** Every firmware change here is flashed and observed; the layout engine's failure modes are visual/behavioural, not compile-time.
5. **Prove the escape hatch first.** The first check after any flash is that OTA + `/otafallback` still work, so a bad build can always be recovered.

### 2.2 Test environment

| Item | Requirement |
|---|---|
| Hardware | ESP32-S3 480×480 round gauge (the board) |
| Power | **A stiff 5V supply.** Full brightness has browned out on weak USB supplies; if you see `E BOD`/brownout resets, drop brightness or use a better supply before investigating anything else. |
| Bus (optional) | Live Haltech CAN for real-data validation; otherwise use **Test Mode**. |
| PC | Joined to the gauge's WiFi AP `Haltech-<name>` (default `http://192.168.4.1`). |
| Second gauge (optional) | For fleet regression only. |

**Windows/PowerShell gotcha:** `curl` is aliased to `Invoke-WebRequest` and does NOT accept Unix flags. Use **`curl.exe`** and quote the `@file` (the `@` is a PowerShell operator):
```powershell
curl.exe -X POST -H "Content-Type: application/json" --data "@test_layouts/classic_face.json" http://192.168.4.1/api/layout
```
Or native: `Invoke-RestMethod -Uri http://192.168.4.1/api/layout -Method Post -ContentType application/json -InFile test_layouts\classic_face.json`.

### 2.3 Test assets (in `test_layouts/`)

| File | Exercises |
|---|---|
| `classic_face.json` | ring, needle, numeric, text, bar, warning (6 of 7 types) |
| `two_page.json` | multi-page (Boost + Temps), page switching |
| `m15_features.json` | `shape:"line"`, `stale_ms`, `visible_if` |

### 2.4 Flashing

Flash **`releases/v2.4.0.bin`** (cumulative) via web `/ota`, or `pio run -t upload` over USB. Note the first boot after enabling the layout feature auto-formats the (previously unused) LittleFS partition — expect a one-time `[LAYOUT] LittleFS mounted` log line.

---

## 3. Test Suites

Work top to bottom after a flash. `[ ]` = check on device. Record FW build string (`/api/state` → `build`) so you know which binary produced a result.

### Suite A — Boot, safety & recovery (run first)

- [ ] **A1 Recovery proven.** `http://192.168.4.1/otafallback` loads; `/ota` accepts a re-flash of the same bin.
- [ ] **A2 Clean boot.** Serial shows reset reason, heap line, `LittleFS mounted`, and **no** `[SAFE]` line. Panel lights with the default gauge face (no layout stored yet).
- [ ] **A3 Heap headroom.** `[BOOT] Setup complete` reports internal heap comfortably positive (six figures) and PSRAM ~7MB free.
- [ ] **A4 Double power-cycle.** Power-cycle twice; no bootloop, no `[SAFE]`.
- [ ] **A5 CAN-less survival.** With no CAN bus connected, display + web stay up (data reads zero/stale). Pulling the transceiver mid-run does not crash.

### Suite B — Layout API round-trip

- [ ] **B1 Deploy.** POST `classic_face.json`. Response body `OK`; panel swaps to the layout within ~1–2 frames (no reboot).
- [ ] **B2 Read back.** `GET /api/layout` returns the stored JSON; `GET /api/state` shows `layoutActive:true`, `layoutName:"Classic Boost"`.
- [ ] **B3 Test Mode animation.** Enable Test Mode (web UI or `POST /api/action/test`). Needle sweeps, ring changes colour through zones, numeric counts, bar tracks.
- [ ] **B4 Revert.** `DELETE /api/layout`. Panel returns to the built-in face; `/api/state` `layoutActive:false`.
- [ ] **B5 Validation rejects bad input.** POST a broken doc (e.g. `{"schema":"ugb-layout","v":1}`) → **HTTP 400** with a short reason; the current face is **untouched**.
- [ ] **B6 Oversize guard.** POST a >16KB body → 400, no crash.

### Suite C — Element rendering fidelity

With `classic_face.json` (or a design built in the Designer) deployed and Test Mode on, compare panel vs. Designer canvas:

- [ ] **C1 text** — string, font, colour, alignment correct.
- [ ] **C2 numeric** — value, decimals, unit suffix; anchor stays put as digit count changes.
- [ ] **C3 needle** — sweep direction/angle matches the canvas (135°+270° default) across the full range.
- [ ] **C4 ring** — colour-zone thresholds switch at the right values.
- [ ] **C5 bar** — fill grows correctly (h/v), zone colours apply.
- [ ] **C6 shape** — rect/circle position, radius, fill/stroke.
- [ ] **C7 warning** — appears/hides on its channel.
- [ ] **C8 background** — solid / theme / gradient all render; theme bg follows the active theme.

### Suite D — Multi-page (v2.3.0)

Deploy `two_page.json`.

- [ ] **D1 Start page.** Boots/loads on page 0 (Boost). `/api/state` shows `layoutPage:0`, `layoutPages:2`.
- [ ] **D2 API switch.** `curl.exe -X POST "http://192.168.4.1/api/layout/page?p=1"` → panel shows page 1 (Temps: Water + Oil P). `?p=0` returns.
- [ ] **D3 Bad index.** `?p=5` → 400, page unchanged.
- [ ] **D4 Designer follow.** In the Designer: Connect → Live: ON → click the page tabs → the physical panel follows.
- [ ] **D5 Memory.** Switching pages repeatedly does not leak (heap stable on `/api/state` across ~20 switches).
- [ ] **D6 Validate-all.** Deploy a layout whose **second** page has a bad element → rejected at upload (400), not on switch.

### Suite E — Visibility features (v2.4.0)

Deploy `m15_features.json`, Test Mode on.

- [ ] **E1 `visible_if`.** The "HIGH BOOST" warning appears only while boost > 15 (display units) and hides below it, tracking the sweep.
- [ ] **E2 `shape:"line"`.** The decorative line renders at its two points and stays put.
- [ ] **E3 `stale_ms`.** Stop the boost data (turn Test Mode off, or disconnect the boost source): the numeric bound with `stale_ms:1500` **blanks** after ~1.5s; restoring data brings it back and it repaints correctly (no stale value flash).
- [ ] **E4 Re-show integrity.** Repeatedly cross the `visible_if` threshold; the warning re-appears cleanly each time (no ghosting / wrong text).

### Suite F — Designer app (browser, `npm run dev`)

- [ ] **F1 Connect.** Enter the gauge URL → Connect → green dot, name + fw + active-layout status.
- [ ] **F2 Live data.** Live: ON → canvas renders real values (rev engine or Test Mode); needle/zones move with the panel.
- [ ] **F3 Deploy.** Edit the Classic Face template, Deploy to Gauge → panel updates; a local validation failure blocks deploy with a reason.
- [ ] **F4 Reject surfaced.** Force a device-side rejection (shouldn't normally happen post local-validate) → the firmware's 400 reason is shown verbatim.
- [ ] **F5 Load-from-Gauge.** Deploy something, click Load from Gauge → the exact layout (labels + elements) returns into the editor; edit + redeploy round-trips.
- [ ] **F6 Revert.** Revert to Default → built-in face returns.
- [ ] **F7 Fonts.** Numeric readouts render in DSEG14 (not a monospace fallback) on the canvas.
- [ ] **F8 Multi-page authoring.** Add/rename/delete pages, set start page (★); the tab bar behaves; undo/redo covers page ops.
- [ ] **F9 Visibility authoring.** Add `visible_if` to an element via the inspector; hidden elements dim to 22% but stay selectable; deploy → matches the device.
- [ ] **F10 Line authoring.** Add a `line`, edit its points, drag it; deploy → renders on the panel.
- [ ] **F11 Save/Open.** Save a design to disk, New, Open it back → identical (round-trip fidelity).

### Suite G — Persistence & crash-safety

- [ ] **G1 Survives reboot.** Deploy a layout, power-cycle → the layout re-loads automatically (not the default face).
- [ ] **G2 Safe-mode disables a bad layout.** (Destructive test.) Deploy a layout that renders but crashes/hangs the panel; on the next boot the `bootok` guard logs `[SAFE] Disabling stored layout` and boots the **default face**. A subsequent good POST re-enables layouts.
- [ ] **G3 Delete clears storage.** DELETE, power-cycle → still default face (file gone).

### Suite H — Regression (existing features must still work)

- [ ] **H1** Built-in gauge face renders and tracks data when **no** layout is stored (all four modes).
- [ ] **H2** Theme slots + gradients apply; trimpot theme-sync switches slots.
- [ ] **H3** GlowCraft page renders; Test Mode rainbow.
- [ ] **H4** Web UI SPA loads, all controls work; `/preview` snapshot streams.
- [ ] **H5** Fleet: a second gauge appears as a peer; Apply-to-ALL / mode push land (if two units available).
- [ ] **H6** No `[CFG][BUG]` lines on serial during any of the above (NVS-write-off-loop-task guard).

### Suite I — Performance

- [ ] **I1 FPS.** With a layout active + Test Mode + Stats overlay… (note: the stats overlay belongs to the built-in face, so read FPS via the built-in face, or via serial timing). Target: comparable to the ~30 fps baseline; a layout of ≤ a dozen elements should not regress it.
- [ ] **I2 Gradient cost.** A layout using a radial/conical background is the heavy case — confirm it renders without the old TLSF/heap crash (the 96K PSRAM pool covers it) and note any FPS drop.
- [ ] **I3 Memory ceiling.** Deploy a max-ish layout (near 64 elements / 4 pages) → no `lv_malloc` failures, heap stable.

---

## 4. Exit criteria (to call the layout engine "bench-validated")

- Suites A, B, D, E, G pass on `v2.4.0`.
- Suite F passes with the Designer against the same firmware.
- Suite H shows zero regressions in the pre-existing gauge/theme/GlowCraft/fleet/web features.
- No brownout resets on the intended supply.

On pass: flash to the car for a real-data soak, push the firmware branch + tags, and create the Designer GitHub remote.

---

## 5. Bug report template

```
Title:
FW build (/api/state "build"):   Designer commit:
Layout file (attach) + which page:
Data source: Test Mode | Live CAN
Steps:
Expected:
Actual (photo of panel + Designer canvas side by side if visual):
Serial log around the event (esp. [LAYOUT]/[SAFE]/[CFG][BUG]/Guru Meditation):
Recoverable via DELETE / power-cycle / OTA? :
```

---

## 6. Change log

| Date | Note |
|---|---|
| 2026-07-12 | Initial progress + test plan (covers firmware v2.1.0–v2.4.0, Designer through M1.5). |
