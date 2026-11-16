# Build it

Written for someone who has never installed Arduino, never compiled anything, and does not
want to learn. Four commands, no IDE, no menus.

If something goes wrong, [`faq.md`](faq.md) is organised by *symptom*, not by cause.

---

## What you need

| | |
|---|---|
| Board | ESP32-S3 **N16R8** (16MB flash, 8MB octal PSRAM) |
| Panel | 1.8" 128×160 ST7735S, 8-pin SPI |
| Computer | Windows 10 or 11, one free USB port |
| Cable | **A data cable.** Charge-only cables look identical and never work |
| Time | 20-40 minutes, most of it downloading |

No servos needed for the first run. Get eyes on the panel before adding moving parts —
if the screen is wrong you want to know that while the wiring is still simple.

---

## Four commands

Open a terminal in this folder (or just double-click `deploy.bat`, which asks you which one).

```
deploy.bat doctor      check what is installed, change nothing
deploy.bat setup       install arduino-cli, ESP32 core, TFT_eSPI, panel config
deploy.bat flash       compile and upload to the board
deploy.bat all         setup, then flash
```

**Start with `doctor`.** It takes five seconds and tells you exactly which of the four things
is missing, so `setup` does not download 500MB you already had.

Not a `.bat` person? The same thing:

```
powershell -ExecutionPolicy Bypass -File deploy.ps1 -Mode doctor
```

### What `setup` actually does

1. Downloads `arduino-cli` into `.tools\` (about 12MB) — the whole compiler, no IDE
2. Registers the ESP32 board index, from `arduino.me` — the Chinese mirror, because
   `espressif.github.io` is often unusably slow from mainland China
3. Installs the ESP32 core (300-600MB, the slow part; go make tea)
4. Installs `TFT_eSPI`
5. **Rewrites `User_Setup.h`** with the pin mapping, driver and clock for this panel.
   Your original is saved as `User_Setup.h.bak`

Step 5 is the one everybody gets wrong by hand, which is why it is a script. Getting it wrong
produces a board that boots, runs, and shows a white or scrambled screen.

### What `flash` does

Finds the board, compiles with these exact flags, uploads:

```
esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB
```

Every one of those matters. `PSRAM=opi` because N16R8 is octal; `FlashSize=16M` because the
default is 4M; `CDCOnBoot=cdc` so `Serial` goes over USB; the partition scheme because the
default app slot is 1.25MB and the binary is 1.5MB.

---

## First power-up

| You see | Meaning |
|---|---|
| Blank for ~1s, then two eyes | normal |
| White for a moment, then eyes | normal — the panel resets before the first frame |
| Eyes but upside down / offset | wrong TAB setting — [`faq.md`](faq.md#the-image-is-offset-or-mirrored) |
| Eyes, but colours wrong | `TFT_RGB_ORDER` — same page |
| White forever | firmware is not running. Watch the serial output: `docs/faq.md#white-screen-forever` |

The eyes move on their own. That is the gaze engine, not a bug: it looks around every few
seconds, favours looking up at you, and gets bored of the same spot.

---

## Adding the neck

Two servos, GPIO 14 (pan) and 18 (tilt). **Servo power comes from its own 5V rail, never from
3V3**, and the grounds must be tied together. A stalled micro servo pulls ~700mA; the board's
regulator supplies ~200mA.

Full wiring, and the calibrated travel numbers, are in [`pinout.md`](pinout.md). Read
[`radio-survival.md`](radio-survival.md) before you solder anything — the 2.4GHz block is the
one part of this board that a wiring mistake kills permanently.
