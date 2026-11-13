# Water Tracker

An ESP32 water tracker that estimates intake by weighing a bottle or glass before and after a drink. It shows daily intake and a clock, and can remind you to drink using the display and an LED strip.

## Hardware

The sketch targets an ESP32-WROOM-32D, a DFRobot 1.47-inch ST7789 SPI display (172 × 320), an HX711 with a 5kg load cell, a rotary encoder with a push switch, a separate push button (MX low profile switch), and a ten-pixel WS2812 strip (2.7mm version).

| Component | Signals | ESP32 GPIOs in signal order |
| --- | --- | --- |
| ST7789 Display | SCLK / MOSI / CS / DC / RST / backlight | 18 / 23 / 27 / 26 / 25 / 22 |
| Rotary encoder | A / B / push | 33 / 32 / 19 |
| Separate button | Push | 21 |
| HX711 | Data / clock | 16 / 17 |
| WS2812 | Data | 13 |

Use a common ground. The encoder switch and separate button connect to ground when pressed and use the ESP32's internal pull-ups. This table lists signal connections. Follow each module's specifications for power and logic levels.

## Install and upload

1. In Arduino IDE, install **esp32 by Espressif Systems** in Boards Manager. The release targets version **3.3.12**.
2. Install **TFT_eSPI** by Bodmer (2.5.43), **Adafruit NeoPixel**, **WiFiManager** by tzapu and **ArduinoJson** version 7 using Library Manager. The HX711 driver is bundled.
3. Open [WaterTracker.ino](WaterTracker/WaterTracker.ino). Keep the entire `WaterTracker` folder together. Its `build_opt.h` provides the display settings for the build.
4. Select **Tools → Board → ESP32 Dev Module**, then **Tools → Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)**. The default partition is too small for this sketch.
5. Select **Tools → Upload Speed → 115200**. Select the board's USB port and click **Upload**. 


## First-time setup

1. On a device without saved Wi-Fi credentials, join **WaterTracker-Setup** using the setup hotspot password **drinkwater**. Open `http://192.168.4.1` if the setup page does not appear.
2. Select your Wi-Fi network and enter its password. The ESP32 stores your credentials locally and reuses them on later boots.
3. Select **Calibrate**. Remove everything and press the encoder, use the encoder to enter a known weight in grams and press, then place that weight on the platform and press again when stable.
4. Use **Zero** to zero the bare platform when needed. Use **Zero Glass/Bottle** to measure the empty weight of an individual container. It starts with the active profile, lets you select another, and saves the empty weight after you confirm. The four profiles start with example weights.

The device uses NTP to get UTC and queries `ipwho.is` over HTTPS to estimate the current time zone offset from the connection's public IP address. The service sees that address. After full power loss, the clock needs an internet connection to restore the current time. Intake and calibration are stored on the ESP32.

## Controls

- Turn the encoder to cycle through different screens: blue dot:intake, red dot: clock and progress, yellow dot: settings, and the individual settings rows.
- Press the encoder on a highlighted row to edit or start a wizard. Turn to adjust and press to save. Hold the encoder or use the separate button to cancel.
- Leave a container on the scale briefly on either data screen, lift it to drink, and return it. A stable decrease of at least 5 g is treated as water consumed. The daily total resets at local midnight once the device has synchronized time.
- The reminder can be disabled or set to 5–240 minutes, with a **10s** preview option. Pick up the container or press the separate button to dismiss an active reminder. Change the strip colour under **LED COLOUR**.

## Third-party notices

- The bundled HX711 driver is by Rob Tillaart. See its [MIT license](WaterTracker/src/hx711/LICENSE).
- The display uses embedded bitmaps derived from [Liberation Sans](https://github.com/liberationfonts/liberation-fonts) and [Jersey 10](https://github.com/google/fonts/tree/main/ofl/jersey10). The Liberation Sans bitmaps are named Tracker Sans in this project. See [font notices and license](FONT-LICENSE.txt).
