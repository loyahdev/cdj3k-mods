# Browsing

One change to the browse screen: you can drag a track to a new position in a
playlist, and your stick keeps the new order.

## The EDIT toggle

The browse screen uses single-finger dragging to scroll through lists. To enable reordering tracks, there is an **EDIT** toggle in the browse header. When EDIT is on, you can drag tracks to new positions; when it is off, dragging scrolls the list as usual. The EDIT toggle is located immediately to the left of **PREVIEW** and lights up when active.

## Reordering a playlist

1. Open a **playlist**.
2. Tap **EDIT** in the browse header.
3. **Drag a track** to where you want it.
4. Tap **EDIT** again when you are done.

While EDIT is on, a drag moves a track. While it is off, the list scrolls
exactly as it always did.

![EDIT off: an ordinary playlist, sorted however you had it.](img/browse-edit-off.png)
![EDIT on and a track mid-drag, on its way to a new position.](img/browse-edit-on.png)

## Playlists only

A playlist's `#` is a stored position that belongs to that list, so moving a
track in it changes that list.

> **Known limitation.** EDIT currently appears on **any** track list, not only
> on playlists. Dragging somewhere it cannot be saved is refused at the point
> of writing, so nothing is corrupted. The move simply does not stick. The
> work to hide the toggle where it does not apply is not finished.