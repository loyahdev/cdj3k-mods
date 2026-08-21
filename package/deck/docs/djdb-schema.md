# djdb — the rekordbox media library, as the deck holds it

Read live off a CDJ-3000 (EP122 3.19) by walking djdb's own table registry from
inside the eject flush. Not from the `.pdb` file format, and not from statics:
`mods/db/djdb.c` dumps this at runtime, so it is what the deck actually has.

## How to get it again

The registry is a 13-bucket hash on the djdb context (`ctx+0x08`), and the
context is **only live inside a djdb operation** — an idle thread always sees
NULL. The one operation that reliably happens is the **eject flush**, so the dump
hangs off a hook on djdb's page writer. Mount the media, eject it, read the log:

```bash
scripts/emuctl.py media-attach /path/to/usb.img
scripts/emuctl.py media-eject
```

Per table: name is a packed string at `table+0x10`, column count at `table+0x04`,
columns at `table+0x18` at **32 bytes each**, next-in-chain at `table+0x78`.
Per column: **name is a packed string at `col+0x00`**, a type-object pointer at
`col+0x08` (its `+0x08` is the type id), and an on-disk field index at `col+0x18`.

74 tables. The ones that matter so far:

## DJDBSONGPLAYLIST — playlist membership and order

| # | name | type | field idx |
|---|---|---|---|
| 0 | `PLAYLISTID` | 1 (int) | 2 |
| 1 | `CONTENTID` | 1 (int) | 1 |
| 2 | **`TRACKNO`** | 1 (int) | 0 |

**`TRACKNO` is the order key on media.** Note it is NOT called `sequenceNo` —
that is the name in the deck's own SQLCipher library, a different back end for
different media. Same concept, different table, different name; do not carry one
name across to the other.

## DJDBPLAYLIST — the playlists themselves

`ID, SEQ, NAME, IMAGEID, ATTRIBUTE, PARENTID` — types `1 1 19 1 15 1`.
`PARENTID` gives the folder tree; `SEQ` orders the playlists, as `TRACKNO`
orders the tracks inside one.

## DJDBCONTENT — the track table, 50 columns

```
 0 ID                 1 FOLDERPATH        2 FILENAMEL         3 FILENAMES
 4 TITLE              5 ARTISTID          6 ALBUMID           7 GENREID
 8 BPM                9 LENGTH           10 TRACKNO          11 BITRATE
12 BITDEPTH          13 COMMNT           14 FILETYPE         15 RATING
16 RELEASEYEAR       17 REMIXERID        18 LABELID          19 ORGARTISTID
20 KEYID             21 STOCKDATE        22 COLORID          23 DJPLAYCOUNT
24 IMAGEID           25 MASTERDBID       26 MASTERCONTENTID  27 ANALYSISDATAPATH
28 SEARCHSTR         29 FILESIZE         30 DISCNO           31 COMPOSERID
32 SUBTITLE          33 SAMPLERATE       34 PROTECTED        35 ANALYSED
36 RELEASEDATE       37 DATECREATED      38 CONTENTLINK      39 TAG
40..49 RESERVED1..RESERVED10
```

types `1 19 19 19 19 1 1 1 1 5 1 1 5 19 11 11 5 1 1 1 1 19 11 5 1 1 1 19 19 1 5 1 19 1 11 11 19 19 1 19 19 19 19 19 19 19 19 19 19 19`

**`BPM` is column 8, type 1 (integer), stored as BPM×100** — the target for a
×2 / ÷2 modifier. So ×2 on a 126.0 track is `12600 -> 25200`, ÷2 is `6300`.

CONFIRMED against the media rather than assumed: scanning the tracks pages of
`export.pdb` for plausible tempo values returns **only** 12000, 12500, 12600 and
6800 — exactly the BPMs the browser shows for those tracks (120.0 High Priestess,
125.0 Bells Of Eternity, 126.0 Rabbit Hole) and nothing spurious. That was
host-side analysis of a copy to pin a constant; it is NOT how a mod reads or
writes the library, which still goes through the deck's own objects.

`ANALYSISDATAPATH` is the ANLZ sidecar, which is where the beat grid lives — so
a grid change and a BPM change are not the same edit.

## Type ids seen

`1` integer · `2` (in DJDBBPMRANGE, likely float) · `3` · `5` · `11` · `15` ·
`16` · `17` · `19` string. From `sub_1c66f80`, ids **3..21 take a conversion
path** on insert and others are stored as the raw pointer, so a writer has to
match the column's own type rather than assume an integer.

## Writing

Do not write the file. The deck buffers a whole session in RAM and flushes at
eject, so the way to change any of the above is to change the **in-memory model**
and let the deck's own flush persist it. See `mods/db/db.h` for the rule and
`mods/db/pdbwatch.c` for the measurement behind it.
