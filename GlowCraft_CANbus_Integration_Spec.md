# GlowCraft LED Controller — CAN Bus Integration Spec
### For: Ultimate Gauge Board — LED Status Display Page

**Vehicle:** Mitsubishi Evo 9 (CT9A)
**ECU:** Haltech S3
**LED Controller:** GlowCraft GC01-P (`glowcraft-8c4f00a00ff0.local`)
**CAN Bus Speed:** 1 Mbit
**CAN Encoding:** Big-endian (Motorola) throughout — confirmed per Haltech Broadcast CAN Protocol v2.0
**Date:** June 2026

---

## 1. Physical CAN Bus Topology

```
┌─────────────┐     Twisted pair (shielded)      ┌──────────────────┐
│  Haltech S3 ├──── CANH / CANL / Shield ────────┤  GlowCraft GC01-P│
│    ECU      │                                   │                  │
└─────────────┘                                   │  Pin 22 = CANH   │
       │                                          │  Pin 23 = CANL   │
       │                                          │  Pin 24 = SigGnd │
       │                                          └──────────────────┘
       │
       │     (same bus)
       │
┌──────┴──────┐
│  IC-7 Dash  │  (existing node — passive listener)
└─────────────┘
```

- Single CAN bus segment shared by Haltech S3, IC-7 dash, and GlowCraft
- GlowCraft has software-selectable 120Ω split termination (Settings → CAN Terminated)
- Confirm termination resistor count on the bus — should be exactly two 120Ω terminators total (one at each physical end of the bus)
- GlowCraft `SigGnd` (Pin 24) wired to the same star ground point as the Haltech CAN shield per installation

---

## 2. CAN Message Architecture — Two Separate Talkers

There are two distinct sources of CAN frames on this bus relevant to the GlowCraft:

| Source | Direction | Purpose |
|---|---|---|
| **Haltech S3** | TX → GlowCraft RX | Vehicle data (RPM, MAP, G-force, switches etc.) |
| **GlowCraft** | TX → Gauge RX | LED status broadcast (show active, state) — **frame structure TBD** |

The Haltech messages are received by GlowCraft and used to drive LED behaviour via Signals/Triggers/Bindings. The GlowCraft's own status broadcast frames are what the gauge page will consume to display LED panel state.

> **Note:** GlowCraft custom CAN broadcast frame IDs and payload structure are yet to be defined. Section 5 below documents what the GlowCraft *can* broadcast (per firmware docs) and the placeholder structure for the gauge to consume once frame layout is finalised.

---

## 3. Haltech → GlowCraft: Signal Definitions

These are the CAN signals currently configured (or planned) in GlowCraft's **CANbus → Signals** tab. All are sourced from Haltech S3 broadcast frames, big-endian, 1 Mbit.

### 3.1 Signals in use

| Signal Name | CAN ID | Byte Position | Haltech Notation | Length | Signed | Scale | Offset | Decoded Units | Rate |
|---|---|---|---|---|---|---|---|---|---|
| Engine RPM | `0x360` | 0–1 | `0-1` | 16-bit | No | 1 | 0 | RPM | 50 Hz |
| Manifold Pressure | `0x360` | 2–3 | `2-3` | 16-bit | No | 0.1 | 0 | kPa (abs) | 50 Hz |
| BOV / Transient Throttle Active | `0x3E4` | byte 1, bit 3 | `1:3` | 1-bit | No | 1 | 0 | boolean (0/1) | 5 Hz |
| Rotary Trim 3 | `0x3E4` | byte 6 | `6` | 8-bit | Yes | 1 | 0 | raw (0–3 in use) | 5 Hz |
| Brake Pedal Switch | `0x3E4` | byte 1, bit 2 | `1:2` | 1-bit | No | 1 | 0 | boolean (0/1) | 5 Hz |
| Hand Brake State | `0x3E4` | byte 7, bit 1 | `7:1` | 1-bit | No | 1 | 0 | boolean (0/1) | 5 Hz |
| Lateral G | `0x36B` | 6–7 | `6-7` | 16-bit | Yes | 0.1 | 0 | m/s² | 20 Hz |
| Longitudinal G | `0x36E` | 6–7 | `6-7` | 16-bit | Yes | 0.1 | 0 | m/s² | 20 Hz |

### 3.2 GlowCraft internal signal processing

Once a signal is decoded:

- **Triggers** (tab: CANbus → Triggers) fire discrete events `CAN_1`…`CAN_30` when a signal value falls in a configured range. These gate show activation/deactivation.
- **Bindings** (tab: show editor → Bindings) pipe a live signal value directly into an animator parameter in real time (no event, just a continuous mapping).

Triggers and Bindings are parallel, independent consumers of the same signal value.

### 3.3 CAN Trigger assignments (current)

