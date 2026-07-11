# Product Requirements Document — Gauge Designer

**Product Name:** Gauge Designer (working title)
**Status:** Draft v0.1 — initial brief for iteration
**Author:** Aaron Gillon
**Stakeholders:** Aaron Gillon (product/eng), Ultimate Gauge Board firmware, builder community (secondary users)
**Date Created:** 2026-07-11
**Last Updated:** 2026-07-11
**Version:** 0.1

> **Scope note:** This PRD covers the **desktop Gauge Designer application**. It has a hard dependency on a new **firmware-side layout engine** (a runtime interpreter that renders exported layouts via LVGL). That engine is a companion workstream on the Ultimate Gauge Board firmware and is summarised here under Technical Considerations + Dependencies; it likely warrants its own short spec (`Layout_Engine_Spec.md`).

---

## Executive Summary

**One-liner:** A cross-platform desktop app for visually designing round automotive gauge faces — graphics, text, and live-data readouts — that deploy to the Ultimate Gauge Board over WiFi with no firmware recompile.

**Overview:**
The Ultimate Gauge Board is an ESP32-S3 / 480×480 round-display gauge that renders engine data from the Haltech Broadcast CAN protocol using LVGL. Today, every gauge face is hand-coded in C and requires a full firmware build + OTA flash to change. Layout, colours, channel bindings, and text are all baked into `ui/gauge_page.cpp`. Iterating on a design means editing C, compiling, and flashing — a loop measured in minutes, only accessible to someone comfortable in the firmware.

Gauge Designer decouples **design** from **firmware**. Users lay out a gauge visually on a canvas that mirrors the real 480×480 round panel, bind elements to any of the ~160 Haltech channels the firmware already knows (via the `/api/channels` registry shipped in firmware v2.1.0), style them, and export a compact **layout description** (JSON). The firmware gains a **layout engine** that interprets that description and builds the LVGL scene at runtime, so a new design is an upload — not a reflash.

