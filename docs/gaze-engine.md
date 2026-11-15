# Making the eyes look *at* something

The eyes on their own read as a face. They do not read as a creature. The difference is
whether the gaze looks *directed* — and that turned out to be six small parameters, not a
smarter animation.

Before this work the vertical axis was **frozen at 62°**. It never looked up, never looked
down. After: it moves between 43° and 76°, and spends about a quarter of its time above level.

## Where the numbers come from

Nothing here was invented. Each parameter traces back to a measurement.

| Source | What it measured | What we took from it |
|---|---|---|
| Blakemore & Donaghy 1980 (cat, *J Physiol* 300:317) | saccades over 4° almost always trigger a head turn; the head lags the eye by **~30ms** most often; the eye then rotates *back* to hold the target (compensation gain 0.96); 20° saccades peak at 250°/s, last 110ms, duration grows **2.1 ms/degree** | eye first, head after; head takes the eye with it and the eye must compensate |
| Bizzi et al. 1971 (macaque) | eye leads head by **20–40ms** on unpredictable targets; order reverses when the target is predictable | we use the unpredictable version |
| Wong 2014 / Bahill 1975 (saccadic main sequence) | peak velocity 30–700°/s, duration 30–100ms; ≤20° the velocity scales with amplitude and then saturates near 500°/s; latency 150–250ms | big movements must be *faster*, not the same speed as small ones |
| Henderson & Hollingworth 1998; Dorr et al. 2010 (human scene viewing) | fixation 240–330ms, mode 230ms, **right-skewed** (both very short and very long are rare); mean saccade 4.6°; **central bias** | dwell time must be random and skewed short; straight ahead gets the most weight |
| Ott et al. 1998; Katzir & Rivlin (chameleon) | stop-and-go scanning; once locked, the *head* turns to the target and the eyes converge; a moving target is tracked with the head, not the eyes | a believable fixation involves the head; let the head do the big moves |

What we could not support, we did not implement. Dogs tilt their head when localising a sound,
but that is a roll about the line of sight — we have no roll axis, so there is no honest way
to fake it.

## The six rules

### 1. Eye first, head ~100–160ms later

Biology says 20–40ms. A hobby servo has inertia that a eye muscle does not, so 20ms would just
look like they move together. 100–160ms reads as deliberate: the eye *flicks*, the head
*arrives*. That gap is the whole difference between "looking" and "turning".

### 2. Small targets move the eye only; large ones bring the head

In an animal the eye covers roughly ±30° and the head takes over beyond that. Inverted for a
screen: the eye can be drawn pointing anywhere, so small offsets move only the pupil and save
the servo. This is also why it stops looking mechanical — the body responds proportionally
instead of to every twitch.

### 3. Duration scales with amplitude

```c
duration_ms = 70 + 11 * amplitude_in_cells
```

A small glance is 81ms; a large swing is 136ms. Everything used to take a flat 260ms, which
made large and small movements feel identical — the single biggest reason the motion looked
fake.

### 4. Dwell time is skewed, not uniform

Biology: mode 230ms, mean 330ms, both tails rare. Implemented as the sum of three uniform
randoms over 0.7–2.4s, which produces that middle-heavy shape for free.

Uniform randomness averages out to a metronome. That is one of the reasons a robot gets boring
to watch: the rhythm is too even.

### 5. Central bias, plus habituation

- Straight ahead (the viewer's face) carries weight **8**; other spots 1–4. Most of the time,
  it is looking at you.
- The last three spots it visited get their weight divided by three, so it does not stare at
  the same corner twice in a row. This is what "attention moves on" looks like in code.

### 6. Breathe, even when idle

A servo holding still is obviously a servo. A 1.2° sine at a 4-second period is invisible as
motion, but the head is never a rock.

## The interest map

| Where | Gaze direction | Pan offset | Tilt offset | Weight |
|---|---|---|---|---|
| **Straight ahead (you)** | centre | 0 | 0 | **8** |
| **Straight up** | 12 o'clock | 0 | **−20° (looking up)** | **4** |
| Up-right / up-left | 1:30 / 10:30 | ±13 | −7 | 3 / 3 |
| Right / left | 3:00 / 9:00 | ±22 | 0 | 2 / 2 |
| Down (the desk) | 6:00 | 0 | +13 | 1 |
| Down-right / down-left | 4:30 / 7:30 | ±14 | +10 | 1 / 1 |

Looking up gets the second-highest weight, deliberately: a small robot on a desk is *below*
your eye level, so up is where the interesting thing is. Its old behaviour — stare straight
ahead, occasionally look down at the table — read as bored.

## The touch cascade

One pat becomes four beats, in order, and the ordering is what sells it:

```
1. eyes snap back to centre        90ms    "it noticed"
2. head lifts 12°                          "it looked up at you"
3. happy squint                   220ms    "it liked it"
4. after 1.3s, head returns, autopilot resumes   "it is not clingy"
```

Run separately these are three unrelated animations. Run in sequence, they read as one
reaction. The touch pin only accepts a rising edge, with a 1.4s cooldown.

## Measured result

26 seconds, 174 samples, on hardware:

```
pan   : −22° … +22° from centre      span 44°
tilt  :  43° … 76°                   span 33°   (previously constant at 62°)
gaze  : all 12 directions used
looking up (<56°): 46/174 = 26% of the time
```

| | Before | After |
|---|---|---|
| Vertical axis | constant 62°, never moved | 43–76°, 26% of the time above level |
| Ordering | eye and head moved independently | eye lands, head follows 100–160ms later |
| Dwell | fixed 2.5–6s | 0.7–2.4s, skewed short |
| Repeats | yes (uniform random) | no (last 3 spots de-weighted) |
| At rest | completely still | 1.2° breathing drift |

## Honest limitations

| Limit | Consequence | Fixable? |
|---|---|---|
| No roll axis | cannot do a real head tilt | needs a third servo |
| No camera | "what it is looking at" is a hard-coded table, not vision | an OV2640 would make it real |
| 12 discrete gaze directions | the eye only points at multiples of 30°, not continuously | needs vector-based rendering |
| Fixed servo speed | large turns have no "throw" | needs amplitude-scaled PWM stepping |

The last row matters most, so it is worth saying plainly: **this looks like it has intent, it
does not have intent.** It appears to be paying attention because the rhythm, the ordering,
the habituation and the central bias are right — not because it can see.

## Where it lives in the code

| File | Contents |
|---|---|
| `firmware/pet_eye/pet_eye.ino` | `GAZE_SPOTS[]`, `gazePick()`, `gazeMicro()`, `handleTouch()` |
| same | deferred head motion (`g_neckPend` / `g_neckLag`), the breathing term |
| `PIN_TOUCH 13` | the pat sensor |
