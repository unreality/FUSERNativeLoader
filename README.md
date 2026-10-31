# FUSERNativeLoader

The FUSER mod loader (a `winhttp.dll` proxy, based on the community FuserSongLoader) with a minimal UE 4.24
reflection core (**fue**), and the mods it loads. The loader itself holds no game features: it bypasses the signature
check for `*_P` paks, loads the mods in `mods/` (see Mods), gives them the Settings screen's **Mods** tab, and keeps
the free unlocks for custom and event songs and custom content. The features are bundled mods:

| Output | What |
|---|---|
| `winhttp.dll` | the loader |
| `mods/FUSERQualityOfLifeImprovements/` | **FuserNative**: key/BPM on the HUD song items and the crate editor's song tiles, Key Labels, real song tabs, song search with `key:` / `bpm:` / `genre:` filter words, Key and BPM sorting, crate-edit tabs, new-crate slot sizes, genre 9 in live requests; its pak holds the cooked Blueprint assets (`/Game/Mods/FuserNative/`) |
| `mods/FUSERFifthDeck/` | the fifth deck and Hot Clip Volume Normalisation; its pak holds the five-deck desk |
| `mods/FUSERMultiplayer/` | the multiplayer client (`src/multiplayer/README.md`) |
| `mods/FUSERBpmRange/` | the BPM range widened from 90-157 to 60-200 |
| `mods/FUSERFixes/` | engine and SDK fixes (the EOS_Shutdown exit guard) |
| `Paks/mods/<Name>_P.pak` (+ `.sig`) | the mods' cooked assets, installed to `Fuser/Content/Paks/mods/` |

## AI Usage Disclosure

This project was coded predominantly using an LLM.

## Using the mod

If you just want to use the mod, go to releases and download the latest version. All information below regarding requirements
are for building/development on the mod.

## Requirements

- Windows, **Visual Studio 2022** (MSVC x64, C++20) and **CMake 3.20+** (e.g. from a Developer PowerShell).
- **Python 3** for the pak tool (standard library only).
- Optional, for `tools/derive_build.py`, `tools/derive_mp_build.py` and `tools/derive_fifth_deck_build.py`: `pefile`, `capstone`, `numpy`.

## Build

```powershell
.\build.ps1                          # out\Release\: winhttp.dll, mods\<Name>\, Paks\mods\ (+ PDBs)
.\build.ps1 -Configuration Debug     # out\Debug\: the diagnostic DLLs, same paks
.\build.ps1 -Package                 # also out\FUSERNativeLoader-<version>.zip (+ -symbols.zip)
```

The **Debug** configuration is the diagnostic build (`FNL_DEBUG`): a console with the loader's tracing, the F6-F10
debug keys (F6 key/BPM self-test, F7 song probe, F8 crate-edit and settings dumps, F9 fue probe, F10 unlock state,
Ctrl+Shift+5 fifth-deck layout and state) and the probes. Both configurations are optimised. The version comes from `VERSION`.

## Install for testing

```powershell
$env:FUSER_GAME_ROOT = '<folder containing Fuser\>'
.\install.ps1 [-Configuration Debug]   # keeps the previous winhttp.dll as winhttp.dll.fnl-backup
.\install.ps1 -Restore
```

