# Expression tiers

Twenty-five expressions, but a robot that cycles through all of them at random is a slot
machine, not a creature. They are split into three tiers, and the split is the design.

## Tier I — idle (6)

What it does when nobody is around. Boring on purpose.

| Mood | Index | When |
|---|---|---|
| `normal` | 0 | the default; about 55% of idle time |
| `blink` | 4 | automatic, layered on top of whatever is showing — not in the pool |
| `peek` | 6 | glancing sideways, now and then |
| `think` | 13 | staring into the middle distance |
| `wink` | 5 | a single slow blink, rare |
| `sleep` | 14 | long idle only; never entered at random, because waking up 4 seconds later looks like narcolepsy |

Nothing else may appear without a reason. **This is the whole point**: if `cry` shows up while
the robot is alone, then `cry` means nothing. Reactions have to be caused.

## Tier II — states (16)

Only by event: a touch, a serial/BLE command, the agent's mood, or a long silence.

| Mood | Index | Cause |
|---|---|---|
| `happy` | 1 | head touched |
| `shy` | 15 | chin touched |
| `love` | 11 | petted repeatedly, or looked at for a while |
| `startle` | 18 | first touch after a long silence |
| `surprise` | 7 | something unexpected — wired to the agent later |
| `angry` | 8 | petted too many times in a row |
| `angry2` | 19 | keep going |
| `smug` | 2 | praised |
| `despise` | 3 | brushed off |
| `speechless` | 12 | the agent said something impossible |
| `cry` | 9 | the agent said something sad; or nobody came for a very long time |
| `confused` | 16 | a command it could not parse |
| `dizzy` | 10 | command flood |
| `nod` | 23 | acknowledged |
| `nono` | 20 | refused |
| `nonoBlink` | 21 | refused, with a blink |

`nod`, `nono`, `startle` and `angry2` return to `normal` on their own timer, so idle may
occasionally borrow them — they leave the screen by themselves and cannot get stuck.

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

## What changed in the firmware

The old idle pool held everything that could sit on screen for a few seconds: happy, shy, cry,
angry, rainbow, dizzy. That is why the rainbow showed up every few minutes — frequent enough
that it stopped meaning anything, and it buried the causality of the touch reactions.

Now the idle pool is five moods, the state pool is event-only, and the eggs are gesture-gated.
Idle interval also went from 2.5-6s to 3.2-7s, so it sits still more.
