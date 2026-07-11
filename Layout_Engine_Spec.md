# Layout Engine Spec — Ultimate Gauge Board

**Status:** Draft v0.1 — the M0 critical-path artifact for the Gauge Designer project
**Author:** Aaron Gillon
**Date:** 2026-07-11
**Companion to:** `Gauge_Designer_PRD.md` (desktop tool), `src/haltech_channels.*` (channel registry), `src/web/api.cpp` (JSON API)

> **Purpose.** Define (1) the **Layout Schema v1** — the versioned JSON format that describes a gauge design — and (2) the **firmware layout engine** that interprets it and renders it via LVGL at runtime. This is the interchange contract the Gauge Designer app and the firmware both agree on. Freezing schema v1 here unblocks parallel work on the app and the firmware.

---

## 1. Overview & Goals

The firmware today renders **one hand-coded gauge face** (`src/ui/gauge_page.cpp`). This spec adds a **layout engine**: a firmware module that reads a Layout Schema document (uploaded over WiFi) and builds/updates the LVGL scene from it — so a new gauge design is a data upload, never a recompile/reflash.

**Goals**
1. Render arbitrary gauge faces from a data description, reusing the proven rendering + concurrency patterns of the current firmware.
2. Add **zero LVGL widget bloat** — the MVP element set maps entirely onto the three primitives already enabled (`lv_obj`, `lv_label`, `lv_line`).
3. Never brick the gauge: validate on upload, always retain the compiled-in default face, and integrate with the existing `bootok` crash-safe guard.
4. Bind elements to the existing Haltech channel registry by stable `chan_key`, honouring the display-unit prefs (v2.1.0).
5. Keep the format human-readable, versioned, and forward-compatible (additive-only within a major version).

**Non-goals (v1):** custom fonts/images (Phase 2), arbitrary scripting/animation, non-round or non-480×480 targets, editing themes (the layout *references* theme slots; theme editing stays in the web UI).

---

## 2. Relationship to Existing Firmware

| Concern | Today | With the layout engine |
|---|---|---|
| Gauge face (page 0) | Built-in `load_current_style()` / `update_gauge_master()` | If a valid layout is stored → engine renders it. Else → built-in face (unchanged). |
| GlowCraft page (page 1) | `build_glowcraft_page()` | Unchanged. |
| Channels | `haltech_value()` / `chan_display()` (registry) | Reused verbatim for bindings. |
| Themes (4 slots) | Web UI + `apply_theme_colors()` | Layout elements may reference theme slots/roles; recolour on `flag_theme_update`. |
| Behavior config (4 modes) | Drives the built-in face | Continues to drive the **built-in** face. Custom layouts bind channels explicitly; legacy modes are independent in v1 (see §11). |
| Persistence / NVS | `config_store` (sole NVS owner) | New: layout file on LittleFS (see §7); `config_store` gains a "layout present" flag. |
| Web API | `/api/*` (v2.1.0) | New: `/api/layout` (GET/POST/DELETE), `/api/fonts`. |
| Concurrency/ownership | loopTask owns LVGL + globals; web/CAN stage work | **Unchanged** — engine build/update run in loopTask; uploads staged like other web writes. |

The engine slots into the existing module structure as `src/ui/layout_engine.{h,cpp}` (+ `src/layout_store.{h,cpp}` for persistence), following the v2.x conventions.

---

## 3. Layout Schema v1

A layout is a single JSON document. Wire format (upload/download) and on-disk format are **the same JSON** — parsed on-device with ArduinoJson 7 (already a dependency) into a compact in-memory model held in PSRAM.

### 3.1 Top level

```json
{
  "schema": "ugb-layout",
  "v": 1,
  "meta": { "name": "Evo Boost", "author": "Aaron", "created": "2026-07-11" },
  "canvas": { "w": 480, "h": 480, "round": true },
  "pages": [ /* 1..N page objects */ ],
  "start_page": 0
}
```

| Field | Type | Req | Notes |
|---|---|---|---|
| `schema` | string | ✓ | Must equal `"ugb-layout"`. |
| `v` | int | ✓ | Major schema version. Firmware rejects unknown majors. |
| `meta` | object | – | Free-form, ignored by firmware except round-trip storage. |
| `canvas` | object | ✓ | Must be `{w:480,h:480,round:true}` for this hardware; firmware validates. |
| `pages` | array | ✓ | 1..`MAX_PAGES` (see §10). |
| `start_page` | int | – | Page index shown first (default 0). |