| Trigger Name | Signal | Active Range | Event Fired | Purpose |
|---|---|---|---|---|
| BOV Flash gate | BOV / Transient Throttle Active | `[1, 1]` | `CAN_2` | Part of BOV Flash show ALL-logic gate |
| *(second BOV condition)* | *(TBD — see show notes)* | — | `CAN_3` | Part of BOV Flash show ALL-logic gate |
| Brake Pressed | Brake Pedal Switch | `[1, 1]` | `CAN_3` | Brake show trigger |
| Handbrake On | Hand Brake State | `[1, 1]` | `CAN_4` | Handbrake show trigger / showmode gate |

> **⚠️ Naming conflict to resolve:** `CAN_3` is listed above for both the second BOV Flash gate condition and the Brake Pressed trigger. Reconcile before implementation — one of these needs to move to a different event slot.

---

## 4. LED Panel Layout

Four panels configured on the GlowCraft:

```
                    ┌──────────────────────────────┐
                    │         FRONT (30×1)          │
                    └──────────────────────────────┘

┌────────┐                                          ┌────────┐
│  LEFT  │                   CAR                   │ RIGHT  │
│  1×50  │                  (plan                  │  1×50  │
│        │                   view)                 │        │
└────────┘                                          └────────┘

                    ┌──────────────────────────────┐
                    │         REAR  (30×1)          │
                    └──────────────────────────────┘
```

| Panel | Dimensions | Pixel Count | Orientation |
|---|---|---|---|
| Front | 30×1 | 30 | Horizontal strip |
| Left | 1×50 | 50 | Vertical strip |
| Rear | 30×1 | 30 | Horizontal strip |
| Right | 1×50 | 50 | Vertical strip |

**Total: 160 pixels**

---

## 5. Show Definitions

### 5.1 BOV Flash

| Property | Value |
|---|---|
| Name | BOV Flash |
| Panels | ALL (Front, Left, Rear, Right) |
| Animator | Color Flash |
| Color | White |
| Frequency | 10 Hz |
| Trigger logic | ALL — both `CAN_2` AND `CAN_3` must be active |

Activated when the BOV fires / transient throttle condition is met. Both CAN event slots must be simultaneously active (AND logic) before the show qualifies to play.

### 5.2 G-Force Reactive (planned)

| Property | Value |
|---|---|
| Name | G-Force Reactive |
| Panels | Left + Right (Lateral G), Front + Rear (Longitudinal G) |
| Trigger | None — runs as default/low-weight show, overridden by event-triggered shows |
| Bindings | Lateral G → Left/Right animator speed or position (Number mapping, ±3 m/s²) |
| Bindings | Longitudinal G → Front/Rear brightness or fade (Number mapping, ±3 m/s²) |

Exact animator types, input/output ranges, and stale timeouts TBD — to be set during tuning.

### 5.3 Brake Show (planned)

| Property | Value |
|---|---|
| Trigger | `CAN_3` (Brake Pressed) |
| Panels | Rear (primary), Front (optional) |
| Animator | TBD — solid red rear, or fill |

### 5.4 Handbrake / Showmode (planned)

| Property | Value |
|---|---|
| Trigger | `CAN_4` (Handbrake On) |
| Purpose | Gate a static display / parked mode performance |
| Animator | TBD |

### 5.5 Rotary Trim 3 mode-select shows (planned)

Rotary Trim 3 (`0x3E4` byte 6) returns values `0`, `1`, `2`, `3` corresponding to the 4-position rotary switch. Intended to select between named show modes (e.g. aggressive/subtle/off/custom). To be implemented as CAN triggers `CAN_5`, `CAN_6`, `CAN_7`, `CAN_8` (one per Rotary Trim 3 position) and assigned to separate shows or performances.

---

## 6. GlowCraft Status Broadcast — Gauge Page Data Source

### 6.1 What the GlowCraft can broadcast

Per firmware documentation, GlowCraft can broadcast a CAN status message describing its current state:

- Which **show** is currently active
- Which **performance** is currently running
- The controller's current **operational state**

This is enabled in Settings → CAN Bus → Status Message, with a configurable message ID. Individual shows can also broadcast their own ID when active (per-show "CAN Status Message" setting in the show editor).

### 6.2 Frame structure — ⚠️ TO BE DEFINED

The actual frame layout for the GlowCraft's status broadcast has not yet been documented by Current Labs in the available firmware docs. The following is a **placeholder structure** for the gauge page to be built against — to be updated once the actual frame format is confirmed (by reading live traffic on the bus or obtaining Current Labs documentation).

**Suggested placeholder Message ID:** `0x500` *(to be confirmed — must not clash with Haltech broadcast IDs)*

```
Byte 0:     Controller State enum
              0x00 = Idle / no show active
              0x01 = Show active
              0x02 = Performance active
              0xFF = Unknown / offline

Byte 1:     Active Show ID (0x00 if none)
Byte 2:     Active Performance ID (0x00 if none)
Bytes 3–7:  Reserved / TBD
```

> Until the actual GlowCraft frame structure is confirmed, the gauge page should be architected so the CAN frame parser is isolated in its own module/function — easy to swap the decode logic once the real layout is known without touching the display layer.

### 6.3 Per-show status IDs (planned)

Each show can be configured to broadcast its own ID when active. Suggested assignments (to be finalised):

