A work in progress for digital gauges, using the hardware from Garage Tinkering.

Gauges configurable through a web interface, and synced to multiple gauges using ESPNow
Set up to connect to a Haltech ECU via Can Bus, has a test mode to give sample data

## Theming

The gauge stores **4 theme slots**, selectable live via **Rotary Trim 3** (Haltech `0x3E4`
byte 6, positions 0–3) when *Trimpot Theme Sync* is on, or from the web UI. Each slot holds a
full colour palette plus its own background fill.

### Background gradients

Each slot's background can be a flat colour or a gradient. Configure it from the web UI's
**Background Gradient** card:

- **Type** — Solid, Linear ↕ (vertical), Linear ↔ (horizontal), Linear ∠ (angled), Radial, Conical
- **Stops** — 2 or 3 colour stops (Stop 1 is the slot background colour)
- **Stop 2 / Stop 3** — gradient colours
- **Angle** — 0–360°, used by Linear ∠ and Conical

Edits apply live (no reload) to the active slot and broadcast to every paired gauge over
ESP‑NOW, so a fleet stays in sync. Each slot keeps its own gradient, so switching slots with
Rotary Trim 3 switches the background too.

Rendered with LVGL's software gradient engine. Radial and conical fills are re-blended per
redraw, so on this 480×480 panel watch the on-screen Stats overlay for frame-rate impact if you
use them with constantly-animating content.

#### Implementation notes

- LVGL config: `LV_USE_DRAW_SW_COMPLEX_GRADIENTS 1` and `LV_GRADIENT_MAX_STOPS 3`
  (`include/lv_conf.h`).
- Render path: `apply_background()` in `src/main.cpp` builds an `lv_grad_dsc_t`
  (`lv_grad_init_stops` + `lv_grad_linear/radial/conical_init`) and is called from
  `load_current_style()`.
- Web endpoint: `GET /grad?gt=&gs=&cbg=&b2=&b3=&ga=` → `handleGrad()`.
- Fleet sync: ESP‑NOW **packet type 8** — `c1`/`c2` = stops 2/3, `c3` = background (stop 1),
  `value` packs `type | (stops << 4) | (angle << 8)`.
- Persistence (NVS, per slot): keys `b2 b3 gt gs ga`; legacy live keys `cbg2 cbg3 cgt cgs cga`
  migrate an existing single theme into slot 0.