### 3.2 Page

```json
{
  "id": 0,
  "name": "Main",
  "bg": { "type": "solid", "color": "#000000" },
  "elements": [ /* 0..MAX_ELEMENTS element objects */ ]
}
```

| Field | Type | Req | Notes |
|---|---|---|---|
| `id` | int | ✓ | Unique within the layout. |
| `name` | string | – | For the designer/UX. |
| `bg` | object | – | Background fill (see §3.5). Default: solid black. |
| `elements` | array | ✓ | Drawn in array order (index 0 = lowest z). |

### 3.3 Element — common fields

Every element object has:

| Field | Type | Req | Notes |
|---|---|---|---|
| `type` | string | ✓ | One of the element types in §3.4. Unknown type → upload rejected. |
| `id` | string/int | – | Optional stable id (designer bookkeeping). |
| `visible_if` | object | – | Optional visibility binding: `{ "chan": <key>, "op": ">", "value": 0 }`. Hides the element unless the condition holds. Ops: `>`, `<`, `>=`, `<=`, `==`, `!=`, `truthy`. |
| `stale_ms` | int | – | If the bound channel is older than this, the element renders "blank"/dimmed (default: no staleness handling). |

Colours are either a hex string (`"#RRGGBB"`) or a **theme token** (§3.6). Coordinates are integers in the 480×480 device space (§4).

### 3.4 Element types (MVP — reuse `obj`/`label`/`line` only)

Each maps to primitives already enabled in `lv_conf.h`. **No new LVGL widgets required.**

#### `text` → `lv_label`
Static text.
```json
{ "type":"text", "x":240, "y":360, "align":"center",
  "str":"BOOST", "font":"montserrat_28", "color":"#969696" }
```
`align`: `left|center|right` (anchor at `x,y`). `font`: §5. 

#### `numeric` → `lv_label`
A channel value, formatted.
```json
{ "type":"numeric", "chan":13826, "x":240, "y":250, "align":"center",
  "font":"dseg14_120", "color":"#FFD700",
  "decimals":1, "unit":true, "prefix":"" }
```
Renders `prefix + display_value + (unit ? " "+unit_str : "")`. `display_value = chan_display(idx, haltech_value(idx))` to `decimals` places. `unit_str = chan_unit_str(idx)` (honours device unit prefs). Uses the same dirty-caching as the current value labels (only `set_text` on change).

#### `needle` → `lv_line`
Rotating pointer (the current gauge needle, generalised).
```json
{ "type":"needle", "chan":13826, "min":-15, "max":30,
  "cx":240, "cy":240, "r0":185, "r1":225,
  "start_deg":135, "sweep_deg":270, "width":8, "color":"#FF8A00" }
```
`min`/`max` in **display units**. Geometry + angle convention per §4. Only re-set points when the pixel endpoints change (existing needle caching).

#### `ring` → `lv_obj` (circular border, colour-zoned)
The current colour-changing outer ring, generalised.
```json
{ "type":"ring", "chan":13826, "min":-15, "max":30, "z1":0, "z2":20,
  "cx":240, "cy":240, "r":232, "width":16,
  "low":"#2196F3", "mid":"#4CAF50", "high":"#F44336" }
```
Colour = `low` if `display<z1`, `mid` if `display<z2`, else `high` (zones in display units; set z1/z2 outside [min,max] for a single-colour ring). Implemented as a `width`-thick full-circle border on an `lv_obj` of size `2r`, radius `r`. Only recolour on zone change.

#### `bar` → `lv_obj` track + `lv_obj` fill (manual sizing)
Linear gauge without needing `LV_USE_BAR`.
```json
{ "type":"bar", "chan":52258, "min":0, "max":100, "z1":-9999, "z2":9999,
  "x":140, "y":420, "w":200, "h":18, "dir":"h", "radius":4,
  "track":"#202020", "low":"#2196F3", "mid":"#4CAF50", "high":"#F44336" }
```
`dir`: `h` (fill grows left→right) or `v` (bottom→top). Fill length = `normalized * (w|h)`, `normalized = clamp((display-min)/(max-min),0,1)`. Fill colour via z1/z2 like `ring`. Only resize/recolour on change.

