# FUSERNativeLoader

The FUSER mod loader (a `winhttp.dll` proxy, based on the community FuserSongLoader) with a minimal UE 4.24
reflection core (**fue**) and the **FuserNative** features built in: key/BPM on the HUD song items and the crate
editor's song tiles, Key Labels, real song tabs, song search with `key:` / `bpm:` / `genre:` filter words, Key and BPM sorting,
crate-edit tabs, new-crate slot sizes, the mod settings rows, and free unlocks for custom and event songs. Three build
outputs:

| Output | What |
|---|---|
| `winhttp.dll` | the loader: pak-signature bypass for `*_P` paks, BPM range patch, fue, unlocks, FuserNative |
| `FUSERMultiplayer.dll` | optional multiplayer client extension (see Multiplayer), loaded by `winhttp.dll` when present beside it |
| `FUSERNativeMods_P.pak` (+ `.sig`) | FuserNative's cooked Blueprint assets (`/Game/Mods/FuserNative/`) |

## AI Usage Disclosure

This project was coded predominantly using an LLM.

## Using the mod

If you just want to use the mod, go to releases and download the latest version. All information below regarding requirements
are for building/development on the mod.

## Requirements

- Windows, **Visual Studio 2022** (MSVC x64, C++20) and **CMake 3.20+** (e.g. from a Developer PowerShell).
- **Python 3** for the pak tool (standard library only).
- Optional, for `tools/derive_build.py` and `tools/derive_mp_build.py`: `pefile`, `capstone`, `numpy`.

## Build

```powershell
.\build.ps1                          # out\Release\: both DLLs (+ PDBs), FUSERNativeMods_P.pak / .sig
.\build.ps1 -Configuration Debug     # out\Debug\: diagnostic winhttp.dll, multiplayer DLL, same pak
.\build.ps1 -Package                 # also out\FUSERNativeLoader-<version>.zip (+ -symbols.zip)
```

The **Debug** configuration is the diagnostic build (`FNL_DEBUG`): a console with the loader's tracing, the F6-F10
debug keys (F6 key/BPM self-test, F7 song probe, F8 crate-edit and settings dumps, F9 fue probe, F10 unlock state)
and the probes. Both configurations are optimised. The version comes from `VERSION`.

## Install for testing

```powershell
$env:FUSER_GAME_ROOT = '<folder containing Fuser\>'
.\install.ps1 [-Configuration Debug]   # keeps the previous winhttp.dll as winhttp.dll.fnl-backup
.\install.ps1 -Restore
```

