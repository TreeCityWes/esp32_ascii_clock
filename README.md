# ESP32 digital clock

A dedicated clock for ESP32-2432S028R / S029R (Cheap Yellow Display).
Large smooth warm-white numerals on black, with a readable date and AM/PM line.
The display is touch-free, with automatic backlight dimming and Wi-Fi time sync.

The time uses Barlow Condensed Medium with antialiased curves in 154-pixel
glyph boxes. Two-digit hours fit with 8-pixel side margins; single-digit hours
are centered without a leading zero. A small, steady round colon and mixed-case
date keep the face quiet. Supporting text uses the same smooth font in 36-pixel
boxes. The screen redraws only when its content changes.

The bundled [Barlow font](https://github.com/google/fonts/tree/main/ofl/barlowcondensed)
is licensed under the SIL Open Font License (see [font license](assets/fonts/OFL.txt)).
Generated masks are stored in firmware flash, so no font/filesystem upload or
internet font request is required. The browser preview uses the same masks and
display color quantization.

Designed for a 2.8-inch, 320 x 240 landscape display. This is a time display,
without an alarm buzzer.

## Wi-Fi and timekeeping

On first setup, join `ascii-clock`, open `192.168.4.1`, and select your network.
Credentials are saved. The setup portal stays open for three minutes; reboot
to reopen it when the clock cannot connect.

Time sync runs every 15 minutes with three NTP servers. Wi-Fi reconnects
automatically, with explicit retries every 30 seconds after the portal closes.
Reconnection requests a fresh time sync. The clock keeps ticking while offline.
Eastern time with DST is configured through `TZ_POSIX` in `platformio.ini`.

The date is replaced by `Sync due` if the last sync is over an hour old,
`Syncing` before time is available, or `Offline` without Wi-Fi.
Initial Wi-Fi setup has its own readable instruction screen.
After power loss, internet time is needed before showing a valid time.
There is no battery-backed RTC, and extended offline operation can drift.

Periodic sync uses the ESP32
[SNTP API](https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32/api-reference/system/system_time.html).

## Build and flash

Install [PlatformIO](https://platformio.org/), then run from the repository root:

```powershell
pio run
pio run -t upload
pio device monitor
```

PlatformIO automatically detects the serial port. To select one explicitly,
use `pio run -t upload --upload-port COM3` and `pio device monitor --port COM3`
(replace `COM3` with your device's port).

If upload sticks at `Connecting...`, hold BOOT, tap RESET, then release BOOT.
Normal builds use the committed font assets; Python font-generation tools are
only needed when changing the typeface.

## Display variants

- Inverted colors: flip `-DINVERT_DISPLAY=1` to `0`, or vice versa.
- Red/blue swapped: change `-DTFT_RGB_ORDER=TFT_BGR` to `TFT_RGB`.
- ST7789 panel: replace `-DILI9341_2_DRIVER=1` with `-DST7789_DRIVER=1`.
- Reversed brightness response: swap `3800, 300` in the backlight `map()` call.

## Preview and checks

Open `index.html` directly. Use `?time=10:58`, `?time=11:59`, or `?time=12:00`
to inspect specific hours. Preview time uses your browser's timezone;
network status is simulated.

With Node.js installed, run `node tests/layout.cjs`. The checks cover all 1,440
minute combinations, date spacing, noon/midnight formatting, and exact parity
between firmware and preview glyph masks.

Hardware checks: hours 10 through 12, touch having no effect, time advancing
without Wi-Fi, synchronization after reconnection, and initial setup without
saved credentials. Assess viewing distance and angle on the physical display.

## Font assets

The source font and its license are bundled in `assets/fonts/`. To regenerate
the firmware header, browser glyph data, and a visual contact sheet:

```powershell
python -m pip install -r tools/requirements.txt
python tools/generate_font.py
node tests/layout.cjs
pio run
```

The contact sheet is written to `artifacts/type-preview.png`. Build outputs,
contact sheets, and Python caches are excluded from version control.

## Files and pins

- `src/main.cpp`: firmware and renderer.
- `platformio.ini`: board, display configuration, timezone, dependencies.
- `index.html`: standalone preview.
- `src/clock_font.h` and `assets/font-data.js`: generated glyph masks.
- `tools/generate_font.py`: font asset generation.
- `tests/layout.cjs`: layout and asset checks.

TFT MOSI 13, MISO 12, SCLK 14, CS 15, DC 2; backlight 21; light sensor 34.
Unused touch CS 33 is held high.
