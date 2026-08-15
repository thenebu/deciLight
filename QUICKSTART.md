# Quick Start Guide - noiselight 🚦

## 1️⃣ Hardware Checklist

- [ ] **ESP32-S3 DevKit** 
- [ ] **I2S MEMS Microphone** (INMP441 or INMP408)
- [ ] **WS2812 NeoPixel LED strip** (12x or more)
- [ ] **5V USB Power Supply** (2A minimum)
- [ ] **Micro USB cable** (programming)
- [ ] **Jumper wires** with proper gauge (AWG 22-24)

## 2️⃣ Wiring

### Microphone → ESP32

| Microphone | ESP32 | Wire Color |
|-----------|-------|-----------|
| VDD (3.3V) | 3.3V | - |
| GND | GND | - |
| WS (L/R) | GPIO 4 | Green |
| SCK (BCLK) | GPIO 6 | White |
| SD (SDI) | GPIO 7 | Yellow |

### NeoPixel → ESP32

| LED Strip | ESP32 | Wire Color |
|-----------|-------|-----------|
| 5V | 5V | Red |
| GND | GND | Black |
| DI (Data) | GPIO 2 | Green |

## 3️⃣ Software Setup

### Install PlatformIO (VS Code)

```bash
# In VS Code: 
# Ctrl+Shift+X → Search "platformio" → Install
```

### Build Project

```bash
# In terminal:
cd ~/Documents/PROJECTEN/noise-light/FW/esp32-noise-light
pio run -e esp32-s3-devkitc1-n4r2
```

### Upload to ESP32

```bash
pio run -t upload -e esp32-s3-devkitc1-n4r2
```

### Monitor Serial Output

```bash
pio device monitor -b 115200
```

## 4️⃣ Testing

### Power On
- LED strip should flash **black** on startup
- Serial monitor should show "NOISE TRAFFIC LIGHT STARTING"

### Test Microphone

Snap fingers near microphone:
- If working: dB value should jump in serial output
- Color should change: GREEN → YELLOW → RED

### Test Thresholds

**Default:**
- GREEN: < 40 dB
- YELLOW: 40-60 dB  
- RED: > 60 dB

**Try:**
1. Silent room → Should be GREEN
2. Normal talking → Should be YELLOW
3. Loud talking/clapping → Should be RED

## 5️⃣ Troubleshooting

### "error: Namespace 'i2s_mode_t' has no member named 'I2S_MODE_MASTER'"

**Fix:** Update ESP32 Arduino core:
```bash
pio update
```

### Microphone not detected

Check:
- [ ] Correct pins: GPIO 4 (WS), 6 (SCK), 7 (SD)
- [ ] Microphone powered (3.3V)
- [ ] Secure connections (no loose wires)
- [ ] Microphone polarity correct (VDD≠GND)

### LED not responding

Check:
- [ ] GPIO 2 connected to LED DI
- [ ] 5V power supplied to LEDs
- [ ] Correct LED count (12) in code
- [ ] Try adjusting brightness in code

### Serial garbage/no output

Set baud rate to **115200** in monitor settings.

## 6️⃣ Customization

### Change Thresholds

Edit `main.cpp`:
```cpp
int dB_min_default = 40;  // Change to 35, 45, etc.
int dB_max_default = 60;  // Change to 55, 70, etc.
```

### Change LED Count

Edit `main.cpp`:
```cpp
#define NUM_LEDS 12  // Change if your strip has different count
```

### Change Colors

Edit traffic light logic in `main.cpp`:
```cpp
if (Leq_dB < dB_min) {
  fill_solid(leds, NUM_LEDS, CRGB::Lime);    // Change GREEN
  Serial.println("QUIET");
}
```

Available colors: `Green`, `Red`, `Yellow`, `Blue`, `White`, `Orange`, `Cyan`, `Purple`, `Lime`, `Pink`

## 7️⃣ Calibration for Accuracy

### Measure Reference Noise

Use phone app (e.g., "Decibel Meter"):
1. Measure known sound level
2. Read dB value from serial monitor
3. Compare: expected vs actual

### Adjust Calibration

If off by ~5 dB, modify in `main.cpp`:
```cpp
#define MIC_OFFSET_DB 3.0103  // Increase or decrease
```

Example: If reading is 5 dB too low:
```cpp
#define MIC_OFFSET_DB 8.0103  // +5.0 offset
```

## 📚 Next Steps

- [ ] Test with different environments (classroom, workshop, etc.)
- [ ] Find optimal thresholds for your use case
- [ ] Create fixtures/enclosure for LED strip
- [ ] Add WiFi monitoring (future enhancement)
- [ ] Set up multiple units in classroom

## 🆘 Get Help

1. Check serial output for errors
2. Review README.md in project root
3. Check deciLight original: https://github.com/bbbenji/deciLight
4. ESP32 docs: https://docs.espressif.com/