#### `shape` → `lv_obj` (rect/circle) or `lv_line`
Static decoration.
```json
{ "type":"shape", "shape":"rect", "x":100,"y":100,"w":80,"h":40,"radius":6,
  "fill":"#14161A", "stroke":"#2A2E36", "stroke_w":1,
  "grad": { "type":"linear-v", "c2":"#000000" } }
```
`shape`: `rect|circle|line`. `line` uses `points:[[x0,y0],[x1,y1],...]`, `stroke`, `stroke_w`. `grad` optional (§3.5 gradient sub-object) — **note gradients are heap-heavy** (see §10).

#### `warning` → `lv_label`
Boolean/indicator light bound to a channel (e.g. Check Engine Light).
```json
{ "type":"warning", "chan":63489, "x":60, "y":60, "align":"center",
  "font":"montserrat_28", "on_str":"CEL", "on_color":"#FF3B30",
  "off_hidden":true }
```
Shows `on_str`/`on_color` when `haltech_value != 0`; hidden (or `off_str`/`off_color`) otherwise. For boolean channels this is exact; for numeric channels pair with `visible_if`.

#### Deferred to Phase 2
- `image` → `lv_image` (requires `LV_USE_IMAGE=1` + LittleFS asset loading — both off today).
- `glowcraft` → reuse the strip-status view as an element.
- Custom fonts.

### 3.5 Backgrounds & gradients

```json
"bg": { "type":"solid", "color":"#001830" }
"bg": { "type":"theme" }                       // inherit active theme's background
"bg": { "type":"gradient", "grad": {
          "kind":"linear-v|linear-h|linear-angle|radial|conical",
          "c1":"#001830", "c2":"#000000", "c3":"#000000",
          "stops":2, "angle":0 } }
```
Gradients reuse `apply_background()` semantics (2/3 stops, the five kinds already supported). The conical/radial kinds are the heavy ones the firmware already flags.

### 3.6 Theme tokens (colour references)

Any colour field may be a token instead of a hex string, so a design inherits live theme-slot colours:
```json
"color": { "type":"theme", "role":"needle" }
"color": { "type":"theme", "slot":2, "role":"high" }
```
`role`: one of `text|low|mid|high|bg|modeLabel|linkIcon|needle|peak` (the `GaugeTheme` fields). `slot` optional (default = active slot). On `flag_theme_update`, the engine recolours token-bound elements. *(Token support may be P1 if it complicates the MVP; hex-only is acceptable for v1.0 of the engine.)*

### 3.7 Worked example (reproduces today's default face)

```json
{
  "schema":"ugb-layout", "v":1,
  "meta":{"name":"Classic Boost"},
  "canvas":{"w":480,"h":480,"round":true},
  "pages":[{
    "id":0, "name":"Main", "bg":{"type":"theme"},
    "elements":[
      {"type":"ring","chan":13826,"min":-15,"max":30,"z1":0,"z2":20,
       "cx":240,"cy":240,"r":232,"width":16,
       "low":{"type":"theme","role":"low"},"mid":{"type":"theme","role":"mid"},"high":{"type":"theme","role":"high"}},
      {"type":"needle","chan":13826,"min":-15,"max":30,"cx":240,"cy":240,
       "r0":185,"r1":225,"start_deg":135,"sweep_deg":270,"width":8,
       "color":{"type":"theme","role":"needle"}},
      {"type":"numeric","chan":13826,"x":284,"y":245,"align":"right",
       "font":"dseg14_120","decimals":0,"color":{"type":"theme","role":"text"}},
      {"type":"numeric","chan":13826,"x":284,"y":245,"align":"left",
       "font":"dseg14_96","decimals":1,"color":{"type":"theme","role":"text"}},
      {"type":"text","x":240,"y":420,"align":"center","str":"BOOST",
       "font":"montserrat_28","color":{"type":"theme","role":"modeLabel"}}
    ]
  }]
}
```
*(chan_key `13826` = `0x360<<4 | 2` = Manifold Pressure.)*

---

## 4. Coordinate System & Rendering Conventions (fidelity contract)

The simulator in the Designer must match these exactly or previews will lie.

