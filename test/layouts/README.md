# Layout element cost-test kit (v2.8.0)

One layout per new element, plus `11_stress.json` (several combined). Load each,
read the live perf numbers, and decide keep/cull. All bound elements use RPM
(`chan 13824`) / MAP (`chan 13826`), which **Test mode** drives with a synthetic
sweep — so you don't need a live CAN bus. Turn **Test: ON** in the web UI first.

## Load a layout

Connect the PC to the gauge WiFi AP, then POST the file to `/api/layout`:

```bash
# from repo root, per file:
curl -X POST --data-binary @test/layouts/01_scale.json http://192.168.4.1/api/layout
```

PowerShell:

```powershell
Invoke-RestMethod -Uri http://192.168.4.1/api/layout -Method Post `
  -InFile test\layouts\01_scale.json -ContentType application/json
```

A `200` means it validated + persisted + rebuilt. A `400` returns the reason.
Revert to the built-in face any time with the web UI (disable layout) or by
posting a layout with `luse` off.

## Read the cost

Two readouts, both live regardless of the active face:
- **Web UI**: the perf line under the footer — `FPS / LV / R / UI / heap`.
- **On-screen**: turn **Stats: ON**; the overlay now works under layouts too.

Fields: `FPS` frames/sec · `LV` lv_timer_handler ms · `R` flush+DMA render ms ·
`UI` gauge/layout update ms · `heap` free internal (min-ever). The panel refresh
caps ~ the RGB timing; watch for **FPS dropping** and **R climbing** — that's the
element being expensive. Baseline first: built-in face, Test ON.

## What each file probes

| File | Element | Expect |
|------|---------|--------|
| 01_scale | `scale` ticks+labels+redline section | cheap steady-state (static ticks) |
| 02_shiftlights | `shiftlights` segmented RPM row | cheap (only changed segs redraw) |
| 03_icons | `image` telltales (A8 recolour), 1 pulsing | cheap |
| 04_led | `led` on/off indicators | cheap |
| 05_bar_symmetric | `bar` center-origin fill | cheap |
| 06_peak_needle | `needle` live + `source:"peak"` marker | cheap |
| 07_rotated_needle | `needle style:"image"` (tier-3) | heavier: per-frame sprite rotation |
| 08_fullscreen_flash | `alert mode:"fullscreen"` pulsing (tier-3) | **expected worst** (full redraw) |
| 09_ring_flash | `alert mode:"ring"` pulsing (tier-3) | cheap alt to compare vs 08 |
| 10_shadow | blur `shadow` on a shape (tier-3) | heavier: blur is costly |
| 11_stress | scale+shift+2 needles+bar+3 icons+numeric | aggregate headroom check |

## Record (fill in on the bench)

| File | FPS | R ms | UI ms | heap K | notes |
|------|-----|------|-------|--------|-------|
| baseline (built-in) |54 |14 |1 |43 | |
| 01_scale |15 |48 |1 |66 | |
| 02_shiftlights |22 |32|1 |42 |bit of tearing on the display |
| 03_icons |44 |0 |0 |20 | |
| 04_led |935 |0 |0 |0 | |
| 05_bar_symmetric |75 |10 |1 |21 |4 squares after the numbers, missing font maybe |
| 06_peak_needle |22 |20 |1 |66 |bit of tearing |
| 07_rotated_needle |35 |1 |1 |20 | |
| 08_fullscreen_flash |9 |93 |1 |111 |bit of tearing |
| 09_ring_flash |10 |90 |1 |112 | |
| 10_shadow |963 |0 |0 |0 | |
| 11_stress |14 |80 |1 |88 |bit of tearing |

Notes:
- 07 rotated needle may need an angle offset if the sprite points the wrong way
  — it's `deg + 90` in `layout_engine.cpp` (LE_NEEDLE update); tune there.
- 08 fullscreen flash only triggers when RPM > 5000 (the sweep crosses it), so
  watch FPS during the red pulses, not between them.
