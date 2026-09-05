cdj3k-mods

[!NOTE]
This is an enhanced fork of the original cdj3k-mods project.
This version includes hardware-tested fixes and improvements to Gate Cue, UI rendering, additional Gate Cue settings, and a second Stems implementation independently recreated from research into OverCue.

A compilation of tools and modifications for the CDJ-3000, with additional fixes and features intended to improve the overall experience.

The modifications install alongside the CDJ-3000’s existing software rather than replacing its firmware and can be removed from the front panel.

What’s different in this fork?

This fork builds on the original cdj3k-mods project while adding fixes and additional functionality developed and tested on real CDJ-3000 hardware.

Gate Cue fixes

* Fixed Gate Cue behavior and playback-state bugs.
* Corrected hold/release behavior so Gate Cue behaves consistently depending on whether the track was already playing.
* Fixed cases where releasing a cue could incorrectly stop playback or return to the cue point.
* Added a Gate Cue settings option for configuring its default behavior.

UI fixes

* Fixed multiple UI rendering issues.
* Improved integration of added controls with the CDJ-3000’s native interface.
* Fixed cases where mod UI elements could render in incorrect views or remain visible when navigating between screens.

Two Stems implementations

This fork includes two different Stems systems, allowing you to choose the implementation that best fits your setup.

Server Stems is the original cdj3k-mods implementation. Stem separation runs on another computer on your network using stemd, with drums, harmonics and vocals available through the deck’s controls.

OverCue-derived Stems is an additional implementation introduced in this fork. Its functionality was independently recreated after studying the behavior and research demonstrated by Sam Leone’s OverCue project.

The OverCue-derived implementation in this repository is independently written and does not contain or redistribute OverCue source code or binaries. OverCue and the accompanying CDJ-3000 research served as an important reference for understanding and recreating the functionality.

Features

* Cues — Gate Cue, Smart Cue, and Preview Hot Cue functionality, with additional Gate Cue fixes and configuration in this fork.
* Stems — choose between the original network-based Server Stems implementation or the additional OverCue-derived Stems implementation.
* X-PAD — a sampler on a touch strip with loop-length control, pitch bending, and eight samples accessible from the hot cue pads.
* Themes — six additional themes, including a true white theme.
* Browsing — reorder tracks inside a playlist directly from the deck.
* Grid Adjust — additional BPM/grid controls including doubling, halving, and fine adjustment of the beat interval.

Documentation at cdj3k-mods.com · Download the latest release

Supported decks

Both CDJ-3000 hardware variants — Renesas and Rockchip (RK3399) — on supported firmware versions 3.13 through 3.22.

The installer refuses firmware older than 3.13. On firmware that the modifications do not fully recognize, nothing is installed and the deck runs stock.

Repository

package/  The mods themselves, Stems components, and the .UPD installer.
          package/deck/docs/mods.md is the developer entry point.
docs/     Project documentation in Markdown.
web/      Documentation website built with Vue 3 + Vite.

Before you install

This is an independent community project. It is not affiliated with, endorsed by, or connected to AlphaTheta Corporation or Pioneer DJ.

CDJ, Pioneer DJ, rekordbox, Pro DJ Link, and related trademarks belong to their respective owners. See TRADEMARKS.

[!CAUTION]
Installing modifications to your CDJ-3000 carries risk and may void your warranty.

This software is provided without warranty. The authors and contributors accept no liability for hardware damage, a deck that fails to start, lost data, interrupted performances, or other problems resulting from its installation or use.

Install and use these modifications at your own risk.

See legal for additional information.

Credits

This project would not be possible without the work and research of the CDJ-3000 modding community.

A huge thank you to nsaintot for the original cdj3k-mods project, cdj3k-emu, and the majority of the foundational research and development that made this project possible.

Special thanks to Sam Leone for his work researching CDJ-3000 Gate Cue behavior and Stems functionality, as well as the creation of OverCue. Research into OverCue’s behavior helped make the independently recreated Stems implementation in this fork possible.

* CDJ-3000 Gate Cue Field Guide
* OverCue

This fork builds on the work of those projects and researchers rather than replacing or taking credit for their contributions.

AI-assisted development

A significant portion of the modifications in this fork were developed with the assistance of AI coding and reasoning tools.

AI assistance has been used extensively for reverse engineering, analyzing firmware behavior, debugging, implementing fixes, recreating functionality from observed behavior, and working with the project’s low-level native code. This was particularly useful because of the complexity of the CDJ-3000 software and the limited documentation available for its internal systems.

Where practical, changes have been reviewed and tested on real CDJ-3000 hardware rather than relying solely on generated code or static analysis.

The use of AI does not replace or diminish the work of the original developers and researchers credited above. Their projects, research, and discoveries provided much of the foundation that made the continued development of this fork possible.

Licence

Licensed under either of:

* Apache Licence, Version 2.0 (LICENSE-APACHE)
* MIT Licence (LICENSE-MIT)

at your option.

Unless stated otherwise, any contribution intentionally submitted for inclusion in this work, as defined in the Apache-2.0 licence, is dual licensed as above, with no additional terms or conditions.