- **Origin** top-left `(0,0)`; **x** right, **y** down; device is **480×480**; centre `(240,240)`.
- **Round mask:** visible area is the inscribed circle, radius 240. Content outside is clipped by the panel; the designer flags off-circle content.
- **Text anchoring:** `align` positions the label's left/centre/right edge at `x`; baseline/box behaviour follows LVGL label metrics (§5 ships metrics).
- **Needle / angular math** (identical to `update_ui`):
  ```
  norm      = clamp((display - min) / (max - min), 0, 1)
  angle_deg = start_deg + norm * sweep_deg
  rad       = angle_deg * PI / 180
  p0 = (cx + r0*cos(rad), cy + r0*sin(rad))
  p1 = (cx + r1*cos(rad), cy + r1*sin(rad))
  ```
  Angle 0° points +x (east), positive is clockwise (because y is down). Default `start_deg=135`, `sweep_deg=270` reproduces the current SSW→N→SSE sweep.
- **Zones/ranges** are always in **display units** (post `chan_display`), matching v2.1.0 behavior config.

---

## 5. Fonts & Assets

**v1 font set** (compiled into firmware, referenced by name):

| Name | Source | Use |
|---|---|---|
| `dseg14_120`, `dseg14_96` | custom (7-seg) | large numeric readouts |
| `firamono_120`, `firamono_96` | custom (mono) | alt numeric |
| `montserrat_28`, `montserrat_20`, `montserrat_14` | LVGL built-in | labels, symbols |

- `GET /api/fonts` returns the available names **plus per-font metrics** (line height, and for the numeric fonts the fixed digit advance) so the Designer can lay out text without the device.
- Referencing an unknown font → upload rejected.
- **Custom fonts / images are Phase 2**: they require `LV_USE_IMAGE=1`, LittleFS asset storage, and on-host LVGL font/image conversion (Tauri/Rust). Out of scope for engine v1.

---

## 6. Firmware Architecture

### 6.1 Modules
```
src/ui/layout_engine.h/.cpp   parse JSON -> in-memory model; build LVGL scene; per-frame update
src/layout_store.h/.cpp       persist/load/delete the layout file (LittleFS); "layout present" flag
src/web/api.cpp               + /api/layout (GET/POST/DELETE), /api/fonts
```

### 6.2 In-memory model
Parsed once on load into a PSRAM-allocated array of runtime elements:
```c
typedef struct {
  uint8_t  type;
  uint16_t chan_key;          // 0 = unbound
  int8_t   chan_idx;          // resolved once at build (-1 = unbound/unknown)
  lv_obj_t* obj;              // primary LVGL handle
  lv_obj_t* obj2;             // e.g. bar fill
  // geometry + style + binding params (min/max/z1/z2/decimals/colors/...)
  // dirty-cache: last rendered value/text/color
} LayoutElement;
```
The raw JSON is kept on disk (for `GET /api/layout` round-trip); only the compact model lives in RAM.

### 6.3 Build / update split (mirrors the current gauge)
- **Build** (on layout load or page switch, loopTask): create LVGL objects for the active page, resolve each binding's `chan_idx` once, seed dirty caches.
- **Update** (per frame, loopTask, from `loop()`): for each bound element, read `haltech_value(chan_idx)` → `chan_display` → apply, **only when the cached value/text/colour changed** (same discipline as `update_ui`/`update_gauge_master`).
- **Page switch** is internal to the engine (its own active-page index), triggered via API or a bound input; tears down the old page's objects and builds the new (or pre-builds all pages if within budget — TBD by measurement).

### 6.4 Concurrency / ownership (unchanged rules)
- All LVGL build/update runs in **loopTask**.
- A layout upload arrives on the web handler (loop context) but is **staged**: written to disk + a `flag_layout_reload` set; `loop()` rebuilds the scene at a safe point (never mid-frame). Matches how themes/config are staged.
- No new task; no LVGL calls off loopTask.

---

## 7. Persistence

**Decision: store the layout JSON as a file on LittleFS**, mounted on the existing unused `spiffs` partition (1.5 MB, already in `default_8MB.csv` — **no repartition**, so no serial flash needed).

**Rationale**
- Layouts want to grow (multi-page, more elements) and Phase 2 adds font/image assets — only a filesystem accommodates that. The 20 KB NVS partition is small and already shared by all config/theme keys.
- Keeps the layout an inspectable file; simple `GET/DELETE`.
- LittleFS is flash-backed — negligible internal-RAM cost (small mount buffers), so it doesn't reopen the v1.1.x memory wounds.

