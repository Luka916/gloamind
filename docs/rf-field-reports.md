# Other people's dead radios

Field reports collected after this build lost its radio, to check whether the failure pattern is
actually unusual. It is not. The pattern — *panel, servos, touch and serial all fine; Bluetooth
and WiFi permanently gone* — shows up repeatedly, and it shows up next to motors.

Each entry: what they saw, what it turned out to be, and whether it matches
[`rf-module-failure.md`](rf-module-failure.md).

---

## 1. The closest match: two boards, a servo and a motor driver

**espressif/arduino-esp32 issue [#12170](https://github.com/espressif/arduino-esp32/issues/12170)**
— *"ESP32 Wifi and BT silently dies randomely"*

> "I have 2 esp32 boards so far that had their WIFI and Bluetooth silently die for no reason
> whatsoever. boards were powered from pc via USB cable... boards were brand new."
>
> Setup: L298N driver, 15kg servo on a dedicated 9V battery, LEDs, **everything common-grounded**,
> ESP32 powered from the PC's USB port.
>
> "when i say dies i meant it completely never works again, i tried alot of stuff like flashing
> new firmware and so much more yet its like normal with no wifi and bluetooth. i had to buy
> another esp32."

Same hardware recipe as this build: USB-powered board, a servo, a motor driver, one shared
ground. Same outcome: the board keeps running, the radio never comes back, reflashing changes
nothing. The issue was never diagnosed — it was closed for lack of logs, which is the usual fate
of this failure, because by the time you notice, the radio is already dead and there is nothing
left to log.

That is the reason [`radio-survival.md`](radio-survival.md) exists at all: the failure is silent,
so it has to be prevented rather than debugged.

## 2. The one that *is* a software bug — and how to rule it out in a minute

**Espressif Bug Advisory [AR2023-009](https://documentation.espressif.com/AR2023-009%20Advisory%20for%20the%20Probabilistic%20Failure%20of%20Wi-Fi%20and%20Bluetooth%20on%20Some%20Chips%20EN.pdf)**
— *Probabilistic Failure of Wi-Fi and Bluetooth on Some Chips*, Feb 2024.

> During boot the bootloader powers down the Wi-Fi and Bluetooth digital modules. They are
> powered back up during initialisation, and there is a probability that the related digital
> modules fail to reset — producing a probabilistic failure of Wi-Fi or Bluetooth.
> Affects ESP32, ESP32-S2, ESP32-S3, ESP32-C3. Fixed in ESP-IDF v4.4.7 / v5.0.6 / v5.1.3 by
> adding an explicit reset in software.

This is worth knowing because it is free to rule out, and it looks similar from the outside.

| | AR2023-009 | This build |
|---|---|---|
| Symptom | radio initialises, cannot send or receive | hard fault inside `esp_bt_controller_enable()` |
| Arduino core | affected: IDF ≤ 4.4.6 / 5.0.4 / 5.1.2 | **3.2.1, IDF 5.4 — already patched** |
| Fix | upgrade ESP-IDF | nothing in software |

So: check your core version before you conclude the hardware is dead. If you are on an affected
IDF, you may just need an update.

## 3. The one test this build never ran

**Espressif forum, ["phy_init: failed to load RF calibration data"](http://bbs.esp32.com/viewtopic.php?f=13&t=4097)**
— answered by ESP_Angus, an Espressif engineer:

> "Restarts during full RF calibration are almost always due to power stability (or power supply
> capacitance) problems. ESP-WROOM32 modules are not made to be used standalone... you'll need at
> least 10uF + 0.1uF on the power input... shorter/thicker power leads, soldering low ESR high
> value capacitors to the power pins."

The failure point in this build was `esp_bt_controller_enable()`, which is exactly where full RF
calibration runs. By that quote, the first suspect should have been the supply, not the chip.

It was partly tested — the servos were moved to their own supply and the board still faulted —
but **the board was never tested with a bulk capacitor soldered directly across 3V3 and GND at
the module**, and it was always fed through the same USB cable.

If you hit this and have not shipped the board back yet, that is the one experiment worth ten
minutes: 470µF + 0.1µF across 3V3/GND at the module, a short thick USB cable, and a different
USB port. If it boots, the radio was starved, not dead.

## 4. Antenna defects do produce "no networks" — at manufacture

**CSDN, [一种 ESP32 开发板 WiFi 及蓝牙失效的处理方法](https://blog.csdn.net/weixin_49632661/article/details/144663244)**

Two modules where `WiFi.scanNetworks()` returned nothing. Comparing the antenna area against a
good module from another vendor under a phone camera showed the feed trace looked different;
a multimeter showed continuity from the feed **to ground**. The copper pour had been connected
to the antenna feed during PCB design. The author cut the short with a knife and immediately
found 30 access points, and Bluetooth worked.

Two things this confirms:

- an RF-path defect is a real way to lose both radios at once, and it can ship from the factory
- **it does not fault the CPU** — the sketch keeps running, it just sees nothing

This build's symptom is the harsher variant: the core faults during calibration instead of
running with a deaf radio. Same region of the chip, one layer further in.

## 5. What a damaged front end looks like under a microscope

**ESP32-C5-MINI-1 reliability write-up** (mynw.cn) — an ESD failure-analysis case:

> A production line saw ~0.8% "Wi-Fi will not start". ATE reported **RF calibration failure**.
> SEM found pinholes in the gate oxide of the LNA input device; TDR showed the input matching
> network shifted by more than 15%. Root cause: ungrounded AOI equipment accumulating −3.2kV,
> discharging into the board on probe contact (CDM).

That is the physical mechanism assumed in [`rf-module-failure.md`](rf-module-failure.md), written
up by people with a SEM: a damaged RF front end shows up as **a calibration failure**, not as a
weak signal. The same article notes ESD damage is covert — "intermittent malfunction, lower
Wi-Fi throughput, Bluetooth dropping" — which is exactly the two hours of normal operation
before the threshold was crossed.

## 6. Why both radios and not one

**SparkFun community, [RTK Reference Station WiFi](http://community.sparkfun.com/t/rtk-reference-station-wifi/62949)** —
from someone who supports these boards:

> "I've never seen or heard of an ESP that has broken wifi and functional bluetooth at the IC
> level."

Wi-Fi and Bluetooth share one antenna, one PLL and one front end. Losing both is one failure.
Losing exactly one, on the same silicon, is not a thing.

## 7. Antenna damage degrades; it does not kill

**bitcraze discussions [#1144](https://github.com/orgs/bitcraze/discussions/1144)** — the antenna
was physically knocked off an ESP32 on an AI Deck. The board still flashed, still ran, and Wi-Fi
still worked at short range. They soldered it back and potted it with hot glue.

Useful calibration for anyone staring at a scratch on their antenna: a broken antenna makes the
radio **weak**, not absent. If your board hard-faults while enabling the radio, the antenna is
not the thing that failed.

## 8. Servos on the same rail: the common thread

**dev.to, ["The Servo Motors Were Killing My ESP32"](https://dev.to/khader_qaabar_4bba14e148c/the-servo-motors-were-killing-my-esp32-and-i-didnt-realize-it-1gg6)**

Servos wired to the same rail as the board: display cutting out, servos behaving erratically,
logic that "felt buggy". Cause: inrush current from the servos dragging the rail down. Fix:
separate power domains, common ground only.

Also worth reading: [Universal Solder's brownout guide](https://www.universal-solder.ca/troubleshooting-esp32-brownout)
for the checklist of "it resets exactly when the motor starts".

Most of these cases end at brownout resets, which are survivable. The ones that end at a dead
radio are the same event with a slightly worse outcome — the spike arrives while the PA is on.

---

## What the collection changes

| Before | After reading these |
|---|---|
| "did I break it with a weird mistake?" | no — this is a known failure mode, reported by other makers with the same hardware |
| "maybe the antenna scratch did it" | it can cause a deaf radio (#4), not a faulting one (#7) |
| "maybe it's a software bug" | one real software bug exists (#2) and it is already patched in this build |
| "it must be unfixable" | mostly, but #3 is a free test that was never run |

The honest summary: **the failure is common enough to have a shape, rare enough that nobody
files a good report, and prevented by about a dollar of parts.**
