# FAQ

Organised by what you see, not by what is wrong. Find your symptom, read two lines, move on.

---

## Nothing happens at all

**No port appears in `deploy.bat doctor`.**

In order of likelihood:

| Check | How |
|---|---|
| **Charge-only USB cable** | The single most common cause. Try a cable that you know transfers files |
| USB port / hub | Plug straight into the machine, not a hub |
| Missing driver | Windows 11 usually has it. If Device Manager shows a yellow triangle, install the CP210x or CH340 driver |
| Board not in download mode | Hold **BOOT**, tap **RESET**, release **BOOT**. The port appears |

`doctor` only reports. It never writes anything, so running it a hundred times is harmless.

## Upload fails

**`Failed to connect to ESP32-S3` / `A fatal error occurred` / port busy.**

```
1. Close anything else holding the port: Arduino IDE, PuTTY, a serial monitor
2. Replug the board
3. Hold BOOT, tap RESET, release BOOT, run flash again
```

If it gets to "Connecting..." and times out, it is almost always the cable or the BOOT timing.

## White screen forever

**The backlight is on, the panel is white, nothing changes.**

The firmware is not running. Two possibilities, and the serial output tells them apart:

| Serial output | Meaning |
|---|---|
| Nothing at all | The board is sitting in download mode (see above), or the sketch never started |
| Boots, then `rst:` in a loop | It crashes before the first frame — read the line above the `rst:` |

A panel with no data at all and the backlight floating also looks white. If you tied BLK to
3V3, it will *always* be lit — that is expected.

## The image is offset or mirrored

**Eyes are shifted a few pixels, or upside down, or the top of the image wraps around.**

The TAB setting. `deploy.ps1` writes `ST7735_REDTAB`. If your panel disagrees, edit
`User_Setup.h` in your Arduino libraries folder and change **one** line:

```cpp
#define ST7735_REDTAB      // try ST7735_GREENTAB, ST7735_GREENTAB2, ST7735_GREENTAB3
```

Four candidates, five seconds each. Do not go looking for a different driver — the offset is
one of four values and the compiler will not help you find it.

## Colours are wrong

**Red and blue swapped, or everything looks like a photo negative.**

```cpp
#define TFT_RGB_ORDER TFT_RGB     // try TFT_BGR instead
// negative-looking image: uncomment TFT_INVERSION_ON
```

Two lines, two settings, four combinations. Nothing else affects colour.

## The image tears or flickers

SPI is too fast. `deploy.ps1` sets 40MHz; if your wiring is long or your jumpers are thin, drop
to 27MHz:

```cpp
#define SPI_FREQUENCY  27000000
```

Long wires are the usual reason. Shortening the jumpers beats lowering the clock.

## The servo does not move

In order, and the first one is not a joke:

| Cause | Check |
|---|---|
| `ledcAttach` with a resolution above **14 bits** | The S3 tops out at 14. Anything wider fails **silently** — no error, no PWM. This cost two days |
| Servo signal wire on the wrong pin | pan = GPIO14, tilt = GPIO18 |
| No common ground | servo GND and board GND must be connected |
| Servo power not connected | 5V on its own rail, not from 3V3 |

## The servo buzzes, jitters, or the board resets when it moves

Power. A micro servo draws ~700mA stalled; a shared rail sags and the board browns out.

- Servo 5V from its own supply, **only GND shared**
- 100µF across the servo supply, **at the servo end**
- Never power a servo from 3V3

`docs/radio-survival.md` explains why this also protects the radio.

## It moves on its own

**The robot reacts to nothing, or smiles at an empty room.**

The touch pin is floating. GPIO13 with nothing attached reads high at random.

```cpp
pinMode(PIN_TOUCH, INPUT_PULLDOWN);
```

## Bluetooth does not appear, and the board reboots every second or two

The radio failed to come up, and the firmware's boot guard is doing its job. The serial log
shows `[BLE]` steps and then a reset.

This build has a post-mortem of exactly that: [`rf-module-failure.md`](rf-module-failure.md).
Short version: two stacks (BT and WiFi) failing on shared silicon is hardware, not software.
Check WiFi too — it is one line:

```cpp
WiFi.scanNetworks();   // also resets? it is the front-end, not the stack
```

The firmware skips the radio after three failed boots so you still get eyes, and `K` over
serial retries it.

## `Sketch too big` / `region 'dram0_0_seg' overflowed`

Wrong board settings. Both are FQBN problems, not code problems:

```
PSRAM=opi        quad makes the board report 0MB and the framebuffer allocation fails
FlashSize=16M    the default 4M cannot hold a 1.5MB binary
PartitionScheme=app3M_fat9M_16MB   the default app slot is 1.25MB
```

`deploy.ps1` passes all of them. If you are building by hand in the IDE, they are in
Tools → PSRAM / Flash Size / Partition Scheme.

## It runs for a minute, then dies

Power, again. Symptoms: resets under load, flicker when the backlight is bright, everything fine
until the servo moves.

- 470µF-1000µF across 5V/GND near the board
- A short, thick USB cable
- Not a laptop port shared with a hub

Average current looks fine. It is the microsecond peaks that reset the chip.

## It worked yesterday and now the panel is scrambled

Check, in this order: did the wires move (a jumper half out looks exactly like a driver bug),
did `User_Setup.h` get overwritten by a library update, is the SPI clock still what you set.

Library updates overwrite `User_Setup.h` silently. Keep a copy of your version — `deploy.ps1`
already saves `User_Setup.h.bak`.
