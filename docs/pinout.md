# Wiring

Board: **ESP32-S3-WROOM-1 N16R8** (16MB flash + 8MB octal PSRAM).

The eyes need six GPIOs. Pan/tilt add two more. Everything after that is only needed if you
are building a full body.

## Minimum: eyes only

| Function | GPIO | Note |
|---|---|---|
| TFT SCK | 12 | SPI clock |
| TFT MOSI | 11 | SPI data |
| TFT CS | 10 | chip select |
| TFT DC | 9 | data / command |
| TFT RES | 8 | reset |
| TFT BLK | 7 | backlight, PWM-able. Tie to 3V3 if you never dim it |

## Adding the neck

| Function | GPIO | Note |
|---|---|---|
| Servo PAN (horizontal) | **14** | 50Hz, 30°–150°, 90° = straight ahead |
| Servo TILT (vertical) | **18** | 50Hz, **23° / 62° / 118°** — see below |
| Touch (TTP223) | **13** | 3.3V supply, digital in, active high |

**TILT angles are inverted on purpose:** a larger number means further *down*.
Calibrated travel on the reference build: 23° looks up, 62° is level, 118° looks down.
The soft limits in the firmware match these numbers, so the servo never grinds against the
shell. If you re-run your own calibration, update `TILT_MIN` / `TILT_HOME` / `TILT_MAX`
together with `NECK_MIN` / `NECK_MAX` — absolute commands are clamped to those limits, while
the relative commands `p +1` / `t -1` deliberately ignore them so you can find the mechanical
stops.

**Servo power must come from its own 5V rail, never from 3V3.** A stalled micro servo pulls
~700mA; the board's 3V3 regulator supplies ~200mA. The grounds must be tied together.

**Put a 100µF electrolytic across the servo supply, at the servo end.** It costs ten cents and
it is the difference between a board that keeps its radio and one that does not — see
[`radio-survival.md`](radio-survival.md) for why, and for the checklist to run before the
first power-up.

**Give the touch pin an internal pull-down.** With nothing wired, GPIO13 floats and reads high
at random — the robot appears to pat itself and starts smiling on its own. `INPUT_PULLDOWN`
removes the ghost for free.

## Pins to avoid

| GPIO | Why |
|---|---|
| **26–37** | consumed by the octal PSRAM and flash. Wiring anything here causes random reboots with nothing on the serial port, and they are very hard to trace |
| **19 / 20** | USB D− / D+ — you lose flashing and serial |
| **48** | on-board RGB LED; an external strip mirror it and colours disagree |
| **0** | BOOT. Pulled low at power-up ⇒ the chip enters download mode and never runs your sketch |
| **45** | VDD_SPI select; must be low at boot |
| **46** | ROM boot strapping; pulled high at boot ⇒ test mode |

> 45 is the one exception among the straps: an I2S data line idles low, which satisfies the
> "must be low at power-up" requirement by accident, so it can be used as an output.

## Optional: full body

| Function | GPIO | Note |
|---|---|---|
| TB6612 PWMA / AIN1 / AIN2 | 4 / 5 / 6 | left wheel, 20kHz |
| TB6612 PWMB / BIN1 / BIN2 | 15 / 16 / 13 | right wheel — **BIN2 collides with touch, pick one** |
| WS2812 ring DIN | 38 | 8 LEDs |
| Battery voltage | 1 | ADC1_CH0, 100k+100k divider |
| Charge detect | 2 | 5V through a divider |
| IR obstacle ×2 | 21 / 47 | TCRT5000 |
| I2C SDA / SCL | 41 / 42 | VL53L0X + PCA9685, addresses 0x29 and 0x40 |
| I2S BCLK / LRC / DIN | 39 / 40 / 45 | MAX98357A |
| UART TX / RX | 43 / 44 | spare, or a second MCU |

`STBY` on the TB6612 goes to 3V3 and costs no GPIO. Leave it floating and the motors do
exactly nothing.

That fills every safe pin. If you need more, an I2C expander (PCF8574, ~$0.40) buys eight
digital IOs for two pins — fine for buttons and LEDs, useless for motor PWM.

## TFT_eSPI setup

`deploy.ps1` writes this file for you. By hand, it is:

```cpp
#define USER_SETUP_INFO "GloaMind-ST7735"

#define ST7735_DRIVER
#define TFT_WIDTH  128
#define TFT_HEIGHT 160
#define ST7735_REDTAB          // the panels used here; see below if yours differs

#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST   8

#define LOAD_GLCD
#define USE_HSPI_PORT
#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
#define TFT_RGB_ORDER TFT_RGB
```

**Backlight is not in here.** GPIO7 is driven by the sketch (`PIN_BLK`), so it can be dimmed and
turned off. Wiring it to 3V3 instead works, and costs you both.

**SPI at 40MHz is the value this build has been running**, with short jumpers. If the image tears
or flickers, drop to `27000000` — long or thin wires are the usual reason, and shortening them
beats lowering the clock.

**Wrong colours:** `TFT_RGB_ORDER TFT_BGR`, then `TFT_INVERSION_ON`. Four combinations, all of
them one line.

**Image offset or mirrored:** that is the TAB, and it is worth trying all four rather than
reasoning about it — `REDTAB`, `GREENTAB`, `GREENTAB2`, `GREENTAB3`. Five seconds each. A wrong
tab shifts or mirrors the picture; it does not change colours.

## Build flags

```
esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB
```

`PSRAM=opi` matters: the N16R8 module is octal PSRAM, and picking the quad option makes the
board report 0MB. `PartitionScheme` matters too — the default gives you 1.25MB of app space
on a board that has 16MB.