**Cost to accept:** a stored layout is a **second artifact** — a bad layout survives an app OTA rollback. Mitigations:
1. The compiled-in **default face is always retained** and is used whenever no valid layout is stored.
2. Upload is **validated before it is persisted** (§9); an invalid layout never reaches disk.
3. `DELETE /api/layout` (and a web "Reset to default face" button) removes the file → instant revert.
4. The `bootok` guard is extended: if a boot doesn't complete with a stored layout active, safe-mode **disables the stored layout** (renders the default face) on the next boot, exactly as it disables gradients today.

**Alternative considered:** NVS blob (single-artifact, honours the Stage-3 philosophy) — rejected as the primary because of the 20 KB partition ceiling and no room for Phase-2 assets. Could be a fallback if LittleFS proves troublesome, capping layouts to a few KB.

**New firmware setup:** `LittleFS.begin(true)` at boot (format-on-fail); `layout_store` owns all FS access (mirrors `config_store` owning NVS).

---

## 8. Web API

All handlers run in loop context (like the rest of `/api/*`).

| Method | Endpoint | Body | Behaviour |
|---|---|---|---|
| `GET` | `/api/layout` | – | Return the stored layout JSON (404 if none → default face active). |
| `POST` | `/api/layout` | layout JSON | Validate → persist → stage reload. 200 on success; 400 + reason on validation failure (current layout untouched). |
| `DELETE` | `/api/layout` | – | Remove stored layout → revert to default face. |
| `GET` | `/api/fonts` | – | Available font names + metrics (§5). |
| `GET` | `/api/state` | – | *(extend)* add `layoutActive: bool` + `layoutName` so the SPA/Designer show what's running. |

**Upload size:** small layouts (a few KB) fit the existing `arg("plain")` + body-guard pattern; if layouts exceed ~8 KB, use a streamed multipart upload like `/ota` (write straight to a temp file, validate, then swap). Designer reports estimated size before sending.

---

## 9. Safety, Validation & Fallback

