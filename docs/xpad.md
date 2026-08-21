# X-PAD

A sampler on a touch strip, under the waveform. Your finger picks a loop length
across the strip and bends the pitch up and down it; the hot cue pads hold eight
samples off your stick.

Off by default. Turn **ENABLE X-PAD** on in [MOD SETTINGS](mod-settings.md), and
an **X-PAD** button appears in the row above the waveform, beside STEMS.

![ENABLE X-PAD, switched on in MOD SETTINGS.](img/mod-settings-xpad.png)

## The panel is the mode

Opening X-PAD borrows the eight hot cue pads and three controls. Closing it
stops the sound and hands every one of them straight back.

| Control | Normally | While X-PAD is open |
| --- | --- | --- |
| The eight pads | Hot cues | The sample banks, A to H |
| MEMORY | Stores a memory cue | **HOLD** |
| CALL / DELETE | Deletes a cue | **OVERDUB** |
| VINYL SPEED ADJUST | Sets the brake | **VOL** |

With the panel shut, every one of these is the button on the lid. DELETE
deletes a cue, MEMORY stores one.

## The strip

Six bricks, left to right, each a loop length in beats.

![The strip: six bricks from 1/16 to 2 beats, with the readout, VOL, OVERDUB and HOLD to the right.](img/xpad-strip.png)

**Across** picks the length. **Up and down** bends the pitch, ±12 semitones, and
the readout beside the strip shows the length and the bend together

![Opening X-PAD, then moving across the strip and up and down it: the readout follows both the loop length and the bend.](vid/xpad-strip.mp4)

## HOLD and OVERDUB

**HOLD** latches the sound when your finger lifts. Without it, lifting off ends
the gesture.

**OVERDUB** arms a four-beat sequencer. It records *which pad fired and where in
the bar*, not the audio, so replay re-triggers the sample rather than layering a
recording of it. There is no gain stacking and no feedback.

The phase is measured against the track's own grid, so a hit stored at beat 2.7
fires at every beat 2.7 for as long as OVERDUB is on. Nothing has to be started
in time and nothing drifts. Turning OVERDUB off clears it.

## The samples

Put audio in **`mods/loops/`** at the root of your stick. The first eight files,
sorted by name, become pads A to H.

```
mods/loops/01-kick.wav
mods/loops/02-clap.wav
...
```

There is no config file. `.wav` and `.flac` are read; dotfiles and rekordbox's
`.asd` siblings are skipped. A file the deck cannot decode is skipped and the
pad stays empty.

The [starter pack](https://github.com/nsaintot/cdj3k-mods/releases/download/extras-files/cdj3k-mods-starter.zip) is eight drums already numbered for the pads — a 909 kit
on A–D and an 808 kit on E–H — with a 707 and a 606 kit in `extras/`.

Unlike [groove circuit](groove-circuit.md), no BPM is declared. A one-shot plays
at its own length, so there is nothing to lock to the grid.

Only the **first** bank is read, the same way groove circuit handles its slots.
These samples belong to the deck rather than to a track.

## When a pad does nothing

- **No `mods/loops/` on the stick**, or fewer than eight files in it.
- **The file did not decode.** Try a plain `.wav`.
- **The panel is shut**, in which case the pad is an ordinary hot cue.
