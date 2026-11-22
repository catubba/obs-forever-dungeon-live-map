# Changelog

## 1.0.0 (2026-11-22)

First release.

- A *Dungeon Live Map* filter for the game capture: twice a second it cuts the minimap out of the frame and finds it among the rooms of the dungeon, on a thread of its own, so OBS never waits for it.
- A *Dungeon Map* source: the rooms of the level you are on, in their colors, with a dot and an arrow where you are. When the minimap matches no place, the dot goes: it does not guess.
- The rooms of every dungeon and raid are laid from your own game files, read only, the first time the plugin meets a build of the game. Nothing from the game is shipped with the plugin.
- The dungeon and the zoom are chosen from lists; the minimap square is set by eye, with sliders, over the game.
- The map keeps the game capture running out of sight, so a scene, or a projector on a second monitor, can show the map alone.
- Everything happens inside OBS: the game's process is never opened, no input is sent, no network is used.

Known limits: only the widest zoom of the minimap (zoom 1) is measured; the open ground of a dungeon is not drawn yet.
