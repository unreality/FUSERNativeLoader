# FUSERNativeLoader {{VERSION}}

Extensions for FUSER, delivered as a replacement `winhttp.dll` (the community FuserSongLoader, with the FuserNative
features built in - derived from UE4SS) plus a small content pak.

## What it adds

- **Key and BPM** on every song item when playing: the key/BPM label swaps with Genre / Year depending on preference
  - Controlled by Settings > General > *Key and BPM Display*: On idle / On hover / Off.
- **Key Labels**: show keys as Flats (Db,default), Sharps (C#), or Both (C#/Db).
- **Larger crates** in the freestyle mode for crates with more than 24 songs.
- **Key and BPM on the crate editor's song tiles**, above each song's title (hidden when *Key and BPM Display* is Off).
- **Song search** in the crate editor's song library 
  - Controlled by Settings > General > *Song Search Bar* on / off.
  - Filter words narrow the list by key, tempo or genre, alone or with a title / artist search:
    - `key:Am` (A minor), `key:C` (C major or minor), `key:Cmaj` (C major only), `key:F#m,Gb` (several keys);
      sharps as `#`, flats as `b`;
    - `bpm:120`, `bpm:90-110`, `bpm:140-` (140 or faster), `bpm:-100` (100 or slower);
    - `genre:rock`, `genre:pop,hiphop` (several genres). Genres: `classical`, `country`, `dance` (or `edm`),
      `hiphop` (or `rap`), `latin`, `pop`, `rnb` (or `r&b`), `rock`, `indie` (or `alternative`).
- **Sort by Key or BPM** in the crate editor's song library, from the Sort dropdown. Key sorts C to B, major before
  minor; songs without a known key or BPM go last.
- **Edit larger crates**: as many song and snapshot tabs as the crate holds (e.g. 8 song tabs for 64 songs).
- **New crate sizes**: No need to edit `Game.ini`, snaps to multiples of 8. Maximum of 144. Controls **new** 
  crates only, existing crates are untouched.
  - Controlled by Settings > General > *New Crate Song Slots* and *New Crate Snapshot Slots*
- Everything the existing custom song loader does (adds `custom_songs`, free to play, and the event-reward songs). 
  `customSongsUnlocked_P.pak` is no longer created or needed, so you no longer need to delete this file when adding new 
  custom songs. If an existing copy is found, it is renamed to `customSongsUnlocked_P.pak.disabled` (and `.sig.disabled`)
- **Multiplayer support**: By setting the `Game.ini` config to connect to a `fuserd` instance, users can join and play freestyle coop on its servers.
- **Shared mixes with custom songs**: when you play another player's mix, the custom songs it uses that you don't
  have are downloaded first, with a "Custom Songs" popup showing the progress. Songs that can't be downloaded are
  named afterwards.
  - Controlled by Settings > General > *Download Mix Songs*: No / Temporarily (default: kept until the game is
    closed) / Permanently (added to your library).

## Requirements

- One of the supported FUSER builds:
  - **Epic Games Store** v1.4.2 or the later (2023) build (`FuserEOS-Win64-Shipping.exe`)
  - **Steam** (`Fuser-Win64-Shipping.exe`)

  On any other executable the FuserNative features and the custom / event song unlocks switch themselves off (custom
  song paks still mount); see `FuserNative.log`.
- Do **not** run it together with UE4SS: remove UE4SS's `dwmapi.dll` first.

## Install

Copy the `Fuser` folder from this package into the game's install folder - the folder that already contains a
`Fuser` folder - merging with it and replacing `winhttp.dll` if you already use the song loader. That installs:

- `Fuser/Binaries/Win64/winhttp.dll`
- `Fuser/Binaries/Win64/FUSERMultiplayer.dll` (optional; remove it to disable the multiplayer hooks)
- `Fuser/Content/Paks/FUSERNativeMods_P.pak` and `.sig`

If an earlier FuserNative build is installed, delete its mod pak from `Fuser/Content/Paks` first (the
`*_FuserNative_P.pak` / `.sig` pair).

Settings are saved in `Fuser/Binaries/Win64/FuserNative.ini`; a log is written to
`Fuser/Binaries/Win64/FuserNative.log`.

## Uninstall

Delete the installed DLLs and pak files (and `FuserNative.ini` / `FuserNative.log`), or put back your previous `winhttp.dll`.

## AI Usage Disclosure

AI/LLM was used heavily in the production of this loader and mods.

## Credits and licence

The mod loader is FuserSongLoader (MIT, (c) 2020 Mettra - see `LICENSE.FuserSongLoader`); function hooking by
funchook (GPLv2+ with a linking exception). FuserNative and its reflection core (fue) are part of
FUSERNativeLoader, and were derived from UE4SS.
