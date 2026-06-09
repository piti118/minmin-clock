# CYD NTP Clock

A feature-rich NTP-synced clock for the **ESP32-2432S028** ("Cheap Yellow Display").  
Designed to be readable at a glance — especially for kids.

---

## Hardware

| Component | Details |
|---|---|
| Board | ESP32-2432S028 (ESP32 + ILI9341 320×240 TFT) |
| Display | ILI9341, landscape, HSPI bus |
| Touch | XPT2046, VSPI bus (separate from display) |
| Backlight | GPIO 21, LEDC PWM |

---

## Features

### Clock face
- **Large time** — bold rounded font (`FreeSansBold24pt7b` at 2× scale), centred on screen
- **Rolling digit animation** — when the minute changes, only the digits that actually changed roll upward; unchanged digits stay still
- **Day of week** — shown at the top in yellow
- **Date** — shown below the time

### Schedule messages for kids
The top row automatically switches to a coloured message based on the time of day:

| Time window | Message | Colour |
|---|---|---|
| Wake up → +45 min | "Good morning, Minmin!" | Yellow |
| Story time − 20 min → Story time | "Bedtime soon, Minmin!" | Orange |
| Story time → Lights out | "Story time, Minmin! <3" | Pink |
| Lights out → +30 min | "Sweet dreams, Minmin!" | Cyan |
| All other times | Day of week | Yellow |

All three schedule times (story time, lights out, wake up) are configurable via the portal.  
Defaults: story time 21:00, lights out 22:00, wake up 07:15.

### Animation strip (bottom of screen)
- A rainbow smiley ball bounces across the bottom bar, completing one pass every 60 seconds — acts as a seconds indicator
- A rainbow dotted trail follows behind the ball
- Four twinkling rainbow stars sit in the screen corners

### Touch brightness control
The screen is divided into three invisible tap zones:

| Zone | Action |
|---|---|
| Left third | Decrease brightness (−20 per 300 ms, minimum 10) |
| Right third | Increase brightness (+20 per 300 ms, maximum 255) |
| Centre (hold 5 s) | Re-run touch calibration |

A brightness percentage overlay appears in the top row for 1.5 s after each adjustment.  
The new brightness level is saved to flash 3 seconds after the last tap.

---

## First-time Setup

1. **Flash** the firmware using PlatformIO (`Upload` task or `platformio run --target upload`).
2. On first boot the **touch calibration** screen appears automatically.  
   Tap the two cross-hairs with a stylus (or fingertip). Calibration data is saved to flash.
3. The **WiFi setup portal** opens next.  
   On your phone or computer, join the Wi-Fi network **`CYD-Clock`** (password: **`esp32clock`**), then open **`192.168.4.1`** in a browser.
4. Fill in the fields and click **Save**. The clock restarts, syncs time, and starts running.

---

## WiFi Portal Settings

| Field | Description | Example |
|---|---|---|
| NTP Server | Time server hostname | `pool.ntp.org` |
| Timezone (POSIX) | Standard POSIX timezone string | see table below |
| Brightness | Backlight level 0–255 | `200` |
| Minmin story time | Story / bedtime start | `21:00` |
| Minmin lights out | Sleep / lights-out time | `22:00` |
| Minmin wake up | Wake-up time | `07:15` |

### POSIX timezone string examples

| Location | String |
|---|---|
| UTC | `UTC0` |
| Bangkok / ICT | `ICT-7` |
| Tokyo | `JST-9` |
| Singapore | `SGT-8` |
| US Eastern (DST) | `EST5EDT,M3.2.0,M11.1.0` |
| US Pacific (DST) | `PST8PDT,M3.2.0,M11.1.0` |
| UK (BST) | `GMT0BST,M3.5.0/1,M10.5.0` |
| Central Europe (CET/CEST) | `CET-1CEST,M3.5.0,M10.5.0/3` |

---

## Re-entering Setup

| How | When |
|---|---|
| Hold **BOOT button** at power-on | Force portal on every boot |
| Hold **BOOT button 3 s** during normal run | Open portal without rebooting |
| Hold **centre of screen 5 s** | Re-run touch calibration only |

---

## Dependencies

Managed automatically by PlatformIO:

| Library | Version |
|---|---|
| `bodmer/TFT_eSPI` | `^2.5.33` |
| `tzapu/WiFiManager` | `^2.0.17` |
| `PaulStoffregen/XPT2046_Touchscreen` | latest (GitHub) |

> **Note:** The XPT2046 library must be pulled from GitHub (`https://github.com/PaulStoffregen/XPT2046_Touchscreen.git`). The registry version is too old and lacks `begin(SPIClass&)` which is required for the separate VSPI bus.

---

## Power Saving

The firmware applies several measures to reduce idle current draw:

- **Bluetooth disabled** at boot (never needed) — saves ~25–35 mA
- **CPU runs at 80 MHz** (down from 240 MHz) — saves ~20–30 mA
- **WiFi MAX_MODEM sleep** between beacon intervals
- **NTP resync every 6 hours** (RTC drift is < 1 s/h, so error stays under 6 s)

The backlight is the largest consumer. Reducing brightness via touch is the most effective way to lower total power use.

---

## Build & Flash

```bash
# Build only
platformio run --environment cyd

# Build and upload
platformio run --target upload --environment cyd

# Serial monitor (115200 baud)
platformio device monitor --environment cyd
```

Or use the **Build (cyd)** and **Upload (cyd)** tasks in VS Code.
