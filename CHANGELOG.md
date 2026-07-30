# Changelog

## 1.0.0 - 2026-07-29

First public release.

**What it does**

- Walk up to any NoA computer terminal and ask for the storage network. It
  searches every communal locker your base can reach at once.
- Type to search by item name. Page Up / Page Down jump between the lockers
  that hold what you searched for, nearest first.
- Results name the locker and its distance, so "where is my copper" has an
  answer without opening twenty containers.
- Read-only by design: the mod never moves, creates, or deletes items. It opens
  the game's own storage screens through the game's own interact function, so it
  is multiplayer-safe and cannot corrupt a save.

**Fixes in this release**

- Switching lockers with Page Up / Page Down now works. It previously popped the
  open screen and then failed to open the next one, which could leave the game
  with no screen up and an unresponsive mouse.
- Escape now lets the game close its own screen instead of racing it, which was
  the other way the mouse could be left stuck.
- The terminal dialogue option no longer goes blank or disappears after
  returning to the main menu and reloading a save.

**Performance**

- Large reduction in per-frame and per-keystroke work: the mod no longer walks
  the entire engine object list several times per keypress, and item names are
  resolved once per item type instead of once per stored item.
