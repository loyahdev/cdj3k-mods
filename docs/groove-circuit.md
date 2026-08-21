# Groove circuit

Put one of your loops on a hot cue pad and play it **in place of a stem**,
locked to the track's beat grid, while the rest of the track carries on.

Groove circuit needs [stems](stems.md) working.

## What it is for

The three stem faders let you take a part out. Groove circuit lets you put a
different one in: drop your own kick loop in where the track's drums were, or
your own chord stab in place of its harmonics, and keep everything else
playing.

## Setting it up

Put your loops on your USB stick or SD card, and name them in a plain text file
at:

```
mods/gc/gc-config.txt
```

on the **root of the stick**. One line per pad:

```
# audio path, slot, stem, bpm
loops/hard-kick.wav,  1, d, 124
Contents/pads/Am.wav, 4, h, 124
```

| Field | Means |
| --- | --- |
| **audio path** | Where the file is. Relative paths are from the **root of the stick**, not from `mods/gc`, so your loops can stay in the folder you keep them in. An absolute path is used as given. |
| **slot** | `1` to `8`, which is pad **A** to **H**. |
| **stem** | Which part it replaces: `d` drums, `h` harmonics, `v` vocals. |
| **bpm** | The tempo of your file. Optional, but give it. See below. |

Lines starting with `#` are comments. Fields are comma-separated, so a path
with spaces in it is fine.

Notes on the file:

- **The first bank only.** These belong to the deck rather than to a track, so
  there is no second set to switch to.
- **Up to 30 seconds** per loop. A longer file is used, but only its first 30
  seconds.
- **A pad no line names is a hot cue,** exactly as it always was. You decide how
  many pads the circuit owns; the rest are untouched.

### Give it a BPM

With a `bpm` on the line, the loop is locked to the track's own beat grid: it
lands on the track's downbeats, follows a tempo that changes through the track,
and stays there through a seek.

Without one, it is stretched by a fixed ratio instead. That is exactly right
while the tempo holds still, and drifts on a track whose grid speeds up or
slows down.

A file whose stated tempo is more than 4× away from the track's is refused
rather than played at forty times speed. That is almost always a typo in the
config.

## Using it

**Open the STEMS row.** Groove circuit only owns the pads while the stem
control row is open. With the row closed, all eight pads are ordinary hot cues:
no colour of ours, nothing in the way.

With the row open:

- **Press a pad** to put its loop in, replacing the stem its line names.
- **Press it again** to take it out.
- **One at a time.** Engaging a slot releases whichever one was running.

The tempo fader works normally throughout, and the loop follows it.

### Pad colours

| Colour | Stem it replaces |
| --- | --- |
| Red | Drums |
| Blue | Harmonics |
| Green | Vocals |

**Steady** means the slot is loaded and ready. **Blinking** means it is the one
currently playing.

![The hot cue pads with groove circuit slots loaded: red pads replace drums, blue harmonics, green vocals. A still cannot show which one is blinking.](img/gc-pads.png)

![The STEMS row open, with circuit pads firing against the track.](vid/gc-pads.mp4)

A pad only lights when it will actually do something, so an unlit pad with a
config line behind it means the row is closed, the file did not load, or the
track has no stems yet.

## What happens to what is playing

The replacement stays engaged until you press its pad again or **load a
different track**. Nothing else takes it off:

- **Pausing does not.** You have thrown no switch, so nothing is switched.
- **Closing the STEMS row does not.** The row is where the controls are, not
  what the feature is. A running replacement keeps running.

With the row closed a running replacement is invisible on the pads. The
[dot on the STEMS button](stems.md#the-dot-on-the-stems-button) shows it
instead.

## Level

Your file plays at whatever level you made it. Substituting audio that has
nothing to do with the track can push the output past full scale, and the three
stem faders are what brings it back down.
