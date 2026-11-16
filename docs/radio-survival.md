# Keeping the radio alive

The 2.4GHz block is the most fragile part of an ESP32-S3 and the only part of the chip that
sits directly on an exposed antenna with no protection diode behind it. It is also the part
you cannot replace. This page is the checklist the first build did *not* follow, and the
post-mortem of what that cost is in [`rf-module-failure.md`](rf-module-failure.md).

None of this is about being careful with software. All of it is wiring.

---

## Why the radio dies and the rest of the chip does not

```
voltage event on the 3V3 rail
   servo inrush  ·  a 5V/GND solder blob touching  ·  static through the antenna
                    │
                    ▼
   the RF front-end takes it first
   nanometre-scale devices, no ESD diode, direct path to the antenna
                    │
                    ▼
   the digital side survives
   ESD diodes, and far more voltage headroom
                    │
                    ▼
   every boot after that, PHY calibration reads a broken block,
   computes a garbage pointer, and the core faults
```

The signature is unmistakable once you have seen it: **the panel, both servos, the touch pad
and USB serial all work; only Bluetooth and WiFi are gone.** No soldering mistake produces
that pattern — a bad joint kills a pin or the whole board.

---

## Layer 1 — split the servo rail (worth the most)

| Do | Part | Cost |
|---|---|---|
| Servo 5V on its own wire from the supply, sharing only GND with the board | — | 0 |
| **100µF electrolytic + 0.1µF ceramic across the servo supply, at the servo end** | | ~$0.10 |
| Schottky in series with the 5V feed, stops back-feed | SS34 | ~$0.03 |
| TVS across 5V to clamp spikes | SMAJ5.0A | ~$0.03 |

The capacitor is the cheapest insurance in the whole build. A micro servo going from idle to
stalled draws a step of ~700mA; the wire inductance turns that step into a spike that travels
back down the rail. The capacitor absorbs the step so the board never sees it.

Put it at the **servo** end, not at the board end. At the board end it protects the board from
what has already arrived.

## Layer 2 — fix the joints (free, and the one most often skipped)

| Do | Why |
|---|---|
| Keep 5V and GND joints **10mm apart**, not 2mm | two fat blobs that touch intermittently are worse than static: they short the rail, it collapses, then the inductance kicks back tens of volts |
| **Heat-shrink**, not masking tape | tape is thin, wears through, and becomes conductive when damp |
| Zip-tie the bundle to the frame | a servo that vibrates is a servo that shakes its own wiring loose |

In the build that lost its radio, both of these were skipped. The joints were large, close,
and taped.

## Layer 3 — static and physical

| Do | Note |
|---|---|
| **Kapton tape over the antenna area** | thin, heat-resistant, and does not detune 2.4GHz the way foil or foam with adhesive does |
| Touch a grounded screw before handling the board | the board being plugged in is what makes this matter — it gives the discharge a path, and the path runs through the chip |
| Do not rest the board on foam, bubble wrap or a plastic bag | these are static generators, and they are what boards get shipped in |
| Keep the room above ~40% humidity | a mopped floor does more than any wrist strap |
| **Unplug before soldering, plug in after checking** | the two steps people skip are the two that matter |

A wrist strap only works if it is clipped to something grounded. Worn loose, it is a bracelet.

## Layer 4 — software (already in this firmware)

| Guard | What it buys |
|---|---|
| Three-layer boot guard | a dead radio can never turn the robot into a white-screen reboot loop |
| Soft limits on both axes | the servo never stalls against the shell, so it never draws stall current for long |
| `z` command | releases the servos, so a powered-up robot on a desk is not holding torque for hours |
| Lower TX power (optional) | less heat in the PA; costs range |

---

## Check before you power up

```
1. Continuity: every 5V joint to GND.        Must be open.
2. Continuity: every signal to its neighbour. Must be open.
3. Visual: no stray strands between joints.
4. Servo rail capacitor fitted, correct polarity.
5. Antenna area clear — no tape, no foam, no metal within 5mm.
```

Five minutes with a multimeter. The alternative is a board with no radio.

## If it is already dead

The radio does not recover, and it does not degrade gracefully — it crosses a threshold and
stops. Diagnose it in four steps, in this order, before spending anything:
```
1. Call esp_bt_controller_init() and esp_bt_controller_enable() separately.
   init OK  →  memory is not the problem, stop looking there.
2. Scan for WiFi.  Also resets?  Same front-end.  It is hardware.
3. Drop TX power to minimum.  Still resets?  Calibration is not completing,
   so it is not the amplifier being weak.
4. Stop.  New board, or a $10 serial-Bluetooth module on two spare GPIOs.
```

Step 2 is the one that settles it, and it takes one line of code. Bluetooth and WiFi are
separate stacks on shared silicon — two stacks failing on the same board is not a software
bug.

Before the four steps, spend one minute on the free check: is your Arduino core old enough to be
affected by Espressif advisory AR2023-009? That is a real software failure of both radios, fixed
in ESP-IDF v4.4.7 / v5.0.6 / v5.1.3. Details, and what other makers found when they hit the
hardware version of this, are in [`rf-field-reports.md`](rf-field-reports.md).
