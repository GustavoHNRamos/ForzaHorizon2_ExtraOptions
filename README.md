# Forza Horizon 2 - Extra Options (time, weather and music fork)

This is a fork of [RokkuDayo's Forza Horizon 2 - Extra Options](https://github.com/RokkuDayo/forzahorizon2_extraoptions). It keeps every feature of the original and adds keyboard controls for the HUD, the time of day, the weather and a local music player. **It targets Azimuth only** (built and tested with Azimuth 0.9.10); the Mezcal adapter is kept from the original but is not built or tested here.

## What this fork adds

| Action | Default key | What it does |
|---|---|---|
| ToggleHUD | F3 | Hides or shows the HUD (speedometer, minimap, notifications), like the game's own `nohud` debug option. Menus and in-car dashboard displays are not affected. |
| NextHour | F5 | Advances the time of day by one hour. |
| Weather | F6 | Cycles the weather: automatic, frozen as it is now, each weather preset, then automatic again. |
| Clock | F7 | Cycles the clock: running, stopped, following your PC's local time. |
| ClockSpeed | `[` | Cycles the clock speed: normal, 4x, 16x, 0.25x. |
| Rain | `]` | Cycles the rain: automatic, none, light, medium, heavy. |
| MusicToggle | Home | Turns the local music player on or off. |
| MusicNext | End | Skips to the next song. |

Each change shows a short pop-up in the HUD (the one Autodrive uses), in the game's text language: English, Brazilian Portuguese, German, French, Spanish (Spain and Mexico), Italian, Japanese, Traditional Chinese, Polish and Russian. Other languages fall back to English.

> **Translations wanted.** The non-English texts were written without native review, so some may sound unnatural, especially Japanese, Traditional Chinese, Russian and Polish. Corrections are welcome as issues or pull requests; every text is in `features/fork_text.c`.

Events and Bucket List challenges can override the time and weather you pick.

### Local music

The Xbox One version could play your Xbox Music playlists through the radio. That service is gone, but the code is still in the game. This fork feeds it your own songs: the game decodes them with Media Foundation and plays them through its radio, with the title and artist (from ID3 tags or the file name) in the radio pop-up.

1. Create a folder named `LocalMusic` beside `forza_durango_release_final.exe` (the game's `Mount` folder) and copy your songs into it. Subfolders are fine.
2. In the game, pick the **Xbox Music** station on the D-pad (after the last station), or press MusicToggle (Home). MusicNext (End) skips a song; changing station leaves the music player.

Limits:
- **MP3 only.** FLAC and M4A do not open in Media Foundation under Wine. Other formats depend on the system.
- **Real files inside Mount.** Azimuth refuses symbolic links and file names with accents or symbols; such files are skipped.
- The Xbox Music station is unlocked in your save, as the Xbox One game did. Without the mod it stays in the D-pad list but plays nothing.
- The game's radio volume still applies. `Volume` in `FH2_Music.ini` raises the music level (default 200%).

## Configuration

The [extras](extras) folder has the files to place beside `winmm.dll` in the Azimuth folder (where `azimuth.exe` is):

- `FH2_ExtraOptions.ini`: the original options plus the fork's `[HUD]`, `[World]`, `[Music]` and `[Keys]` sections. Each feature can be turned off there.
- `FH2_Music.ini` (optional): music file types, folders to skip, volume, and the music folder.

### Keys

Every action has a line in `[Keys]`. A key can be `None`, `F1`-`F24`, a letter or digit, a name (`Home`, `End`, `Insert`, `Delete`, `PageUp`, `PageDown`, arrows `Up`/`Down`/`Left`/`Right`, `Tab`, `Space`, `Backspace`, `Enter`, `Pause`, `ScrollLock`, `CapsLock`, `Numpad0`-`Numpad9`, `NumpadAdd`, `NumpadSubtract`, `NumpadMultiply`, `NumpadDivide`, `NumpadDecimal`), a punctuation character such as `[` (it follows your keyboard layout), or a virtual-key code such as `0x24`. `;` and `#` start a comment in the INI, so use their virtual-key codes. Avoid `Esc`, which Azimuth uses to quit.

### In-game menu

The fork's settings are also in the game's **Options > HUD** menu, below the original mod's rows: the three feature switches (restart the game to apply), the My Music volume, and a key for each action (both apply at once). Choices are written back to `FH2_ExtraOptions.ini` and `FH2_Music.ini`, so the menu and the files always agree. A key set in the INI that is not in the menu's list shows as "Other (INI)".

When the game starts, `FH2_ExtraOptions.log` lists each enabled action with the name Windows gives its key, and warns when two actions share a key.

## Installation

Same as the original, for Azimuth 0.9.10: copy `winmm.dll`, `FH2_ExtraOptions.ini`, `Gamerpic.png` and, if you want, `FH2_Music.ini` into the Azimuth folder, where `azimuth.exe` is. Keep Azimuth's own `azimuth-tls.dll` in `data\bin`; if an older Extra Options release replaced it, put back the original from the Azimuth download.

On Linux (Wine/Proton), Wine must load this `winmm.dll` instead of its own: set the DLL override `winmm=n,b`. In Lutris, add it under the game's Runner options > DLL overrides (`wine: overrides: winmm: n,b` in its YAML); a `WINEDLLOVERRIDES` environment variable is replaced by Lutris there. Restart Lutris after the change. Without the override the game starts without the mod and `FH2_ExtraOptions.log` is not written.

## How it works

Like the original, everything happens in memory; no game or Azimuth file is changed. The fork reuses what the game already has:

- **HUD**: the Opacity of the game's Hud and Notifications controls, captured from their vtables (the `nohud` debug path).
- **Time and weather**: the TimeOfDay fields and the WeatherManager requests used by the Bucket List challenges and the `timespeed`, `timestopped`, `pauseweather` and `weather*intensity` debug options, driven from ForzaSimulation's per-frame update on the game thread.
- **Music**: the radio's Xbox Music path (`RadioSystem`, `SongStream`, Media Foundation `MFCreateSourceReaderFromURL`, FMOD event `event:/Master/Radio/XboxLiveStreaming`). This build never enables the FMOD sink for it, so the fork builds that sink from the engine's FMOD Studio system. The radio's own Xbox Music station is offered while there are songs (RadioSystem slot 24) and unlocked on the D-pad like the game's intro popup did; leftover Xbox Music service errors are kept from re-showing the station pop-up.

Addresses and hooks are listed in `patches.json` (`runtime_hooks`) and commented in `features/hud_toggle.c`, `features/world_controls.c`, `features/local_music.c`, `features/key_bindings.c` and `features/fork_text.c`.

## Credits

All original features and the mod framework are by Arthur (RokkuDayo). The fork additions were researched with Ghidra and written with the help of an AI assistant (Claude), following the original's disclosure. Licensed under the GNU GPL v3, like the original.

---

# Original README

# Forza Horizon 2 - Extra Options
A quality-of-life mod for the Xbox One version of Forza Horizon 2 that adds several tweaks to the game.

# Features
 - Change your car anywhere.
 - Autodrive (as seen in Forza Horizon 6).
 - Drift Camera (as seen in Forza Horizon 4/5/6).
 - FOV sliders for every camera type.
 - Roof toggle for supported cars during free-roam.
 - Lights and turn-signals toggles for all cars.
 - New rim style category with all stock rims.
 - Custom spawn locations.
 - "mediapc" modding folder support.
 
# Enhancements
 - Intro skip.
 - All DLC packs properly unlocked.
 - VIP status & "Treasure Map" unlocks.
 - Toggle for 4K liveries w/ enhanced sampling.
 - Unrestricted Photo Mode.
 - "Return to Garage" button in the pause menu.
 - Aero tuning without equipping Forza Aero parts.
 - New rim style category with all aftermarket rims.
 - Toggle for having the minimap follow the orbit camera.
 - Bypass for Azimuth's save data checks.
 - Custom Gamerpic support.

# Information

Most of the features can now be toggled and tweaked inside the game itself through the native menus.

### Treasure Map
The treasure map is disabled by default. Open the INI and set "UnlockTreasureMap" to 1 if you wish to have it.

### Autodrive
Autodrive is engaged by pressing D-Pad Down on a controller or F2 on a keyboard.

### Vehicle Lights
Lights can be toggled by clicking the right-stick.

Turn signals can be engaged by holding D-Pad Left/Right for 1 second.

They will de-engage once a turn is made and the steering is centered.

Note: Some cars have glitched turn signals. This is not a bug with the mod, just an issue with how the assets were authored.
Cars that have been confirmed to have glitched turn signals: Focus RS '09, Corvette ZR1 '09, Carrera GT '03, Cayman GTS '15.

### Roofs
Roofs can be toggled by pressing and holding the right-stick for 3 seconds while stationary.

Note: The 3 cars with proper roof animations (BMW Z4 iDrive, Mazda MX-5 (2013), and Ferrari California) will look broken if you are using vanilla car model quality. Set "Car Detail" to High or Ultra in Azimuth.
The models will still look broken in car meets as the LOD1 is forced there. I will report this to the Azimuth developer instead of having the mod mess with that.

Note #2: This is a rather experimental feature, so it doesn't look perfect in some cars due to the way the ForzaVista animations were authored.
The Lotus Exige, for example, will have its roof floating above the car. It's beyond the view of the camera but you will spot its shadow. This will be looked into in future updates.

# Installation

The installation is different depending on the translation layer you're using.

Azimuth:

 - Extract all the 3 files from the "Azimuth" folder in this ZIP to Azimuth's main folder where "Azimuth.exe" is located.

Mezcal:

 - Extract all the 3 files from the "Mezcal" folder in this ZIP to "Forza Horizon 2\Mount" which is where "forza_durango_release_final.exe" is located.

Note: Add this command if you're running Linux and using Wine: `WINEDLLOVERRIDES="winmm.dll=n,b" %command%`

# Custom gamerpic 
 - You can edit the "Gamerpic.png" image located where you extracted the mod files to with any image you like and it will be used as your avatar in-game.
 - The maximum dimensions supported are 2048x2048 and the image must weight less than 16 megabytes.

# AI Usage Disclosure
Codex was used in this project to create the DLL. The actual research was done manually, mostly based on things I already toyed with in the other titles from the series, but I don't yet have the C language knowledge to code something like this fully from scratch (I was initially editing the raw executable). Leaving this here as I believe people have the right to know.
