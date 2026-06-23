# GlowCraft Strip Configuration Guide

How to add, remove, resize, or reposition the GlowCraft LED strips shown on the
gauge's GlowCraft page. Read this before editing strip definitions.

Related docs: `GlowCraft_CANbus_Integration_Spec.md` (CAN protocol & overall feature spec).

---

## 1. How it's structured

Each strip is defined in **three parallel tables**. They are **index-aligned** —
the Nth entry in every table describes the same strip — so they must always be
edited together and kept in the same order.

| # | File | Table | Defines |
|---|---|---|---|
| 1 | `lib/GlowCraft_Driver/src/GlowCraft_Driver.h` | `enum GlowCraftStripId` | Strip order **and** its CAN ID |
| 2 | `lib/GlowCraft_Driver/src/GlowCraft_Driver.cpp` | `glowcraft_strips[]` | Name, pixel count, installed flag |
| 3 | `src/main.cpp` | `strip_layout[]` | On-screen position & size |

`GC_STRIP_COUNT` (the last enum value) is the strip count and the size of the
other two arrays — it must always stay last.

### CAN IDs are automatic

You never set a CAN ID by hand. `glowcraft_init()` assigns:

```
can_id = GLOWCRAFT_CAN_BASE + strip_index      // GLOWCRAFT_CAN_BASE = 0x500
```

So a strip's ID is just `0x500 +` its position in the enum. This is why **order
matters** and why new strips are **appended at the end** — inserting in the
middle shifts every later strip's CAN ID.

---

## 2. The three tables today

### Table 1 — enum (`GlowCraft_Driver.h`)

```cpp
enum GlowCraftStripId {
  GC_STRIP_FRONT = 0,       // 0x500
  GC_STRIP_FRONT_BADGE,     // 0x501
  GC_STRIP_LEFT_HEADLIGHT,  // 0x502
  GC_STRIP_RIGHT_HEADLIGHT, // 0x503
  GC_STRIP_LEFT,            // 0x504
  GC_STRIP_RIGHT,           // 0x505
  GC_STRIP_REAR,            // 0x506
  GC_STRIP_FRONT_PLATE,     // 0x507  (planned, not installed)
  GC_STRIP_COUNT            // <-- always last
};
```

### Table 2 — registry (`GlowCraft_Driver.cpp`)

```cpp
GlowCraftStrip glowcraft_strips[GC_STRIP_COUNT] = {
  // name              pixels installed
  { "Front",            30,   true  },
  { "Front Badge",       4,   true  },
  { "Left Headlight",    2,   true  },
  { "Right Headlight",   2,   true  },
  { "Left",             50,   true  },
  { "Right",            50,   true  },
  { "Rear",             30,   true  },
  { "Front Plate",       0,   false },  // not installed
};
```

### Table 3 — layout (`main.cpp`)

```cpp
struct StripLayout { int16_t x, y, w, h, radius; };
static const StripLayout strip_layout[GC_STRIP_COUNT] = {
  /* FRONT           */ { 172,  86, 136, 16, 6 },
  /* FRONT_BADGE     */ { 222, 110,  36, 26, 6 },
  /* LEFT_HEADLIGHT  */ { 158, 112,  44, 22, 6 },
  /* RIGHT_HEADLIGHT */ { 278, 112,  44, 22, 6 },
  /* LEFT            */ { 152, 176,  16, 170, 6 },
  /* RIGHT           */ { 312, 176,  16, 170, 6 },
  /* REAR            */ { 172, 398, 136, 16, 6 },
  /* FRONT_PLATE     */ { 205, 146,  70, 14, 4 },
};
```

---

## 3. Field reference

**Registry (`glowcraft_strips[]`)**

| Field | Meaning |
|---|---|
| `name` | Label for the strip (logging/clarity). |
| `pixel_count` | Number of LEDs. **Informational only** — strips render a single representative colour, so this value does not affect drawing. Keep it accurate for documentation. |
| `installed` | `true` = drawn as a filled strip and driven by live CAN / test data. `false` = drawn as an outline placeholder and ignored by updates. |

**Layout (`strip_layout[]`)** — all values in pixels on the 480×480 screen.

| Field | Meaning |
|---|---|
| `x`, `y` | Top-left corner. Origin is top-left of the screen; **front of car = top**. |
| `w`, `h` | Width and height of the bar. |
| `radius` | Corner rounding. |

---

## 4. Common tasks

### Change a pixel count
Edit the number in **Table 2** only. (Remember it's informational — to make the
bar visually bigger/smaller, change `w`/`h` in **Table 3**.)

### Resize or move a strip on screen
Edit that strip's row in **Table 3** (`x, y, w, h, radius`). Keep it inside the
round panel — see §5.

### Turn a planned strip into an installed one
Example: the Front Plate gets fitted with 8 px.
1. **Table 2:** change its row to `{ "Front Plate", 8, true },`.
2. **Table 3:** adjust its coords if the placeholder position isn't right.
3. On the GlowCraft, broadcast that strip's colour on its CAN ID (`0x507`).

No enum change needed — it already exists. The filled fill and the test rainbow
start working automatically once `installed` is `true`.

### Add a brand-new strip
Example: add a **Rear Plate** (12 px). Append in **all three tables**, in the
same position (just before `GC_STRIP_COUNT`):

1. **Table 1 (enum):**
   ```cpp
   GC_STRIP_REAR_PLATE,   // 0x508
   GC_STRIP_COUNT
   ```
2. **Table 2 (registry):**
   ```cpp
   { "Rear Plate", 12, true },
   ```
3. **Table 3 (layout):**
   ```cpp
   /* REAR_PLATE */ { 205, 432, 70, 14, 4 },
   ```
4. **GlowCraft side:** configure it to broadcast the rear plate's colour on
   `0x508`.

> ⚠️ Always append at the **end** (before `GC_STRIP_COUNT`). Inserting in the
> middle renumbers every later strip's CAN ID and silently breaks them.

### Remove / retire a strip
Prefer setting `installed = false` (Table 2) over deleting the rows — that keeps
all CAN IDs stable and just shows it as a placeholder. Only delete the entry from
**all three tables** if you're sure no later strip relies on its ID position.

---

## 5. Layout coordinate reference

- Screen: **480 × 480**, centre `(240, 240)`.
- Car body outline (drawn in `build_glowcraft_page()`): roughly `x 150–330`,
  `y 70–420`.
- Front bar sits near `y ≈ 86`, rear bar near `y ≈ 398`.
- Side flanks are the vertical bars at `x ≈ 152` (left) and `x ≈ 312` (right).
- The panel is **round** — keep every widget within ~230 px of centre or it will
  clip at the edge. Front cluster (badge + headlights) lives around `y 110`.

```
                 [===== FRONT (y~86) =====]
              [HL-L]  [BADGE]  [HL-R]   (y~112)
                    [ FRONT PLATE ]      (placeholder, y~146)
   |L|                                        |R|
   |E|                                        |I|
   |F|            (car body outline)          |G|   side flanks
   |T|                                        |H|   x~152 / x~312
                 [===== REAR (y~398) =====]
```

---

## 6. After editing

1. Build: `pio run -e esp32-s3-devkitc1-n8r8`.
2. Flash `firmware.bin` (USB or the web OTA page at `/ota`).
3. Verify on the GlowCraft page with **Test Mode ON** — every `installed` strip
   should cycle the rainbow; placeholders stay as outlines.
4. With real CAN, a strip goes dim if no frame arrives within
   `GLOWCRAFT_OFFLINE_TIMEOUT_MS` (2000 ms).
