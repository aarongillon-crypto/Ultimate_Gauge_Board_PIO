# Haltech / vehicle CAN bus — observed frame reference

Practical map of **every CAN ID seen on this car's bus**, so future work
(new decodes, GlowCraft signals, debugging) doesn't start from scratch.

Three sources feed this doc (all in the repo root):
1. **`Broadcast_CAN_Protocol_Document_Version_2.pdf`** — the Haltech *ECU
   Broadcast* protocol, v2.0 08/2025. The gauge's channel registry
   (`src/haltech_channels.cpp`) is a 1:1 transcription of it.
2. **`Multiplexed_CAN_Protocol_Document_Version_2.pdf`** — the *device* protocol
   for Nexus peripherals (PD16, **IO16**, TI4L) and the Haltech dash. Covers the
   `0x3xx` / `0x6xx` device frames and `0x700`.
3. **Live capture** via the on-gauge CAN sniffer (`GET /api/cansniff`, web UI
   "CAN Sniffer (non-Haltech)" card, added v2.6.1) — shows every ID *not* in the
   registry as raw hex. This is how the frames below were found.

> **Bit numbering:** LSB-first (bit0 = `0x01`), matching both Haltech docs and
> the `BITS()` macro in the registry. Multi-byte values are big-endian.

This car's devices: **Haltech Nexus-class ECU** (broadcast + internal R5 PDM
mux on `0x700`), **Nexus IO16** input/output expander (with HBO outputs), and
the **GlowCraft** LED controller. No physical PD16.

---

## 1. ID ranges at a glance

| Range | Owner | Gauge decodes it? |
|-------|-------|-------------------|
| `0x360`–`0x3F3` | Haltech ECU broadcast | ✅ in registry (some sub-fields skipped) |
| `0x469`–`0x477` | Haltech ECU broadcast | ✅ in registry |
| `0x6F0`–`0x701` | Haltech ECU broadcast | ✅ in registry (`0x700` is the R5 PDM mux — skipped) |
| `0x320`–`0x341` | Nexus device → ECU (inputs: AVI/DPI/button state) | ❌ multiplexed, not decoded |
| `0x6A8`–`0x6EF` | Nexus device ↔ ECU (PD16/IO16/TI4L control + status) | ❌ multiplexed, not decoded |
| `0x500`–`0x52F` | **GlowCraft** (we assign these) | strips `0x500`+ on the LED page; `0x520` signals frame (park) |
| everything else | unknown / third-party | ❌ |

> The gauge is **TX-capable** as of v2.6.2 (`canbus_send()`), but currently
> transmits nothing — the primitive is kept for future use. (`0x550` was reserved
> for the abandoned iC-7 page-seek; free again.)

**Complete ECU-broadcast set (v2.0 PDF)** for cross-checking:
`0x360–0x377 0x380 0x3E0–0x3EF 0x3F0–0x3F3 0x469 0x470–0x477 0x6F0–0x6F9 0x6FF
0x700 0x701`. Anything not in that list is a Nexus device (see §3) or third-party.

---

## 2. Frames observed on this car (2026-07-19)

Captured on the bench, engine off, park light toggled. Payloads are raw sniffer
bytes.

| CAN ID | DLC | Payload | Identity | Confidence |
|--------|-----|---------|----------|------------|
| `0x6A9` | 8 | `83 02 00 00 07 D0 00 00` | **IO16 A** (base `0x6A8` +1) — see §3 | **High** (base-ID table) |
| `0x6AA` | 8 | `00 00 00 00 00 00 00 00` | **IO16 A** (base `0x6A8` +2) | **High** |
| `0x700` | 8 | `24 00 00 00 00 00 00 00` | **ECU internal R5 PDM** output mux — see §3 | **High** (both PDFs) |
| `0x37F` | 8 | `04 C1 00 00 00 00 00 00` | Unknown — not in any PDF | Low |
| `0x60B` | 6 | `22 00 18 05 00 00` | Likely **CAN keypad** (Grayhill 3K, Haltech variant) — see §4 | Medium |
| `0x60C` | 5 | `22 00 18 02 00` | Likely CAN keypad (same device) | Medium |
| `0x60D` | 6 | `22 00 18 05 00 00` | Likely CAN keypad (same device) | Medium |

