# Noise Traffic Light 🚦

A real-time noise monitor that displays sound levels as a traffic light using RGB LEDs.

## 📋 Features

- **Real-time noise detection** via I2S MEMS microphone
- **3-color traffic light display**:
  - 🟢 **GREEN** – Noise is low (quieter than threshold)
  - 🟡 **YELLOW** – Noise is moderate (within range)
  - 🔴 **RED** – Noise is high (exceeded threshold)
- **12 NeoPixel RGB LED strip** for bright, visible feedback
- **A-weighted sound level measurement** (dBA) for realistic perception
- **Configurable thresholds** via persistent storage
- **WiFi client mode** with automatic AP fallback for first-time setup
- **MQTT / Home Assistant integration** with MQTT discovery
- **Over-the-air (OTA) firmware updates** once connected to your home network
- **Config export/import** for backing up or cloning device settings
- **5-minute history graph** in the web UI

## 🔧 Hardware Setup

### Pin Configuration

| Component | Pin | Color | Description |
|-----------|-----|-------|-------------|
| **WS2812 LED Data** | GPIO 2 | - | NeoPixel strip (12 LEDs) |
| **I2S L/R Select** | GPIO 4 | Green | Microphone channel select (set HIGH = RIGHT channel) |
| **I2S Word Select** | GPIO 5 | Blue | Microphone L/R clock |
| **I2S Serial Clock** | GPIO 6 | White | Microphone bit clock |
| **I2S Serial Data** | GPIO 7 | Yellow | Microphone audio data |
| **Power (5V)** | 5V | - | USB power supply |
| **GND** | GND | - | Ground |

### Wiring Diagram

```
ESP32                          I2S Microphone (e.g., INMP441)
───────                        ──────────────────
GPIO 4 (green, HIGH)───────────→ L/R (channel select = RIGHT)
GPIO 5 (blue) ─────────────────→ WS (L/R Clock)
GPIO 6 (white) ─────────────────→ SCK (Bit Clock)
GPIO 7 (yellow)─────────────────→ SD (Serial Data)
GND ───────────────────────────→ GND
3V3 ───────────────────────────→ VDD (if 3.3V version)
```

```
ESP32                          NeoPixel Strip (WS2812)
───────                        ──────────────────
GPIO 2 ────────────────────────→ DI (Data In)
5V ────────────────────────────→ 5V
GND ───────────────────────────→ GND
```

## 📊 How It Works

### Audio Processing Pipeline

1. **I2S Sampling** (48 kHz, 32-bit)
   - Captures continuous audio from the microphone
   - High-precision digital sampling

2. **IIR Filtering**
   - **Equalizer filter** (INMP441): Flattens microphone frequency response
   - **A-weighting filter**: Mimics human ear sensitivity (emphasizes mid-frequencies)

3. **Sound Level Calculation**
   - Converts filtered samples to dB (decibels)
   - Uses microphone calibration: 94 dB SPL reference
   - LEQ (Equivalent Continuous Sound Level) over 0.15 second blocks

4. **Decision Logic**
   ```
   if (Leq < dB_min)     → GREEN   (quiet)
   if (dB_min ≤ Leq < dB_max) → YELLOW  (moderate)
   if (Leq ≥ dB_max)     → RED     (loud)
   ```

## 🎚️ Configuration

### Default Thresholds

```cpp
dB_min_default = 40   // Below 40 dB → GREEN
dB_max_default = 60   // Above 60 dB → RED
```

### Recommended Settings

| Environment | dB_min | dB_max | Notes |
|------------|--------|--------|-------|
| Library/Silent | 30 | 50 | Very quiet spaces |
| Classroom | 40 | 60 | Normal teaching |
| Active classroom | 45 | 70 | Group work sessions |
| Workshop | 60 | 80 | Tolerate machinery |

## 🌐 WiFi & Web Configuration

### WiFi Modes

The device connects to your home network as a **WiFi client (STA)**. If no network is
configured yet, or the configured network can't be reached at boot, it falls back to
its own **Access Point (AP)** so you can always reach the web UI:

- **SSID (Network Name):** `NoiseLight`
- **Password:** `12345678`
- **IP Address:** `192.168.4.1`
- **Port:** `80` (HTTP)

Once connected to your home network, the device is also reachable via mDNS at
**`http://noiselight.local`**.

### First-Boot WiFi via .env (optional)

If your phone/laptop can't see or join the `NoiseLight` AP, or you'd rather skip that
step entirely, copy `.env.example` to `.env` (gitignored, never committed) and fill in
your home WiFi credentials:

