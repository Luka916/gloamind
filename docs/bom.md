# Parts

Prices are mainland-China retail, late 2026. They move; treat them as an estimate.

## Minimum — eyes only

| Part | Spec | ~Price |
|---|---|---|
| ESP32-S3 board | **N16R8**, Type-C, headers fitted | ¥26 |
| TFT panel | 1.8" 128×160 **ST7735S**, 8-pin SPI | ¥12 |

¥38 total. No servos, no touch sensor, no LED ring needed.

## If you are building a body

| Part | Spec | ~Price |
|---|---|---|
| SG90 pan/tilt set | 2 servos + 2-axis bracket | ¥13 |
| TTP223 | capacitive touch | ¥2.5 |
| WS2812 ring | 8 pixels | ¥6 |
| TB6612FNG | dual H-bridge | ¥7.5 |
| TT motor pair | **1:120 gearbox, dual shaft**, with wheels | ¥12 |
| Caster | small | ¥2 |

> 1:120, not 1:48. 1:48 is 200rpm and the thing bolts like an RC car. 1:120 is 80rpm with
> twice the torque, which reads as a slow shuffle.

## Tools

| Part | Why |
|---|---|
| Soldering iron + solder | the panel header is 8 pins and the driver board is worse |
| Desoldering braid, 2mm | first attempt will bridge something, and without braid you just stare at it |
| Multimeter | check adjacent pins for shorts before applying power, every time |
| Heat-shrink, assorted colours | two months later you will not know which wire is SCK |

## Power

Simplest: one USB-C cable to a phone charger. Nothing to modify.

If you want it to run on a battery and switch between charger and cell without rebooting, that
needs power-path management. A cheap route is an IP5306 power-bank module (¥8) plus a lithium
cell plus a 1000µF electrolytic across the output to ride out the switchover.

One trap with the IP5306: it cuts output after 32 seconds below 45mA, so the firmware must not
enter deep sleep.