**None reacted to the park light** — confirming that signal isn't on the Haltech
bus, which is why the GlowCraft now supplies it on `0x520`.

### The `0x60B`–`0x60D` triplet — likely the CAN keypad
Identical `22 00 18 …` header across three consecutive IDs points at **one
device emitting three related frames** (e.g. key state / LED status / heartbeat).
The prime suspect is the **Grayhill 3K CAN keypad** (see §4). Caveats that keep
this at *medium*, not confirmed:
- Native Grayhill is **29-bit J1939** (`0x18FF02xx`); these are **11-bit**, so
  the **Haltech variant runs a non-J1939 11-bit mode** with a reassigned frame —
  matches the "Haltech software is different" warning.
- The idle payload `22 00 18…` isn't the all-`00`/all-`11` pattern you'd expect
  from the standard 2-bit-per-key layout at rest, so the Haltech bit-packing
  likely differs from the Grayhill doc.

**To confirm & reverse the real layout:** open the sniffer, **press keypad
buttons one at a time**, and watch which of `0x60B`–`0x60D` changes and which
bits move. Grayhill uses 2 bits/key (§4) — the Haltech version probably keeps
that but re-packs it. `0x37F` (just below broadcast `0x380`, undefined in v2.0)
is still open — actuate other suspects (IO16, GlowCraft) to place it.

---

## 3. Nexus Multiplexed CAN protocol (decodes `0x6xx` / `0x700` / `0x3xx`)

Nexus peripherals **multiplex** to save CAN IDs: **byte0 = Mux ID**, where the
top 3 bits are the IO *type* and the low 4 bits are the IO *index*. The same CAN
ID + payload shape carries many channels, distinguished by byte0.

**Mux IO types (byte0 bits 7:5):**
| Val | Type | | Val | Type |
|-----|------|-|-----|------|
| 0 | 25A HCO (high-current output) | | 4 | AVI (analog voltage input) |
| 1 | 8A HCO | | 5 | DPI (digital pulse input) |
| 2 | **Half Bridge (HBO)** | | 6 | DPO (digital pulse output) |
| 3 | SPI (switch/pulse input) | | 7 | Button |

IO *index* (byte0 bits 3:0) selects the channel within the type (e.g. `0`=HBO1).

**Device base CAN IDs** (each device occupies a block of `base .. base+6`):
| Device | Control block (ECU→dev / dev↔ECU) | Input-report block (dev→ECU) |
|--------|-----------------------------------|------------------------------|
| PD16 A–D | `0x6D0 / 0x6D8 / 0x6E0 / 0x6E8` | `0x320 / 0x321 / 0x322 / 0x323` |
| **IO16 A / B** | **`0x6A8` / `0x6B0`** | **`0x330–0x333` / `0x334–0x337`** |
| TI4L | `0x6B8` | `0x338` |
| Haltech Dash (iC-7, uC-10) | `0x680` | `0x340`, `0x341` |

**Block offsets** from a control base: `base` = output control (25A/8A/HBO duty,
freq, target current, RX 20 Hz); `base+1`, `base+2` = pin config (retries, drive
type, safe state, fuse current, RX 2 Hz); `base+3` = input/status report (SPI/
AVI/DPI/Button state, TX 20 Hz); `base+4..+6` = lower-rate status (TX 2–5 Hz).
The input-report block (`0x330`+) streams AVI 1–4 state+voltage (`5000` = 5.00 V)
on the base, AVI 5–8 on base+1, etc.

### Decoding this car's frames
- **`0x6A9` = `83 …`** → IO16 A, base+1 (pin config). `byte0 0x83` = type **4
  (AVI)**, index **3 (AVI4)**. `bytes4–5 = 0x07D0 = 2000` → consistent with an
  AVI threshold/reading of **2.00 V** (mV scale). Exact field depends on the
  base+1 config row.
- **`0x6AA` = `00 …`** → IO16 A, base+2. `byte0 0x00` = type 0 (25A HCO), index 0
  — idle/unconfigured.
