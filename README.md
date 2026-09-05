# cdj3k-mods

> [!IMPORTANT]
> **This is an enhanced fork of the original [cdj3k-mods](https://github.com/nsaintot/cdj3k-mods) project.**
>
> This version includes Gate Cue fixes, UI rendering improvements, additional settings, and a second Stems implementation recreated from research into [OverCue](https://overcue.gg).

A compilation of tools and modifications for the CDJ-3000, with additional fixes and features intended to improve the overall experience.

This modification installs alongside the CDJ-3000's existing software and does not ship with any licensed AlphaTheta or Pioneer DJ firmware.

### What’s different in this fork?
Gate Cue fixes
* Corrected hold/release behavior so Gate Cue behaves consistently depending on whether the track was already playing.
* Added a Gate Cue settings option for configuring its default behavior.

UI fixes
* Fixed multiple UI rendering issues.
* Fixed cases where mod UI elements could render in incorrect views or remain visible when navigating between screens.

## Two Stems implementations
This fork includes two different Stems systems, allowing you to choose the version that best fits your setup.

"Server" Stems is from the original [cdj3k-mods](https://github.com/nsaintot/cdj3k-mods) by nsaintot. Stem separation runs on another computer on your network using [stemd](https://github.com/nsaintot/stemd), with drums, harmonics and vocals available through the deck’s controls.

OverCue Stems is an additional implementation introduced in this fork. Its functionality was recreated after studying the behaviour and research demonstrated by Sam Leone’s OverCue project.

It is improved on here being able to load stems much faster and be more stable in normal use.

> [!NOTE]
>The OverCue implementation in this repository is independently written and does not contain or redistribute OverCue source code or binaries. OverCue served as an important reference for understanding and recreating the functionality.

To actually use the OverCue's stems, download the app from https://overcue.gg and export to your music USB inside the app.

Features

* Cues — Gate Cue, Smart Cue, and Preview Hot Cue functionality, with additional Gate Cue fixes and configuration in this fork.
* Stems — choose between the original network-based Server Stems implementation or the additional OverCue-derived Stems implementation.
* X-PAD — a sampler on a touch strip with loop-length control, pitch bending, and eight samples accessible from the hot cue pads.
* Themes — six additional themes, including a true white theme.
* Browsing — reorder tracks inside a playlist directly from the deck.
* Grid Adjust — additional BPM/grid controls including doubling, halving, and fine adjustment of the beat interval.

Documentation at https://cdj3k-mods.com · [Download the latest release](https://github.com/loyahdev/cdj3k-mods/releases/tag/0.1.4)

Supported decks

Both CDJ-3000 hardware variants — Renesas and Rockchip (RK3399) — on supported firmware versions 3.13 through 3.22.

The installer refuses firmware older than 3.13. On firmware that the modifications do not fully recognize, nothing is installed and the deck runs stock.

## Before you install

This is an independent community project. It is not affiliated with, endorsed by, or connected to AlphaTheta Corporation or Pioneer DJ.

CDJ, Pioneer DJ, rekordbox, Pro DJ Link, and related trademarks belong to their respective owners. See [TRADEMARKS](https://github.com/nsaintot/cdj3k-mods/blob/main/TRADEMARKS.md).

> [!CAUTION]
>Installing modifications to your CDJ-3000 carries risk and may void your warranty.
>
> This software does not include a warranty. The authors and contributors accept no liability for hardware damage, a deck that fails to start, lost data, interrupted performances, or other problems resulting from its installation or use. [Legal](https://github.com/nsaintot/cdj3k-mods/blob/main/docs/legal.md)
>
> Install and use these modifications at your own risk.

To install and view guides on features please view the instructions at https://cdj3k-mods.com/docs/getting-started

## Credits

This project would not be possible without the work and research of the CDJ-3000 modding community.

A huge thank you to nsaintot for the original [cdj3k-mods](https://github.com/nsaintot/cdj3k-mods) project, [cdj3k-emu](https://github.com/nsaintot/cdj3k-emu), and the majority of the research and development that made this project possible.

Special thanks to Sam Leone for his work researching CDJ-3000 Gate Cue behavior and Stems functionality, as well as the creation of OverCue. Research into OverCue’s behavior helped make the independently recreated Stems implementation in this fork possible.

* [CDJ-3000 Prompt Field Guide](https://cdj3000-gate-cue-field-guide.samleone0.chatgpt.site/)
* [OverCue](https://overcue.gg)

This fork builds on the work of those projects and researchers rather than replacing or taking credit for their contributions.

## AI-assisted development

A significant portion of the modifications in this fork were developed with the assistance of AI coding tools.

AI has been used extensively for reverse engineering, analyzing firmware behavior, debugging, implementing fixes, recreating functionality from observed behaviour, and working with the project’s low-level native code. This was particularly useful because of the complexity of the CDJ-3000 software and the limited documentation available.

This entire project was tested on a real CDJ-3000 that I own rather than relying on generated code or emulated testing.

**The use of AI does not replace or diminish the work of the original developers and researchers credited above. Their projects, research, and discoveries provided much of the foundation that made the continued development of this fork possible.**

## Licence

Licensed under either of:

* Apache Licence, Version 2.0 ([LICENSE-APACHE](https://github.com/nsaintot/cdj3k-mods/blob/main/LICENSE-APACHE))
* MIT Licence ([LICENSE-MIT](https://github.com/nsaintot/cdj3k-mods/blob/main/LICENSE-MIT))

at your option.

Unless stated otherwise, any contribution intentionally submitted for inclusion in this work, as defined in the Apache-2.0 licence, is dual licensed as above, with no additional terms or conditions.
