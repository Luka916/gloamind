# Post-mortem: a board that lost only its radio

A field failure worth writing down, because the symptom is misleading and the diagnostic path
is reusable.

**Short version:** the 2.4GHz radio died. Everything else on the chip kept working — CPU,
PSRAM, SPI panel, both servos, touch, USB serial. It took a dozen experiments to prove that,
and the proof is more interesting than the conclusion.

## The symptom

The board had been running BLE fine for two hours. Then, between two serial commands, it
stopped advertising. From then on every boot looked like this:

```
[BLE] 1 mem_release
[BLE] 2 BLEDevice::init
rst:0x3 (RTC_SW_SYS_RST)
```

A reboot every ~1.5 seconds, forever. `rst:0x3` is a software system reset, and there was no
panic dump printed before it — so whatever failed, did not fail loudly.

## Step 1: find the exact call

`BLEDevice::init()` does two things internally, and only instrumenting outside it hides which
one dies. So they were called separately, with a step number written to `RTC_NOINIT` memory
before each (RTC memory survives a soft reset, so the step survives the crash that eats the
serial log):

```c
mark(20);
esp_err_t r = esp_bt_controller_init(&cfg);   // returns ESP_OK
mark(30);
esp_bt_controller_enable(ESP_BT_MODE_BLE);    // never returns
```

```
· step 20
>>> controller_init 返回 0x0      ← ESP_OK
· step 30                          ← dead here
```

**`esp_bt_controller_init()` succeeds.** It allocates the controller task, the buffers, the
HCI plumbing — all the software-side work. Heap at that moment: 321KB free, 254KB largest
contiguous block. Memory was never the issue.

What dies is `esp_bt_controller_enable()`: the step that powers up the RF block, starts the
PLL and runs PHY calibration.

That single fact rules out an entire class of explanation. Anything that lives in the digital
domain — heap layout, fragmenting, NVS contents, flash contents, partition tables — is
irrelevant, because the failure happens *after* all of that succeeded.

## Step 2: confirm it is the radio, not Bluetooth

BLE and WiFi share one RF front-end: same antenna, same PLL, same power amplifier. But they
are two completely separate software stacks. So: scan for WiFi.

- `esp_bt_controller_init` + `enable` → reset
- `esp_wifi_init` + `esp_wifi_start` + `WiFi.scanNetworks()` → **also reset**

Two independent stacks, one shared piece of silicon. That is a hardware answer, not a
software one.

## Step 3: try to make it not need so much radio

If the power amplifier were merely degraded, a lower drive level might sneak through
calibration. Four configurations, each run in its own boot, advancing automatically after a
crash (progress kept in NVS, because the crash was sometimes wiping `RTC_NOINIT`):

| Config | Result |
|---|---|
| TX power forced to the minimum, −24dBm | reset at `enable` |
| −24dBm + BLE 5 extended features off | reset at `enable` |
| −24dBm + AGC hardware re-correction off | reset at `enable` |
| all defaults, and the classic-BT memory **not** released (control) | reset at `enable` |

**Dropping the transmit power by 33dB does not help.** That is the decisive one. A damaged PA
that cannot deliver power fails *when it is asked for power*; backing off should buy something.
It bought nothing, which means calibration is not completing at all — the block downstream of
the DAC is not coming up.

The obvious last resort — a smaller controller task stack — was tried and rejected by the IDF
itself: `BLE_INIT: Invalid controller task prioriy or stack size`, returning `0x102`. The
minimum is 4096 bytes, not 2048.

## What was already ruled out, before any of that

| Experiment | Result | Eliminated |
|---|---|---|
| print heap / largest block | 293KB / 228KB | running out of memory |
| unplug the servos (kill the 5V rail) | still resets | brownout from servo stall current |
| roll back to the firmware from an hour earlier | still resets | the code written that night |
| comment out `pinMode(13)` | still resets | the touch pin |
| erase the NVS partition | still resets | corrupted BT calibration data in NVS |
| `esp_bt_controller_mem_release(CLASSIC_BT)` | executes fine, then resets | classic BT holding RAM |
| `esptool erase_flash` (whole chip) | still resets | anything left in flash |
| a minimal sketch: no panel, no servos, no filesystem, BLE only | still resets at the same step | application code or peripheral contention |