Settings: `Fuser\Binaries\Win64\FuserNative.ini`; log: `Fuser\Binaries\Win64\FuserNative.log`.
`ExtraSortOptions=0` in the ini removes Key and BPM from the crate editor's Sort dropdown (no Settings row).
Remove or rename `FUSERMultiplayer.dll` to run the base FUSERNative features without multiplayer hooks.
Custom content paks (`tools/fuser_content_pak.py`) go in `Fuser\Content\Paks\custom_content\`, next to `custom_songs\`.
Server content (fuserd's items, kept by the multiplayer DLL) lives in `Fuser\Content\ServerContent\`; `ServerContentWaitSeconds`
(default 30, 0 = never) caps how long the title screen waits for the login sync before the main menu.
The multiplayer DLL requires `winhttp.dll` and is not a standalone proxy.

## Layout

| Path | |
|---|---|
| `src/dllmain.cpp` | the loader (proxy exports, pak-signature bypass, BPM patches, legacy unlock-pak rename) |
| `src/fnl_entry.cpp` | `fnl_start`: fue, unlocks, custom content, FuserNative; the debug fue probe |
| `src/fue/` | the reflection core: objects, properties, ProcessEvent / tick / native hooks, per-build addresses |
| `src/unlocks.cpp` | custom and event song unlocks, patched into the live `DT_UnlocksSongs` at startup |
| `src/custom_content.cpp` | custom and server content (`custom_content.hpp`): the content pak reader, the item registry, the startup registration (`custom_content/` free, server content locked) and the live one |
| `src/content/` | the content types (`content_types.cpp`: one handler per type; `content_projector.cpp`: projector screens) and the unlock rows every type shares (`content_unlocks.cpp`) |
| `src/data_table.cpp` | `UDataTable` loading, row walking and `AddRow`, shared by the table patches; inserting into the managers' `TMap<FName, …>`s after boot |
| `src/native/fusernative.cpp` | the FuserNative features |
| `src/debug/unlock_probe.cpp` | debug builds: unlock-state probe (F10) |
| `src/multiplayer/` | optional multiplayer DLL: matchmaking join, P2P tunnel, custom song sync, Diamond store, server content (`server_content.cpp`) |
| `lib/` | funchook (prebuilt `funchook.lib` + header); `lib/libarchive/`: static libarchive 3.8.9 + zlib 1.3.2 for the multiplayer DLL's community song archives, rebuilt by `tools/build_libarchive.ps1` |
| `assets/Content/` | the cooked assets packed into `FUSERNativeMods_P.pak` |
| `modkit/FuserModkit/` | the UE 4.24 editor project those assets are authored and cooked in |
| `docs/MOD_DEVELOPMENT.md` | a step-by-step tutorial for adding a new feature (assets, native code, settings, pak) |
| `CUSTOM_CONTENT.MD` | custom content (projector screens): pak format, loading requirements, test results, content tiers |
| `package/README.md` | the player README (`{{VERSION}}` filled in by `build.ps1 -Package`) |
| `tools/fuser_pak_create.py` | pak + sig writer |
| `tools/fuser_content_pak.py` | packs a custom content item (a projector screen video) into `<id>_P.pak` + `.sig` for `custom_content/` |
| `tools/derive_build.py` | derives fue's addresses for another FUSER executable (e.g. Steam) by static analysis |
| `tools/derive_mp_build.py` | derives `FUSERMultiplayer.dll`'s ~30 game addresses for another executable (`host_hook.cpp`, `kVerified`) |

## Multiplayer

`FUSERMultiplayer.dll` is the client side of multiplayer against a fuserd server; sessions are hosted by the FDS
dedicated server. It handles:

- the matchmaking join, with `JoinAddressOverride=` and the P2P tunnel (`P2P=1`) for joiners;
- custom songs: catalogue and community installs, song transfers between players (Download Songs / Send Songs to
  Other Players, Upload Speed Limit (`SendSongsKBps`, 256 KB/s-4 MB/s, default 2 MB/s) and Simultaneous Uploads
  (`SendSongsMax`, 1-4, default 2) in the game's settings; the upload rows are greyed out while Send Songs is off,
  and a change applies to the next transfer, not to ones under way), the sync notices on the event highway, and in
  the lobby a "Custom Songs Sync" panel with every player's progress, from fuserd's sync states (`SyncProgressBar=0`
  turns it off). Only performers' crate
  songs are fetched: a viewer's crate is never played;
- the server's crate song limit (fuserd `multiplayer.max_crate_songs`, read from `multiplayer/limits/`): entering
  multiplayer or the Main Stage with more songs fails the game's own crate check, with a "Crate Error" popup naming
  the limit. The same call, logged in, reports the loader's version and exe build to fuserd (at login and on each
  entry to multiplayer), which keeps stock FUSER players apart from loader users;
- the Diamond store's installs and tiles;
- a shared mix's custom songs: when a mix's playback starts, its song list comes from fuserd
  (`community/recording_songs/`), the songs this machine lacks are fetched like crate songs, and the playback waits
  for them (at most 2 minutes) behind a "Custom Songs" status popup, with a popup naming any that couldn't be fetched.
  Mixes the game only previews (the playback-complete screen's next mixes, the mix browser) aren't checked.
  Download Mix Songs in the game's settings (`DownloadMixSongs`, No / Temporarily / Permanently, default
  Temporarily) places them;
- the missing-stem skip (`MissingCelSkip`), so a song this machine lacks doesn't stall a DJ's queue;
- diagnostics: `DiagnosticLogs=1` (off by default, in every build) logs the clock samples during a set
  (`HostLobbyClockTrace=1` adds lobby samples) and the state / connection watch after a join. The Debug build
  (`FNL_DEBUG`) adds trace hooks, which a release build never installs: the drop chain and deck trace (`DeckTrace`),
  the leave trace (`LeaveTrace=1`), and the keys below.

**Player-hosted sessions** (fuserd makes the creating player the host when it runs no FDS servers) are a stub:
the loader logs that a local FDS server is needed and lets the stock join run. The earlier hosts - the player's
own game as a listen server, and a second headless game process (the sidecar) - were removed.
So were the client-side fixes every joiner ran in that era (`FDS_WORKAROUND`, `ClientWorkarounds=1`), removed on
2026-10-08 once FDS was confirmed to behave correctly without them.

Keys (Debug build only; game window focused): `Ctrl+Shift+J` direct join to `HostAddress` + `JoinOptions` (an FDS needs
`?playerSessionId=?gameSessionId=?hypeUserId=?buildConfig=`; the join also loads the lobby music and sets the
multiplayer game mode, which a plain `open` skips), `Ctrl+Shift+T` an 8 s ProcessEvent trace, `Ctrl+Shift+R` report, `Ctrl+Shift+U` install `StoreTestInstall`,
`Ctrl+Shift+N` STUN probe (`P2PProbe=1` runs it once after login), `Ctrl+Shift+X` song transfer test. Loader
messages go to `FuserNative.log`
(`FuserNative.2.log` for a second instance from the same install).

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

## The cooked assets

`assets/Content/Mods/FuserNative/` holds the cooked `BPL_FuserNative` (the key/BPM call points, bound to native code
at runtime) and `WBP_FuserKeyBpm` (the key/BPM line widget). They are authored in `modkit/FuserModkit/` (a
Blueprint-only UE 4.24 project; File > Cook Content for Windows) and copied here from its
`Saved/Cooked/WindowsNoEditor/FuserModkit/Content/`. `docs/MOD_DEVELOPMENT.md` walks through the whole cycle.

## AI Usage Disclosure

AI/LLM was used heavily in the production of this loader.

## Licences

The loader is based on FuserSongLoader / FuserModDoorstop, MIT, (c) 2020 Mettra (`LICENSE.FuserSongLoader`).
funchook is GPLv2+ with a linking exception (see `lib/funchook.h`).
FUSERMultiplayer.dll links libarchive (BSD-2, `lib/libarchive/LICENSE.libarchive`) and zlib (zlib licence,
`lib/libarchive/LICENSE.zlib`); both notices ship in the player package.
