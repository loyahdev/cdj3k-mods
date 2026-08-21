# Grid adjust

Added extra BPM controls to the grid adjustment panel, so the track can be adjusted to the correct BPM.

Open **GRID ADJUST** as you normally would. The deck's own five buttons are
there, shifted left, with the new group beside them on its own plate.

|                |                                                                |
| -------------- | -------------------------------------------------------------- |
| **\|\|\|×2**   | Twice the tempo. Every other beat becomes a bar line           |
| **\|\|\|×1/2** | Half the tempo                                                 |
| **▸\|\|\|◂**   | Reduce the interval by 1 ms — beats closer together, tempo up  |
| **◂\|\|\|▸**   | Enlarge the interval by 1 ms — beats further apart, tempo down |
| **RESET**      | Back to the tempo the track loaded with                        |

The BPM readout above the group shows where you are.

## What each one is for

**×2 and ×1/2** fix a track analysed at the wrong multiple — a 140 BPM track read
as 70, or a half-time track read at double. One press and the grid lines up.

**Enlarge and Reduce** are for a grid that is close but drifts across the track:
the first beats sit right and the last ones do not. A millisecond at a time
changes the spacing without moving the downbeat.

## Limits

Multiplication is capped at ×8 and ×1/8. Enlarge and Reduce go 200 steps either
way, or ±200 ms on the beat interval.

A track the deck has no grid for cannot be adjusted, and the buttons do nothing.
