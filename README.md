# OBS Forever Dungeon Live Map

An OBS Studio plugin that shows where your character is inside a dungeon or a raid of WoW: Forever, on a live map, by reading the minimap from the game capture.

## How it works

- Add the filter to the source that captures the game and mark where the minimap is, once.
- Before going in, pick the dungeon from a list.
- Several times a second the plugin cuts the minimap out of the captured frame and looks for that picture among the rooms of the dungeon. Where it fits is where you are.
- The dungeon maps are built from your own game files, read only. Nothing from the game is shipped with the plugin.

## What it does not do

It does not read or change the memory of the game, inject anything into it, send keys or clicks, or write any file of the game. It only looks at the frames OBS already captures, like any stream overlay.

## Status

Early work, not usable yet.

## License

GPL-3.0-or-later. The minimap matching comes from [Twinmap Forever](https://github.com/catubba/twinmap-forever).

Not affiliated with or endorsed by Blizzard Entertainment.