Full-chip erase is the one that settles it. After that the flash contains nothing but what
the programmer just wrote, and the failure is identical.

## Conclusion

> The 2.4GHz RF analog block is damaged. The digital half of the chip is intact.

Everything digital kept working afterwards, which is the tell: 63fps panel, 25 expressions,
both servos, capacitive touch, USB serial, a 10MB filesystem that mounts. Only the radio was
gone.

## How it probably happened

Not proven — there is no scope and no spare board — but ranked by how well each fits the
evidence:

1. **Antenna damage or a mismatch.** A cut or shorted PCB antenna raises VSWR, so a growing
   fraction of the transmit power reflects back into the PA as heat instead of leaving the
   board. This is the only mechanism that explains "worked for two hours, then failed
   permanently" without anyone doing anything at the moment of failure. On the reference
   board the antenna area had a visible mark across it.
2. **An intermittent short on the servo supply.** Two solder joints carrying 5V and GND sat
   close together and were only loosely dressed. A servo twitch can bridge them for
   microseconds; the inductance of the supply wire turns that into a spike well above 5V. Not
   enough to kill the whole chip, easily enough to punch through the most fragile analog
   circuit on it.
3. **ESD.** A bare module held in the hand in winter, with the antenna area exposed. The RF
   front-end is nanometre-scale, connects straight to the antenna, and has no protection
   diode; the digital pins do. That asymmetry produces exactly this selection.

Whichever it was, the trigger was probably cumulative rather than a single event — a marginal
front-end that had been running hot for two hours, taken over the edge by a burst of
back-to-back commands.

## The part worth stealing: never let a dead radio brick the robot

A board that reboots every 1.5 seconds is a brick: you cannot see anything, cannot type
anything, cannot even get a serial log because the port disappears before you can read it.

Three layers, added after the incident, all of which stayed:

```c
RTC_NOINIT_ATTR uint32_t g_bootCount;   // survives soft reset
RTC_NOINIT_ATTR uint32_t g_bleStep;     // which step it died on
```

**Layer 1 — count failed boots.** Increment on every boot, clear only when BLE comes up. At
three consecutive failures, skip BLE entirely this boot. The eyes appear; the robot lives.

A counter kept only in `RTC_NOINIT` is not enough, and this cost a second bug. `RTC_NOINIT` is
documented as surviving a software reset — it does not survive *this* kind of reset. The crash
wipes it, so the counter restarts at 1 on every attempt, and "three failures and stop" never
fires: `K` became an infinite white-screen loop. The counter is now mirrored into NVS and the
larger of the two values wins. Verified by sending `K` and watching the log: three boots, then
the guard trips and the eyes come back.

**Layer 2 — persist the verdict in NVS.** Once the flag is set, every boot skips BLE without
even trying, so power-cycling is instant instead of 4 seconds of white screen. NVS was chosen
over the filesystem deliberately: the LittleFS partition on this board ended up read-only
after enough crash-resets, and a flag that cannot be written is not a flag. NVS took fifteen
consecutive crashes and never lost a byte.

**Layer 3 — a way back in.** Serial command `K` clears both the flag and the counter and
reboots, so a user can retry the radio whenever they like without reflashing.

Failure modes should be survivable by design. This one cost a robot's radio and would have
cost the whole robot if the guard had not been there.

## If you hit this yourself

```
1. Prove init vs enable separately.     If init is ESP_OK, memory is not your problem.
2. Scan for WiFi.                       Also resets? It is the front-end, not the stack.
3. Try minimum TX power.                Still resets? Calibration is not completing.
4. Then stop.                           The remaining options are a new board or an external
                                        module on a spare UART.
```

Cost of the fix: about $10 for a serial-Bluetooth module on two free GPIOs, or about $40 for a
new board and re-soldering eight panel wires.

Before you spend that, do the one test that was skipped here: a **470µF + 0.1µF across 3V3 and
GND at the module**, plus a short thick cable. An Espressif engineer's rule of thumb is that
restarts during full RF calibration are usually a supply problem, and calibration is exactly the
step that fails here.

Whether this failure pattern is unusual, and what other makers found when they hit it, is
collected in [`rf-field-reports.md`](rf-field-reports.md).
