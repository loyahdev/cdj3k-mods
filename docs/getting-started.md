# Getting started

What the mods need, what happens when the deck starts up, and how to take them
out again.

> [!WARNING]
> **This is unstable software.** Expect bugs, freezes and crashes. It is not
> finished and it is not tested the way a firmware release is.
>
> **Do not put it on a deck you are relying on.** Not the night of a gig, not on
> a club's unit, and not on the only player you own. Try it somewhere a restart
> costs you nothing.

## What you need

- A **CDJ-3000** running firmware **3.13 to 3.22**, either hardware variant.
  Older firmware is refused by the installer.
- A USB stick or SD card with your library on it, as usual.
  [X-PAD](xpad.md) and [groove circuit](groove-circuit.md) will load any additional files they need from the same drive.
- For [Stems](stems.md) only: a computer on the same network running
  [stemd](https://github.com/nsaintot/stemd). Separation does not happen on the
  deck.

## Installing

The mods ship as a **`.UPD` file**, the deck's own update format.

1. Download the `.UPD` from the
   [latest release](https://github.com/nsaintot/cdj3k-mods/releases/latest).
2. Copy it to a USB stick formatted **FAT32** or **exFAT**, and make sure it is
   the only `.UPD` on there. With two of them the deck stops and asks you to
   sort it out rather than choosing one.
3. Put the stick in the **top USB port**. Updates are not read from the SD slot.
4. Run the deck's firmware update procedure, the same way you would for any
   other update. (the process will restart the unit 2 times after the installation)

**If the deck keeps asking you to connect a USB device**, it is looking past
your stick rather than reading it. A stick formatted on a Mac with a *GUID
Partition Map* is the usual reason: the deck finds the small hidden EFI
partition first, sees no update on it, and stops there. Reformat with a
**Master Boot Record** partition map and a single FAT32 partition.

**This is a patch, not a full replacement.** A `.UPD` file updates only  
specific parts of the system, it doesn't need to contain the entire system image.  
The original application isn’t replaced,
and no system reflash occurs.

[Removal](#removing) happens entirely from the front panel.

Worth being careful about, as with any update:

- **Do not cut the power while it is updating**, and do not pull the stick.
- **It is third-party code, and the risk is yours.** It is open source, so you
  can read it and build it yourself before you trust it. It still voids your
  warranty, nobody is liable if it goes wrong, and there is no vendor behind it
  to call. [Legal](legal.md) sets all of that out in full.

## Preparing your stick

Nothing extra is needed to play. Two features read their own files from the
stick you are playing from, both under a `mods/` folder at its root:

```
mods/loops/            up to eight samples for X-PAD, taken in name order
mods/gc/gc-config.txt  which pads groove circuit owns, and what they play
```

[X-PAD](xpad.md) needs no configuration, it takes the first eight files it
finds. [Groove circuit](groove-circuit.md) is the one with a config file,
because a loop has to declare its own tempo to stay locked to the grid.

Neither is touched by installing or removing the mods.

## Removing

1. **Power the unit off.**
2. Hold **CUE/LOOP CALL ◀** and **TEMPO ±6/±10/±16/WIDE** together, and **power it on**
   while holding them. The deck comes up in its service mode, and the mods are
   taken off as it starts.
3. **Reboot the unit** normally.

The mods are gone and the deck is stock. Nothing is put back, because nothing
was replaced.

There is also a **removal `.UPD`** published next to the release. Flash it the
same way you flashed the mods and it takes them off, the same rule applies, it
has to be the only `.UPD` on the stick. It is there for the case where you would
rather do it from a stick; the front panel needs nothing but the deck, so it is
still the one to reach for first.

## What happens when the deck starts

The mods look at the firmware they have landed in, and either install into all
of it or none of it.

**Installation is all-or-nothing.** If any required component is missing from your firmware, no mods are installed and the deck remains fully stock. Partial or incomplete installations are not allowed.

Possible outcomes:

| | |
| --- | --- |
| Firmware recognised | All features install. Firmware version shows `-m`. |
| Firmware not recognised | No features install. Deck remains unchanged. |

**Check the firmware version.** If the mods are active, an `-m` is added to the version number on the UTILITY screen. This suffix is also your entry point to [MOD SETTINGS](mod-settings.md).

A refused firmware is one the mods have no support for yet.

## Nothing is permanent

Installing writes to the deck, but it does not replace what is on it. The mods
are a **patch**: they sit alongside the deck's own application rather than
standing in for it. That application is still what plays your music, and the
mods are something in front of it.

Your media is not touched. The one thing that writes to your USB stick is the
playlist reorder in [browsing](browsing.md), and only while you hold EDIT on.

Neither way back needs the stock firmware:

- **Switch it off.** Every feature can be turned off in
  [MOD SETTINGS](mod-settings.md), and a feature that is off leaves the deck
  running its own code. Everything off behaves as a stock deck.
- **Take it out.** [Removing](#removing) is done on the front panel.

## What the mods never do

- Replacing a stock feature
- Nothing is uploaded anywhere else, and there is no telemetry.
- Your library is read through the deck's own database. The mods do not keep a
  second copy of it.
