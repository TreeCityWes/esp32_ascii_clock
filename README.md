# esp32 ascii clock

An over-engineered clock for the ESP32-2432S028R / S029R ("Cheap Yellow Display").
The time is drawn as giant block digits on a 53×20 character grid, with a color
gradient that slowly sweeps across them and an animated ASCII scene behind them.
Date and live weather sit along the bottom.

```
 ###   ###       #   #  #   #
#   # #   #      #   #  #   #      ~~~-----~~~~
#   # #   #   #  #####  #####   ~~~~---   -----~~~
#   # #   #      #   #  #   #  ----~~~~~~~
 ###   ###    #      #      #
```

## Faces

| Face    | Digits                    | Background                    | Auto window |
|---------|---------------------------|-------------------------------|-------------|
| Sunrise | gold → coral → violet     | sky bands above, waves below  | 05–09       |
| Water   | cyan → blue → seafoam     | layered scrolling waves       | 09–17       |
| Night   | pale blue-white           | twinkling stars               | 17–22       |
| Space   | pink → purple → cyan      | drifting multi-layer starfield| 22–05       |

Tap anywhere on the screen to cycle **Sunrise → Water → Night → Space → Auto**.
Auto picks the face from the time of day.

## Features

- **No credentials in code.** First boot opens a Wi‑Fi access point named
  `ascii-clock`. Join it, open `192.168.4.1`, choose your network. It's remembered.
- **NTP time** with DST rules (Eastern by default, see `TZ_POSIX` in `platformio.ini`).
- **Weather** from [Open‑Meteo](https://open-meteo.com), location auto-detected
  from your public IP. Refreshes every 15 minutes. No API key.
- **Auto-dimming backlight** from the onboard light sensor.
- Seconds shown as a dot walking under the digits.

## Hardware

Any Sunton ESP32-2432S028R / S029R board (2.8" 240×320, resistive touch). Both the
original micro-USB (ILI9341) and the newer 2-USB (ST7789) revisions are covered by
the same pin map; see [Display variants](#display-variants).

## Build & flash

Requires [PlatformIO](https://platformio.org/) (`pip install platformio`).

```powershell
pio run              # build
pio run -t upload    # flash (port is set to COM3 in platformio.ini)
pio device monitor   # serial log at 115200
```

The CH340 on these boards often won't enter download mode on its own. When the
upload shows `Connecting....`, **hold BOOT**, tap **RESET**, release BOOT. Don't
touch the buttons again until the write finishes.

## Display variants

If colors look wrong on first boot:

- **Inverted (white background, dark digits):** flip `-DINVERT_DISPLAY=1` to `0`
  in `platformio.ini`, or vice versa.
- **Red and blue swapped:** change `-DTFT_RGB_ORDER=TFT_BGR` to `TFT_RGB`.
- **Garbage / offset image:** your panel is the ST7789 revision. Replace
  `-DILI9341_2_DRIVER=1` with `-DST7789_DRIVER=1`.

If the screen gets *dimmer* in a bright room, the light-sensor polarity differs on
your batch — swap the `3800, 300` pair in the `map()` call in `loop()`.

## Browser mock

`index.html` is a standalone mock of the display (canvas, no build step). Open it
directly or serve the folder; `?face=water` etc. picks a face. Useful for tuning
palettes and wave math before flashing.

## Layout

```
platformio.ini   board, libraries, TFT_eSPI pin map, app config
src/main.cpp     firmware: grid renderer, faces, Wi-Fi/NTP/weather, touch
index.html       browser mock of the same renderer
```

## Pin map (TFT_eSPI, set in platformio.ini)

| Signal      | GPIO | Signal       | GPIO |
|-------------|------|--------------|------|
| TFT MOSI    | 13   | Touch MOSI   | 32   |
| TFT MISO    | 12   | Touch MISO   | 39   |
| TFT SCLK    | 14   | Touch CLK    | 25   |
| TFT CS      | 15   | Touch CS     | 33   |
| TFT DC      | 2    | Touch IRQ    | 36   |
| Backlight   | 21   | Light sensor | 34   |
