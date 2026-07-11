// Haltech Broadcast CAN Protocol V2 channel registry — the single source of
// truth for every channel the gauge can decode. Transcribed from
// Broadcast_CAN_Protocol_Document_Version_2.pdf (08/2025, in repo root).
//
// This table is deliberately data-driven and machine-readable: the firmware
// decoder walks it generically, /api/channels serves it as JSON, and the
// planned PC/Web layout-designer GUI consumes the same contract. Adding a
// channel = one table row.
//
// Conventions (see PDF pages 3-4):
// - Big-endian, 11-bit IDs, byte 0 = first byte, bit 7 = MSB.
// - value = raw * scale + bias, in CANONICAL units (the doc's "Units" column),
//   with ONE deliberate deviation: manifold/target-boost/pre-intercooler
//   pressures (doc: kPa Abs) are stored as GAUGE kPa (bias -101.3) so a boost
//   gauge reads 0 at atmospheric like every other pressure channel. Barometric
//   pressure stays absolute.
// - Display conversion (psi/°F/mph...) is a separate layer: chan_display() /
//   chan_unit_str() honour the global unit preferences.
#pragma once
#include <Arduino.h>

// Canonical units. U_KPA is GAUGE pressure (doc conversions already subtract
// 101.3); U_KPA_ABS is absolute. U_KELVIN converts to °C/°F for display.
enum ChanUnit : uint8_t {
  U_NONE = 0,   // raw / unitless
  U_RPM, U_KPA, U_KPA_ABS, U_PCT, U_KELVIN, U_LAMBDA, U_KMH, U_MS2,
  U_VOLT, U_MSEC, U_DEG, U_DEGPS, U_DB, U_CCPM, U_LITRE, U_KML, U_L100KM,
  U_PPM, U_GM3, U_MM, U_MMPS, U_SEC, U_CC, U_METRE, U_BOOL, U_ENUM,
};

struct HaltechChannel {
  uint16_t can_id;      // 11-bit CAN ID
  uint8_t  offset;      // starting byte within the 8-byte payload
  uint8_t  size;        // bytes: 1, 2 or 4 (ignored when bit-addressed)
  uint8_t  bit_start;   // 0xFF = byte-aligned; else MSB bit number within byte
  uint8_t  bit_len;     // bit count when bit-addressed
  bool     is_signed;
  float    scale, bias; // canonical = raw * scale + bias
  ChanUnit unit;
  const char* name;
};

extern const HaltechChannel HALTECH_CHANNELS[];
extern const int HALTECH_CHANNEL_COUNT;

// Stable channel key for configs/API: (can_id << 4) | byte_offset for scalar
// channels; bit-addressed channels add +1 in the LSB-shifted nibble space is
// NOT used — bit channels are not bindable as gauge modes. 0 = invalid.
#define CHAN_KEY(id, off) ((uint16_t)(((id) << 4) | ((off) & 0x0F)))

// Registry lookup.
int chan_index_from_key(uint16_t key);            // -1 if unknown
uint16_t chan_key(int index);                     // 0 if out of range

// Display-unit preferences (persisted; applied by chan_display/chan_unit_str).
extern bool units_press_psi;   // pressures: true=psi, false=kPa (default psi)
extern bool units_temp_f;      // temps: true=°F, false=°C (default °C)
extern bool units_speed_mph;   // speeds: true=mph, false=km/h (default km/h)
extern bool units_lambda_afr;  // lambda: true=AFR (λ×14.7), false=λ (default AFR)

float chan_display(int index, float canonical);   // canonical -> display units
const char* chan_unit_str(int index);             // display unit suffix, "" if none
