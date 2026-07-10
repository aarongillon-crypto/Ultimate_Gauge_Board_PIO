# Bluetooth LE Integration Guide

## Overview
This branch adds Bluetooth Low Energy (BLE) support to the Ultimate Gauge Board, enabling iOS (and Android) apps to connect, monitor gauge data in real-time, and configure settings.

## BLE Device Name
The gauge advertises as: `Haltech-XXXX` where XXXX is the last 2 bytes of the MAC address.

## BLE Services & Characteristics

### Gauge Data Service
**Service UUID:** `12340000-1234-1234-1234-123456789abc`

Read-only/Notify characteristics for real-time gauge monitoring:

| Characteristic | UUID | Type | Description |
|---------------|------|------|-------------|
| Current Value | `12340010-...` | Float (4 bytes) | Current gauge reading (PSI, AFR, °C, etc.) |
| Gauge Mode | `12340011-...` | UInt8 (1 byte) | Current mode: 0=Boost, 1=AFR, 2=Water, 3=Oil |
| Peak Value | `12340012-...` | Float (4 bytes) | Peak hold value |
| RPM | `12340013-...` | Int16 (2 bytes) | Engine RPM |

### Configuration Service
**Service UUID:** `12340001-1234-1234-1234-123456789abc`

Read/Write characteristics for gauge configuration:

| Characteristic | UUID | Type | Description |
|---------------|------|------|-------------|
| Mode Setting | `12340020-...` | UInt8 | Write to change mode (0-3) - triggers reboot |
| Text Color | `12340021-...` | 4× UInt32 | Write 16 bytes: text, low, mid, high colors |
| Brightness | `12340025-...` | UInt8 | Brightness level (10-100) |
| Peak Hold | `12340026-...` | UInt8 | Enable/disable peak hold (0=off, 1=on) |

## Color Format
Colors are sent as 32-bit RGB values (big-endian):
- Example: `0xFF6600` (orange) = bytes `[0xFF, 0x66, 0x00, 0x00]`

## Testing with iOS

### Option 1: LightBlue App (Quick Test)
1. Download "LightBlue" from the App Store (free BLE scanner)
2. Build and upload the firmware to your ESP32-S3
3. Open LightBlue and scan for devices
4. Connect to "Haltech-XXXX"
5. You should see two services listed
6. Click "Gauge Data Service" to see live readings
7. Enable notifications on characteristics to see real-time updates
8. Click "Configuration Service" to test writing values

### Option 2: nRF Connect (Advanced Testing)
1. Download "nRF Connect for Mobile" (free, more features)
2. Connect to your gauge
3. Enable notifications on gauge data characteristics
4. View live data updates at ~30Hz
5. Test writing configuration values

## Data Update Rate
- BLE characteristics are updated every 33ms (~30 Hz) when a client is connected
- Updates only sent when BLE client is connected (saves power/resources)

## Memory Considerations
- BLE stack uses ~40-60KB of heap memory
- WiFi and BLE run simultaneously (coexistence enabled)
- Monitor heap usage if experiencing stability issues

## Building an iOS App

### Recommended Technologies:
1. **SwiftUI + CoreBluetooth** (Native iOS)
   - Best performance and iOS integration
   - Example code structure provided below

2. **Flutter + flutter_blue_plus** (Cross-platform)
   - Single codebase for iOS and Android
   - Good documentation and community support

### Basic iOS App Structure (SwiftUI):
```swift
import SwiftUI
import CoreBluetooth

class BLEManager: NSObject, ObservableObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    @Published var gaugeValue: Float = 0.0
    @Published var gaugeMode: UInt8 = 0
    @Published var isConnected = false

    var centralManager: CBCentralManager!
    var gaugePeripheral: CBPeripheral?

    let gaugeDataServiceUUID = CBUUID(string: "12340000-1234-1234-1234-123456789abc")
    let currentValueCharUUID = CBUUID(string: "12340010-1234-1234-1234-123456789abc")

    // Implement CBCentralManagerDelegate methods...
    // Implement CBPeripheralDelegate methods...
}

struct GaugeView: View {
    @StateObject var bleManager = BLEManager()

    var body: some View {
        VStack {
            Text("\(bleManager.gaugeValue, specifier: "%.1f")")
                .font(.system(size: 72))
            Text(modeName(bleManager.gaugeMode))
                .font(.title)
        }
    }
}
```

## Next Steps
1. ✅ Build and upload firmware to ESP32-S3
2. ✅ Test BLE connectivity with LightBlue app
3. ✅ Verify data updates and configuration writes
4. 🔲 Build iOS app using SwiftUI or Flutter
5. 🔲 Add data logging and graphing features
6. 🔲 Implement alert notifications for threshold values

## Troubleshooting

### Can't see BLE device on iPhone
- Make sure Bluetooth is enabled
- Check that firmware uploaded successfully
- Device advertises as "Haltech-XXXX"
- Try restarting the ESP32-S3

### Connection drops frequently
- Check heap memory usage (add debugging)
- Reduce WiFi activity if possible
- Increase BLE connection interval

### Notifications not working
- Ensure you've subscribed to notifications in your app
- Check that BLE is connected before sending updates
- Verify characteristic has NOTIFY property enabled

## Performance Notes
- BLE coexists with WiFi on ESP32-S3
- Both can run simultaneously without major issues
- If experiencing performance problems, consider:
  - Disabling WiFi AP when BLE is active
  - Reducing BLE update rate
  - Optimizing LVGL rendering

## Future Enhancements
- [ ] Add configuration for BLE enable/disable (save power)
- [ ] Implement data logging service
- [ ] Add OTA firmware update via BLE
- [ ] Multi-device pairing/bonding
- [ ] BLE passkey security
