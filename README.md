# OBS Forever Dungeon Live Map

An OBS Studio plugin that shows where your character is inside a dungeon or a raid of WoW: Forever, on a live map, by reading the minimap from the game capture.

## How it works

- A filter goes on the source that captures the game. You mark once where the minimap is.
- You pick the dungeon from a list before going in.
- Twice a second the filter cuts the minimap out of the captured frame and looks for that picture among the rooms of the dungeon. Where it fits is where you are.
- A source draws the map of the dungeon with a dot and an arrow where you are. When the minimap matches no place, the dot goes: it does not guess.
- The maps are built from your own game files, read only, the first time the plugin meets a build of the game. Nothing from the game is shipped with the plugin.

## What it does not do

It does not read or change the memory of the game, inject anything into it, send keys or clicks, or write any file of the game. It only looks at the frames OBS already captures, like any stream overlay, and reads the game's files the way the game itself does.

## Requirements

- Windows 10 or 11, 64 bit.
- OBS Studio 31 or later.
- World of Warcraft: Forever installed with the Battle.net app.

## Install

1. Close OBS.
2. Copy the folder `obs-forever-dungeon-live-map` into `C:\ProgramData\obs-studio\plugins\`, so that you have:
   - `C:\ProgramData\obs-studio\plugins\obs-forever-dungeon-live-map\bin\64bit\obs-forever-dungeon-live-map.dll`
   - `C:\ProgramData\obs-studio\plugins\obs-forever-dungeon-live-map\data\locale\en-US.ini`
3. Start OBS.

## Set it up

1. **Capture the game.** Add a *Game Capture* source for WoW: Forever, if you have none yet.
2. **Add the filter.** Right click the game capture, *Filters*, then **+** under *Effect Filters* and choose *Dungeon Live Map*. The first time, the plugin lays the rooms of every dungeon from your game files: it takes some seconds, and the dungeon list fills in once it is done (close and open the filter's properties again).
3. **Mark the minimap.** In the filter, tick *Show the minimap square over the game*. A green square and ring appear over the game in the filter's preview. Move the sliders *left*, *top* and *side* until the sides of the square touch the round minimap. Then untick the box.
4. **Pick the dungeon and the zoom.** Choose the dungeon or raid from the list. Keep the minimap at its widest zoom in the game and choose zoom *1* in the filter: the other zooms are not measured yet.
5. **Add the map.** In your scene, add a *Dungeon Map* source, and scale and place it as you like. It follows the first Dungeon Live Map filter by itself.
6. **Hide the game if you want the map alone.** Turn off the eye of the game capture in the scene, but keep it there: the map keeps it running out of sight.

Go into the dungeon: the dot follows you. Change the dungeon in the filter before entering another one.

If the plugin does not find the game, set *World of Warcraft folder* in the filter to the folder that holds `_forever_`.

## Build

Windows, Visual Studio 2022 or later with the C++ tools, CMake 3.28 or later:

```
cmake --preset windows-x64
cmake --build --preset windows-x64
```

The first configure downloads and builds the OBS sources it needs (a few minutes).

## Status

Works in the dungeons tried so far, at the widest zoom of the minimap. Not released yet.

## License

GPL-3.0-or-later. The minimap matching and the reading of the game files come from [Twinmap Forever](https://github.com/catubba/twinmap-forever).

Not affiliated with or endorsed by Blizzard Entertainment.