This is the natural next step of the firmware's trajectory: v2.1.0 deliberately made data (channels, themes, behavior config) machine-readable and API-exposed precisely so an external design tool could consume it. Gauge Designer turns the gauge from a fixed-function device into a user-configurable display platform, and opens gauge design to builders who can't (or don't want to) touch C.

**Quick Facts:**
- **Target Users:** The developer (primary); Haltech/GlowCraft hardware builders (secondary).
- **Problem Solved:** Gauge faces are hardcoded — changing a design requires firmware edit + build + flash, gated on C expertise.
- **Key Metric:** Time-to-first-deployed-custom-design (target: minutes, no C, no toolchain).
- **Target Launch:** MVP TBD — see Timeline (phased, dependency-gated on the firmware layout engine).

---

## Table of Contents

1. [Problem Statement](#problem-statement)
2. [Goals & Objectives](#goals--objectives)
3. [User Personas](#user-personas)
4. [User Stories & Requirements](#user-stories--requirements)
5. [Success Metrics](#success-metrics)
6. [Scope](#scope)
7. [Technical Considerations](#technical-considerations)
8. [Design & UX Requirements](#design--ux-requirements)
9. [Timeline & Milestones](#timeline--milestones)
10. [Risks & Mitigation](#risks--mitigation)
11. [Dependencies & Assumptions](#dependencies--assumptions)
12. [Open Questions](#open-questions)

---

## Problem Statement

### The Problem
Gauge faces on the Ultimate Gauge Board are defined entirely in firmware C (`ui/gauge_page.cpp`: object creation, absolute positions, fonts, colours, channel reads). Creating or changing a design requires editing that code, rebuilding with PlatformIO, and flashing over OTA. This makes design iteration slow, error-prone, and inaccessible to anyone who isn't a firmware developer.

### Current State
- **Layout changes** = C edits to `load_current_style()` + `update_gauge_master()`, recompile, flash.
- **What's already configurable at runtime** (v2.x): theme colours + gradients (4 slots), per-mode channel binding + range/zones + label, display units, secondary readout. But the **arrangement and composition** of elements — where the needle is, what text sits where, adding a second readout, a warning light, an image — is fixed in code.
- **Workaround today:** none, really. You live with the one hardcoded face, tweaking only the exposed colour/channel/range knobs.

### Impact
**User impact:**
- Every non-trivial visual change is a firmware round-trip (edit → `pio run` ~40s → OTA → verify), gated on C fluency and a working toolchain.
- Builders who own the hardware but not the codebase cannot create their own gauge faces at all.
- No way to preview a design without flashing it to real hardware.

**Project impact:**
- The gauge's flexibility is capped by whatever faces the maintainer hand-codes.
- The data-driven groundwork already paid for in v2.1.0 (channel registry, JSON API, theme/behavior config) is under-utilised without a tool to author against it.

### Why Now?
Firmware v2.1.0 shipped the enabling contract: a stable, machine-readable channel registry (`chan_key = (can_id<<4)|offset`) served at `GET /api/channels`, plus an established JSON API + OTA + gzipped-SPA delivery pattern. The hard data-modelling work is done. A design tool is the missing half — and it was explicitly the intended consumer of that contract.

---

## Goals & Objectives

### Project Goals
1. **Decouple design from firmware:** New gauge faces deploy as data (a layout file over WiFi), never requiring a recompile/reflash for a design change.
2. **Open design to non-coders:** A builder with the hardware can create, preview, and deploy a gauge face without touching C or installing a toolchain.
3. **Leverage the v2.1.0 contract:** Consume `/api/channels`, themes, and behavior config as the single source of truth so the tool and firmware never drift.
4. **Establish a durable layout format:** A versioned, documented **Layout Schema** that both the designer and firmware engine agree on — the long-term interchange format for gauge designs.

### User Goals
1. **Design visually:** Drag/place/style elements on a true-to-hardware round canvas and see exactly what the gauge will show.
2. **Bind to real data:** Point any element at any Haltech channel by name, with correct units and live values.
3. **Deploy fast:** Push a finished design to the gauge in seconds and see it on the real panel.
4. **Share:** Save, export, and exchange designs as files with other builders.

### Non-Goals
- **Not** a general-purpose LVGL IDE or a replacement for hand-written firmware UIs.
- **Not** a firmware flashing/OTA tool (existing web `/ota` + PlatformIO cover that; Designer may *link* to OTA but won't own firmware images).
- **Not** a Haltech tuning/NSP tool — it consumes broadcast data, it doesn't configure the ECU.
- **Not** (MVP) a real-time animation/scripting environment — bindings + thresholds, not arbitrary logic.

---

## User Personas

### Primary Persona: "The Maintainer" (Aaron)
- **Role:** Owns the firmware, the hardware, and the car. High tech + embedded fluency.
- **Behaviors:** Iterates on gauge faces frequently; wants to try layouts quickly without a C round-trip; values a clean data model and a format he can version in git.
- **Needs:** Fast design iteration; a layout format that maps cleanly onto LVGL and firmware memory limits; live preview on real hardware; ability to fall back to C when needed.
- **Pain points:** Firmware round-trips for cosmetic changes; no preview without flashing; layout logic tangled into `gauge_page.cpp`.
- **Quote:** _"I want to move the AFR readout and add an oil-temp bar without opening the compiler."_

### Secondary Persona: "The Builder"
- **Role:** Owns/assembles the gauge hardware (Haltech + GlowCraft scene). Comfortable with phone/desktop apps and wiring, **not** with C or PlatformIO.
- **Behaviors:** Wants a gauge that looks the way *they* want; will follow a guide but won't debug a build; shares setups in forums/Discord.
- **Needs:** An install-and-go desktop app; templates to start from; forgiving UX; a one-click "send to gauge."
- **Pain points:** Can't currently customise layout at all; the firmware toolchain is a wall.
- **Quote:** _"Give me a canvas and my channel list and let me drag stuff around."_

---

## User Stories & Requirements

### Epic A: Canvas & Layout Editing

#### Must-Have (P0)

##### Story A1: True-to-hardware canvas
```
As a designer,
I want a 480×480 round canvas that matches the real panel,
So that what I design is what the gauge shows.
```
**Acceptance Criteria:**
- [ ] Canvas is a 480×480 workspace with the round bezel/safe-area overlaid.
- [ ] Pixel coordinates map 1:1 to the device; off-round-area content is visibly flagged.
- [ ] Zoom + pan; snapping to centre, edges, and a grid.

##### Story A2: Place and arrange elements
```
As a designer,
I want to add, position, resize, layer, and delete elements,
So that I can compose a gauge face.
```
**Acceptance Criteria:**
- [ ] Add elements from a palette (see Element Types below).
- [ ] Move (drag + arrow-key nudge), resize (handles + numeric), rotate where meaningful (needle/line).
- [ ] Z-order control; multi-select; align/distribute; undo/redo.
- [ ] Per-element inspector panel with numeric position/size/style.

##### Story A3: Element types (MVP set)
```
As a designer,
I want a core set of gauge building blocks,
So that I can build the common gauge faces.
```
**Acceptance Criteria — MVP element types:**
- [ ] **Static text** — string, font, size, colour, alignment.
- [ ] **Numeric readout** — bound channel, decimals, unit suffix, font/colour, prefix/label.
- [ ] **Needle** — bound channel, min/max, start/sweep angle, length/width/colour (mirrors current needle).
- [ ] **Arc / ring** — bound channel, min/max, colour-zone thresholds (mirrors current colour-changing ring).
- [ ] **Bar / linear gauge** — bound channel, min/max, orientation, fill/zone colours.
- [ ] **Image** — imported bitmap, position/scale/opacity.
- [ ] **Shape** — rect/circle/line with fill, stroke, corner radius, gradient.
- [ ] **Warning indicator** — bound boolean channel (e.g. Check Engine Light), on/off visibility + colour.

#### Should-Have (P1)
- **Story A4: Multi-page designs** — a layout holds N pages; page-switch mapping (e.g. mode button / rotary trim) is configurable. *(Firmware already supports a multi-screen page system.)*
- **Story A5: Templates & starter faces** — ship the current default gauge + a few presets as editable starting points.
- **Story A6: Reusable style/theme tokens** — reference the firmware's 4 theme slots so a design inherits live theme colours instead of hardcoding hex.

### Epic B: Data Binding

#### Must-Have (P0)

##### Story B1: Channel picker from the live registry
```
As a designer,
I want to bind an element to any Haltech channel by name,
So that it shows real engine data.
```
**Acceptance Criteria:**
- [ ] Channel list sourced from `/api/channels` (live device) or a bundled snapshot (offline).
- [ ] Search/filter by name; grouped by CAN ID/category; shows unit + current live value when connected.
- [ ] Binding stored by stable `chan_key`, not by index/name (survives registry additions).
- [ ] Units respect the device's display-unit prefs (psi/kPa, °C/°F, km/h/mph, AFR/λ).

##### Story B2: Ranges, zones, and formatting
```
As a designer,
I want to set a channel's display range, colour zones, and number format,
So that the readout/needle/arc behaves correctly.
```
**Acceptance Criteria:**
- [ ] Per-binding min/max, zone thresholds (z1/z2), decimals, smoothing hint.
- [ ] Defaults pulled from the device's behavior config where one exists for that channel.

### Epic C: Preview & Deploy

#### Must-Have (P0)

##### Story C1: Accurate on-screen simulator (offline)
```
As a designer,
I want to preview the design with simulated or live data without hardware,
So that I can iterate away from the car.
```
**Acceptance Criteria:**
- [ ] Renders the layout with the same geometry/units the firmware will use.
- [ ] Data source: manual sliders per bound channel, a demo/sweep mode, or live device.
- [ ] Round-panel masking applied; visually matches the device within reason.

##### Story C2: Connect to a live gauge
```
As a designer,
I want the app to find and connect to my gauge over its WiFi AP,
So that I can use real channel data and deploy.
```
**Acceptance Criteria:**
- [ ] Enter/discover the gauge AP (`Haltech-<name>`, default `192.168.4.1`); show connection status + firmware version.
- [ ] Pull `/api/channels`, `/api/themes`, `/api/config` on connect.
- [ ] Graceful offline mode when no device is reachable (bundled registry snapshot).

##### Story C3: Deploy a layout to the gauge
```
As a designer,
I want to push my finished design to the gauge and see it live,
So that deployment is a single action, not a firmware build.
```
**Acceptance Criteria:**
- [ ] Export the design to the Layout Schema (validated before send).
- [ ] Upload to a new firmware endpoint (`POST /api/layout`); firmware persists + renders it.
- [ ] Confirmation of success; the gauge shows the new face without reflashing.
- [ ] Layout size stays within the firmware's declared budget (tool warns before send).

### Epic D: Files & Sharing (P1)
- **Story D1: Save/open/versioned project files** — human-readable, git-friendly.
- **Story D2: Import/export a shareable layout file** — one file another builder can open or push.
- **Story D3: Round-trip fidelity** — export → import reproduces the design exactly.

### Functional Requirements (summary)

| Req ID | Description | Priority |
|--------|-------------|----------|
| FR-001 | 480×480 round canvas, 1:1 device mapping, zoom/pan/grid/snap | Must |
| FR-002 | MVP element palette (text, numeric, needle, arc, bar, image, shape, warning) | Must |
| FR-003 | Element inspector: numeric position/size + style | Must |
| FR-004 | Channel binding via `/api/channels`, stored by `chan_key` | Must |
| FR-005 | Per-binding range/zones/format | Must |
| FR-006 | On-screen simulator with manual/demo/live data | Must |
| FR-007 | Live device connect (WiFi AP), pull registry/themes/config | Must |
| FR-008 | Export + validate Layout Schema; deploy via `POST /api/layout` | Must |
| FR-009 | Save/open project files; import/export shareable layout | Should |
| FR-010 | Multi-page designs + page-switch mapping | Should |
| FR-011 | Templates/presets incl. current default face | Should |
| FR-012 | Theme-token references (inherit live theme slots) | Should |
| FR-013 | Custom font/image import + conversion to LVGL assets | Should |

### Non-Functional Requirements

| Req ID | Category | Target |
|--------|----------|--------|
| NFR-001 | Footprint | Lightweight native binary (Tauri; no bundled Node/Chromium runtime) |
| NFR-002 | Platforms | Windows first; macOS/Linux via same Tauri codebase |
| NFR-003 | Offline | Fully usable offline (design + simulate) with a bundled registry snapshot |
| NFR-004 | Layout size | Exported layout + assets fit the firmware budget (see Technical) |
| NFR-005 | Preview fidelity | Simulator geometry/units match firmware render within a documented tolerance |
| NFR-006 | Data model | Bindings by stable `chan_key`; forward-compatible with registry growth |
| NFR-007 | Schema | Layout Schema is versioned; firmware rejects unknown major versions cleanly |

---

## Success Metrics

Framework: **HEART** (Happiness, Engagement, Adoption, Retention, Task Success) — appropriate for a creative/authoring tool, not AARRR/revenue.

### North Star
**Time-to-first-deployed-custom-design** — from opening the app to a custom face live on the gauge.
- **Definition:** Wall-clock minutes, first-run, no C, no toolchain.
- **Baseline (today):** effectively ∞ for non-coders; ~several minutes + build/flash for the maintainer.
- **Target:** < 15 minutes for a new user following a starter template; < 2 minutes to deploy an edit for an experienced user.

### Supporting Metrics (Task Success / Engagement)
| Metric | Target |
|--------|--------|
| Design → deploy round-trip time (experienced user) | < 30 seconds from "Deploy" to on-panel |
| Export→import round-trip fidelity | 100% (no visual drift) |
| Simulator vs. hardware visual match | Within documented tolerance; no "surprises" on deploy |
| Firmware round-trips required for a layout change | 0 (the whole point) |
| Crash-free design sessions | Qualitative: stable enough to trust before a track day |

*(Formal analytics/telemetry are out of scope for a builder tool; success is judged by task completion and the maintainer's + early builders' feedback.)*

---

## Scope

### In Scope — Phase 1 (MVP)
- 480×480 round canvas + editing (place/move/resize/layer/undo).
- MVP element palette (FR-002).
- Channel binding from live `/api/channels` or bundled snapshot; range/zones/format.
- On-screen simulator (manual + demo data).
- Live device connect over WiFi AP; pull registry/themes/config.
- Export + validate **Layout Schema v1**; deploy via `POST /api/layout`.
- Save/open project files; single-page designs.
- One starter template (the current default gauge face).

### In Scope — Phase 2 (Post-MVP)
- Multi-page designs + page-switch mapping.
- Template library + shareable-layout import/export.
- Theme-token references (inherit live theme slots).
- Custom font/image import + LVGL asset conversion.
- GlowCraft LED-strip element (reuse the existing strip-status view as a design element).

### Out of Scope
- Firmware image building/flashing (belongs to PlatformIO / web `/ota`).
- ECU/NSP tuning or CAN *transmit*.
- Arbitrary scripting/animation logic (bindings + thresholds only in MVP).
- Cloud accounts, hosted sharing marketplace, telemetry backends.
- Non-round or non-480×480 displays (single known target for now).

### Future Considerations
- A hosted/community layout gallery.
- "Compile-to-C" export path for frozen/performance-critical designs (the hybrid option we deferred).
- Fleet-aware deploy (push one design to multiple gauges via the existing ESP-NOW fleet).
- Support for additional panel sizes/shapes if the hardware line grows.

---

## Technical Considerations

### High-Level Architecture
Two coupled deliverables:

```
┌─────────────────────────────┐         WiFi AP (192.168.4.1)
│   Gauge Designer (desktop)  │  ── GET /api/channels ─────────┐
│   Tauri: web UI + Rust core │  ── GET /api/themes|config ────┤
│                             │  ── POST /api/layout (JSON) ───►│
│  • Canvas editor (web)      │                                 │
│  • Simulator (web/canvas)   │                          ┌──────▼───────────────┐
│  • Schema export/validate   │                          │  Gauge firmware       │
│  • Device I/O (Rust)        │                          │  ESP32-S3 / LVGL 9.3  │
└─────────────────────────────┘                          │                       │
        │ project files (.json)                          │  layout_engine (NEW): │
        ▼                                                 │  parse layout -> LVGL │
   local disk / git                                       │  persist (LittleFS?)  │
                                                          └───────────────────────┘
```

### Technology Stack (Designer app)
- **Shell:** Tauri (Rust core + system webview) — small binary, cross-platform, no bundled Chromium/Node.
- **Frontend:** Web tech (HTML/CSS/JS or a light framework), reusing the existing SPA's visual language. Canvas via HTML5 `<canvas>` or SVG for the editor/simulator.
- **Rust core:** filesystem (project/layout files, native dialogs), HTTP client to the gauge API, WiFi-AP reachability checks, schema validation, and (Phase 2) LVGL font/image asset conversion.
- **No backend server** — the "server" is the gauge itself.

### The Layout Schema (the central interchange contract)
A versioned JSON document describing a gauge design. First-class deliverable — documented and shared between designer and firmware.

Illustrative shape (to be finalised in `Layout_Engine_Spec.md`):
```json
{
  "schema": "ugb-layout",
  "v": 1,
  "canvas": { "w": 480, "h": 480, "round": true },
  "pages": [
    { "id": 0, "name": "Main", "bg": { "type": "theme" },
      "elements": [
        { "type": "arc", "chan": 13826, "min": -15, "max": 30,
          "z1": 0, "z2": 20, "cx": 240, "cy": 240, "r": 232, "w": 16 },
        { "type": "needle", "chan": 13826, "min": -15, "max": 30,
          "cx": 240, "cy": 240, "r0": 185, "r1": 225, "w": 8, "color": "#FF8A00" },
        { "type": "numeric", "chan": 13826, "x": 240, "y": 250,
          "decimals": 1, "font": "dseg14_120", "color": "#FFD700", "unit": true },
        { "type": "text", "x": 240, "y": 360, "str": "BOOST",
          "font": "montserrat_28", "color": "#969696" },
        { "type": "warning", "chan": 63489, "x": 60, "y": 60,
          "onColor": "#FF3B30" }
      ] }
  ]
}
```
- Bindings use the stable **`chan_key`** (e.g. `0x360<<4|2 = 13826` for Manifold Pressure), matching the firmware registry and `/api/channels`.
- Fonts referenced by name from a firmware-provided font set (custom-font import is Phase 2).
- Colours are hex or a `{ "type": "theme", "slot": n, "role": "..." }` reference (Phase 2 token inheritance).

### Firmware Layout Engine (companion workstream — critical dependency)
New firmware module, in the same modular style as v2.x:
- `POST /api/layout` (upload) + `GET /api/layout` (retrieve current); validated, size-guarded (reuse the 4KB-body pattern, or a larger streamed upload like `/ota`).
- Persist the active layout (LittleFS on the existing unused 1.5 MB `spiffs` partition, or a dedicated NVS blob — TBD; note the "single-artifact OTA" philosophy from Stage 3 and weigh a second persisted artifact).
- A `layout_engine` that walks the parsed schema and builds LVGL objects, then a per-frame update path that reads bound channels via `haltech_value()` / `chan_display()` and applies the dirty-region caching discipline the current gauge uses.
- Ownership rules preserved: engine builds/updates run in **loopTask**; uploads staged like other web writes.
- Fallback: if no valid layout is stored, render the built-in default face (never brick the display). Preserve the crash-safe `bootok` guard — a pathological layout must degrade to safe mode, not a boot loop.

### Memory & Performance Budget (from current firmware)
- Target: ESP32-S3 (8 MB flash / 8 MB PSRAM). Current firmware: Flash **49.8%** of a 3.34 MB app slot, internal RAM **~18%** (~59 KB used), LVGL pool **96 KB in PSRAM**, PSRAM ~7.3 MB free.
- Draw buffers are 1/20-screen partial in DMA SRAM (hardware-proven — the engine must not change this).
- Implication: the layout engine has ample PSRAM for parsed-layout structures, but internal DMA SRAM and the 96 KB LVGL pool are the scarce resources. The Designer must **cap element count / complexity** and report the estimated cost before deploy (NFR-004).
- Gradients (radial/conical) are heap-heavy — the Designer should surface that cost, mirroring the firmware's known constraint.

### API Requirements (new + reused)
- **New (firmware):** `POST /api/layout`, `GET /api/layout`, optionally `GET /api/fonts` (available font set).
- **Reused (firmware, v2.1.0):** `GET /api/channels`, `GET /api/themes`, `GET /api/config`, `GET /api/state`.

### Security / Trust
- Local-network, single-user context (the gauge AP). No auth today on the gauge API. A malformed/oversized layout must be rejected safely; the firmware validates and size-guards, and always retains the built-in default as a fallback. No secrets, no PII.

---

## Design & UX Requirements

### UX Principles
- **WYSIWYG against the real target** — the canvas *is* the 480×480 round panel; no guesswork.
- **Data-first** — binding to a channel is a primary, low-friction action, not buried config.
- **Forgiving** — undo/redo everywhere; validate before deploy; never let a user brick the gauge.
- **Reuse the firmware's visual language** — the app should feel like a sibling of the existing web UI.

### Primary User Flow (MVP)
1. Launch app → offline canvas (bundled registry) **or** connect to gauge (live registry + values).
2. Start from a template or blank round canvas.
3. Add elements; bind to channels; set ranges/zones/format; style.
4. Preview in the simulator (manual sliders / demo sweep / live data).
5. Deploy → validate → `POST /api/layout` → confirm on-panel.
6. Save project file; optionally export a shareable layout.

### Alternative / Error Flows
- No device reachable → offline mode with clear indicator; deploy disabled with an explanatory tooltip.
- Layout exceeds budget → block deploy, show what to trim.
- Deploy failure (network/validation) → clear error, design preserved, retry.

### Key Screens (to mock)
- Editor (canvas + palette + inspector + page tabs).
- Channel binding picker (searchable registry with live values).
- Simulator / live preview.
- Connect/device panel (AP status, firmware version, pulled registry).

### Accessibility
- Keyboard-first editing (nudge, select, undo/redo, delete); visible focus; sensible contrast in the app chrome. Full WCAG-AA is not a hard gate for a personal/builder desktop tool but keyboard operability is expected.

---

## Timeline & Milestones

Phased and **dependency-gated on the firmware layout engine** — the Designer can't be truly validated until the engine can render what it exports. Dates TBD (single developer, alongside firmware).

| Phase | Deliverables |
|-------|-------------|
| **M0 — Schema spec** | `Layout_Engine_Spec.md`: Layout Schema v1, `/api/layout`, persistence + memory budget agreed. *(Blocks everything.)* |
| **M1 — Firmware engine spike** | Minimal `layout_engine` renders a hand-written layout.json for 2–3 element types; `POST /api/layout`; default-face fallback + safe-mode preserved. |
| **M2 — Designer skeleton** | Tauri app; round canvas; place/move/resize; inspector; project save/open. |
| **M3 — Binding + simulator** | Channel picker from `/api/channels` (+ bundled snapshot); ranges/zones/format; simulator with manual/demo data. |
| **M4 — Connect + deploy (MVP)** | Live device connect; export/validate schema; deploy via `/api/layout`; full round-trip on hardware. **← MVP** |
| **M5 — Files, templates, multi-page** | Shareable import/export; template library; multi-page + page-switch mapping; theme tokens. |
| **M6 — Assets** | Custom font/image import + LVGL conversion; GlowCraft element. |

### Key checkpoints
- Schema frozen (M0) before parallel app/firmware work.
- First full hardware round-trip (M4) = MVP definition.
- Beta with 1–2 external builders after M5.

---

## Risks & Mitigation

| Risk | Impact | Prob. | Mitigation |
|------|--------|-------|-----------|
| Layout engine is a large, tricky firmware build (parser + generic renderer + memory) | High | High | Spike early (M1) with a minimal element set; grow incrementally; reuse the existing dirty-region + ownership patterns; keep the hybrid "compile-to-C" escape hatch in back pocket. |
| Simulator diverges from hardware render (LVGL quirks, fonts, gradients) | High | Med | Define a tolerance (NFR-005); validate the default template on hardware early; treat the device as source of truth; ship font metrics from firmware. |
| Memory/perf blow-up from rich layouts (esp. gradients, many objects) | High | Med | Designer reports estimated cost + caps complexity (NFR-004); firmware size-guards + rejects; default-face fallback. |
| Persisting a second artifact (layout) breaks the "single-artifact OTA" rollback safety | Med | Med | Decide persistence in M0; if LittleFS, document that a layout survives app rollback and provide "reset to default"; keep built-in face independent of stored layout. |
| Schema churn forces rework on both sides | Med | Med | Freeze v1 at M0; version + reject-unknown-major; additive-only changes (same discipline as CONFIG_SYNC). |
| Scope creep toward "full LVGL IDE" | Med | Med | Non-Goals + strict MVP element set; defer scripting/animation. |
| Bricking a gauge with a bad layout (safety — this goes in a car) | High | Low | Firmware validates, size-guards, and **always** keeps the built-in default + `bootok` safe-mode; never render an unstored/invalid layout. |
| Tauri/webview font + canvas fidelity across OSes | Low | Med | Windows-first; verify canvas/text rendering per-OS before macOS/Linux claims. |

---

## Dependencies & Assumptions

### Dependencies
**Internal (firmware):**
- [ ] **Layout Schema v1** finalised (M0). *(Blocks the whole project.)*
- [ ] Firmware `layout_engine` + `POST /api/layout` + persistence.
- [ ] `/api/channels`, `/api/themes`, `/api/config` — **done** (v2.1.0).
- [ ] Firmware-provided font set (+ metrics for accurate simulation).

**External:**
- [ ] Tauri + toolchain (Rust) on the dev machine.
- [ ] LVGL font/image conversion tooling for Phase 2 asset import.

### Assumptions
- Single hardware target: 480×480 round ESP32-S3 gauge (8 MB PSRAM) on the current firmware line.
- The gauge is reachable over its own WiFi AP during deploy; offline design is fully supported otherwise.
- Bindings are stable by `chan_key`; the registry only grows (additive), so old layouts keep working.
- The maintainer can evolve firmware + tool together; external builders consume releases, not source.

---

## Open Questions

- [ ] **Layout persistence on the device — LittleFS vs. NVS blob vs. bundled-in-OTA?**
  - **Context:** Stage 3 deliberately chose single-artifact OTA (no LittleFS) for rollback safety. A stored layout reintroduces a second artifact. The 1.5 MB `spiffs` partition exists but is unused.
  - **Options:** (a) LittleFS layout file; (b) NVS blob (size-limited, simplest, no partition change); (c) layout embedded via OTA (defeats "no reflash").
  - **Lean:** (b) NVS blob for MVP if layouts stay small; revisit if assets grow.

- [ ] **How far does the generic renderer go vs. a fixed widget vocabulary?**
  - **Context:** A fully generic "any LVGL object + any style" engine is powerful but heavy and risky; a curated widget set (the MVP element types) is tractable and safe.
  - **Lean:** Curated vocabulary for v1; expand deliberately.

- [ ] **Custom fonts/images — convert in-app (Rust) or require pre-converted assets?**
  - **Context:** LVGL needs fonts/images in its own C/binary formats; conversion is non-trivial and eats flash.
  - **Lean:** Phase 2; MVP ships a fixed font set (dseg14, firamono, montserrat) + no custom images.

- [ ] **Does the Designer own theme editing too, or defer to the existing web SPA?**
  - **Context:** The gauge web UI already edits theme slots; duplicating that risks drift.
  - **Lean:** Designer *references* theme slots (tokens), edits stay in the gauge web UI for MVP.

- [ ] **Product naming + whether this lives in-repo or as its own repository.**
  - **Context:** Tight coupling to firmware (shared schema) argues for a monorepo or a submodule; independent release cadence argues for separate.

---

## Appendix

### Related Documents
- `Broadcast_CAN_Protocol_Document_Version_2.pdf` — the channel source of truth.
- `src/haltech_channels.*` — the firmware channel registry (chan_key contract).
- `src/web/api.cpp` — existing JSON API the Designer consumes.
- *(To create)* `Layout_Engine_Spec.md` — Layout Schema v1 + firmware engine.

### Glossary
- **Layout Schema:** The versioned JSON format describing a gauge design; the interchange contract between Designer and firmware.
- **Layout Engine:** New firmware module that interprets a Layout Schema and renders it via LVGL at runtime.
- **`chan_key`:** Stable channel identifier `(can_id << 4) | byte_offset`; how bindings reference Haltech channels.
- **Element:** A single design primitive on the canvas (text, numeric, needle, arc, bar, image, shape, warning).

### Change Log
| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 0.1 | 2026-07-11 | Aaron Gillon | Initial brief — architecture decisions captured (runtime engine, Tauri, builder audience, offline-first + live connect). |