| Show | Suggested Broadcast ID |
|---|---|
| BOV Flash | `0x501` |
| G-Force Reactive | `0x502` |
| Brake Show | `0x503` |
| Handbrake / Showmode | `0x504` |
| Rotary Trim Mode 0 | `0x505` |
| Rotary Trim Mode 1 | `0x506` |
| Rotary Trim Mode 2 | `0x507` |
| Rotary Trim Mode 3 | `0x508` |

---

## 7. Gauge Page — Display Spec

### 7.1 Purpose

A new page on the Ultimate Gauge Board (IC-7 replacement project, ESP32-P4 / M5Stack Tab5) that shows a visual representation of the car from above/side, with each LED panel segment displayed in its current colour/state in real time.

### 7.2 Data the page needs

| Data Point | Source | CAN ID | Update Rate |
|---|---|---|---|
| Active show name / ID | GlowCraft status broadcast | `0x500` (TBD) | On change |
| Per-show active flag | Per-show broadcast IDs | `0x501`–`0x508` (TBD) | On change |
| Engine RPM | Haltech | `0x360` | 50 Hz |
| Manifold Pressure | Haltech | `0x360` | 50 Hz |
| Lateral G | Haltech | `0x36B` | 20 Hz |
| Longitudinal G | Haltech | `0x36E` | 20 Hz |
| Brake Pedal Switch | Haltech | `0x3E4` | 5 Hz |
| Hand Brake State | Haltech | `0x3E4` | 5 Hz |
| BOV / Transient Throttle | Haltech | `0x3E4` | 5 Hz |
| Rotary Trim 3 | Haltech | `0x3E4` | 5 Hz |

### 7.3 Panel visualisation — layout intent

Display a simplified top-down silhouette of the Evo 9 with the four LED strips represented as coloured bars in their physical positions:

```
        ┌──[  FRONT STRIP — 30px  ]──┐
        │                            │
   [L   │                            │   R]
   [E   │         EVO 9              │   I]
   [F   │        (body               │   G]
   [T   │       silhouette)          │   H]
   [50] │                            │  [T]
        │                            │  [50]
        └──[  REAR STRIP  — 30px  ]──┘
```

Each strip should:
- Render its pixels as individual coloured rectangles (or a gradient bar as an approximation when the full pixel state isn't available from the CAN broadcast)
- Reflect the currently active show's colour/animation intent — e.g. white flashing for BOV Flash, red rear for braking, reactive blue↔red gradient for lateral G
- Show a static representation of the *last known* or *triggered* state when a show is active, rather than trying to animate at full frame rate on the gauge display

### 7.4 Suggested page states

| State | Display |
|---|---|
| No show active / GlowCraft idle | All strips rendered dim/dark |
| BOV Flash active | All strips white, pulsing indicator |
| G-Force Reactive | Left/Right strip colour shifts with Lateral G value; Front/Rear brightness shifts with Longitudinal G value |
| Brake active | Rear strip red |
| Handbrake on | Static display mode indicator |
| Rotary Trim mode | Mode badge / indicator shown alongside strip colours |
| GlowCraft offline (no CAN frames) | Strips greyed out, "GlowCraft offline" label |

---

## 8. Open Items / To-Do Before Building

- [ ] Resolve `CAN_3` event slot conflict between BOV Flash gate and Brake Pressed trigger
- [ ] Confirm actual GlowCraft CAN status broadcast frame structure (bus sniff or Current Labs documentation)
- [ ] Confirm GlowCraft status message ID doesn't conflict with any Haltech broadcast IDs (Haltech uses `0x360`–`0x3EF`, `0x469`–`0x6FF`, `0x700`–`0x701`)
- [ ] Finalise Rotary Trim 3 show names and CAN event slot assignments (`CAN_5`–`CAN_8`)
- [ ] Define G-Force show animator types and binding input/output ranges after in-car tuning
- [ ] Confirm Brake Pedal Switch and Handbrake start bit positions in GlowCraft (live verify by pressing pedal and watching decoded value — big-endian bit numbering has two valid conventions, see reference doc §4.1)
- [ ] Check all existing Haltech signals in GlowCraft are set to **Big-endian (Motorola)** not little-endian (confirmed per Haltech protocol doc — worth verifying in GlowCraft UI)

---

## 9. Reference Documents

| Document | Location |
|---|---|
| GlowCraft GForce & Switch Bindings Reference | `GlowCraft_GForce_Bindings_Reference.md` (this repo) |
| Haltech ECU Broadcast CAN Protocol v2.0 | `Broadcast_CAN_Protocol_Document_Version_2.pdf` |
| GlowCraft GC01-P Reference Manual (RA) | `https://current-labs.com/pdf/GlowCraft%20LED%20Controller%20(GC01-P)%20-%20Reference%20Manual%20(RA).pdf` |
| GlowCraft Beta Firmware Docs | `https://current-la-firmware.s3.us-west-1.amazonaws.com/c111/beta/documentation.json` |
| Ultimate Gauge Board Project | `aarongillon-crypto/Ultimate_Gauge_Board_PIO` (GitHub) |