- **`0x700` = `24 …`** → ECU **internal R5 PDM** output mux (broadcast doc: "HBO,
  HCO8, HCO25 info per the PD16→ECU protocol for the internal R5 PDM," 50 Hz).
  `byte0 0x24` = type **1 (8A HCO)**, index **4 (8A-HCO5)**; rest zero = that
  output at 0 % / off. Present even without a physical PD16 because the ECU has
  the internal R5.

### If we ever want the gauge to read the IO16
The IO16's live **inputs** (analog voltages, digital/button states) are on
`0x330–0x333` (AVI/DPI/Button, TX 20–100 Hz). Decoding them means walking the
Mux ID in byte0 like the tables above — a separate mini-decoder from the
broadcast registry, out of scope today but well-specified in the mux PDF.

---

## 4. CAN keypad (Grayhill 3K — Haltech variant) — suspected `0x60B`–`0x60D`

Source: `3kg1-programming-manual-j1939.pdf` (repo root). **This is the native
Grayhill J1939 spec; the Haltech-supplied keypad uses different firmware and is
NOT guaranteed identical** — treat this as the decoding *starting point*, then
confirm bit positions live with the sniffer.

**Native Grayhill Key Press Data frame:**
- **PGN 65282 (`0xFF02`)**, Proprietary B PDU2, priority 6, **8 data bytes**,
  TX ~100 ms (programmable). Native 29-bit ID `0x18FF0280` (source addr `0x80`).
  The PGN is **reassignable by config command** — how Haltech likely remaps it.
- **2 bits per key**, 4 keys per byte, LSB-first. Byte1 = keys 1–4, byte2 =
  keys 5–8, … byte8 = keys 29–32. Each 2-bit field:

  | Value | Meaning |
  |-------|---------|
  | `00` | Key not pressed |
  | `01` | Key pressed |
  | `10` | Error |
  | `11` | Unused key (position not populated) |

  Example (Grayhill): pressing key 12 → `00 00 40 00 00 FF FF FF` (key 12 is
  byte3 bits 7–8 → `0x40`; the trailing `FF`s are unused keys 21–32 on a 20-key
  unit).
- **LED control** is received on Auxiliary I/O PGNs (2 bits per indicator) or a
  Proprietary A PGN — relevant only if we ever drive the keypad LEDs from here.

**Why we care:** if confirmed, the gauge could read keypad buttons to drive
actions (page switch, mode change, peak reset) — realising the CAN-page-switch
idea directly, without needing the GlowCraft to relay anything.

**Confirm procedure:** sniffer open → press one key → note which ID and which
2-bit field flips to `01`. Map each physical key to its byte/bit, then (if
wanted) add a tiny decoder keyed on that ID. Don't assume the Grayhill byte
order survives the Haltech firmware — verify every key.

> Spotting extended frames in the sniffer: the web view suffixes an `x` on
> extended (29-bit) IDs (e.g. `0x280x`). Your `0x60B`–`0x60D` showed **no** `x`,
> so they are genuine 11-bit frames — the Haltech keypad is not on raw J1939.

---

## 5. How to positively identify an unknown frame

1. **Open the sniffer** (web UI → "CAN Sniffer (non-Haltech)", or `GET /api/cansniff`).
2. **Actuate the suspected source** and watch which ID's bytes change:
   - IO16 output → trigger the function driving an HBO/HCO, watch `0x6A8`/`0x700`.
   - IO16 input → apply voltage / press a wired button, watch `0x330`+.
   - GlowCraft → toggle a show, watch `0x520` (and the `0x60x` triplet).
3. **Cross-reference the device manual / NSP** — CAN-ID swapping means a device's
   base may be remapped from the defaults above.
4. Record confirmed frames here; add genuinely useful ones to the registry.

> The sniffer dims frames stale >3 s, so a frame that only appears while you
> actuate something is easy to spot.

---

## 6. Open items & future tests

### 6a. Confirm the CAN keypad (`0x60B`–`0x60D`) — FUTURE TEST
Next time at the car: sniffer open, **press each keypad key one at a time**,
record which ID and which 2-bit field flips to `01` (see §4). Produces the real
Haltech key→byte/bit map, after which a small decoder + bindable actions
(page switch, mode, peak reset) can be added. Also place `0x37F` by actuating
remaining suspects.

### 6b. Switching the Haltech iC-7 display page over CAN — BLOCKED

> **BLOCKED / not achievable (confirmed 2026-07-19).** The plan required the iC-7
> to key a page-change condition off a value the *gauge* transmits. **The Haltech
> ecosystem does not allow custom CAN condition sources — iC-7 conditions can only
> reference native Haltech channels.** The gauge cannot produce a native channel
> (those come from the ECU), so it can never command the dash. This kills any
> "gauge relays a page command to the iC-7" design, endstop-seek included.
>
> **Code status:** the seek attempt was reverted in **v2.6.2** — `src/dash_page.*`,
> the "iC-7 Dash Page Sync" web card, its API actions and NVS keys are all removed.
> Only the reusable `canbus_send()` TX primitive was kept (gauge is now TX-capable,
> but transmits nothing today).
>
> **Native-only paths that remain (all dash-side, no gauge):** configure iC-7 page
> up/down conditions directly on a native control — Rotary Trim 3 (`0x3E4` b6), or
> a **CAN keypad button** / IO16 input (clean per-press events). These give
> *relative* paging synced to that control. **Absolute "go to page N" is still not
> possible** on the iC-7 (relative-only conditions + no current-page feedback).

The original analysis is kept below for context.
**Question:** can we command the iC-7 to change its displayed screen by sending
a CAN frame (gauge as relay), e.g. off Rotary Trim 3?

**Doc answer: No direct command exists.** All three protocol PDFs were searched —
the Multiplexed CAN protocol shares I/O only (25A/8A/HBO outputs, AVI/DPI/SPI/
Button inputs, pin config). There is **no page/screen/navigation/display frame**,
and no RX "press a dash button" injection (the dash only *reports* its buttons,
`base+3` type 7, TX). So you cannot tell the iC-7 "show page N" over CAN.

**How iC-7 page changes actually work:** in the dash config (NSP) — via its
buttons and/or **conditional page-change rules** (change page when a condition on
a channel is met). That's a dash-side feature, not a bus command.

**Practical path (no relay needed for the trigger):** the iC-7 already receives
Rotary Trim 3 directly from the ECU broadcast (`0x3E4` byte 6 — the same frame
this gauge themes from). So configure iC-7 page-change conditions **against the
rotary-trim channel in NSP**; no gauge involvement required. Check NSP for
conditional/priority page visibility bound to that channel.

**Where the gauge *could* help:** if NSP page conditions are awkward against the
raw signed trim byte, the gauge (which already decodes Trim 3 → a clean 0–3 slot
for theming) could **re-broadcast a tidy "page select" value on a dedicated CAN
ID**, giving one clean channel to write one condition per page against. This
would require adding a small **CAN TX** path (the gauge is currently RX-only) and
a chosen ID clear of the ranges in §1. It only helps if the iC-7 supports
conditional page switching at all — the relay can't create a capability the dash
lacks. **Verify the NSP feature first; only then is the TX relay worth building.**

**Update — the iC-7 only offers *relative* page conditions (button up / button
down), and there is NO current-page feedback channel.** Confirmed by searching
both device PDFs for page/screen/active/current/menu/index/position/view — the
only "state" fields are I/O states (output active, input on/off, button
pressed). The dash never broadcasts which page it is showing, so **absolute
"go to page N" is not directly possible.**

**Workaround — seek from a known endstop** (only reliable absolute method):
1. To select page *k*, pulse **UP** N times (N = page count). If the iC-7 page
   list has a **hard end that does not wrap**, this homes to page 0 regardless of
   start.
2. Pulse **DOWN** *k* times → land on page *k*.
This self-corrects every seek (no shadow counter; survives manual button use).
Preconditions: (a) **navigation must not wrap** — ✅ CONFIRMED 2026-07-19: the
iC-7 hard-stops at both ends, no wrap, so the endstop seek is viable; (b) known
page count;
(c) gauge **CAN TX** to toggle the condition channel (true→false with dwell per
edge). Tradeoff: the seek is **visible** (dash flicks up then down to target) —
a sub-second flicker for a few pages, worse if the dash animates transitions.
Dead-reckoning (shadow counter, no re-home) avoids the flicker but desyncs
permanently on any unexpected change — avoid, given zero feedback to recover.

---

## 7. Regenerating the PDF text

The PDFs aren't page-renderable in this environment; extract text with:

```
pip install pypdf
python -c "from pypdf import PdfReader; open('out.txt','w',encoding='utf-8').write('\n'.join((p.extract_text() or '') for p in PdfReader('Multiplexed_CAN_Protocol_Document_Version_2.pdf').pages))"
```
