# Tab5 Monitor — Phase 0 ESP-NOW spike

> ## ⏸️ PAUSED — ESP-NOW not viable on the Tab5
> This spike answered its question with a **no**: `esp_now_*` is undefined at link
> on the ESP32-P4 (arduino-esp32 3.3.8), and `esp_wifi_remote` 1.6.1 doesn't remote
> ESP-NOW in ESP-IDF either. The board compiles and **regular WiFi works**, but
> ESP-NOW does not. Work is paused — see `../M5Tab5_Remote_Monitor_Plan.md` §9 for
> the recommended "gateway gauge" resume path. The code below is kept as evidence
> and as a potential WiFi-client starting point.

Throwaway spike to answer one question: **does ESP-NOW work on the M5Stack Tab5
(ESP32-P4 + C6) against the existing gauges?** See `../M5Tab5_Remote_Monitor_Plan.md`
for the full plan; this is Phase 0.

## What it does

- Listens for ESP-NOW frames on **channel 1** and prints each one (type, length,
  source MAC) to USB serial **and** the Tab5 screen, with a running RX counter.
- Every 5 s, broadcasts a **test-mode toggle** (`type 4`) command — the same
  packet the gauges already accept.

## How to test

1. Power on **at least one gauge** (it broadcasts presence on ch1 every 2 s).
2. Open **this folder** (`tab5_monitor/`) as its own PlatformIO project.
3. Build + flash to the Tab5 over USB:
   `pio run -e m5stack-tab5 -t upload`
4. Open the serial monitor (115200) and watch the Tab5 screen.

## What success looks like

- **Receive works:** `RX count` climbs and you see `type=1` packets from the
  gauge's MAC (~every 2 s).
- **Send works:** a nearby gauge's **Test Mode toggles on/off every 5 s**
  (gauge face animates / web UI shows Test Mode flipping).

If both happen, Phase 0 passes and we proceed to the telemetry protocol (Phase 1).

## If it doesn't build / work — likely fixes (P4 Arduino is new)

- **Board id rejected:** run `pio boards esp32p4` and set the right `board` in
  `platformio.ini` (candidates: `esp32-p4-evboard`, `esp32-p4`, `esp32p4`).
- **`esp_now_*` / `esp_wifi_*` link errors:** ESP-NOW on P4 routes through
  `esp_wifi_remote`; may need a matching arduino-esp32 / pioarduino version or an
  extra component. Capture the exact error and we adjust.
- **Builds but RX count stays 0:** channel mismatch or the C6/antenna isn't up.
  Confirm `M5.begin()` succeeded and that `esp_wifi_set_channel` returned `ESP_OK`
  in the serial log. Verify a gauge is actually powered and on channel 1.
- **RX works but send has no effect:** confirm the gauge reacts to `type 4` from
  other senders (it should — it only filters its own MAC).

Paste the serial output (and any build errors) back and we'll iterate.