```bash
cp .env.example .env
# edit .env: WIFI_SSID=..., WIFI_PASSWORD=...
platformio run -t upload -e esp32-s3-devkitc1-n4r2
```

`load_env.py` bakes these in as the NVS default a freshly flashed device tries on its
very first boot — it only matters until the device's WiFi settings are saved once via
the web UI (or already exist in NVS from a previous flash), at which point the
NVS-stored value always wins over the `.env` default.

### Setup Instructions

1. **First-time connection (AP fallback):**
   - Open your device's WiFi settings (phone, tablet, laptop)
   - Select network `NoiseLight`
   - Enter password: `12345678`

2. **Open the configuration page:**
   - Open browser and go to: `http://192.168.4.1`
   - The web interface will load automatically

3. **Join your home WiFi:**
   - Enter your network's SSID/password in the **Network** section of the web UI and save
   - The device reconnects using your credentials; if successful, it drops the AP and is
     reachable at `http://noiselight.local` (or its new DHCP address) going forward
   - If the connection fails, the device falls back to AP mode again so you're never locked out

4. **Configure settings in real-time:**
   - Live dB level display updates every 200ms
   - All sliders update preview instantly
   - Press **Save Configuration** to persist settings to device storage

### Web Interface Features

![Noise Light Web Interface](./doc/webUI.jpeg)

**Display Mode**
- **Traffic Light:** All LEDs show single color (GREEN/YELLOW/RED)
- **VU Meter:** LEDs create gradient bar showing sound intensity

**LED Settings**
- **Brightness:** 0–255 (default: 57)
- **Color Selection:** Choose custom colors for each noise level

**Switchover Points (dB)**
- **Floor Level:** Noise floor baseline (default: 37 dB)
- **Green→Yellow:** Transition threshold (default: 52 dB)
- **Yellow→Red:** Alert threshold (default: 62 dB)
- **Color Preview:** Visual bar shows LED colors across dB range

**Response Timing**
- **Decay Time:** How long to hold current color after sound stops (0–3000 ms, default: 2400 ms)
- **Response Time:** Minimum update interval between LED changes (0–500 ms, default: 50 ms)

**History Graph**
- Live line chart of the last 5 minutes of dB readings (1 sample/sec), served from `/api/history`

**Tagesstatistik (Daily Time Distribution)**
- Stacked bar chart showing how much of each hour (today, 24 buckets) was spent in each
  color state (NORMAL/WARNING/ALERT), served from `/api/hourly`
- Auto-resets at local midnight; a **Zurücksetzen** (Reset) button also clears it manually
  via `/api/hourly/reset`
- Needs the device's clock to be synced via NTP first (see **Time Zone** below) — until
  then the chart shows a note instead of data
- Bucketing uses the raw (non-decayed) noise classification each loop tick, the same
  classification MQTT publishes as its "level" sensor — not the display-smoothed color,
  which would understate quiet periods

**Time Zone**
- The **Network** section has a **Zeitzone (POSIX TZ)** field, e.g.
  `CET-1CEST,M3.5.0,M10.5.0/3` (Europe/Berlin, the default) or `EST5EDT,M3.2.0,M11.1.0`
  (US Eastern) — passed straight to the ESP32's `configTzTime()`, so the Tagesstatistik
  hour buckets line up with your local wall clock, DST transitions included
- Unlike a fixed UTC offset, a POSIX TZ string encodes *when* the clock shifts (the
  `M3.5.0,M10.5.0/3` part), so summer/winter time is handled automatically — no manual
  adjustment twice a year
- Requires an internet connection (NTP sync against `pool.ntp.org`/`time.nist.gov`); doesn't
  apply while running in AP-fallback mode with no internet access

### Persistent Storage

All configuration changes are automatically saved to ESP32's **NVS (Non-Volatile Storage)**:
- Settings survive power loss
- Load automatically on device startup
- Config accessible via `/api/config`, WiFi/MQTT settings via `/api/network`

### Config Export/Import

The web UI can export the full device configuration (LED/threshold settings **and**
WiFi/MQTT credentials) as a JSON file via `/api/config/export`, and restore it on
another device (or after a reset) via `/api/config/import`. Handy for backing up a
working setup or cloning it to a second unit.

> ⚠️ The exported file contains your WiFi and MQTT passwords in plain text — store it
> like any other credential file.

## 🏠 MQTT / Home Assistant Integration

Once connected to your home network, the device can publish its noise readings to an
MQTT broker and announce itself to **Home Assistant** via MQTT discovery — no manual
entity configuration needed.

### Setup

1. In the web UI's **Network** section, enter your MQTT broker's host/port and,
   if required, username/password, then save.
