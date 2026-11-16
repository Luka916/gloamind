# Expression tiers

Twenty-five expressions, but a robot that cycles through all of them at random is a slot
machine, not a creature. They are split into tiers, and the split is the design: **a reaction
only means something if it has a cause.**

## Tier I — idle (9)

What it does when nobody is around. Boring on purpose.

| Mood | Index | Weight | Notes |
|---|---|---|---|
| `normal` | 0 | high (~1/3 of idle) | the default |
| `happy` | 1 | ×2 | content, not excited — it is a resting face, not a reward |
| `blink` | 4 | ×2 | **closed eyes, held for about a second** — not a 100ms blink. See the sleep ladder below |
| `smug` | 2 | ×1 | pleased with itself |
| `despise` | 3 | ×1 | see the note below — this is the same eyes as `angry`, flipped |
| `wink` | 5 | ×1 | one eye only, rare |
| `peek` | 6 | ×1 | glances sideways |
| `think` | 13 | ×1 | stares into the middle distance |
| `surprise` | 7 | ×1 | big round eyes. **It is not surprise, it is just wide eyes** — nobody said anything, so it must not read as an event |

`despise` and `angry` share one shape family. `angry` mirrors the two eyes into a V; `despise`
flips the right eye over so **both eyes slant the same way**. That is why 轻视 can live in the
idle pool and 生气 cannot: one is a mood, the other is a reaction to you.

## Tier I½ — semi-idle (2)

Moods that are neither "always there" nor "must be caused". They appear at roughly 12% of idle
changes, and the agent can also call them directly.

| Mood | Index | When |
|---|---|---|
| `speechless` | 12 | nobody has interacted for a while, and it has run out of things to do |
| `confused` | 16 | same silence, plus an unparseable command |

The reasoning: a pet does not get *speechless* at you. It gets speechless because nothing is
happening. So it should be reachable from boredom, just rarely.

## The sleep ladder

Three stages, not one jump. A robot that goes from alert to asleep in a single frame reads as
a power cut.

```
0 – 90 s    awake, idle pool as normal
 90 s       1st blink, 1.2 s closed, 1.6 s open
            2nd blink, 2.1 s closed, 1.2 s open      <- the lids are getting heavier
            3rd blink, 3.0 s closed                    <- does not open again
 asleep     head sags 9°, gaze freezes, and it stays in whatever direction it fell asleep in
 any touch  startle (not happy) -> back to normal after ~2 s
```

The blinks lengthen and the open intervals shorten, because that is what falling asleep looks
like. Sleeping does *not* recentre the neck: a creature falls asleep facing wherever it happened
to be facing, and being rotated back to straight-ahead first would read as a machine parking
itself.

Two details that matter:

- **Gaze freezes when asleep.** A sleeping robot whose eyes keep darting around the room looks
  broken, not asleep. `autoBehavior()` returns early and parks the neck.
- **Waking up does not smile.** It gets startled first. A creature that smiles the instant it is
  woken was not asleep.

## Tier II — states (10)

Only by event: a touch, a serial/BLE command, the agent's mood, or a long silence.

| Mood | Index | Cause |
|---|---|---|
| `happy` | 1 | head touched (1st–3rd time) |
| `shy` | 15 | chin touched (1st–3rd time) |
| `despise` | 3 | **4th–7th** touch in one session |
| `angry` | 8 | **8th–11th** touch |
| `angry2` | 19 | **12th and beyond** |
| `startle` | 18 | first touch after sleeping |
| `love` | 11 | petted repeatedly over minutes, or looked at for a while |
| `dizzy` | 10 | command flood, or low battery later |
| `nod` | 23 | acknowledged — command received, or the agent agreeing |
| `nono` / `nonoBlink` | 20 / 21 | refused — the agent disagreeing, or not knowing |

`cry` (9) is left to the agent only. Nothing the hardware can do makes a believable reason to
cry, and a robot that cries because nobody petted it is manipulative rather than alive.

## Why 4 / 8 / 12

These are not round numbers picked for feel. Each comes from something measurable about real
animals being petted.

| Threshold | Source | What it says |
|---|---|---|
| **4** | Oregon Humane Society, cat behaviour guide | "The cat can show enjoyment for the initial three strokes but will bite or scratch your hand on the fourth one." So the 4th touch is **the first sign of impatience** — not anger. It gets `despise` and looks away, it does not escalate yet. |
| **8** | University of Lincoln, 34 cats, 8 body areas, 15 one-second strokes each | Cats showed a **median of 8.5 negative behaviours** when petted by their owner (5.0 with a stranger — owners pet longer, further and more confidently). At 8 it is genuinely annoyed. |
| **12** | Lincoln again, upper end (~15 strokes on the head before overstimulation) | Past the point where a cat would have left. This is `angry2` with the shake. |

The window is **45 seconds** — one continuous session — and the counter resets after **25 seconds**
of no contact. Cats do not hold a grudge for a day; the annoyance decays, and so should this.

The dog side gave the shape rather than the numbers: the widely-taught **7-second rule** (pet for
seven seconds, stop, and see whether the dog leans back in) and the **3–5 second consent test**
recommended by veterinary behaviour groups both say the same thing — tolerance is short, and the
animal answers by choosing to come back. The robot's equivalent of "leaning back in" is that it
keeps looking at you between touches. That is what the 1st–3rd touches are.

Also relevant, and the reason 头顶 is the place that gets annoying: in a 2023 Eötvös Loránd study
of 92 dogs, the **top of the head scored 34% positive** while the chest scored 87%. The head is
the zone humans reach for and the zone animals tolerate rather than enjoy.

## Tier III — easter eggs (3)

| Mood | Index |
|---|---|
| `rainbow` | 17 |
| `dead` | 22 |
| `glitch` | 24 |

Not in any pool. They require a specific gesture, they are all touch-triggered, and **the exact
condition is deliberately not documented** — here or in the help text or in the README. An
easter egg that comes with instructions is not an easter egg.

The code that decides is at the end of `handleTouch()`, under a comment that says as much.

## Tuning it without a touch pad

`x [n]` goes through the exact same path as a real touch, `n` times, and prints which threshold
it crossed. It exists because the thresholds above are only worth anything if they can be
verified without a human sitting there tapping a pad twelve times.

```
x 14
  touch head
  touch head
  touch head
  touch head (despise)
    第 4 下 -> despise
  ...
    第 8 下 -> angry
  ...
    第 12 下 -> angry2
```

`i` reports `pet=N/45s drowsy=0/1 asleep=0/1 idle=Ns` for the same reason.

## What changed, and when

`2026-11-16 15:05` — first split. The idle pool had held everything that could sit still for a few
seconds (happy, shy, cry, angry, rainbow, dizzy). The rainbow appeared every few minutes, which
made it wallpaper, and it buried the causality of the touch reactions.

`2026-11-16 17:00` — sleep direction and the blink progression. `angry2` removed from the
random small-action pool (it was firing with no cause, which is exactly what this whole document
is against). Probabilities moved to 84 / 8 / 8.

`2026-11-16 16:20` — expanded on request. Tier I went from 6 to 9 (`happy`, `smug`, `despise`,
`surprise` in; `sleep` moved out of the pool and into the ladder above). `speechless` and
`confused` became semi-idle. The 4/8/12 escalation was added, with the sources above.
