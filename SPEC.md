# CYD NTP Clock — Specification

## Hardware

| Item | Detail |
|---|---|
| Board | ESP32-2432S028 (2 USB variant — Micro-USB + USB-C) |
| Display | ILI9341 320×240 TFT, landscape orientation (rotation 1) |
| Library | TFT_eSPI via PlatformIO (`bodmer/TFT_eSPI@^2.5.33`) |
| Backlight | GPIO 21, active HIGH, PWM via `ledcWrite` (channel 0, 5 kHz, 8-bit) |
| Boot button | GPIO 0, pulled up (LOW = pressed) |
| SPI bus | HSPI — MISO 12, MOSI 13, SCLK 14, CS 15, DC 2 |

## Features

### 1. WiFi / Configuration Portal
- Uses `WiFiManager` (tzapu, `^2.0.17`).
- **First boot** (no saved credentials): portal opens automatically.
- **Force portal**: hold BOOT button (GPIO 0) at power-on, OR hold it for 3 seconds during normal operation.
- During the 3-second hold a red bar grows across the bottom of the screen as visual feedback; release early to cancel.
- Portal AP: **SSID** `CYD-Clock` · **password** `esp32clock`.
- Portal URL: `192.168.4.1` — user fills in:
  - WiFi SSID + password (handled by WiFiManager)
  - NTP server (default `pool.ntp.org`)
  - Timezone as a POSIX string (default `UTC0`)
  - Backlight brightness 0–255 (default `200`)
- Portal times out after **3 minutes** and the board reboots with whatever was saved.
- All parameters persisted to flash via `Preferences` namespace `clk`.

### 2. NTP Time Sync
- Calls `configTzTime(posixTZ, ntpServer)` on boot.
- Waits up to 10 s for initial sync (polls `getLocalTime` in a loop).
- Re-syncs every **1 hour** in the background.
- `WiFi.setSleep(true)` (modem sleep) enabled to reduce idle power draw.

### 3. Clock Display
Layout (landscape 320×240):

```
┌────────────────────────────────┐
│         Wednesday              │  ← Font 4 (26 px), yellow, y=18
│                                │
│        12:34:56                │  ← Font 7 (48 px 7-seg), cyan, y=120
│                                │
│       Jun 3, 2026              │  ← Font 4 (26 px), white, y=202
│ ████████████░░░░░░░░░░░░░░░░░░ │  ← seconds progress bar, y=230-238
└────────────────────────────────┘
```

- **Day of week** — full name, yellow, redrawn only when it changes.
- **Time** — `HH:MM:SS` using TFT_eSPI Font 7 (7-segment style), cyan, redrawn every second.
- **Date** — `Mon D, YYYY` format, white, redrawn only when day changes.
- **Seconds bar** — green bar fills left-to-right over 60 seconds; sub-second fraction sourced from `gettimeofday()` for smooth animation. Redrawn every loop tick (~10 fps).

### 4. Power
- Modem sleep (`WiFi.setSleep(true)`) — radio dozes between DTIM beacons.
- Main loop sleeps `100 ms` per tick (10 fps sufficient for smooth bar).
- Backlight brightness user-configurable down to 0 (off) via portal.

## File Layout

```
2-NTPClock/
├── 2-NTPClock.ino   # Full sketch
├── platformio.ini   # Build config (env:cyd, ILI9341_2_DRIVER)
└── SPEC.md          # This file
```

## POSIX Timezone Reference

| Location | String |
|---|---|
| UTC | `UTC0` |
| Bangkok (UTC+7) | `ICT-7` |
| Tokyo (UTC+9) | `JST-9` |
| UK (with DST) | `GMT0BST,M3.5.0/1,M10.5.0` |
| CET (Europe, with DST) | `CET-1CEST,M3.5.0,M10.5.0/3` |
| US Eastern (with DST) | `EST5EDT,M3.2.0,M11.1.0` |
| US Pacific (with DST) | `PST8PDT,M3.2.0,M11.1.0` |

## Known Constraints

- The board model `ESP32-2432S028` with 2 USB ports physically uses **ILI9341** (not ST7789), despite some documentation suggesting otherwise. Always use `env:cyd` (`-DILI9341_2_DRIVER`).
- Font 7 only contains digits and `: - .` — do not attempt to render letters with it.
- `TFT_RST` is wired to the board reset line, set to `-1` in config.