Settings: `Fuser\Binaries\Win64\FuserNative.ini`; log: `Fuser\Binaries\Win64\FuserNative.log`.
`ExtraSortOptions=0` in the ini removes Key and BPM from the crate editor's Sort dropdown (no Settings row).
Set `enabled=0` in `Fuser\Binaries\Win64\mods\FUSERMultiplayer\mod.ini` (or rename the folder to end in `.disabled`) to
run the other mods without multiplayer hooks; every bundled mod turns off the same way.
Custom content paks (`tools/fuser_content_pak.py`) go in `Fuser\Content\Paks\custom_content\`, next to `custom_songs\`.
Server content (fuserd's items, kept by the multiplayer DLL) lives in `Fuser\Content\ServerContent\` (see
`src/multiplayer/README.md`).

## Mods

`winhttp.dll` loads the mods in `Fuser\Binaries\Win64\mods\<Name>\`, each described by a `mod.ini` (keys in
`src/loader/mod_manager.cpp`) and built against `sdk/fnl_mod.h`. A mod's `fnl_mod_init` runs at its stage: `early`
(inside the loader's DllMain, before any pak is mounted) or `game` (the first engine callback, before the unreal-init
callbacks). It gets the loader's logging, pattern search, hooks, code patches, import redirects, engine events and
`FuserNative.ini` values; code it changes through them is recorded, so every pattern search still sees the game as
shipped. A mod built on fue (the C++ tier, `sdk=` in its `mod.ini`) must match the loader's `FNL_SDK_VERSION`.
`FuserNative.log` lists each mod as it loads, or why it didn't; Settings > About FUSER lists every installed mod.
Writing a mod: `sdk/README.md` (the SDK: headers, `lib\winhttp.lib` from `out\<cfg>\lib\`, a template mod; also packed
as `out\FUSERNativeLoader-<version>-sdk.zip` by `build.ps1 -Package`). The design: `MOD_LOADER.MD`. Checks:
`tests/mod_loader/`.

## Layout

| Path | |
|---|---|
| `src/dllmain.cpp` | the loader (proxy exports, pak-signature bypass, legacy unlock-pak rename) |
| `src/fnl_entry.cpp` | `fnl_start`: fue, the mods' early stage, the Mods tab, unlocks, custom content; the debug fue probe |
| `src/loader/` | the mod loader: `mod_manager.cpp` (manifests, load order, stages, the `FnlApi` table), `code_patch.cpp` (pattern search, hooks, patches and import redirects, with the patch journal) and `settings.cpp` (the settings registry and the Settings screen's Mods tab) |
| `sdk/` | the SDK: `fnl_mod.h` (the C tier), the C++ tier's headers (`fue/fue.hpp`, `data_table.hpp`, `custom_content.hpp`, `content/content_types.hpp`, `unlocks.hpp`, `json_lite.hpp`; `FNL_API` marks what `winhttp.dll` exports), `README.md` (writing a mod) and `template/` |
| `src/fue/` | the reflection core (`fue.cpp`; its header is `sdk/fue/fue.hpp`): objects, properties, ProcessEvent / tick / native hooks, per-build addresses |
| `src/unlocks.cpp` | custom and event song unlocks, patched into the live `DT_UnlocksSongs` at startup |
| `src/custom_content.cpp` | custom and server content (`custom_content.hpp`): the content pak reader, the item registry, the startup registration (`custom_content/` free, server content locked) and the live one |
| `src/content/` | the content types (`content_types.cpp`: one handler per type; `content_projector.cpp`: projector screens) and the unlock rows every type shares (`content_unlocks.cpp`) |
| `src/data_table.cpp` | `UDataTable` loading, row walking and `AddRow`, shared by the table patches; inserting into the managers' `TMap<FName, …>`s after boot |
| `src/qol/` | the Quality of Life Improvements mod (`mod.ini`, `plugin_entry.cpp`: its entry and the genre 9 requests patch): `fusernative.cpp`, the FuserNative features |
| `src/bpm_range/` | the BPM Range mod (early stage, C tier) |
| `src/fixes/` | the Fixes mod (C tier): the EOS_Shutdown exit guard |
| `src/debug/unlock_probe.cpp` | debug builds: unlock-state probe (F10) |
| `src/fifth_deck/` | the Fifth Deck mod (`mod.ini`, `plugin_entry.cpp`, `fifth_deck.cpp`): the fifth deck (Settings > Mods > Fifth Deck, `FifthDeck=` in FuserNative.ini; the three builds below): a deck on track `Deck0A` (17, unused outside Battle) with its controls (`BP_DeckUi`), at the right end of a widened desk (`SM_WorkspaceDesk_5Deck` in its pak, the stock desk with its arc lengthened, swapped onto the desk; without it, no fifth deck). While it is up, track 17's silent `UDummyAudioTrack` is swapped for a real `UAudioTrack` playing the stem event `event:/song/stems/lead` (a deck's level and compressor), and each of its stems is moved off the Lead bus onto its own FMOD channel group under `bus:/Music/MusicSub` (master effects, no other deck's effects). Hot Clips keep their own track. A smart riser also plays its stem's riser (the stem's `SongTransitionCelData`, as each stock deck's riser plays on its `TransitionSong` track): track 18 (`Deck1A`) gets a second real `UAudioTrack` for it, playing Deck3's riser event once (the riser tracks play their cel once, other tracks loop it) at the riser tracks' level, muted with the fifth deck. In a freestyle set it goes up with the toggle on, once the desk view is ready; turned off on stage, its stems are ejected and the desk restored. Solo, mute, the queue switch, smart risers and the multiplayer fades and deck resets (round start, lobby) include it. In a multiplayer set it goes up on every client running this, hidden with the toggle off (no controls, desk unchanged), so another player's fifth deck plays, queues and lands on the beat here; clients without it hear nothing from it. A performer switch leaves it playing, like the other decks; while a player controls the decks with Fifth Deck off (or turns it off during their turn), their client keeps it clear in the background: stems ejected, solo / mute released. Recordings, saved moments and the hottest mix leave it out. Also Settings > Mods > Hot Clip Volume Normalisation (`HotClipVolume=`, off by default, independent of the deck): keeps Hot Clips on their louder quickclip event after a song audition. Debug builds: Ctrl+Shift+5 lays the desk out again and logs the deck's state |
| `src/multiplayer/` | the multiplayer mod (`mod.ini`, `plugin_entry.cpp`, `README.md`): matchmaking join, custom song sync and transfers, Diamond store, crate song limit, shared mixes, server content (`server_content.cpp`) |
| `lib/` | funchook (prebuilt `funchook.lib` + header); `lib/libarchive/`: static libarchive 3.8.9 + zlib 1.3.2 for the multiplayer DLL's community song archives, rebuilt by `tools/build_libarchive.ps1` |
| `assets/<Mod>/Content/` | each mod's cooked assets, packed into `<Mod>_P.pak` (`FUSERQualityOfLifeImprovements`: `BPL_FuserNative`, `WBP_FuserKeyBpm`; `FUSERFifthDeck`: `SM_WorkspaceDesk_5Deck`, from `tools/desk_5deck_fbx.py`, imported with `tools/modkit_import_mesh.py`) |
| `modkit/FuserModkit/` | the UE 4.24 editor project those assets are authored and cooked in |
| `docs/MOD_DEVELOPMENT.md` | a step-by-step tutorial for adding a new feature (assets, native code, settings, pak) |
| `CUSTOM_CONTENT.MD` | custom content (projector screens): pak format, loading requirements, test results, content tiers |
| `package/README.md` | the player README (`{{VERSION}}` filled in by `build.ps1 -Package`) |
| `tools/fuser_pak_create.py` | pak + sig writer |
| `tools/fuser_content_pak.py` | packs a custom content item (a projector screen video) into `<id>_P.pak` + `.sig` for `custom_content/` |
| `tools/derive_build.py` | derives fue's addresses for another FUSER executable (e.g. Steam) by static analysis |
| `tools/derive_mp_build.py` | derives `FUSERMultiplayer.dll`'s ~30 game addresses for another executable (`host_hook.cpp`, `kVerified`) |
| `tools/derive_fifth_deck_build.py` | derives the fifth deck's ~25 game addresses for another executable (`fifth_deck/fifth_deck.cpp`, `kBuildAddrs`) |
| `tools/desk_5deck_fbx.py` | Blender script: builds a five-deck desk FBX from the cooked stock `SM_WorkspaceDesk_Simple.uexp` (arc lengthened, ends kept), for `tools/modkit_import_mesh.py` |

## Supported executables

fue runs only on executables listed in `kBuilds` (`src/fue/fue.cpp`), selected by PE identity:

| Build | Exe | Status |
|---|---|--------|
| EGS v1.4.2 (engine CL 1310155) | `FuserEOS-Win64-Shipping.exe` | tested |
| EGS, later build (engine CL 1317808) | `FuserEOS-Win64-Shipping.exe` | tested |
| Steam (engine CL 1317755) | `Fuser-Win64-Shipping.exe` (SteamStub-wrapped) | tested |

Each entry holds 8 RVAs and prologue patterns, checked before anything is patched, and the globals are sanity-checked
at the first ProcessEvent: on any mismatch fue (and with it FuserNative and the unlocks) stays off, and the game runs
with the plain loader. To add a build, run
`python tools/derive_build.py --target <exe> --name "<name>" --ref <FuserEOS-Win64-Shipping.exe>` (or set
`FUSER_GAME_ROOT`) and paste the printed entry once every address is CONFIRMED. A SteamStub-wrapped exe has an
encrypted code section on disk: analyse an unpacked copy (e.g. from Steamless) as `--target` and pass the shipped exe
as `--identity`, so the entry matches the exe that actually runs. If the engine changelist differs from EGS's, the
tool will not report OVERALL confirmed; check that the engine functions fue relies on are unchanged before adding it.

`FUSERMultiplayer.dll` has its own per-build table (`kVerified` in `src/multiplayer/host_hook.cpp`, about 30 RVAs:
the functions it calls and hooks), with rows for all three builds above. Its multiplayer hooks install only on a
build whose row is complete, and the log says which (`[host] build <name>: multiplayer addresses verified`). To
add a build, run `python tools/derive_mp_build.py --target <exe> --name "<fue build name>" --ref <EGS v1.4.2 exe>`
(an unpacked copy for Steam) and paste the printed row once every address is CONFIRMED. Besides locating each
function, it compares the function's whole body with EGS v1.4.2's, ignoring only fields that move with the code
(relative branches and references, jump tables). A body that differs means a field offset or vtable slot the DLL
hard-codes may have changed, so review it before adding the row. `check` entries in its list cover the
unreflected offsets that none of the called functions use. `RETIRED` entries are the removed listen-host code's
functions: still derived, but left out of the row. `--self-test` must reproduce the EGS v1.4.2 row.

The fifth deck (`src/fifth_deck/fifth_deck.cpp`) has a third table, `kBuildAddrs` (about 25 RVAs), with rows for all three
builds. On a build without a row there is no fifth deck and no Fifth Deck setting. To add a build, run
`python tools/derive_fifth_deck_build.py --target <exe> --name "<fue build name>" --ref <EGS v1.4.2 exe>` (an unpacked
copy for Steam) and paste the printed row once every address is CONFIRMED with its body the same; its `check`
entries cover the unreflected offsets the module hard-codes. `--self-test` must reproduce the EGS v1.4.2 row.

## The cooked assets

`assets/FUSERQualityOfLifeImprovements/Content/Mods/FuserNative/` holds the cooked `BPL_FuserNative` (the key/BPM
call points, bound to native code at runtime) and `WBP_FuserKeyBpm` (the key/BPM line widget);
`assets/FUSERFifthDeck/Content/Mods/FuserNative/` holds `SM_WorkspaceDesk_5Deck`. Each mod's folder becomes its own
`<Mod>_P.pak`; the asset paths stay under `/Game/Mods/FuserNative/`. They are authored in `modkit/FuserModkit/` (a
Blueprint-only UE 4.24 project; File > Cook Content for Windows) and copied here from its
`Saved/Cooked/WindowsNoEditor/FuserModkit/Content/`. `docs/MOD_DEVELOPMENT.md` walks through the whole cycle.

## AI Usage Disclosure

AI/LLM was used heavily in the production of this loader.

## Licences

The loader is based on FuserSongLoader / FuserModDoorstop, MIT, (c) 2020 Mettra (`LICENSE.FuserSongLoader`).
funchook is GPLv2+ with a linking exception (see `lib/funchook.h`).
FUSERMultiplayer.dll links libarchive (BSD-2, `lib/libarchive/LICENSE.libarchive`) and zlib (zlib licence,
`lib/libarchive/LICENSE.zlib`); both notices ship in the player package.
