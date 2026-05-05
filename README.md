# ESP32 Smart Display

A small e-paper dashboard that sits by the front door. Originally built to track where the e-bike battery was left (charging upstairs or stored downstairs), it grew to include weather, a rain window, time, and a daily quote. The display only updates when something changes, so the battery lasts a long time and the screen is always readable in any light.

The 3D-printed enclosure is available on Printables: https://www.printables.com/model/1711551-esp32-smart-display

## Parts

- Seeed Studio XIAO EE04 (ESP32-S3 Plus with e-paper display board)
- Waveshare 2.9" e-paper display, 296x128, black/white, 24-pin FPC (WAV-12563 / SSD1680 driver)
- 3.7V LiPo battery (LP603449 or similar, fits the enclosure)
- 3D printed enclosure (see link above)

## Display layout

```
Battery downstairs / Battery charging
─────────────────────────────────────
Overcast                    14C (11C)
No rain today
─────────────────────────────────────
You've got this today           20:34
```

The battery label at the top is toggled manually with a button. The weather and rain window updates every 15 minutes. The clock updates every minute. The daily quote cycles by day of the month.

## Wiring

The XIAO EE04 board connects directly to the Waveshare display via the 24-pin FPC connector on the board — no additional wiring needed for the display itself.

Button pins (active-low, internal pull-up):

| Button | GPIO | Function |
|--------|------|----------|
| KEY0 | GPIO2 | Toggle battery label (charging / downstairs) |
| KEY1 | GPIO3 | Force immediate weather refresh |
| KEY2 | GPIO5 | Serial debug dump |

Battery voltage is read via GPIO1 (ADC), with GPIO6 used to enable the ADC circuit.

## Software dependencies

Install these via the Arduino Library Manager or the links below:

- [Seeed_GFX](https://github.com/Seeed-Studio/Seeed_Arduino_GFX) — display driver, includes TFT_eSPI and the EPaper extension
- [ArduinoJson](https://arduinojson.org/) — JSON parsing for the weather API
- WiFi, HTTPClient, time.h — included with the ESP32 Arduino core

## Generating driver.h

The `driver.h` file tells the Seeed_GFX library which display and board combination to use. Use the [Seeed_GFX driver generator](https://seeed-studio.github.io/Seeed_GFX/) to create one for your setup, then place it in the same folder as `door_display.ino`. For this project, the file contains:

```cpp
#define BOARD_SCREEN_COMBO 504  // 2.9 inch monochrome e-paper (SSD1680)
#define USE_XIAO_EPAPER_DISPLAY_BOARD_EE04
```

## Configuration

At the top of `door_display.ino`, set your WiFi credentials and location:

```cpp
const char* WIFI_SSID     = "your_network";
const char* WIFI_PASSWORD = "your_password";

const float LATITUDE  = 51.4661;
const float LONGITUDE = -0.0395;
```

The timezone is configured via `GMT_OFFSET_SEC` and `DST_OFFSET_SEC`. For the UK, leave `GMT_OFFSET_SEC = 0` and `DST_OFFSET_SEC = 3600`. Adjust as needed for other timezones.

Weather data comes from the [Open-Meteo API](https://open-meteo.com/), which is free and requires no API key.