2. The device connects automatically and publishes a Home Assistant discovery payload
   on `homeassistant/sensor/<device-id>/...`, exposing two entities:
   - **Noise Level** – the current dB reading
   - **Noise Level Status** – `normal` / `warning` / `alert`
3. State is published every ~2 seconds to `noiselight/<device-id>/state`, with an
   availability topic (`.../availability`) so Home Assistant marks the device offline
   if the connection drops.

The device ID is derived from the last 6 hex digits of its MAC address (e.g.
`noiselight-a1b2c3`).

## 📡 OTA (Over-the-Air) Updates

Once the device is connected to your home network (STA mode), firmware can be updated
over WiFi instead of USB, using the dedicated `esp32-s3-devkitc1-n4r2-ota` PlatformIO
environment:

```bash
platformio run -t upload -e esp32-s3-devkitc1-n4r2-ota
```

This uploads to `noiselight.local` and requires the OTA password configured in
`include/config.h` (`OTA_PASSWORD`, must match `upload_flags` in `platformio.ini`).
**Change the default password before deploying to your home network** — anyone on the
same network can otherwise attempt an OTA flash. OTA is only enabled after a successful
WiFi STA connection; it's never exposed while the device is on the AP fallback.

## 🚀 Building & Uploading

### Prerequisites

- PlatformIO (VS Code extension or CLI)
- Arduino IDE (optional, for direct compilation)

### Build

```bash
# Using PlatformIO
platformio run -e esp32-s3-devkitc1-n4r2

# Or in VS Code: Ctrl+Shift+B → Build
```

### Upload

```bash
# Using PlatformIO (USB)
platformio run -t upload -e esp32-s3-devkitc1-n4r2

# Or over WiFi, once on your home network (see OTA section above)
platformio run -t upload -e esp32-s3-devkitc1-n4r2-ota

# Or in VS Code: Ctrl+Shift+B → Upload
```

## 📈 Serial Output

The device outputs dB measurements to the serial monitor (115200 baud):

```
Leq: 45.3 dB | Min: 40 | Max: 60 | GREEN
Leq: 62.1 dB | Min: 40 | Max: 60 | RED
Leq: 55.2 dB | Min: 40 | Max: 60 | YELLOW
```

## 🔧 Calibration

### Microphone Sensitivity

If readings are consistently off:

1. **Measure known sound level** (e.g., using phone app)
2. **Compare with serial output**
3. **Adjust MIC_OFFSET_DB** in `main.cpp`:
   ```cpp
   #define MIC_OFFSET_DB 3.0103  // Increase/decrease calibration offset
   ```

### Optimal Setup

- Mount microphone in **center of room**
- **Avoid** placing near walls or corners (reflections)
- **Keep away** from vibration sources
- Ensure **clear air path** (not covered)

## 🔴 Troubleshooting

| Issue | Cause | Solution |
|-------|-------|----------|
| Always GREEN | Microphone not connected | Check I2S pins (GPIO 4,6,7) |
| Always RED | Calibration off | Adjust MIC_OFFSET_DB |
| No serial output | Wrong baud rate | Set to 115200 |
| No LED response | Wrong LED pin | Verify GPIO 2 connection |
| Microphone noise floor high | Electrical noise | Shield data lines, check power supply |
| Device stuck on `NoiseLight` AP | Home WiFi unreachable/wrong password | Check credentials in the web UI, or reconnect to the AP and re-enter them |
| MQTT entities not appearing in Home Assistant | Broker unreachable or discovery not yet sent | Verify host/port/credentials in Network settings; discovery re-sends on every reconnect |
| OTA upload fails/times out | Device on AP fallback, or wrong `--auth` password | Confirm device is on your home network (`noiselight.local` resolves) and `OTA_PASSWORD` matches `platformio.ini` |

## 📚 References

- **Original deciLight Project**: https://github.com/bbbenji/deciLight
- **FastLED Documentation**: http://fastled.io/
- **ESP-IDF I2S API**: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html
- **A-weighting Filter**: https://en.wikipedia.org/wiki/A-weighting
- **PubSubClient (MQTT)**: https://github.com/knolleary/pubsubclient
- **Home Assistant MQTT Discovery**: https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery

## 📝 License

This project is adapted from [deciLight](https://github.com/bbbenji/deciLight) (GPL-3.0).
Licensed under GNU General Public License v3.0.

## 🎨 Future Enhancements

- [ ] IR remote control for threshold adjustment
- [ ] Multiple noise zones (classroom network)
- [ ] Adjustable color mapping
- [ ] Multi-day history for the Tagesstatistik view (currently single-day, resets at midnight)
