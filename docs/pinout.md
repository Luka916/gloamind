# Wiring

Board: **ESP32-S3-WROOM-1 N16R8** (16MB flash + 8MB octal PSRAM).

The eyes need six GPIOs. Everything below the first table is for whatever you build around them.

| Function | GPIO | Note |
|---|---|---|
| TFT SCK | 12 | SPI clock |
| TFT MOSI | 11 | SPI data |
| TFT CS | 10 | chip select |
| TFT DC | 9 | data / command |
| TFT RES | 8 | reset |
| TFT BLK | 7 | backlight, or tie to 3V3 for always-on |

Optional extras, if you are building a full body: TTP223 touch on 14, servos on 17 / 18,
WS2812 ring on 38, TB6612 motor driver on 4 / 5 / 6 and 15 / 16 / 13 (with STBY tied to 3V3).

## Pins to avoid

| GPIO | Why |
|---|---|
| **26 ~ 37** | taken by octal PSRAM and flash. Symptom is a **random reboot with nothing on the serial port** — very hard to trace |
| **19 / 20** | USB D- / D+, using them kills flashing |
| **48** | on-board RGB LED, anything you attach shares it |
| **0 / 45 / 46** | boot straps, a pull-up or pull-down load stops the board booting |

> 45 is the one exception: an I2S data line idles low, which satisfies the "must be low at
> power-up" requirement, so it can be used as an output.

## TFT_eSPI setup

In `User_Setup.h`:

```cpp
#define ST7735_DRIVER
#define TFT_WIDTH  128
#define TFT_HEIGHT 160
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST   8
#define TFT_BL    7
#define TFT_BACKLIGHT_ON HIGH
```

Run SPI at **27MHz**. At 40MHz the image tears, and how early it tears depends on wire length
and how good your jumpers are.

Wrong colours: check `TFT_INVERSION_ON` and `TFT_BGR` first. Do **not** go looking for a
different TAB setting — that only shifts the image by a pixel or two.
