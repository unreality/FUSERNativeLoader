# FUSERNativeLoader {{VERSION}}

A mod loader for FUSER, delivered as a replacement `winhttp.dll` plus a bundled set of mods. Every mod is optional: keep the ones you want, turn off or
delete the rest (see [Turning mods off and removing them](#turning-mods-off-and-removing-them)).

Mod settings are on the Settings screen's **Mods** tab, under each mod's heading. Settings > About FUSER lists every
installed mod and whether it loaded.

## Requirements

- One of the supported FUSER builds:
  - **Epic Games Store** v1.4.2 or the later (2023) build (`FuserEOS-Win64-Shipping.exe`)
  - **Steam** (`Fuser-Win64-Shipping.exe`)

  On any other executable the mods and the custom / event song unlocks switch themselves off (custom song paks still
  mount); "About FUSER" in the settings menu shows each mod as "not for this game build", and `FuserNative.log` says why.
- Do **not** run it together with UE4SS: remove UE4SS's `dwmapi.dll` first.

## Installation

Copy the `Fuser` folder from this package into the game's install folder - the folder that already contains a
`Fuser` folder - merging with it and replacing `winhttp.dll` if you already use the song loader. That installs:

- `Fuser/Binaries/Win64/winhttp.dll`: the loader
- `Fuser/Binaries/Win64/mods/<Name>/`: each mod, its DLL and its `mod.ini`
- `Fuser/Content/Paks/mods/`: the paks of the mods that have one (`FUSERQualityOfLifeImprovements_P.pak` and
  `FUSERFifthDeck_P.pak`, each with a `.sig`)

Files an earlier version left behind - `FUSERMultiplayer.dll` beside `winhttp.dll` and
`Fuser/Content/Paks/FUSERNativeMods_P.pak` / `.sig` - are renamed to `.disabled` at the next start.

If an earlier FuserNative build is installed, delete its mod pak from `Fuser/Content/Paks` first (the
`*_FuserNative_P.pak` / `.sig` pair).

Settings are saved in `Fuser/Binaries/Win64/FuserNative.ini`; a log is written to
`Fuser/Binaries/Win64/FuserNative.log`.

The directory layout should look similar to this when installed:

FUSER/
├Engine/
└Fuser/
 ├── Binaries/
 │   └── Win64/
 │       ├── winhttp.dll                          
 │       └── mods/
 │           ├── FUSERBpmRange/
 │           │   ├── FUSERBpmRange.dll               
 │           │   └── mod.ini
 │           ├── FUSERFifthDeck/
 │           │   ├── FUSERFifthDeck.dll                    
 │           │   └── mod.ini
 │           ├── FUSERFixes/
 │           │   ├── FUSERFixes.dll                       
 │           │   └── mod.ini
 │           ├── FUSERMultiplayer/
 │           │   ├── FUSERMultiplayer.dll                  
 │           │   └── mod.ini
 │           └── FUSERQualityOfLifeImprovements/
 │               ├── FUSERQualityOfLifeImprovements.dll    
 │               └── mod.ini
 └── Content/
     └── Paks/
         └── mods/
             ├── FUSERFifthDeck_P.pak                      
             ├── FUSERFifthDeck_P.sig
             ├── FUSERQualityOfLifeImprovements_P.pak
             └── FUSERQualityOfLifeImprovements_P.sig

## Components

### The loader - `winhttp.dll`

Needed by everything else; it has no settings of its own.

- Everything the existing custom song loader does: adds `custom_songs`, free to play, and the event-reward songs.
  `customSongsUnlocked_P.pak` is no longer created or needed, so you no longer need to delete this file when adding
  new custom songs. If an existing copy is found, it is renamed to `customSongsUnlocked_P.pak.disabled` (and
  `.sig.disabled`).
- Custom content (such as projector screens) from `Fuser/Content/Paks/custom_content/`.
- Loads the mods in `Fuser/Binaries/Win64/mods/`, adds the Settings screen's **Mods** tab and the mod list in About
  FUSER.

### Quality of Life Improvements - `FUSERQualityOfLifeImprovements`

- **Key and BPM** on every song item when playing: the key/BPM label swaps with Genre / Year depending on preference
  - Controlled by Settings > Mods > *Key and BPM Display*: On idle / On hover / Off.
- **Key Labels**: show keys as Flats (Db, default), Sharps (C#), or Both (C#/Db).
- **Larger crates** in the freestyle mode for crates with more than 24 songs.
- **Key and BPM on the crate editor's song tiles**, above each song's title (hidden when *Key and BPM Display* is Off).
- **Song search** in the crate editor's song library
  - Controlled by Settings > Mods > *Song Search Bar* on / off.
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
  - Controlled by Settings > Mods > *New Crate Song Slots* and *New Crate Snapshot Slots*
- **Indie/Alternative songs in live requests**: custom songs using the community's tenth genre no longer crash the
  multiplayer requests screen.

### Fifth Deck - `FUSERFifthDeck`

- **A fifth deck** on a widened desk, in freestyle and multiplayer sets. Another player's fifth deck plays for you
  in multiplayer even while yours is off.
  - Controlled by Settings > Mods > *Fifth Deck* (off by default; it can be switched on stage).
- **Hot Clip Volume Normalisation**: stops Hot Clips staying at the quieter preview volume after you audition a song.
  - Controlled by Settings > Mods > *Hot Clip Volume Normalisation* (off by default).

### Multiplayer - `FUSERMultiplayer`

- **Multiplayer improvements**: By setting the `Game.ini` config to connect to a `fuserd` instance, users can join and
  play freestyle coop on its servers, with its store, community songs and server content.
  - Settings > Mods > *Download Songs*: whether songs from the server's store are downloaded No / Temporarily /
    Permanently (default).
- **Shared mixes with custom songs**: when you play another player's mix, the custom songs it uses that you don't
  have are downloaded first, with a "Custom Songs" popup showing the progress. Songs that can't be downloaded are
  named afterwards.
  - Controlled by Settings > Mods > *Download Mix Songs*: No / Temporarily (default: kept until the game is
    closed) / Permanently (added to your library).
- **Songs from other players**: in multiplayer, a custom song in someone's crate that the server doesn't have can be
  sent to you by a player who has it. Songs are checked before they're loaded, and anything that isn't plain song
  data is refused.
  - Controlled by Settings > Mods > *P2P Song Download*: Off / From Followed (default: only players you follow) /
    Always (anyone in the session). Players you've blocked are never asked.
  - Settings > Mods > *Send Songs to Other Players* lets your game send your custom songs the same way, with
    *Upload Speed Limit* and *Simultaneous Uploads* to limit it.

Everything this mod adds works through a `fuserd` server, so players who don't play online can remove it.

### BPM Range - `FUSERBpmRange`

- **A wider BPM range**: a set's tempo can go from 60 to 200 BPM instead of 90 to 157. No settings.

### Fixes - `FUSERFixes`

- **EOS exit crash guard**: the Epic Games Store build's online SDK (EOS) can crash while the game is closing, which
  brings up the crash reporter. This catches that. No settings.

It only does anything on the Epic Games Store builds (`FuserEOS-Win64-Shipping.exe`). The Steam build doesn't use EOS
and doesn't have the bug, so Steam players can remove it.


## Turning mods off or removing them

No mod in this bundle requires another, so any of them can be turned off or removed. Changes take effect the next time the
game starts.

**To turn a mod off** (and keep it to turn back on later), either:

- open its `mod.ini` in `Fuser/Binaries/Win64/mods/<Name>/` and change `enabled=1` to `enabled=0`, or
- rename its folder to end in `.disabled`, e.g. `mods/FUSERFifthDeck.disabled`.

A mod that's off has none of its features and no heading on the Mods tab; About FUSER lists it as "disabled". Its
pak, if it has one, can stay where it is. To turn it back on, set `enabled=1` or rename the folder back.

**To remove a mod** you don't need, delete its folder from `Fuser/Binaries/Win64/mods/` and, for Quality of Life
Improvements and Fifth Deck, its `_P.pak` and `_P.sig` from `Fuser/Content/Paks/mods/`. For example:

- Steam players: delete `mods/FUSERFixes/` (the bug it fixes is only in the Epic Games Store build).
- Players who don't play online: delete `mods/FUSERMultiplayer/`.

Settings from a removed mod saved stay in `FuserNative.ini`, unused, and come back if you reinstall it.

**When updating**, copying a new package over the old one puts back every mod, enabled: delete or turn off the
ones you don't want again (a new `mods/<Name>/` folder loads even when a `<Name>.disabled` folder is beside it, so
delete the old one).

## Uninstall

Delete `winhttp.dll`, the `Fuser/Binaries/Win64/mods/` folder and the `Fuser/Content/Paks/mods/` folder (and
`FuserNative.ini` / `FuserNative.log`), or put back your previous `winhttp.dll`.

## AI Usage Disclosure

AI/LLM was used heavily in the production of this loader and mods.

## Credits and licence

The mod loader is FuserSongLoader (MIT, (c) 2020 Mettra - see `LICENSE.FuserSongLoader`); function hooking by
funchook (GPLv2+ with a linking exception). The mods and the loader's reflection core (fue) are part of
FUSERNativeLoader, and were derived from UE4SS. The multiplayer mod uses libarchive (see `LICENSE.libarchive`) and
zlib (see `LICENSE.zlib`).
