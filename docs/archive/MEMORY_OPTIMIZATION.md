# Memory Optimization Guide

## The Problem
ESP32-S3 runs out of heap memory when trying to run WiFi + LVGL + BLE simultaneously. The error `ESP_ERR_NO_MEM (0x101)` occurs during BLE initialization.

## Memory Usage Breakdown
- **WiFi Stack**: ~40-50KB
- **BLE Stack**: ~40-60KB
- **LVGL**: 128KB (configured in lv_conf.h)
- **Display Buffers**: Additional memory
- **Application Code**: Variables, stack, etc.

## Solutions Implemented

### 1. Initialize BLE Before WiFi
BLE now initializes first, ensuring it can claim memory before WiFi:
```cpp
// BLE first (in setup)
ble_init(bleName);
// Then WiFi
setup_wifi();
```

### 2. Reduced BLE Memory Footprint
Added optimization flags in `platformio.ini`:
- `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1` - Single connection only
- `CONFIG_BT_NIMBLE_MAX_BONDS=1` - Minimal bonding
- `CONFIG_BT_NIMBLE_MAX_CCCDS=8` - Reduced descriptors
- `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256` - Smaller MTU

### 3. Graceful Error Handling
BLE initialization now fails gracefully if memory is unavailable:
- Returns `false` on failure
- Device continues to work with WiFi only
- Helpful debug messages printed

## Monitor Memory Usage

Add this to your code to track heap:
```cpp
Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
Serial.printf("Largest free block: %d bytes\n", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
```

## If You Still Have Memory Issues

### Option 1: Reduce LVGL Memory
Edit `include/lv_conf.h` line 72:
```c
// Reduce from 128KB to 64KB or less
#define LV_MEM_SIZE (64 * 1024U)
```

### Option 2: Use External PSRAM
ESP32-S3 has 8MB of PSRAM. You can move LVGL buffers there:

In `lv_conf.h`:
```c
#define LV_MEM_ADR 0     // Keep as 0 to use PSRAM
```

Add to `platformio.ini`:
```ini
board_build.arduino.memory_type = qio_opi  # Already set
board_build.partitions = default_8MB.csv   # Already set
```

### Option 3: Make WiFi and BLE Mutually Exclusive
Disable WiFi when BLE is active:
```cpp
// When BLE client connects, disable WiFi AP
void onBLEConnect() {
    WiFi.softAPdisconnect(true);
    esp_wifi_stop();
}

// When BLE disconnects, re-enable WiFi
void onBLEDisconnect() {
    setup_wifi();
}
```

### Option 4: Use WiFi in Station Mode Only
Instead of AP mode, connect to existing WiFi (uses less memory):
```cpp
WiFi.begin("your-ssid", "your-password");
// No WiFi.softAP() call
```

## Current Status

After the optimizations:
- ✅ BLE initializes before WiFi
- ✅ BLE memory usage reduced
- ✅ Graceful fallback if BLE fails
- ✅ Memory diagnostics added

Try building and uploading again. Check the Serial Monitor for:
```
Free heap before BLE init: XXXX bytes
BLE Initialized and Advertising
Free heap after BLE init: XXXX bytes
```

If BLE still fails to initialize, you'll see:
```
BLE initialization failed - continuing without BLE support
```

And the device will work normally with just WiFi.

## Recommended Next Steps

1. **Build and test** with current optimizations
2. **Check Serial Monitor** for heap statistics
3. **If BLE works**: Great! Test with iOS app
4. **If BLE still fails**: Try Option 1 (reduce LVGL memory) or Option 3 (mutual exclusion)

## Memory Debugging Commands

Use these in Serial Monitor (add to code):
```cpp
void printMemoryStats() {
    Serial.printf("=== Memory Stats ===\n");
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("Min free heap: %d bytes\n", ESP.getMinFreeHeap());
    Serial.printf("Heap size: %d bytes\n", ESP.getHeapSize());
    Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
    Serial.printf("PSRAM size: %d bytes\n", ESP.getPsramSize());
}
```

Call this:
- Before any initialization
- After LVGL init
- After BLE init
- After WiFi init
- Periodically in loop

This will show exactly where memory is being consumed.
