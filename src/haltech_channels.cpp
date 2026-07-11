// Haltech Broadcast CAN Protocol V2 channel table. Transcribed 1:1 from the
// PDF in the repo root (document version 2.0, 08/2025). See header for
// conventions. Skipped for now (add rows as needed): 0x3E4/0x473 switch
// bit-matrices (except CEL/battery light), 0x370 combined-gear (redundant
// with 0x470), 0x472/0x477/0x6F8 secondary enums, 0x6F7 generic-output
// bitfield, 0x700 PDM multiplex.
#include "haltech_channels.h"

#define BYTE_ALIGNED 0xFF, 0
// {can_id, offset, size, bit_start, bit_len, signed, scale, bias, unit, name}
#define ROW(id, off, sz, sg, sc, bi, un, nm) { id, off, sz, BYTE_ALIGNED, sg, sc, bi, un, nm }
#define BITS(id, off, bs, bl, un, nm)        { id, off, 1, bs, bl, false, 1.0f, 0.0f, un, nm }

const HaltechChannel HALTECH_CHANNELS[] = {
  // ---- 0x360 (50 Hz) ----
  ROW(0x360, 0, 2, false, 1.0f,    0.0f,    U_RPM,     "RPM"),
  ROW(0x360, 2, 2, false, 0.1f,   -101.3f,  U_KPA,     "Manifold Pressure"),   // stored as gauge kPa (see header)
  ROW(0x360, 4, 2, false, 0.1f,    0.0f,    U_PCT,     "Throttle Position"),
  ROW(0x360, 6, 2, false, 0.1f,   -101.3f,  U_KPA,     "Coolant Pressure"),
  // ---- 0x361 (50 Hz) ----
  ROW(0x361, 0, 2, false, 0.1f,   -101.3f,  U_KPA,     "Fuel Pressure"),
  ROW(0x361, 2, 2, false, 0.1f,   -101.3f,  U_KPA,     "Oil Pressure"),
  ROW(0x361, 4, 2, false, 0.1f,    0.0f,    U_PCT,     "Engine Demand"),
  ROW(0x361, 6, 2, false, 0.1f,   -101.3f,  U_KPA,     "Wastegate Pressure"),
  // ---- 0x362 (50 Hz) ----
  ROW(0x362, 0, 2, false, 0.1f,    0.0f,    U_PCT,     "Injection Stage 1 Duty"),
  ROW(0x362, 2, 2, false, 0.1f,    0.0f,    U_PCT,     "Injection Stage 2 Duty"),
  ROW(0x362, 4, 2, true,  0.1f,    0.0f,    U_DEG,     "Ignition Angle (Leading)"),
  // ---- 0x363 (20 Hz) ----
  ROW(0x363, 0, 2, true,  0.1f,    0.0f,    U_KMH,     "Wheel Slip"),
  ROW(0x363, 2, 2, true,  0.1f,    0.0f,    U_KMH,     "Wheel Diff"),
  ROW(0x363, 6, 2, false, 1.0f,    0.0f,    U_RPM,     "Launch Control End RPM"),
  // ---- 0x364 (50 Hz) ----
  ROW(0x364, 0, 2, false, 0.001f,  0.0f,    U_MSEC,    "Inj Stage 1 Avg Time"),
  ROW(0x364, 2, 2, false, 0.001f,  0.0f,    U_MSEC,    "Inj Stage 2 Avg Time"),
  ROW(0x364, 4, 2, false, 0.001f,  0.0f,    U_MSEC,    "Inj Stage 3 Avg Time"),
  ROW(0x364, 6, 2, false, 0.001f,  0.0f,    U_MSEC,    "Inj Stage 4 Avg Time"),
  // ---- 0x368 (20 Hz) ----
  ROW(0x368, 0, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Wideband Sensor 1"),
  ROW(0x368, 2, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Wideband Sensor 2"),
  ROW(0x368, 4, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Wideband Sensor 3"),
  ROW(0x368, 6, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Wideband Sensor 4"),
  // ---- 0x369 (20 Hz) ----
  ROW(0x369, 0, 2, false, 1.0f,    0.0f,    U_NONE,    "Trigger Error Count"),
  ROW(0x369, 2, 2, false, 1.0f,    0.0f,    U_NONE,    "Trigger Counter"),
  ROW(0x369, 6, 2, false, 1.0f,    0.0f,    U_NONE,    "Trigger Sync Level"),
  // ---- 0x36A (20 Hz) ----
  ROW(0x36A, 0, 2, false, 0.01f,   0.0f,    U_DB,      "Knock Level 1"),
  ROW(0x36A, 2, 2, false, 0.01f,   0.0f,    U_DB,      "Knock Level 2"),
  // ---- 0x36B (20 Hz) ----
  ROW(0x36B, 0, 2, false, 1.0f,   -101.3f,  U_KPA,     "Brake Pressure Front"),
  ROW(0x36B, 2, 2, false, 0.22f,  -101.3f,  U_KPA,     "NOS Pressure 1"),
  ROW(0x36B, 4, 2, false, 10.0f,   0.0f,    U_RPM,     "Turbo Speed 1"),
  ROW(0x36B, 6, 2, true,  0.1f,    0.0f,    U_MS2,     "Lateral G"),
  // ---- 0x36C (20 Hz) ----
  ROW(0x36C, 0, 2, false, 0.1f,    0.0f,    U_KMH,     "Wheel Speed FL"),
  ROW(0x36C, 2, 2, false, 0.1f,    0.0f,    U_KMH,     "Wheel Speed FR"),
  ROW(0x36C, 4, 2, false, 0.1f,    0.0f,    U_KMH,     "Wheel Speed RL"),
  ROW(0x36C, 6, 2, false, 0.1f,    0.0f,    U_KMH,     "Wheel Speed RR"),
  // ---- 0x36D (20 Hz) ----
  ROW(0x36D, 4, 2, true,  0.1f,    0.0f,    U_DEG,     "Exhaust Cam Angle 1"),
  ROW(0x36D, 6, 2, true,  0.1f,    0.0f,    U_DEG,     "Exhaust Cam Angle 2"),
  // ---- 0x36E (20 Hz) ----
  ROW(0x36E, 0, 2, false, 1.0f,    0.0f,    U_BOOL,    "Engine Limiting Active"),
  ROW(0x36E, 2, 2, true,  0.1f,    0.0f,    U_DEG,     "LC Ignition Retard"),
  ROW(0x36E, 4, 2, true,  0.1f,    0.0f,    U_PCT,     "LC Fuel Enrich"),
  ROW(0x36E, 6, 2, true,  0.1f,    0.0f,    U_MS2,     "Longitudinal G"),
  // ---- 0x36F (20 Hz) ----
  ROW(0x36F, 0, 2, false, 0.1f,    0.0f,    U_PCT,     "Generic Output 1 Duty"),
  ROW(0x36F, 2, 2, false, 0.1f,    0.0f,    U_PCT,     "Boost Control Output"),
  // ---- 0x370 (20 Hz) ----
  ROW(0x370, 0, 2, false, 0.1f,    0.0f,    U_KMH,     "Vehicle Speed"),
  ROW(0x370, 4, 2, true,  0.1f,    0.0f,    U_DEG,     "Intake Cam Angle 1"),
  ROW(0x370, 6, 2, true,  0.1f,    0.0f,    U_DEG,     "Intake Cam Angle 2"),
  // ---- 0x371 (10 Hz) ----
  ROW(0x371, 0, 2, false, 1.0f,    0.0f,    U_CCPM,    "Fuel Flow"),
  ROW(0x371, 2, 2, false, 1.0f,    0.0f,    U_CCPM,    "Fuel Flow Return"),
  // ---- 0x372 (10 Hz) ----
  ROW(0x372, 0, 2, false, 0.1f,    0.0f,    U_VOLT,    "Battery Voltage"),
  ROW(0x372, 4, 2, false, 0.1f,   -101.3f,  U_KPA,     "Target Boost Level"),   // stored as gauge kPa
  ROW(0x372, 6, 2, false, 0.1f,    0.0f,    U_KPA_ABS, "Barometric Pressure"),
  // ---- 0x373-0x375 EGT 1-12 (10 Hz) ----
  ROW(0x373, 0, 2, false, 0.1f,    0.0f,    U_KELVIN,  "EGT 1"),
  ROW(0x373, 2, 2, false, 0.1f,    0.0f,    U_KELVIN,  "EGT 2"),
  ROW(0x373, 4, 2, false, 0.1f,    0.0f,    U_KELVIN,  "EGT 3"),
  ROW(0x373, 6, 2, false, 0.1f,    0.0f,    U_KELVIN,  "EGT 4"),
  ROW(0x374, 0, 2, false, 0.1f,    0.0f,    U_KELVIN,  "EGT 5"),
  ROW(0x374, 2, 2, false, 0.1f,    0.0f,    U_KELVIN,  "EGT 6"),
  ROW(0x374, 4, 2, false, 0.1f,    0.0f,    U_KELVIN,  "EGT 7"),
  ROW(0x374, 6, 2, false, 0.1f,    0.0f,    U_KELVIN,  "EGT 8"),
  ROW(0x375, 0, 2, false, 0.1f,    0.0f,    U_KELVIN,  "EGT 9"),
  ROW(0x375, 2, 2, false, 0.1f,    0.0f,    U_KELVIN,  "EGT 10"),
  ROW(0x375, 4, 2, false, 0.1f,    0.0f,    U_KELVIN,  "EGT 11"),
  ROW(0x375, 6, 2, false, 0.1f,    0.0f,    U_KELVIN,  "EGT 12"),
  // ---- 0x376 (10 Hz) ----
  ROW(0x376, 0, 2, false, 0.1f,    0.0f,    U_KELVIN,  "Ambient Air Temp"),
  ROW(0x376, 2, 2, true,  0.1f,    0.0f,    U_PCT,     "Relative Humidity"),
  ROW(0x376, 4, 2, false, 100.0f,  0.0f,    U_PPM,     "Specific Humidity"),
  ROW(0x376, 6, 2, false, 0.1f,    0.0f,    U_GM3,     "Absolute Humidity"),
  // ---- 0x377 (50 Hz) ----
  ROW(0x377, 0, 2, false, 0.1f,   -101.3f,  U_KPA,     "Boost Pre-Intercooler"), // stored as gauge kPa
  // ---- 0x380 (10 Hz) ----
  ROW(0x380, 0, 2, false, 1.0f,    0.0f,    U_NONE,    "Generic Output 1 Value"),
  ROW(0x380, 2, 2, false, 1.0f,    0.0f,    U_NONE,    "Generic Output 2 Value"),
  ROW(0x380, 4, 2, false, 1.0f,    0.0f,    U_NONE,    "Generic Output 3 Value"),
  ROW(0x380, 6, 2, false, 1.0f,    0.0f,    U_NONE,    "Generic Output 4 Value"),
  // ---- 0x3E0 (5 Hz) ----
  ROW(0x3E0, 0, 2, false, 0.1f,    0.0f,    U_KELVIN,  "Coolant Temperature"),
  ROW(0x3E0, 2, 2, false, 0.1f,    0.0f,    U_KELVIN,  "Air Temperature"),
  ROW(0x3E0, 4, 2, false, 0.1f,    0.0f,    U_KELVIN,  "Fuel Temperature"),
  ROW(0x3E0, 6, 2, false, 0.1f,    0.0f,    U_KELVIN,  "Oil Temperature"),
  // ---- 0x3E1 (5 Hz) ----
  ROW(0x3E1, 0, 2, false, 0.1f,    0.0f,    U_KELVIN,  "Gearbox Oil Temp"),
  ROW(0x3E1, 2, 2, false, 0.1f,    0.0f,    U_KELVIN,  "Diff Oil Temp"),
  ROW(0x3E1, 4, 2, false, 0.1f,    0.0f,    U_PCT,     "Fuel Composition"),
  ROW(0x3E1, 6, 2, false, 0.1f,    0.0f,    U_KELVIN,  "Air Temp Pre-Intercooler"),
  // ---- 0x3E2 (5 Hz) ----
  ROW(0x3E2, 0, 2, false, 0.1f,    0.0f,    U_LITRE,   "Fuel Level"),
  ROW(0x3E2, 2, 2, true,  0.1f,    0.0f,    U_CCPM,    "Fuel Volume Est Flow"),
  ROW(0x3E2, 4, 2, false, 0.1f,    0.0f,    U_KML,     "Avg Fuel Mileage"),
  ROW(0x3E2, 6, 2, false, 0.1f,    0.0f,    U_LITRE,   "Fuel Level 2"),
  // ---- 0x3E3 (5 Hz) ----
  ROW(0x3E3, 0, 2, true,  0.1f,    0.0f,    U_PCT,     "Fuel Trim ST Bank 1"),
  ROW(0x3E3, 2, 2, true,  0.1f,    0.0f,    U_PCT,     "Fuel Trim ST Bank 2"),
  ROW(0x3E3, 4, 2, true,  0.1f,    0.0f,    U_PCT,     "Fuel Trim LT Bank 1"),
  ROW(0x3E3, 6, 2, true,  0.1f,    0.0f,    U_PCT,     "Fuel Trim LT Bank 2"),
  // ---- 0x3E4 (5 Hz) — trimpots + warning lights (switch matrix rows TBD) ----
  ROW(0x3E4, 4, 1, true,  1.0f,    0.0f,    U_NONE,    "Rotary Trim Pot 1"),
  ROW(0x3E4, 5, 1, true,  1.0f,    0.0f,    U_NONE,    "Rotary Trim Pot 2"),
  ROW(0x3E4, 6, 1, true,  1.0f,    0.0f,    U_NONE,    "Rotary Trim Pot 3"),
  BITS(0x3E4, 7, 7, 1,                      U_BOOL,    "Check Engine Light"),
  BITS(0x3E4, 7, 6, 1,                      U_BOOL,    "Battery Light"),
  // ---- 0x3E5 (50 Hz) ----
  ROW(0x3E5, 0, 1, false, 1.0f,    0.0f,    U_BOOL,    "Ignition Switch"),
  ROW(0x3E5, 1, 1, false, 1.0f,    0.0f,    U_SEC,     "Turbo Timer Remaining"),
  ROW(0x3E5, 2, 1, false, 1.0f,    0.0f,    U_SEC,     "Turbo Timer Engine Time"),
  ROW(0x3E5, 4, 2, true,  0.1f,    0.0f,    U_DEG,     "Steering Wheel Angle"),
  ROW(0x3E5, 6, 2, false, 1.0f,    0.0f,    U_RPM,     "Driveshaft RPM"),
  // ---- 0x3E6 (20 Hz) ----
  ROW(0x3E6, 0, 2, false, 0.22f,  -101.3f,  U_KPA,     "NOS Pressure 2"),
  ROW(0x3E6, 2, 2, false, 0.22f,  -101.3f,  U_KPA,     "NOS Pressure 3"),
  ROW(0x3E6, 4, 2, false, 0.22f,  -101.3f,  U_KPA,     "NOS Pressure 4"),
  ROW(0x3E6, 6, 2, false, 10.0f,   0.0f,    U_RPM,     "Turbo Speed 2"),
  // ---- 0x3E7-0x3E9 generic sensors (raw; scaling is user-configured in NSP) ----
  ROW(0x3E7, 0, 2, false, 1.0f,    0.0f,    U_NONE,    "Generic Sensor 1"),
  ROW(0x3E7, 2, 2, false, 1.0f,    0.0f,    U_NONE,    "Generic Sensor 2"),
  ROW(0x3E7, 4, 2, false, 1.0f,    0.0f,    U_NONE,    "Generic Sensor 3"),
  ROW(0x3E7, 6, 2, false, 1.0f,    0.0f,    U_NONE,    "Generic Sensor 4"),
  ROW(0x3E8, 0, 2, false, 1.0f,    0.0f,    U_NONE,    "Generic Sensor 5"),
  ROW(0x3E8, 2, 2, false, 1.0f,    0.0f,    U_NONE,    "Generic Sensor 6"),
  ROW(0x3E8, 4, 2, false, 1.0f,    0.0f,    U_NONE,    "Generic Sensor 7"),
  ROW(0x3E8, 6, 2, false, 1.0f,    0.0f,    U_NONE,    "Generic Sensor 8"),
  ROW(0x3E9, 0, 2, false, 1.0f,    0.0f,    U_NONE,    "Generic Sensor 9"),
  ROW(0x3E9, 2, 2, false, 1.0f,    0.0f,    U_NONE,    "Generic Sensor 10"),
  ROW(0x3E9, 4, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Target Lambda"),
  ROW(0x3E9, 7, 1, true,  1.0f,    0.0f,    U_NONE,    "Torque Mgmt Knob"),
  // ---- 0x3EA (50 Hz) ----
  ROW(0x3EA, 0, 2, false, 0.1f,   -101.3f,  U_KPA,     "Gearbox Line Pressure"),
  ROW(0x3EA, 2, 2, false, 0.1f,    0.0f,    U_PCT,     "Injection Stage 3 Duty"),
  ROW(0x3EA, 4, 2, false, 0.1f,    0.0f,    U_PCT,     "Injection Stage 4 Duty"),
  ROW(0x3EA, 6, 2, false, 0.1f,   -101.3f,  U_KPA,     "Crank Case Pressure"),
  // ---- 0x3EB (50 Hz) ----
  ROW(0x3EB, 0, 4, false, 1.0f,    0.0f,    U_MSEC,    "Race Timer"),
  ROW(0x3EB, 4, 2, true,  0.1f,    0.0f,    U_DEG,     "Ignition Angle Bank 1"),
  ROW(0x3EB, 6, 2, true,  0.1f,    0.0f,    U_DEG,     "Ignition Angle Bank 2"),
  // ---- 0x3EC / 0x3ED torque management (50 Hz) ----
  ROW(0x3EC, 0, 2, true,  1.0f,    0.0f,    U_RPM,     "TM Driveshaft Target"),
  ROW(0x3EC, 2, 2, true,  1.0f,    0.0f,    U_RPM,     "TM Driveshaft Error"),
  ROW(0x3EC, 4, 2, true,  0.1f,    0.0f,    U_DEG,     "TM Error Ign Correction"),
  ROW(0x3EC, 6, 2, true,  0.1f,    0.0f,    U_DEG,     "TM Timed Ign Correction"),
  ROW(0x3ED, 0, 2, true,  0.1f,    0.0f,    U_DEG,     "TM Combined Ign Corr"),
  // ---- 0x3EE / 0x3EF wideband 5-12 (20 Hz) ----
  ROW(0x3EE, 0, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Wideband Sensor 5"),
  ROW(0x3EE, 2, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Wideband Sensor 6"),
  ROW(0x3EE, 4, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Wideband Sensor 7"),
  ROW(0x3EE, 6, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Wideband Sensor 8"),
  ROW(0x3EF, 0, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Wideband Sensor 9"),
  ROW(0x3EF, 2, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Wideband Sensor 10"),
  ROW(0x3EF, 4, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Wideband Sensor 11"),
  ROW(0x3EF, 6, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Wideband Sensor 12"),
  // ---- 0x3F0 / 0x3F1 shock travel (50 Hz) ----
  ROW(0x3F0, 0, 2, false, 0.1f,    0.0f,    U_MM,      "Shock FL (Uncal)"),
  ROW(0x3F0, 2, 2, false, 0.1f,    0.0f,    U_MM,      "Shock FR (Uncal)"),
  ROW(0x3F0, 4, 2, false, 0.1f,    0.0f,    U_MM,      "Shock RL (Uncal)"),
  ROW(0x3F0, 6, 2, false, 0.1f,    0.0f,    U_MM,      "Shock RR (Uncal)"),
  ROW(0x3F1, 0, 2, true,  0.1f,    0.0f,    U_MM,      "Shock Travel FL"),
  ROW(0x3F1, 2, 2, true,  0.1f,    0.0f,    U_MM,      "Shock Travel FR"),
  ROW(0x3F1, 4, 2, true,  0.1f,    0.0f,    U_MM,      "Shock Travel RL"),
  ROW(0x3F1, 6, 2, true,  0.1f,    0.0f,    U_MM,      "Shock Travel RR"),
  // ---- 0x3F2 / 0x3F3 ride height (20 Hz) ----
  ROW(0x3F2, 0, 2, true,  0.1f,    0.0f,    U_MM,      "Ride Height Front"),
  ROW(0x3F2, 2, 2, true,  0.1f,    0.0f,    U_MM,      "Ride Height Front Uncal"),
  ROW(0x3F2, 4, 2, true,  0.1f,    0.0f,    U_MMPS,    "Ride Height Front Deriv"),
  ROW(0x3F3, 0, 2, true,  0.1f,    0.0f,    U_MM,      "Ride Height Rear"),
  ROW(0x3F3, 2, 2, true,  0.1f,    0.0f,    U_MM,      "Ride Height Rear Uncal"),
  ROW(0x3F3, 4, 2, true,  0.1f,    0.0f,    U_MMPS,    "Ride Height Rear Deriv"),
  // ---- 0x469 (5 Hz) ----
  ROW(0x469, 0, 2, false, 0.1f,    0.0f,    U_KELVIN,  "ECU Temperature"),
  ROW(0x469, 4, 2, false, 0.1f,    0.0f,    U_PCT,     "Oil Level"),
  // ---- 0x470 (20 Hz) ----
  ROW(0x470, 0, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Wideband Overall"),
  ROW(0x470, 2, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Wideband Bank 1"),
  ROW(0x470, 4, 2, false, 0.001f,  0.0f,    U_LAMBDA,  "Wideband Bank 2"),
  ROW(0x470, 6, 1, true,  1.0f,    0.0f,    U_ENUM,    "Gear Selector"),
  ROW(0x470, 7, 1, true,  1.0f,    0.0f,    U_ENUM,    "Gear"),
  // ---- 0x471 (50 Hz) ----
  ROW(0x471, 0, 2, true,  0.1f,    0.0f,    U_KPA,     "Injector Press Diff"),
  ROW(0x471, 2, 2, false, 0.1f,    0.0f,    U_PCT,     "Accelerator Pedal"),
  ROW(0x471, 4, 2, false, 0.1f,    0.0f,    U_KPA,     "Exhaust Manifold Press"),
  // ---- 0x472 cruise control (20 Hz) ----
  ROW(0x472, 0, 2, false, 0.1f,    0.0f,    U_KMH,     "Cruise Target Speed"),
  ROW(0x472, 2, 2, false, 0.1f,    0.0f,    U_KMH,     "Cruise Last Target"),
  ROW(0x472, 4, 2, true,  0.1f,    0.0f,    U_KMH,     "Cruise Speed Error"),
  // ---- 0x473 (10 Hz) ----
  ROW(0x473, 0, 4, false, 1.0f,    0.0f,    U_CC,      "Total Fuel Used"),
  // ---- 0x474 IMU (20 Hz) ----
  ROW(0x474, 0, 2, true,  0.1f,    0.0f,    U_MS2,     "Vertical G"),
  ROW(0x474, 2, 2, true,  0.1f,    0.0f,    U_DEGPS,   "Pitch Rate"),
  ROW(0x474, 4, 2, true,  0.1f,    0.0f,    U_DEGPS,   "Roll Rate"),
  ROW(0x474, 6, 2, true,  0.1f,    0.0f,    U_DEGPS,   "Yaw Rate"),
  // ---- 0x475 fuel pumps (5 Hz) ----
  ROW(0x475, 0, 2, false, 0.1f,    0.0f,    U_PCT,     "Primary Fuel Pump Duty"),
  ROW(0x475, 2, 2, false, 0.1f,    0.0f,    U_PCT,     "Aux 1 Fuel Pump Duty"),
  ROW(0x475, 4, 2, false, 0.1f,    0.0f,    U_PCT,     "Aux 2 Fuel Pump Duty"),
  ROW(0x475, 6, 2, false, 0.1f,    0.0f,    U_PCT,     "Aux 3 Fuel Pump Duty"),
  // ---- 0x476 brakes (20 Hz) ----
  ROW(0x476, 0, 2, false, 1.0f,   -101.3f,  U_KPA,     "Brake Pressure Rear"),
  ROW(0x476, 2, 2, false, 0.1f,    0.0f,    U_PCT,     "Brake Front Ratio"),
  ROW(0x476, 4, 2, false, 0.1f,    0.0f,    U_PCT,     "Brake Rear Ratio"),
  ROW(0x476, 6, 2, true,  1.0f,    0.0f,    U_KPA,     "Brake Press Difference"),
  // ---- 0x477 limiter (10 Hz) ----
  ROW(0x477, 0, 2, false, 1.0f,    0.0f,    U_RPM,     "Engine Limiter Max RPM"),
  ROW(0x477, 2, 2, false, 0.1f,    0.0f,    U_PCT,     "Cut Percentage"),
  // ---- 0x6F0-0x6F2 tyres (5 Hz) ----
  ROW(0x6F0, 0, 2, false, 0.1f,   -101.3f,  U_KPA,     "Tyre Pressure FL"),
  ROW(0x6F0, 2, 2, false, 0.1f,   -101.3f,  U_KPA,     "Tyre Pressure FR"),
  ROW(0x6F0, 4, 2, false, 0.1f,   -101.3f,  U_KPA,     "Tyre Pressure RL"),
  ROW(0x6F0, 6, 2, false, 0.1f,   -101.3f,  U_KPA,     "Tyre Pressure RR"),
  ROW(0x6F1, 0, 2, false, 0.1f,    0.0f,    U_KELVIN,  "Tyre Temp FL"),
  ROW(0x6F1, 2, 2, false, 0.1f,    0.0f,    U_KELVIN,  "Tyre Temp FR"),
  ROW(0x6F1, 4, 2, false, 0.1f,    0.0f,    U_KELVIN,  "Tyre Temp RL"),
  ROW(0x6F1, 6, 2, false, 0.1f,    0.0f,    U_KELVIN,  "Tyre Temp RR"),
  ROW(0x6F2, 0, 2, false, 0.001f,  0.0f,    U_VOLT,    "Tyre Sensor Batt FL"),
  ROW(0x6F2, 2, 2, false, 0.001f,  0.0f,    U_VOLT,    "Tyre Sensor Batt FR"),
  ROW(0x6F2, 4, 2, false, 0.001f,  0.0f,    U_VOLT,    "Tyre Sensor Batt RL"),
  ROW(0x6F2, 6, 2, false, 0.001f,  0.0f,    U_VOLT,    "Tyre Sensor Batt RR"),
  // ---- 0x6F3 (5 Hz) ----
  ROW(0x6F3, 0, 2, false, 0.1f,   -101.3f,  U_KPA,     "Front Tyre Rec Press"),
  ROW(0x6F3, 2, 2, false, 0.1f,   -101.3f,  U_KPA,     "Rear Tyre Rec Press"),
  ROW(0x6F3, 5, 1, false, 1.0f,    0.0f,    U_ENUM,    "Engine Protection Level"),
  ROW(0x6F3, 6, 2, false, 1.0f,    0.0f,    U_ENUM,    "Engine Protection Reason"),
  // ---- 0x6F4 (100 Hz) — lights + engine state ----
  BITS(0x6F4, 0, 0, 1,                      U_BOOL,    "Park Light"),
  BITS(0x6F4, 0, 1, 1,                      U_BOOL,    "Head Light"),
  BITS(0x6F4, 0, 2, 1,                      U_BOOL,    "High Beam"),
  BITS(0x6F4, 0, 3, 1,                      U_BOOL,    "Left Indicator"),
  BITS(0x6F4, 0, 4, 1,                      U_BOOL,    "Right Indicator"),
  BITS(0x6F4, 1, 3, 4,                      U_ENUM,    "Engine State"),
  // ---- 0x6F6 trip (5 Hz) ----
  ROW(0x6F6, 0, 4, true,  1.0f,    0.0f,    U_CC,      "Fuel Used Since Trip 1"),
  ROW(0x6F6, 4, 4, true,  1.0f,    0.0f,    U_METRE,   "Trip Meter 1"),
  // ---- 0x6F7 (10 Hz) ----
  ROW(0x6F7, 4, 2, false, 0.1f,    0.0f,    U_KELVIN,  "Calculated Air Temp"),
  ROW(0x6F7, 6, 2, false, 0.1f,    0.0f,    U_PCT,     "Water Inj Solenoid Duty"),
  // ---- 0x6F8 / 0x6F9 (5 Hz) ----
  ROW(0x6F8, 0, 1, true,  1.0f,    0.0f,    U_ENUM,    "Exhaust Cutout State"),
  ROW(0x6F8, 6, 2, false, 0.1f,    0.0f,    U_PCT,     "Fuel Level Pct 0"),
  ROW(0x6F9, 0, 2, false, 0.1f,    0.0f,    U_PCT,     "Fuel Level Pct 1"),
  // ---- 0x6FF misc pressures (20 Hz) ----
  ROW(0x6FF, 0, 2, true,  0.1f,   -101.3f,  U_KPA,     "Torque Converter Press"),
  ROW(0x6FF, 2, 2, true,  0.1f,   -101.3f,  U_KPA,     "Transfer Case Press"),
  ROW(0x6FF, 4, 2, true,  0.1f,   -101.3f,  U_KPA,     "Air Conditioner Press"),
  ROW(0x6FF, 6, 2, true,  0.01f,  -101.3f,  U_KPA,     "Power Steering Press"),
  // ---- 0x701 fuel economy (20 Hz) ----
  ROW(0x701, 0, 2, false, 0.1f,    0.0f,    U_L100KM,  "Fuel Economy Average"),
  ROW(0x701, 2, 2, false, 0.1f,    0.0f,    U_L100KM,  "Fuel Economy Instant"),
  ROW(0x701, 4, 2, true,  0.1f,    0.0f,    U_KELVIN,  "Air Conditioner Temp"),
};
const int HALTECH_CHANNEL_COUNT = sizeof(HALTECH_CHANNELS) / sizeof(HALTECH_CHANNELS[0]);

int chan_index_from_key(uint16_t key) {
  for (int i = 0; i < HALTECH_CHANNEL_COUNT; i++) {
    const HaltechChannel& c = HALTECH_CHANNELS[i];
    if (c.bit_start == 0xFF && CHAN_KEY(c.can_id, c.offset) == key) return i;
  }
  return -1;
}

uint16_t chan_key(int index) {
  if (index < 0 || index >= HALTECH_CHANNEL_COUNT) return 0;
  const HaltechChannel& c = HALTECH_CHANNELS[index];
  if (c.bit_start != 0xFF) return 0;   // bit channels are not key-addressable
  return CHAN_KEY(c.can_id, c.offset);
}

// ---------------- display units ----------------
bool units_press_psi = true;    // matches the gauge's historical psi display
bool units_temp_f = false;      // °C
bool units_speed_mph = false;   // km/h
bool units_lambda_afr = true;   // matches the gauge's historical AFR display

float chan_display(int index, float v) {
  if (index < 0 || index >= HALTECH_CHANNEL_COUNT) return v;
  switch (HALTECH_CHANNELS[index].unit) {
    case U_KPA:      return units_press_psi ? v * 0.145038f : v;
    case U_KPA_ABS:  return units_press_psi ? v * 0.145038f : v;
    case U_KELVIN:   return units_temp_f ? (v - 273.15f) * 1.8f + 32.0f : v - 273.15f;
    case U_KMH:      return units_speed_mph ? v * 0.621371f : v;
    case U_LAMBDA:   return units_lambda_afr ? v * 14.7f : v;
    default:         return v;
  }
}

const char* chan_unit_str(int index) {
  if (index < 0 || index >= HALTECH_CHANNEL_COUNT) return "";
  switch (HALTECH_CHANNELS[index].unit) {
    case U_RPM:     return "RPM";
    case U_KPA:     return units_press_psi ? "psi" : "kPa";
    case U_KPA_ABS: return units_press_psi ? "psi(a)" : "kPa(a)";
    case U_PCT:     return "%";
    case U_KELVIN:  return units_temp_f ? "\xC2\xB0""F" : "\xC2\xB0""C";
    case U_LAMBDA:  return units_lambda_afr ? "AFR" : "\xCE\xBB";
    case U_KMH:     return units_speed_mph ? "mph" : "km/h";
    case U_MS2:     return "m/s\xC2\xB2";
    case U_VOLT:    return "V";
    case U_MSEC:    return "ms";
    case U_DEG:     return "\xC2\xB0";
    case U_DEGPS:   return "\xC2\xB0/s";
    case U_DB:      return "dB";
    case U_CCPM:    return "cc/min";
    case U_LITRE:   return "L";
    case U_KML:     return "km/L";
    case U_L100KM:  return "L/100km";
    case U_PPM:     return "ppm";
    case U_GM3:     return "g/m\xC2\xB3";
    case U_MM:      return "mm";
    case U_MMPS:    return "mm/s";
    case U_SEC:     return "s";
    case U_CC:      return "cc";
    case U_METRE:   return "m";
    default:        return "";
  }
}