**Validate on upload (reject the whole document on any failure, keep current):**
- `schema=="ugb-layout"` and `v` is a supported major.
- `canvas` is 480×480 round.
- `pages` within `1..MAX_PAGES`; each page's `elements` within `0..MAX_ELEMENTS`; total elements within `MAX_TOTAL_ELEMENTS`.
- Every `type` is known; every required field per type present and in range; coordinates within a sane bound (e.g. −512..1023).
- Every `chan` resolves via `chan_index_from_key` (unknown channel → reject with the offending key, so the Designer can target the device's registry).
- Every `font` is in the device set.
- Serialized size within the storage cap.

**Fallback ladder (never a blank/bricked screen):**
1. No stored layout → built-in default face.
2. Stored layout fails to parse/validate at boot → log, delete-or-ignore, built-in default face.
3. Crash during build → `bootok` safe-mode on next boot disables the stored layout → default face.
4. The built-in default face is compiled in and never removable.

**Web/OTA stay up regardless** (recovery path unchanged from v1.1.1: web + OTA come up before risky rendering).

---

## 10. Memory & Performance Budget

Baseline (firmware v2.1.0): Flash **49.8%** of the 3.34 MB app slot; internal RAM **~18%** (~59 KB); **LVGL pool 96 KB in PSRAM**; PSRAM ~7.3 MB free; draw buffers 1/20-screen partial in DMA SRAM (**must not change**).

- **Parsed model + raw JSON:** PSRAM — ample.
- **LVGL objects + glyph caches:** the scarce resource is the **96 KB LVGL pool** (glyph caches for the big DSEG14 fonts dominate; gradients are heap-heavy). This, not flash/PSRAM, sets the practical ceiling.
- **Caps (initial, tune by measurement):** `MAX_PAGES = 4`, `MAX_ELEMENTS = 32`/page, `MAX_TOTAL_ELEMENTS = 64`. Enforced at validation.
- **Per-element rough cost:** ~200–400 B of LVGL object overhead each; large-font text and gradients cost far more (glyph cache / gradient map). The Designer should **estimate and display the budget** before deploy (PRD NFR-004), warning on gradients and many large-font elements.
- **Frame cost:** the per-frame update is O(bound elements) with dirty-caching — same class as today's gauge; target ≥ the current ~30 fps. Only-build-active-page keeps object count low.

---

## 11. Interaction With Existing Systems

- **Themes:** layout colour tokens (§3.6) resolve from the live theme slots; recolour on `flag_theme_update`. Theme editing stays in the web UI.
- **Legacy modes / behavior config:** in v1 these drive the **built-in** face only. A custom layout binds channels explicitly and switches pages independently. *(Future: a `"chan":"active-mode"` pseudo-binding, or map page index ↔ mode, so a custom layout can reuse the mode buttons/rotary.)*
- **Pages / GlowCraft:** a valid stored layout **overrides page 0** (the gauge page). GlowCraft (page 1) is unchanged. Multi-page is handled inside the engine. `current_page` semantics preserved.
- **Fleet (ESP-NOW):** out of scope for engine v1. *(Future: a `LAYOUT_PUSH` v2 fleet packet or an "apply my layout to all" that ships the file — larger than a CAN-sized packet, so likely a chunked transfer.)*
- **Trimpot theme-sync:** unaffected (operates on theme slots, orthogonal to layout).

---

## 12. Versioning & Forward-Compatibility

- `schema`/`v` gate every document. Firmware **rejects unknown majors** with a clear error; the Designer targets the connected device's supported version (surfaced via `/api/state` / `/api/fonts`).
- Within a major: **additive only.** Unknown *object fields* are ignored (forward-compat); unknown *element types* are **rejected on upload** (fail fast so the Designer knows the target can't render them) — the same fail-fast-on-upload / ignore-unknown-fields discipline used for CONFIG_SYNC.
- The Designer should fetch device capabilities before export so it only emits what the target supports.

---

## 13. Open Questions

- [ ] **Theme tokens in v1.0 or defer to v1.1?** Hex-only is simpler and unblocks the engine; tokens add live-theme inheritance. *Lean: ship hex-only first, add tokens once the pipeline is proven.*
- [ ] **Pre-build all pages vs. build-on-switch?** Depends on measured pool headroom. *Lean: build-on-switch for MVP; pre-build if budget allows.*
- [ ] **Upload transport for larger layouts** — extend `arg("plain")` + guard, or a streamed multipart like `/ota`? *Lean: JSON body for ≤8 KB; revisit if assets/multi-page push past it.*
- [ ] **LittleFS format-on-first-boot** — acceptable to auto-format the spiffs partition on existing devices (it's currently unused, so nothing to lose)? *Lean: yes.*
- [ ] **Does a custom layout replace or coexist with the legacy mode buttons?** (See §11.) *Lean: coexist; modes drive the built-in face, layout is independent, revisit a bridge later.*

---

## 14. Implementation Phases (maps to PRD milestones)

- **M0 (this doc):** freeze Schema v1, the API, persistence + budget. *Unblocks parallel app/firmware work.*
- **M1 — engine spike:** `layout_engine` renders a hand-written `layout.json` for `text` + `numeric` + `needle` (the minimum to reproduce a readable face); `POST /api/layout`; LittleFS store; default-face fallback + `bootok` integration. Reproduce the §3.7 example on hardware.
- **M1.5 — full MVP element set:** add `ring`, `bar`, `shape`, `warning`, backgrounds/gradients, `visible_if`; `GET /api/fonts`; validation complete.
- **M2+ (Designer app):** consumes this contract (PRD M2–M4).
- **Phase 2:** theme tokens (if deferred), `image`/custom fonts (needs `LV_USE_IMAGE=1` + asset pipeline), `glowcraft` element, fleet layout push.

---

## Appendix — chan_key quick reference (from `src/haltech_channels.*`)
`chan_key = (can_id << 4) | byte_offset`. Examples:

| Channel | CAN ID / off | chan_key (dec) |
|---|---|---|
| Manifold Pressure (BOOST) | 0x360 / 2 | 13826 |
| Wideband 1 (AFR) | 0x368 / 0 | 13952 |
| Coolant Temperature (WATER) | 0x3E0 / 0 | 15872 |
| Oil Pressure | 0x361 / 2 | 13842 |
| Battery Voltage | 0x372 / 0 | 14112 |
| Check Engine Light (bit) | 0x3E4 / 7 (bit) | — (bit channels not key-bindable) |

*(The Designer pulls the authoritative list live from `GET /api/channels`.)*
